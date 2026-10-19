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

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

// A cancellation flag shared by whoever starts background work and the work
// itself (background-tasks.hpp), plus callbacks for blocking calls that need a
// nudge to notice it: win-http.cpp wakes its wait on the network, so a call
// stuck for up to 90 s ends at once. Pure C++: no Qt, no libobs.
namespace trigglow::tasks {

class CancelToken {
public:
	CancelToken() = default;
	CancelToken(const CancelToken &) = delete;
	CancelToken &operator=(const CancelToken &) = delete;

	bool IsCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

	// Runs `onCancel` once: on the thread that calls Cancel(), or right here
	// if the token is already cancelled. Returns the id for Unsubscribe(), or
	// 0 when it already ran. Callbacks run under the token's lock: keep them
	// short (set an event) and never call back into this token from one.
	// Subscribing doesn't change the token, so work that only holds a const
	// reference to it can still subscribe.
	uint64_t Subscribe(std::function<void()> onCancel) const;

	// Once this returns, that callback is not running and never will. 0 is
	// ignored.
	void Unsubscribe(uint64_t id) const;

	// Idempotent and thread-safe.
	void Cancel();

private:
	mutable std::mutex mutex_;
	std::atomic<bool> cancelled_{false};
	mutable uint64_t nextId_ = 1;
	mutable std::vector<std::pair<uint64_t, std::function<void()>>> subscribers_;
};

// Subscribe() for the length of a scope. A null token subscribes to nothing.
class CancelSubscription {
public:
	CancelSubscription(const CancelToken *token, std::function<void()> onCancel)
		: token_(token),
		  id_(token ? token->Subscribe(std::move(onCancel)) : 0)
	{
	}
	~CancelSubscription()
	{
		if (token_)
			token_->Unsubscribe(id_);
	}
	CancelSubscription(const CancelSubscription &) = delete;
	CancelSubscription &operator=(const CancelSubscription &) = delete;

private:
	const CancelToken *token_;
	uint64_t id_;
};

} // namespace trigglow::tasks
