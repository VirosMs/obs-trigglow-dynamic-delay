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

#include "cancel-token.hpp"

namespace trigglow::tasks {

uint64_t CancelToken::Subscribe(std::function<void()> onCancel) const
{
	if (!onCancel)
		return 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!cancelled_.load(std::memory_order_relaxed)) {
			const uint64_t id = nextId_++;
			subscribers_.emplace_back(id, std::move(onCancel));
			return id;
		}
	}
	// Already cancelled: nobody else can reach this callback, run it here.
	try {
		onCancel();
	} catch (...) {
		// Callbacks only set events; one that throws anyway must not undo the
		// cancel.
	}
	return 0;
}

void CancelToken::Unsubscribe(uint64_t id) const
{
	if (id == 0)
		return;
	// Cancel() runs the callbacks under this same lock, so taking it waits
	// for one that is running right now.
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
		if (it->first == id) {
			subscribers_.erase(it);
			return;
		}
	}
}

void CancelToken::Cancel()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (cancelled_.load(std::memory_order_relaxed))
		return;
	cancelled_.store(true, std::memory_order_release);
	for (auto &subscriber : subscribers_) {
		try {
			subscriber.second();
		} catch (...) {
			// As in Subscribe(): the next callbacks still run.
		}
	}
	subscribers_.clear();
}

} // namespace trigglow::tasks
