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

#include <functional>
#include <utility>

#include "background-tasks.hpp"

// The plugin's one tasks::Registry (background-tasks.hpp), tied to Qt's event
// loop: a job's result reaches the UI thread through a queued call on a
// QObject that the plugin owns and deletes at Shutdown(), so no queued call
// of this DLL is left in Qt's queue once OBS unloads it.
//
// Who owns what (one tasks::Group each, a member of the owner):
//   AuthManager           the sign-in check, the legacy migration, the
//                         sign-in start and its polls;
//   SupportService        Trigi's questions, summaries and events, tickets,
//                         their log attachments and reading the log (one per
//                         support dialog);
//   TrigglowDelayDock     the update check.
// Two calls belong to no owner and only Shutdown() cancels them: revoking the
// token on "Sign out" and telling the server a Trigi conversation was
// abandoned when the dialog closes.
namespace trigglow::uitasks {

// obs_module_load(), on the UI thread, before anything starts a job.
void Start();

// obs_module_unload(), on the UI thread, before the owners are destroyed:
// aborts every network call in flight, drops every result not delivered yet,
// refuses new jobs and waits for the threads, at most tasks::kShutdownLimit.
// Logs how long that took.
void Shutdown();

// The registry, or null outside Start()..Shutdown(). UI thread.
tasks::Registry *Registry();

// For RunAsync(): a job threw (its result is then R{}), or couldn't start.
void LogJobFailure(const char *component);
void LogJobRefused(const char *component);

// Runs work(token) on a background thread of `group`, then done(result) on the
// UI thread. done never runs once the group was cancelled (its owner is being
// destroyed) or the plugin shut down. Network calls in `work` must use the
// token (api::Client::WithCancel(), http::Request::cancel) so they end at
// once. false: refused, and done never runs.
template<typename R>
bool RunAsync(tasks::Group &group, const char *component, std::function<R(const tasks::CancelToken &)> work,
	      std::function<void(const R &)> done)
{
	const bool started =
		group.Run([component, work = std::move(work), done = std::move(done)](const tasks::TaskContext &ctx) {
			// R's default is the "no answer" outcome: an exception in the
			// work (out of memory) ends up as that instead of ending OBS.
			R result{};
			try {
				result = work(ctx.Token());
			} catch (...) {
				LogJobFailure(component);
			}
			ctx.Post([done, result = std::move(result)]() { done(result); });
		});
	if (!started && !group.Cancelled())
		LogJobRefused(component);
	return started;
}

} // namespace trigglow::uitasks
