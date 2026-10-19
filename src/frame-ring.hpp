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
#include "storage-codec.hpp"

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
//
// Since v0.7.0 (RAM design 2.4) every payload says which storage codec made
// it, whether it is a key (decodable on its own) and where it sits in its
// bitstream (chain, seq): one ring can hold several codecs at once (raw
// fallbacks between MJPEG frames today; a hardware inter-frame codec and its
// MJPEG fallback later), and the reader decodes a P-frame only right after its
// reference (ringselect::Decodable()). Raw and MJPEG payloads are all keys.
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

// The packet a decoder decoded last (invariant I2). !valid: none yet, or
// since a discontinuity -- only a key can come next.
struct ChainCursor {
	bool valid = false;
	uint32_t chain = 0;
	uint32_t seq = 0;
};

// Whether a stored packet can be decoded next: a key always; a P-frame only
// right after its reference, i.e. the packet before it in the same chain.
inline bool Decodable(bool key, uint32_t chain, uint32_t seq, const ChainCursor &cursor)
{
	return key || (cursor.valid && chain == cursor.chain && seq == cursor.seq + 1);
}

// What PlanDecodeStart() needs of a slot.
struct ChainSlot {
	bool ready = false;
	bool key = false;
	uint32_t chain = 0;
	uint32_t seq = 0;
};

// Where to start decoding so the Ready frame at position `due` can be shown
// (invariant I5). found: keyPos is the key of due's chain it can be decoded
// from, with every packet in between still Ready (slots dropped before the
// encode carry no seq, so they don't count as gaps); catchUp of them must be
// decoded unshown first (0 when due is a key itself: always so for raw and
// MJPEG). nextKeyPos: the first Ready key after due (kNoPosition if none yet).
constexpr size_t kNoPosition = SIZE_MAX;
struct DecodeStart {
	bool found = false;
	size_t keyPos = 0;
	size_t catchUp = 0;
	size_t nextKeyPos = kNoPosition;
};

template<typename ChainAt> DecodeStart PlanDecodeStart(size_t count, size_t due, ChainAt chainAt)
{
	DecodeStart plan;
	if (due >= count)
		return plan;
	const ChainSlot target = chainAt(due);
	if (!target.ready)
		return plan;
	for (size_t p = due + 1; p < count; ++p) {
		const ChainSlot slot = chainAt(p);
		if (slot.ready && slot.key) {
			plan.nextKeyPos = p;
			break;
		}
	}
	if (target.key) {
		plan.found = true;
		plan.keyPos = due;
		return plan;
	}
	uint32_t expected = target.seq;
	for (size_t back = 1; back <= due; ++back) {
		const ChainSlot slot = chainAt(due - back);
		if (!slot.ready)
			continue; // Dropped before its encode (no seq), or not stored: a gap shows in the seqs.
		if (slot.chain != target.chain || slot.seq != expected - 1)
			return plan; // Another chain, or a packet of this one is missing: wait for the next key.
		expected = slot.seq;
		++plan.catchUp;
		if (slot.key) {
			plan.found = true;
			plan.keyPos = due - back;
			return plan;
		}
	}
	plan.catchUp = 0; // Its key was overwritten already.
	return plan;
}

enum class StartChoice : uint8_t { CatchUp, WaitForKey };

// Decode the catch-up frames now (unshown) or hold the frame on screen until
// the next key: whatever shows a fresh frame sooner. decodeNs: what one decode
// takes (p50); intervalNs: the capture interval.
inline StartChoice ChooseDecodeStart(const DecodeStart &plan, size_t due, uint64_t decodeNs, uint64_t intervalNs)
{
	if (!plan.found)
		return StartChoice::WaitForKey;
	if (plan.catchUp == 0 || plan.nextKeyPos == kNoPosition || plan.nextKeyPos <= due)
		return StartChoice::CatchUp; // Nothing to decode first, or no key in sight to wait for.
	return plan.catchUp * decodeNs <= (plan.nextKeyPos - due) * intervalNs ? StartChoice::CatchUp
									       : StartChoice::WaitForKey;
}

} // namespace ringselect

class FrameRing {
public:
	struct Payload {
		FrameArena::Allocation allocation;
		uint32_t bytes = 0; // Meaningful bytes (<= allocation.bytes).
		uint32_t width = 0;
		uint32_t height = 0;
		// Raw: tightly packed I420 (Y, U, V). Replaces v0.6.0's `compressed`.
		storage::CodecId codec = storage::CodecId::Raw;
		bool key = true;                    // Decodable on its own (raw and MJPEG: always).
		uint32_t chain = storage::kNoChain; // Bitstream session (storage::ChainTracker).
		uint32_t seq = 0;                   // Position in its chain, consecutive in the bitstream.
	};

	struct Reservation {
		size_t index = 0;
		uint64_t generation = 0; // 0 = none.
		uint64_t captureNs = 0;
		uint32_t tag = 0; // The caller's label for the capture (the degradation rung it was taken at).
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
		storage::CodecId codec = storage::CodecId::Raw;
		bool key = true;
		uint32_t chain = storage::kNoChain;
		uint32_t seq = 0;
		uint32_t tag = 0; // Reservation::tag.
	};

	// The newest slot (any state) with captureNs <= target, for telling a
	// late decode from a dropped frame. codec/key/chain/seq: Ready only.
	struct DueSlot {
		bool exists = false;
		uint64_t captureNs = 0;
		SlotState state = SlotState::Free;
		uint32_t tag = 0; // Reservation::tag.
		storage::CodecId codec = storage::CodecId::Raw;
		bool key = true;
		uint32_t chain = storage::kNoChain;
		uint32_t seq = 0;
	};

	// PlanDecodeStart(), in capture times.
	struct DecodePlan {
		bool due = false; // There is a Ready frame to show for the target (SelectForDisplay()).
		uint64_t dueNs = 0;
		bool found = false; // A key it can be decoded from is still in the ring.
		uint64_t keyNs = 0;
		uint32_t catchUp = 0;         // Frames to decode unshown from keyNs on, before the due one.
		bool nextKey = false;         // A Ready key after the due frame.
		uint32_t framesToNextKey = 0; // Slots from the due frame to it.
		ringselect::StartChoice choice = ringselect::StartChoice::WaitForKey;
	};

	// One slot as Snapshot() reports it (codec/key/chain/seq: Ready only).
	struct SlotSummary {
		SlotState state = SlotState::Free;
		uint64_t captureNs = 0;
		storage::CodecId codec = storage::CodecId::Raw;
		bool key = true;
		uint32_t chain = storage::kNoChain;
		uint32_t seq = 0;
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
	// go below the previous reservation), labelled `tag` (handed back with the
	// reservation, every view of the slot and Due()). Frees the payload it
	// held. False -- nothing changes, the capture is lost -- when that slot is
	// still Reserved/Encoding or pinned by a reader (an encoder or the decoder
	// is a whole ring behind).
	bool Reserve(uint64_t captureNs, Reservation &out, uint32_t tag = 0);

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
	// slot <= targetNs instead (free with raw and MJPEG, all keys; a P-frame
	// it lands on fails ringselect::Decodable() and the reader moves on to the
	// next key). Dropped slots, and not-yet-Ready ones already due (<=
	// targetNs), are skipped; a not-yet-Ready one in the future stops the
	// search (wait for it).
	bool PinNextForDecode(uint64_t afterNs, uint64_t targetNs, uint64_t windowEndNs, uint64_t lateNs,
			      ReadView &out);
	// Sync path: pins ringselect::SelectForDisplay(targetNs, maxBack).
	bool PinForDisplay(uint64_t targetNs, size_t maxBack, ReadView &out);
	// The capture time of the slot PinForDisplay() would pin, without pinning
	// it: the frame that should be on screen now. False if there is none.
	bool DisplayCandidateNs(uint64_t targetNs, size_t maxBack, uint64_t &captureNs) const;
	void Unpin(const ReadView &view);

	// --- Queries ---
	DueSlot Due(uint64_t targetNs) const;
	bool OldestCaptureNs(uint64_t &out) const; // False while nothing was reserved yet.
	Counts GetCounts() const;                  // O(slots): telemetry only.
	// Where a reader that has to (re)start on the frame shown for targetNs
	// begins decoding (ringselect::PlanDecodeStart()/ChooseDecodeStart(),
	// decodeNs being one decode's cost). O(log n) plus the slots walked back
	// to the key and on to the next one. With raw and MJPEG the due frame is
	// always its own key: catchUp 0, choice CatchUp.
	DecodePlan PlanDecodeStart(uint64_t targetNs, size_t maxBack, uint64_t decodeNs, uint64_t intervalNs) const;
	// Every slot, oldest first. O(slots): tests and diagnostics only.
	std::vector<SlotSummary> Snapshot() const;

private:
	struct Header {
		uint64_t captureNs = 0;
		uint64_t generation = 0;
		SlotState state = SlotState::Free;
		uint16_t pins = 0;
		uint32_t tag = 0;
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
