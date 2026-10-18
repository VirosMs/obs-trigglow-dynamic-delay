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

#include "frame-arena.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// The delay ring (v0.6.0, buffer pipeline design B.1/B.3): a fixed number of
// slot HEADERS plus a FrameArena holding the payloads. Pure C++, no OBS.
//
// Every slot runs a small state machine, and whoever owns a state is the only
// one that touches the slot's payload in it:
//
//   Free/Ready/Dropped --Reserve (graphics thread, in capture order)--> Reserved
//   Reserved --BeginEncode (encoder, generation must match)--> Encoding
//   Reserved/Encoding --MarkDropped (no raw frame, arena full, cancelled)--> Dropped
//   Encoding --Publish (encoder)--> Ready
//   Ready --Pin/Unpin (decoder, reads the payload without the lock)--> Ready
//
// captureNs is assigned at Reserve(), in capture order, so timestamps never
// decrease from the oldest slot to the newest whatever order the encoders
// finish in: playback's binary search stays valid with Reserved/Encoding/
// Dropped slots in between. "Can this be shown" lives in the state, not in a
// sentinel timestamp (v0.5.1's kPendingTimestamp is gone).
//
// Each reservation gets a new generation, unique across all rings. Work in flight
// carries it, and a stale job (the slot was reused, or the ring replaced)
// simply fails its BeginEncode/Publish and throws its result away.
//
// One mutex guards the headers and indices only, and it's only ever held for
// O(1)/O(log n) work: payload copies, encodes and decodes all happen outside
// it. It's never nested with any other lock (the arena's own mutex is always
// taken after releasing it).
namespace trigglow {

enum class SlotState : uint8_t { Free, Reserved, Encoding, Ready, Dropped };

const char *SlotStateName(SlotState state);

// Pure age-based selection over a ring viewed in LOGICAL order: positions
// 0..count-1, oldest first, captureNs non-decreasing. Exposed for tests.
namespace ringselect {

// Largest position with captureNs <= targetNs, or `count` if none (even the
// oldest frame is younger than targetNs).
template<typename CaptureAt> size_t LastAtOrBefore(size_t count, uint64_t targetNs, CaptureAt captureAt)
{
	if (count == 0 || captureAt(0) > targetNs)
		return count;
	size_t lo = 0, hi = count; // Answer in [lo, hi).
	while (hi - lo > 1) {
		const size_t mid = lo + (hi - lo) / 2;
		if (captureAt(mid) <= targetNs)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

// The frame to SHOW for targetNs: the newest Ready slot at or before the due
// one (LastAtOrBefore), looking back over at most maxBack non-Ready slots
// (dropped or still encoding) -- that's a repeat of an older frame, never a
// frame from the future, so the delay can't drift. `count` if none.
template<typename CaptureAt, typename ReadyAt>
size_t SelectForDisplay(size_t count, uint64_t targetNs, size_t maxBack, CaptureAt captureAt, ReadyAt readyAt)
{
	const size_t due = LastAtOrBefore(count, targetNs, captureAt);
	if (due == count)
		return count;
	for (size_t back = 0; back <= maxBack && back <= due; ++back) {
		if (readyAt(due - back))
			return due - back;
	}
	return count;
}

} // namespace ringselect

class FrameRing {
public:
	struct Payload {
		FrameArena::Allocation allocation;
		uint32_t bytes = 0; // Meaningful bytes (<= allocation.bytes).
		uint32_t width = 0;
		uint32_t height = 0;
		bool compressed = false; // false: tightly packed I420 (Y, U, V).
	};

	struct Reservation {
		size_t index = 0;
		uint64_t generation = 0; // 0 = none.
		uint64_t captureNs = 0;
	};

	// A Ready slot pinned by a reader: `data` stays valid until Unpin().
	struct ReadView {
		size_t index = 0;
		uint64_t generation = 0;
		uint64_t captureNs = 0;
		const uint8_t *data = nullptr;
		uint32_t bytes = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		bool compressed = false;
	};

	// The newest slot (any state) with captureNs <= target, for telling a
	// late decode from a dropped frame.
	struct DueSlot {
		bool exists = false;
		uint64_t captureNs = 0;
		SlotState state = SlotState::Free;
	};

	struct Counts {
		size_t buffered = 0; // Slots reserved at least once (the logical window).
		size_t reserved = 0;
		size_t encoding = 0;
		size_t ready = 0;
		size_t dropped = 0;
		size_t pinned = 0;
		uint64_t reserveFailures = 0; // Captures lost because the next slot was busy.
	};

	FrameRing(size_t slotCount, const FrameArena::Config &arena);
	FrameRing(const FrameRing &) = delete;
	FrameRing &operator=(const FrameRing &) = delete;

	size_t SlotCount() const { return headers_.size(); }
	FrameArena &Arena() { return arena_; }

	// --- Graphics thread (the only writer of the write position) ---

	// Claims the next slot for a capture taken at captureNs (clamped to never
	// go below the previous reservation). Frees the payload it held. False --
	// nothing changes, the capture is lost -- when that slot is still
	// Reserved/Encoding or pinned by a reader (an encoder or the decoder is a
	// whole ring behind).
	bool Reserve(uint64_t captureNs, Reservation &out);

	// --- Any thread ---

	// Reserved/Encoding -> Dropped if the generation still matches.
	bool MarkDropped(size_t index, uint64_t generation);

	// --- Encoders ---

	// Reserved -> Encoding if the generation still matches.
	bool BeginEncode(size_t index, uint64_t generation);
	// Encoding -> Ready, taking ownership of payload.allocation. False if the
	// slot was superseded: the caller still owns (and must free) the allocation.
	bool Publish(size_t index, uint64_t generation, const Payload &payload);

	// --- Readers (decode worker, or the graphics thread on the sync path) ---

	// Decode-ahead: pins the next Ready slot after afterNs whose captureNs is
	// <= windowEndNs. When afterNs is more than lateNs behind targetNs (the
	// reader fell behind, or just started), jumps straight to the newest Ready
	// slot <= targetNs instead (MJPEG is all-intra: seeking is free). Dropped
	// slots, and not-yet-Ready ones already due (<= targetNs), are skipped;
	// a not-yet-Ready one in the future stops the search (wait for it).
	bool PinNextForDecode(uint64_t afterNs, uint64_t targetNs, uint64_t windowEndNs, uint64_t lateNs,
			      ReadView &out);
	// Sync path: pins ringselect::SelectForDisplay(targetNs, maxBack).
	bool PinForDisplay(uint64_t targetNs, size_t maxBack, ReadView &out);
	void Unpin(const ReadView &view);

	// --- Queries ---
	DueSlot Due(uint64_t targetNs) const;
	bool OldestCaptureNs(uint64_t &out) const; // False while nothing was reserved yet.
	Counts GetCounts() const;                  // O(slots): telemetry only.

private:
	struct Header {
		uint64_t captureNs = 0;
		uint64_t generation = 0;
		SlotState state = SlotState::Free;
		uint16_t pins = 0;
		Payload payload;
	};

	// Under mutex_. Logical position (0 = oldest) -> slot index.
	size_t IndexAt(size_t position) const { return (oldest_ + position) % headers_.size(); }
	void FillViewLocked(size_t index, ReadView &out);

	FrameArena arena_;
	mutable std::mutex mutex_;
	std::vector<Header> headers_;
	size_t writeIndex_ = 0; // Next slot Reserve() claims.
	size_t buffered_ = 0;   // Slots reserved at least once, <= headers_.size().
	size_t oldest_ = 0;     // Index of logical position 0.
	uint64_t lastCaptureNs_ = 0;
	uint64_t reserveFailures_ = 0;
};

} // namespace trigglow
