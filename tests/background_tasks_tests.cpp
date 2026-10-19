/*
Trigglow Dynamic Delay for OBS
Copyright (C) 2026 Trigglow (VirosMs)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// Unit tests for the background jobs' lifecycle (src/cancel-token.hpp,
// src/background-tasks.hpp): the cancellation token and its callbacks, the
// registry and its groups, results reaching the "UI thread" (this test's main
// thread, which plays Qt's event loop: it waits for the waker and calls
// Drain()), and above all that nothing is delivered once an owner is gone or
// the registry is shut down, and that shutting down stays within its limit.
// A "network call" here is a job blocked on a condition variable that only
// its cancellation callback wakes, as win-http.cpp's wait is.

#include "background-tasks.hpp"
#include "cancel-token.hpp"
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using trigglow::tasks::CancelSubscription;
using trigglow::tasks::CancelToken;
using trigglow::tasks::Group;
using trigglow::tasks::Registry;
using trigglow::tasks::TaskContext;
using Clock = std::chrono::steady_clock;

namespace {

long long MsSince(Clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// The test's stand-in for Qt's event loop: the waker records a request, the
// main thread waits for one and runs Drain().
class FakeUiLoop {
public:
	Registry::Waker Waker()
	{
		return [this] {
			std::lock_guard<std::mutex> lock(mutex_);
			++wakes_;
			cv_.notify_all();
		};
	}

	// Waits for a wake request (consuming it). false on timeout.
	bool WaitForWake(std::chrono::milliseconds limit)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (!cv_.wait_for(lock, limit, [this] { return wakes_ > 0; }))
			return false;
		--wakes_;
		return true;
	}

	int Wakes()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return wakes_;
	}

private:
	std::mutex mutex_;
	std::condition_variable cv_;
	int wakes_ = 0;
};

// A blocking call that only returns when the job is cancelled (or after
// `limit`), like a WinHTTP wait on a request the server never answers.
// Returns true when it was cancelled.
bool BlockLikeNetwork(const TaskContext &ctx, std::chrono::milliseconds limit)
{
	std::mutex m;
	std::condition_variable cv;
	bool woken = false;
	const CancelSubscription wake(&ctx.Token(), [&] {
		std::lock_guard<std::mutex> lock(m);
		woken = true;
		cv.notify_all();
	});
	std::unique_lock<std::mutex> lock(m);
	return cv.wait_for(lock, limit, [&] { return woken; });
}

// Waits (polling) until `registry` has no job running, at most `limit`.
bool WaitIdle(const Registry &registry, std::chrono::milliseconds limit)
{
	const Clock::time_point end = Clock::now() + limit;
	while (registry.Running() > 0) {
		if (Clock::now() > end)
			return false;
		std::this_thread::sleep_for(5ms);
	}
	return true;
}

void TestTokenCallbacks()
{
	testcheck::g_currentTest = "token callbacks";
	CancelToken token;
	int a = 0, b = 0, c = 0;
	const uint64_t ida = token.Subscribe([&] { ++a; });
	const uint64_t idb = token.Subscribe([&] { ++b; });
	CHECK(ida != 0 && idb != 0 && ida != idb);
	token.Unsubscribe(idb);
	CHECK(!token.IsCancelled());
	token.Cancel();
	token.Cancel(); // Idempotent: callbacks run once.
	CHECK(token.IsCancelled());
	CHECK(a == 1);
	CHECK(b == 0); // Unsubscribed before the cancel.
	// After the cancel: runs at once, on this thread, and there is nothing to
	// unsubscribe.
	CHECK(token.Subscribe([&] { ++c; }) == 0);
	CHECK(c == 1);
	token.Unsubscribe(ida); // Already gone: harmless.
	token.Unsubscribe(0);
	CHECK(token.Subscribe(nullptr) == 0);

	// The scope helper; a null token subscribes to nothing.
	CancelToken other;
	int d = 0;
	{
		const CancelSubscription sub(&other, [&] { ++d; });
		const CancelSubscription none(nullptr, [&] { ++d; });
	}
	other.Cancel();
	CHECK(d == 0);
}

// Unsubscribe() must not return while that callback runs: win-http.cpp closes
// the event the callback sets right after unsubscribing.
void TestUnsubscribeWaitsForRunningCallback()
{
	testcheck::g_currentTest = "unsubscribe waits";
	CancelToken token;
	std::atomic<bool> inside{false};
	std::atomic<bool> finished{false};
	const uint64_t id = token.Subscribe([&] {
		inside = true;
		std::this_thread::sleep_for(150ms);
		finished = true;
	});
	std::thread canceller([&] { token.Cancel(); });
	while (!inside)
		std::this_thread::yield();
	token.Unsubscribe(id);
	CHECK(finished.load());
	canceller.join();
}

void TestRunAndDeliver()
{
	testcheck::g_currentTest = "run and deliver";
	FakeUiLoop ui;
	Registry registry(ui.Waker());
	Group group(&registry);
	const std::thread::id uiThread = std::this_thread::get_id();
	std::atomic<int> delivered{0};
	std::atomic<int> wrongWorker{0}; // CHECK() isn't thread-safe: count, check later.
	bool onUiThread = true;
	for (int i = 0; i < 3; ++i) {
		CHECK(group.Run([&](const TaskContext &ctx) {
			if (ctx.Cancelled() || std::this_thread::get_id() == uiThread)
				++wrongWorker;
			const bool queued = ctx.Post([&] {
				onUiThread = onUiThread && std::this_thread::get_id() == uiThread;
				++delivered;
			});
			if (!queued)
				++wrongWorker;
		}));
	}
	CHECK(WaitIdle(registry, 5000ms));
	CHECK(wrongWorker == 0);
	// One wake for the three results: the waker only asks for a Drain().
	CHECK(ui.WaitForWake(2000ms));
	CHECK(ui.Wakes() == 0);
	CHECK(registry.Queued() == 3);
	registry.Drain();
	CHECK(delivered == 3);
	CHECK(onUiThread);
	CHECK(registry.Queued() == 0);
	CHECK(registry.Shutdown(2000ms));
}

// An owner going away (its Group cancelled) drops the results already queued
// for it, but not other owners' results.
void TestGroupCancelDropsQueuedResults()
{
	testcheck::g_currentTest = "group cancel drops results";
	FakeUiLoop ui;
	Registry registry(ui.Waker());
	Group kept(&registry);
	std::atomic<int> keptDelivered{0};
	std::atomic<int> goneDelivered{0};
	{
		Group gone(&registry);
		CHECK(gone.Run([&](const TaskContext &ctx) { ctx.Post([&] { ++goneDelivered; }); }));
		CHECK(kept.Run([&](const TaskContext &ctx) { ctx.Post([&] { ++keptDelivered; }); }));
		CHECK(WaitIdle(registry, 5000ms));
		CHECK(registry.Queued() == 2);
	} // ~Group: the owner is destroyed before the UI thread drained.
	CHECK(registry.Queued() == 1);
	registry.Drain();
	CHECK(goneDelivered == 0);
	CHECK(keptDelivered == 1);

	// A group cancelled by an earlier result in the same Drain().
	auto victim = std::make_unique<Group>(&registry);
	std::atomic<int> victimDelivered{0};
	CHECK(kept.Run([&](const TaskContext &ctx) { ctx.Post([&] { victim->CancelAndWait(100ms); }); }));
	CHECK(WaitIdle(registry, 5000ms));
	CHECK(victim->Run([&](const TaskContext &ctx) { ctx.Post([&] { ++victimDelivered; }); }));
	CHECK(WaitIdle(registry, 5000ms));
	registry.Drain();
	CHECK(victimDelivered == 0);
	CHECK(victim->Cancelled());
	CHECK(!victim->Run([](const TaskContext &) {})); // Refused once cancelled.
	CHECK(registry.Shutdown(2000ms));
}

// The owner's destructor aborts a job stuck "on the network", within
// kOwnerLimit, and its result never arrives.
void TestGroupDestructorAbortsBlockedJob()
{
	testcheck::g_currentTest = "owner destroyed mid-call";
	FakeUiLoop ui;
	Registry registry(ui.Waker());
	std::atomic<bool> started{false};
	std::atomic<bool> wasCancelled{false};
	std::atomic<int> delivered{0};
	const Clock::time_point start = Clock::now();
	{
		Group owner(&registry);
		CHECK(owner.Run([&](const TaskContext &ctx) {
			started = true;
			wasCancelled = BlockLikeNetwork(ctx, 90000ms);
			ctx.Post([&] { ++delivered; }); // Too late: refused.
		}));
		while (!started)
			std::this_thread::yield();
	}
	const long long ms = MsSince(start);
	std::printf("  owner destroyed with a blocked call: %lld ms\n", ms);
	CHECK(ms < trigglow::tasks::kOwnerLimit.count());
	CHECK(wasCancelled.load());
	CHECK(registry.Running() == 0);
	registry.Drain();
	CHECK(delivered == 0);
	CHECK(registry.Shutdown(2000ms));
}

// What obs_module_unload() does: several calls stuck for up to 90 s, results
// already queued, then Shutdown(). It returns well within kShutdownLimit, no
// result runs afterwards (queued or late), and nothing new starts.
void TestShutdownWithBlockedCalls()
{
	testcheck::g_currentTest = "shutdown with blocked calls";
	FakeUiLoop ui;
	Registry registry(ui.Waker());
	Group dialog(&registry);
	Group dock(&registry);
	std::atomic<int> started{0};
	std::atomic<int> cancelledCalls{0};
	std::atomic<int> delivered{0};
	std::atomic<bool> lateRefused{true};
	auto blocked = [&](const TaskContext &ctx) {
		++started;
		if (BlockLikeNetwork(ctx, 90000ms))
			++cancelledCalls;
		if (ctx.Post([&] { ++delivered; }))
			lateRefused = false;
	};
	CHECK(dialog.Run(blocked));
	CHECK(dialog.Run(blocked));
	CHECK(dock.Run(blocked));
	CHECK(registry.Run(blocked)); // A fire-and-forget call without an owner.
	// One result already waiting for the UI thread.
	CHECK(dock.Run([&](const TaskContext &ctx) { ctx.Post([&] { ++delivered; }); }));
	while (started < 4 || registry.Running() > 4)
		std::this_thread::yield();
	CHECK(registry.Queued() == 1);

	const Clock::time_point start = Clock::now();
	const bool allEnded = registry.Shutdown(trigglow::tasks::kShutdownLimit);
	const long long ms = MsSince(start);
	std::printf("  shutdown with 4 blocked calls: %lld ms\n", ms);
	CHECK(allEnded);
	CHECK(ms < 500);
	CHECK(cancelledCalls == 4);
	CHECK(lateRefused.load());
	CHECK(registry.Running() == 0);
	CHECK(registry.Queued() == 0);
	registry.Drain(); // A wake already posted still reaches Drain(): nothing runs.
	CHECK(delivered == 0);
	CHECK(registry.IsShutDown());
	CHECK(dialog.Cancelled() && dock.Cancelled());

	// Nothing starts any more.
	std::atomic<bool> ran{false};
	CHECK(!registry.Run([&](const TaskContext &) { ran = true; }));
	CHECK(!dialog.Run([&](const TaskContext &) { ran = true; }));
	Group later(&registry);
	CHECK(later.Cancelled());
	CHECK(!later.Run([&](const TaskContext &) { ran = true; }));
	Group none(nullptr);
	CHECK(none.Cancelled());
	CHECK(!none.Run([&](const TaskContext &) { ran = true; }));
	std::this_thread::sleep_for(20ms);
	CHECK(!ran.load());
	CHECK(registry.Shutdown(100ms)); // Again: fine.
}

// A job that ignores its token (a long file read) can't hold the shutdown
// past its limit; when it ends, its result is dropped.
void TestShutdownLimit()
{
	testcheck::g_currentTest = "shutdown limit";
	FakeUiLoop ui;
	auto registry = std::make_unique<Registry>(ui.Waker());
	std::atomic<bool> started{false};
	std::atomic<bool> posted{true};
	std::atomic<int> delivered{0};
	CHECK(registry->Run([&](const TaskContext &ctx) {
		started = true;
		std::this_thread::sleep_for(700ms); // Doesn't look at the token.
		posted = ctx.Post([&] { ++delivered; });
	}));
	while (!started)
		std::this_thread::yield();
	const Clock::time_point start = Clock::now();
	const bool allEnded = registry->Shutdown(200ms);
	const long long ms = MsSince(start);
	std::printf("  shutdown limit 200 ms with a stubborn job: %lld ms\n", ms);
	CHECK(!allEnded);
	CHECK(ms >= 150 && ms < 600);
	// The Registry object may go away before the job ends (its state is
	// shared with the job).
	CHECK(WaitIdle(*registry, 5000ms));
	registry.reset();
	std::this_thread::sleep_for(20ms);
	CHECK(!posted.load());
	CHECK(delivered == 0);
}

// Exceptions in a job don't escape, and the count still goes back to zero.
void TestThrowingJob()
{
	testcheck::g_currentTest = "throwing job";
	FakeUiLoop ui;
	Registry registry(ui.Waker());
	Group group(&registry);
	CHECK(group.Run([](const TaskContext &) { throw 42; }));
	CHECK(WaitIdle(registry, 5000ms));
	CHECK(registry.Failures() == 1);
	std::atomic<int> delivered{0};
	CHECK(group.Run([&](const TaskContext &ctx) { ctx.Post([&] { ++delivered; }); }));
	CHECK(ui.WaitForWake(5000ms));
	registry.Drain();
	CHECK(delivered == 1);
	// A throwing result doesn't stop the ones after it.
	CHECK(group.Run([&](const TaskContext &ctx) {
		ctx.Post([] { throw 7; });
		ctx.Post([&] { ++delivered; });
	}));
	CHECK(WaitIdle(registry, 5000ms));
	registry.Drain();
	CHECK(delivered == 2);
	CHECK(registry.Failures() == 2);
	CHECK(registry.Shutdown(2000ms));
}

// Jobs posting all the time while the UI thread drains and then shuts down:
// once Shutdown() has begun, no result ever runs. Repeated, with the
// shutdown at varying moments.
void TestShutdownRace()
{
	testcheck::g_currentTest = "shutdown race";
	int deliveredAfter = 0;
	long long worstMs = 0;
	for (int round = 0; round < 60; ++round) {
		FakeUiLoop ui;
		Registry registry(ui.Waker());
		Group group(&registry);
		bool shuttingDown = false; // UI thread only, like the results.
		int delivered = 0;
		for (int j = 0; j < 6; ++j) {
			group.Run([&](const TaskContext &ctx) {
				for (int k = 0; k < 200 && !ctx.Cancelled(); ++k) {
					ctx.Post([&] {
						++delivered;
						if (shuttingDown)
							++deliveredAfter;
					});
					if (k % 20 == 0)
						std::this_thread::sleep_for(1ms);
				}
				BlockLikeNetwork(ctx, 90000ms);
			});
		}
		const Clock::time_point until = Clock::now() + std::chrono::milliseconds(round % 7);
		while (Clock::now() < until) {
			if (ui.WaitForWake(1ms))
				registry.Drain();
		}
		shuttingDown = true;
		const Clock::time_point start = Clock::now();
		CHECK(registry.Shutdown(trigglow::tasks::kShutdownLimit));
		const long long ms = MsSince(start);
		if (ms > worstMs)
			worstMs = ms;
		registry.Drain();
		(void)delivered;
	}
	std::printf("  60 rounds: worst shutdown %lld ms\n", worstMs);
	CHECK(deliveredAfter == 0);
	CHECK(worstMs < 1000);
}

} // namespace

int main()
{
	TestTokenCallbacks();
	TestUnsubscribeWaitsForRunningCallback();
	TestRunAndDeliver();
	TestGroupCancelDropsQueuedResults();
	TestGroupDestructorAbortsBlockedJob();
	TestShutdownWithBlockedCalls();
	TestShutdownLimit();
	TestThrowingJob();
	TestShutdownRace();
	return testcheck::Finish("background_tasks_tests");
}
