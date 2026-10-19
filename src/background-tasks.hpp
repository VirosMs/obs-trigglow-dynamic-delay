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

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "cancel-token.hpp"

// Every background job of the plugin that isn't the video pipeline (the
// account, Trigi, tickets, the log for a ticket, the update check) runs
// through one Registry, so OBS can close cleanly while one of them is still
// waiting on the network (Trigi may take up to 90 s):
//
//   - Each job runs on its own thread and gets a CancelToken. Network calls
//     pass it to WinHTTP (win-http.hpp), which aborts a blocked call at once
//     when it is cancelled.
//   - A job hands its result to the UI thread with TaskContext::Post(). The
//     result waits in the registry's queue until the UI thread runs Drain();
//     the Waker only asks for that (it posts, it never runs anything).
//   - A Group belongs to one owner (a dialog, the dock, AuthManager). When the
//     owner goes away the group is cancelled: its jobs are aborted and none of
//     its results is delivered any more, even one already queued, so a result
//     never reaches a destroyed object.
//   - Shutdown() (obs_module_unload) cancels everything, drops every queued
//     result, refuses new jobs and waits for the threads, at most `limit`.
//     On Windows each thread also keeps the plugin's DLL loaded until it has
//     completely exited (FreeLibraryAndExitThread), so a thread still running
//     after the limit can never execute code from an unloaded DLL.
//
// Pure C++ (no Qt, no libobs) so it is tested on its own
// (tests/background_tasks_tests.cpp); ui-tasks.hpp is the plugin's glue to
// Qt's event loop.
namespace trigglow::tasks {

// How long obs_module_unload() waits in all for the threads to end.
inline constexpr std::chrono::milliseconds kShutdownLimit{2000};
// How long an owner's destructor waits for its group's threads.
inline constexpr std::chrono::milliseconds kOwnerLimit{500};

namespace detail {
struct RegistryState;
struct GroupState;
} // namespace detail

struct TaskContextAccess;

// What a job gets.
class TaskContext {
public:
	// Cancelled when the job's group is cancelled or the registry shuts down.
	const CancelToken &Token() const { return *token_; }
	bool Cancelled() const { return token_->IsCancelled(); }

	// Queues `fn` to run on the UI thread at the next Registry::Drain(),
	// unless by then the job's group was cancelled or the registry shut down.
	// false: dropped already (it never runs).
	bool Post(std::function<void()> fn) const;

private:
	friend struct TaskContextAccess;
	TaskContext(std::shared_ptr<detail::RegistryState> registry, std::shared_ptr<detail::GroupState> group,
		    std::shared_ptr<CancelToken> token);

	std::shared_ptr<detail::RegistryState> registry_;
	std::shared_ptr<detail::GroupState> group_; // Null for a job without a group.
	std::shared_ptr<CancelToken> token_;
};

using Work = std::function<void(const TaskContext &)>;

class Registry {
public:
	// Asks the UI thread to call Drain() soon. Called from any thread with the
	// registry's lock held: it must only post, never run Drain() itself.
	using Waker = std::function<void()>;

	explicit Registry(Waker waker);
	// Never blocks: if Shutdown() wasn't called, cancels like Shutdown(0).
	~Registry();
	Registry(const Registry &) = delete;
	Registry &operator=(const Registry &) = delete;

	// A job owned by no one, for fire-and-forget calls meant to outlive the
	// object that started them (telling the server a conversation was
	// abandoned, revoking a token): only Shutdown() cancels it. false: refused
	// (shut down, or no thread could be started); `work` never runs then.
	bool Run(Work work);

	// UI thread: runs the results queued by Post(), in order, skipping those
	// whose group was cancelled meanwhile. Nothing runs after Shutdown().
	void Drain();

	// Cancels every job (groups included), drops every queued result, refuses
	// new jobs and waits until the threads have ended, at most `limit`. true
	// when all of them ended in time. Safe to call again.
	bool Shutdown(std::chrono::milliseconds limit);

	bool IsShutDown() const;
	// Jobs whose work hasn't returned yet.
	size_t Running() const;
	// Results waiting for Drain().
	size_t Queued() const;
	// Jobs and delivered results that threw (the exception went no further).
	size_t Failures() const;

private:
	friend class Group;
	std::shared_ptr<detail::RegistryState> state_;
};

// The jobs of one owner. Create it with the owner, as a member.
class Group {
public:
	// registry: null (before the plugin started or after it shut down) makes
	// a group that refuses every job.
	explicit Group(Registry *registry);
	// CancelAndWait(kOwnerLimit).
	~Group();
	Group(const Group &) = delete;
	Group &operator=(const Group &) = delete;

	// false: refused (group cancelled, registry shut down, no thread); `work`
	// never runs then.
	bool Run(Work work);

	// Cancels this group's jobs, drops its queued results and refuses new
	// jobs, then waits for its threads, at most `limit` (not at all once the
	// registry is shut down: Shutdown() already waited). true when they all
	// returned in time. A job that didn't still can't deliver anything.
	bool CancelAndWait(std::chrono::milliseconds limit);

	// Cancelled itself, or the registry shut down.
	bool Cancelled() const;
	size_t Running() const;

private:
	std::shared_ptr<detail::RegistryState> registry_;
	std::shared_ptr<detail::GroupState> state_;
	uint64_t parentSubscription_ = 0;
};

} // namespace trigglow::tasks
