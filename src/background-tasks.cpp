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

#include "background-tasks.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <system_error>
#include <thread>
#endif

namespace trigglow::tasks {

namespace detail {

struct GroupState {
	std::shared_ptr<CancelToken> token = std::make_shared<CancelToken>();
	// Both guarded by RegistryState::mutex.
	bool cancelled = false;
	size_t running = 0;
};

struct QueuedResult {
	std::shared_ptr<GroupState> group; // Null for a job without a group.
	std::function<void()> fn;
};

// Shared by the Registry, its groups and every job, so a job that outlives
// the Registry object still has a valid lock and counters to report to.
struct RegistryState {
	explicit RegistryState(Registry::Waker w) : waker(std::move(w)) {}
	~RegistryState()
	{
#ifdef _WIN32
		for (HANDLE h : threads)
			CloseHandle(h);
#endif
	}
	RegistryState(const RegistryState &) = delete;
	RegistryState &operator=(const RegistryState &) = delete;

	std::mutex mutex;
	std::condition_variable changed; // A job's work returned.
	bool shutDown = false;
	size_t running = 0;
	size_t failures = 0; // Jobs and results that threw (swallowed: never into OBS).
	bool wakePending = false;
	std::deque<QueuedResult> queue;
	Registry::Waker waker;
	// Group-less jobs use it directly; every group's token follows it.
	std::shared_ptr<CancelToken> token = std::make_shared<CancelToken>();
#ifdef _WIN32
	// Handles of the threads started and not yet seen finished. Changed by
	// Run() only while not shut down, so Shutdown() can wait on them.
	std::vector<HANDLE> threads;
#endif
};

} // namespace detail

// The only way to build a TaskContext: jobs get theirs from StartJob().
struct TaskContextAccess {
	static TaskContext Make(std::shared_ptr<detail::RegistryState> registry,
				std::shared_ptr<detail::GroupState> group, std::shared_ptr<CancelToken> token)
	{
		return TaskContext(std::move(registry), std::move(group), std::move(token));
	}
};

namespace {

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
// Any address inside this module, for GetModuleHandleExW().
const char kModuleAnchor = 0;

struct ThreadStart {
	std::function<void()> body;
	HMODULE module = nullptr;
};

DWORD WINAPI ThreadMain(LPVOID param)
{
	HMODULE module = nullptr;
	{
		std::unique_ptr<ThreadStart> start(static_cast<ThreadStart *>(param));
		module = start->module;
		start->body();
	}
	// Drops this thread's reference to the module and ends the thread in one
	// step: no code of this module runs once the reference is gone, even if
	// it was the last one (OBS already unloaded the plugin).
	if (module)
		FreeLibraryAndExitThread(module, 0);
	return 0;
}

// Called with the registry's lock held (so Shutdown() always sees the
// handle). The new thread only takes that lock once its work is done. Takes
// `body` (moved, not copied: it may hold a token); gives it back on failure.
bool StartThread(detail::RegistryState &state, std::function<void()> &body)
{
	auto start = std::make_unique<ThreadStart>();
	start->body = std::move(body);
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&kModuleAnchor),
				&start->module))
		start->module = nullptr;
	HANDLE handle = CreateThread(nullptr, 0, ThreadMain, start.get(), 0, nullptr);
	if (!handle) {
		if (start->module)
			FreeLibrary(start->module);
		body = std::move(start->body);
		return false;
	}
	[[maybe_unused]] ThreadStart *const owned = start.release(); // ThreadMain owns it now.
	state.threads.push_back(handle);
	return true;
}

void ReapFinishedThreads(detail::RegistryState &state)
{
	auto &threads = state.threads;
	threads.erase(std::remove_if(threads.begin(), threads.end(),
				     [](HANDLE h) {
					     if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
						     return false;
					     CloseHandle(h);
					     return true;
				     }),
		      threads.end());
}
#else
// Elsewhere the threads are detached and Shutdown() waits for their work to
// return. (There is no HTTP client on macOS and Linux yet, so every job
// there is short.)
bool StartThread(detail::RegistryState &, std::function<void()> &body)
{
	try {
		std::thread(std::move(body)).detach();
		return true;
	} catch (const std::system_error &) {
		return false;
	}
}

void ReapFinishedThreads(detail::RegistryState &) {}
#endif

// Waits until no job's work is running, then (Windows) until their threads
// are completely gone. false if `deadline` came first.
bool WaitForJobs(detail::RegistryState &state, Clock::time_point deadline)
{
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		if (!state.changed.wait_until(lock, deadline, [&state] { return state.running == 0; }))
			return false;
	}
#ifdef _WIN32
	// Only called once shut down: Run() no longer touches the list.
	std::vector<HANDLE> handles;
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		handles = state.threads;
	}
	for (size_t i = 0; i < handles.size(); i += MAXIMUM_WAIT_OBJECTS) {
		const size_t count = std::min<size_t>(MAXIMUM_WAIT_OBJECTS, handles.size() - i);
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
		const DWORD waitMs = left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
		const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(count), handles.data() + i, TRUE, waitMs);
		if (r == WAIT_TIMEOUT || r == WAIT_FAILED)
			return false;
	}
#endif
	return true;
}

// Starts one job. `group` may be null; `token` is the group's or the
// registry's.
bool StartJob(const std::shared_ptr<detail::RegistryState> &state, const std::shared_ptr<detail::GroupState> &group,
	      const std::shared_ptr<CancelToken> &token, Work work)
{
	if (!work)
		return false;
	std::function<void()> body = [state, group, token, work = std::move(work)]() mutable {
		{
			// The work and everything it captured are gone before the job
			// counts as finished.
			Work local = std::move(work);
			const TaskContext context = TaskContextAccess::Make(state, group, token);
			bool threw = false;
			try {
				local(context);
			} catch (...) {
				// A job reports its own failures; an exception must never
				// end OBS.
				threw = true;
			}
			if (threw) {
				std::lock_guard<std::mutex> lock(state->mutex);
				++state->failures;
			}
		}
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			--state->running;
			if (group)
				--group->running;
		}
		state->changed.notify_all();
	};

	std::lock_guard<std::mutex> lock(state->mutex);
	if (state->shutDown || (group && group->cancelled))
		return false;
	ReapFinishedThreads(*state);
	++state->running;
	if (group)
		++group->running;
	if (StartThread(*state, body))
		return true;
	--state->running;
	if (group)
		--group->running;
	return false;
}

} // namespace

// --- TaskContext ----------------------------------------------------------------

TaskContext::TaskContext(std::shared_ptr<detail::RegistryState> registry, std::shared_ptr<detail::GroupState> group,
			 std::shared_ptr<CancelToken> token)
	: registry_(std::move(registry)),
	  group_(std::move(group)),
	  token_(std::move(token))
{
}

bool TaskContext::Post(std::function<void()> fn) const
{
	if (!fn)
		return false;
	std::lock_guard<std::mutex> lock(registry_->mutex);
	if (registry_->shutDown || (group_ && group_->cancelled))
		return false;
	registry_->queue.push_back({group_, std::move(fn)});
	if (!registry_->wakePending && registry_->waker) {
		registry_->wakePending = true;
		try {
			registry_->waker();
		} catch (...) {
			registry_->wakePending = false;
		}
	}
	return true;
}

// --- Registry ---------------------------------------------------------------------

Registry::Registry(Waker waker) : state_(std::make_shared<detail::RegistryState>(std::move(waker))) {}

Registry::~Registry()
{
	try {
		if (!IsShutDown())
			Shutdown(std::chrono::milliseconds(0));
	} catch (...) {
		// Only a lock that can't be taken throws here: nothing safe is left
		// to do, and the jobs keep their own reference to the state.
	}
}

bool Registry::Run(Work work)
{
	return StartJob(state_, nullptr, state_->token, std::move(work));
}

void Registry::Drain()
{
	std::deque<detail::QueuedResult> items;
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		state_->wakePending = false;
		if (state_->shutDown)
			return;
		items.swap(state_->queue);
	}
	for (detail::QueuedResult &item : items) {
		bool deliver = false;
		{
			// Checked right before each one: an earlier result may have
			// closed the owner of this one.
			std::lock_guard<std::mutex> lock(state_->mutex);
			deliver = !state_->shutDown && !(item.group && item.group->cancelled);
		}
		if (deliver) {
			try {
				item.fn();
			} catch (...) {
				// Never into Qt's event loop.
				std::lock_guard<std::mutex> lock(state_->mutex);
				++state_->failures;
			}
		}
		item.fn = nullptr;
	}
}

bool Registry::Shutdown(std::chrono::milliseconds limit)
{
	const Clock::time_point deadline = Clock::now() + limit;
	std::deque<detail::QueuedResult> dropped;
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		state_->shutDown = true;
		dropped.swap(state_->queue);
	}
	dropped.clear(); // Outside the lock: their captures may do anything.
	state_->token->Cancel();
	return WaitForJobs(*state_, deadline);
}

bool Registry::IsShutDown() const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->shutDown;
}

size_t Registry::Running() const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->running;
}

size_t Registry::Queued() const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->queue.size();
}

size_t Registry::Failures() const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->failures;
}

// --- Group ------------------------------------------------------------------------

Group::Group(Registry *registry)
	: registry_(registry ? registry->state_ : nullptr),
	  state_(std::make_shared<detail::GroupState>())
{
	if (!registry_) {
		state_->cancelled = true;
		state_->token->Cancel();
		return;
	}
	// Runs at once if the registry is already shut down.
	std::shared_ptr<CancelToken> own = state_->token;
	parentSubscription_ = registry_->token->Subscribe([own] { own->Cancel(); });
}

Group::~Group()
{
	try {
		CancelAndWait(kOwnerLimit);
		if (registry_)
			registry_->token->Unsubscribe(parentSubscription_);
	} catch (...) {
		// As in ~Registry(): only a lock that can't be taken throws here.
	}
}

bool Group::Run(Work work)
{
	if (!registry_)
		return false;
	return StartJob(registry_, state_, state_->token, std::move(work));
}

bool Group::CancelAndWait(std::chrono::milliseconds limit)
{
	if (!registry_)
		return true;
	const Clock::time_point deadline = Clock::now() + limit;
	std::deque<detail::QueuedResult> dropped;
	{
		std::lock_guard<std::mutex> lock(registry_->mutex);
		state_->cancelled = true;
		auto &queue = registry_->queue;
		for (auto it = queue.begin(); it != queue.end();) {
			if (it->group == state_) {
				dropped.push_back(std::move(*it));
				it = queue.erase(it);
			} else {
				++it;
			}
		}
	}
	dropped.clear();
	state_->token->Cancel();
	std::unique_lock<std::mutex> lock(registry_->mutex);
	// Once the registry is shut down its own wait was the only one: OBS's
	// unload doesn't wait again for each owner.
	if (registry_->shutDown)
		return state_->running == 0;
	return registry_->changed.wait_until(lock, deadline, [this] { return state_->running == 0; });
}

bool Group::Cancelled() const
{
	if (!registry_)
		return true;
	std::lock_guard<std::mutex> lock(registry_->mutex);
	return state_->cancelled || registry_->shutDown;
}

size_t Group::Running() const
{
	if (!registry_)
		return 0;
	std::lock_guard<std::mutex> lock(registry_->mutex);
	return state_->running;
}

} // namespace trigglow::tasks
