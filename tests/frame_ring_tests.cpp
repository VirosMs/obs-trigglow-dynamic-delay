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

// Unit tests for trigglow::FrameRing (src/frame-ring.hpp): the slot state
// machine, generations, reservation failures, age-based selection with
// dropped slots in the way, decode-ahead, and payload freeing.

#include "check.hpp"
#include "frame-ring.hpp"

#include <cstring>
#include <vector>

using trigglow::FrameArena;
using trigglow::FrameRing;
using trigglow::SlotState;
namespace ringselect = trigglow::ringselect;

namespace {

constexpr uint64_t kDelta = 10; // A frame interval, in fake nanoseconds.

FrameArena::Config ArenaConfig()
{
	FrameArena::Config config;
	config.blockBytes = 4096;
	return config;
}

// Encodes and publishes `reservation` with a 100-byte payload whose bytes all
// equal `tag`.
bool PublishTagged(FrameRing &ring, const FrameRing::Reservation &reservation, uint8_t tag)
{
	if (!ring.BeginEncode(reservation.index, reservation.generation))
		return false;
	FrameRing::Payload payload;
	payload.allocation = ring.Arena().Allocate(100);
	std::memset(payload.allocation.data, tag, 100);
	payload.bytes = 100;
	payload.width = 4;
	payload.height = 2;
	payload.compressed = true;
	if (!ring.Publish(reservation.index, reservation.generation, payload)) {
		ring.Arena().Free(payload.allocation);
		return false;
	}
	return true;
}

void TestPureSelection()
{
	testcheck::g_currentTest = "ringselect";
	const std::vector<uint64_t> ts = {100, 110, 120, 120, 130, 140};
	const std::vector<bool> ready = {true, true, false, true, false, false};
	auto captureAt = [&](size_t p) {
		return ts[p];
	};
	auto readyAt = [&](size_t p) {
		return static_cast<bool>(ready[p]);
	};
	CHECK_EQ(ringselect::LastAtOrBefore(0, 500, captureAt), 0u); // Empty -> count.
	CHECK_EQ(ringselect::LastAtOrBefore(ts.size(), 99, captureAt), ts.size());
	CHECK_EQ(ringselect::LastAtOrBefore(ts.size(), 100, captureAt), 0u);
	CHECK_EQ(ringselect::LastAtOrBefore(ts.size(), 125, captureAt), 3u); // Last of the equal pair.
	CHECK_EQ(ringselect::LastAtOrBefore(ts.size(), 1000, captureAt), 5u);
	// Due 5 and 4 aren't Ready: back to 3 (maxBack 2), but not with maxBack 1.
	CHECK_EQ(ringselect::SelectForDisplay(ts.size(), 1000, 2, captureAt, readyAt), 3u);
	CHECK_EQ(ringselect::SelectForDisplay(ts.size(), 1000, 1, captureAt, readyAt), ts.size());
	// Never a frame from the future: target 119 -> due 1 (110).
	CHECK_EQ(ringselect::SelectForDisplay(ts.size(), 119, 8, captureAt, readyAt), 1u);
	CHECK_EQ(ringselect::SelectForDisplay(ts.size(), 50, 8, captureAt, readyAt), ts.size());
}

void TestStateMachine()
{
	testcheck::g_currentTest = "state machine";
	FrameRing ring(4, ArenaConfig());
	FrameRing::Reservation r0, r1;
	CHECK(ring.Reserve(100, r0));
	CHECK(ring.Reserve(110, r1));
	CHECK(r1.generation != r0.generation);
	CHECK_EQ(r0.index, 0u);
	CHECK_EQ(r1.index, 1u);
	// Publish without BeginEncode: refused.
	FrameRing::Payload empty;
	CHECK(!ring.Publish(r0.index, r0.generation, empty));
	// Wrong generation: refused.
	CHECK(!ring.BeginEncode(r0.index, r0.generation + 1000));
	CHECK(ring.BeginEncode(r0.index, r0.generation));
	CHECK(!ring.BeginEncode(r0.index, r0.generation)); // Only from Reserved.
	CHECK(PublishTagged(ring, r1, 1));
	CHECK(!ring.MarkDropped(r1.index, r1.generation)); // Ready can't be dropped.
	CHECK(ring.MarkDropped(r0.index, r0.generation));  // Encoding can.
	FrameRing::Payload late;
	late.allocation = ring.Arena().Allocate(10);
	CHECK(!ring.Publish(r0.index, r0.generation, late)); // Dropped meanwhile: the encoder frees it.
	ring.Arena().Free(late.allocation);

	const FrameRing::Counts counts = ring.GetCounts();
	CHECK_EQ(counts.buffered, 2u);
	CHECK_EQ(counts.ready, 1u);
	CHECK_EQ(counts.dropped, 1u);

	// Monotonic captureNs: a smaller timestamp is clamped up.
	FrameRing::Reservation r2;
	CHECK(ring.Reserve(50, r2));
	CHECK_EQ(r2.captureNs, 110u);
}

void TestReserveFailures()
{
	testcheck::g_currentTest = "reserve failures";
	FrameRing ring(3, ArenaConfig());
	FrameRing::Reservation r[3];
	for (int i = 0; i < 3; ++i)
		CHECK(ring.Reserve(100 + i * kDelta, r[i]));
	// Slot 0 is still Reserved: the next capture can't take it.
	FrameRing::Reservation next;
	CHECK(!ring.Reserve(130, next));
	CHECK_EQ(ring.GetCounts().reserveFailures, 1u);
	// Encoding: still busy.
	CHECK(ring.BeginEncode(r[0].index, r[0].generation));
	CHECK(!ring.Reserve(130, next));
	// Ready but pinned by a reader: busy.
	FrameRing::Payload payload;
	payload.allocation = ring.Arena().Allocate(64);
	payload.bytes = 64;
	CHECK(ring.Publish(r[0].index, r[0].generation, payload));
	FrameRing::ReadView view;
	CHECK(ring.PinForDisplay(100, 0, view));
	CHECK_EQ(view.index, 0u);
	CHECK(!ring.Reserve(130, next));
	ring.Unpin(view);
	// Free to go now; its payload is released.
	CHECK_EQ(ring.Arena().GetStats().liveBytes, 64u);
	CHECK(ring.Reserve(130, next));
	CHECK_EQ(next.index, 0u);
	CHECK_EQ(ring.Arena().GetStats().liveBytes, 0u);
	// A stale job for the reused slot can't touch it.
	CHECK(!ring.BeginEncode(r[0].index, r[0].generation));
	// Dropped slots are reusable.
	CHECK(ring.MarkDropped(r[1].index, r[1].generation));
	CHECK(ring.Reserve(140, next));
	CHECK_EQ(next.index, 1u);
}

// 10 slots at 10ns spacing; a few dropped and one still encoding.
void TestSelectionWithDrops()
{
	testcheck::g_currentTest = "selection by age with drops";
	FrameRing ring(10, ArenaConfig());
	std::vector<FrameRing::Reservation> r(10);
	for (int i = 0; i < 10; ++i)
		CHECK(ring.Reserve(1000 + i * kDelta, r[i]));
	for (int i = 0; i < 10; ++i) {
		if (i == 4 || i == 5)
			CHECK(ring.MarkDropped(r[i].index, r[i].generation));
		else if (i == 8)
			CHECK(ring.BeginEncode(r[i].index, r[i].generation)); // Still encoding.
		else
			CHECK(PublishTagged(ring, r[i], static_cast<uint8_t>(i)));
	}
	FrameRing::ReadView view;
	// Exactly on a frame.
	CHECK(ring.PinForDisplay(1030, 4, view));
	CHECK_EQ(view.captureNs, 1030u);
	CHECK_EQ(view.data[0], 3u);
	ring.Unpin(view);
	// Between frames: the older one (never the future).
	CHECK(ring.PinForDisplay(1039, 4, view));
	CHECK_EQ(view.captureNs, 1030u);
	ring.Unpin(view);
	// Due slot dropped (1050, and 1040 before it): repeat 1030.
	CHECK(ring.PinForDisplay(1055, 4, view));
	CHECK_EQ(view.captureNs, 1030u);
	ring.Unpin(view);
	FrameRing::DueSlot due = ring.Due(1055);
	CHECK(due.exists);
	CHECK_EQ(due.captureNs, 1050u);
	CHECK(due.state == SlotState::Dropped);
	// Due slot still encoding: the previous Ready one.
	CHECK(ring.PinForDisplay(1085, 4, view));
	CHECK_EQ(view.captureNs, 1070u);
	ring.Unpin(view);
	CHECK(ring.Due(1085).state == SlotState::Encoding);
	// The same choice without pinning (what the decoded frame shown is
	// compared against).
	uint64_t candidate = 0;
	CHECK(ring.DisplayCandidateNs(1055, 4, candidate));
	CHECK_EQ(candidate, 1030u);
	CHECK(ring.DisplayCandidateNs(1085, 4, candidate));
	CHECK_EQ(candidate, 1070u);
	CHECK(!ring.DisplayCandidateNs(999, 4, candidate));
	CHECK(!ring.DisplayCandidateNs(1055, 1, candidate)); // 1050 and 1040 dropped, 1030 is 2 back.
	CHECK_EQ(ring.GetCounts().pinned, 0u);
	// Before the oldest frame: nothing.
	CHECK(!ring.PinForDisplay(999, 4, view));
	CHECK(!ring.Due(999).exists);
	uint64_t oldest = 0;
	CHECK(ring.OldestCaptureNs(oldest));
	CHECK_EQ(oldest, 1000u);

	// Wrap around: two more captures overwrite the two oldest.
	FrameRing::Reservation w0, w1;
	CHECK(ring.Reserve(1100, w0));
	CHECK(ring.Reserve(1110, w1));
	CHECK(ring.OldestCaptureNs(oldest));
	CHECK_EQ(oldest, 1020u);
	CHECK(!ring.PinForDisplay(1015, 4, view)); // 1000/1010 are gone.
	CHECK(ring.PinForDisplay(1025, 4, view));
	CHECK_EQ(view.captureNs, 1020u);
	ring.Unpin(view);
}

void TestDecodeAhead()
{
	testcheck::g_currentTest = "decode ahead";
	FrameRing ring(16, ArenaConfig());
	std::vector<FrameRing::Reservation> r(12);
	for (int i = 0; i < 12; ++i)
		CHECK(ring.Reserve(1000 + i * kDelta, r[i]));
	for (int i = 0; i < 12; ++i) {
		if (i == 3)
			CHECK(ring.MarkDropped(r[i].index, r[i].generation));
		else if (i == 7 || i == 6)
			CHECK(ring.BeginEncode(r[i].index, r[i].generation));
		else
			CHECK(PublishTagged(ring, r[i], static_cast<uint8_t>(i)));
	}
	FrameRing::ReadView view;
	// First call (afterNs 0): jumps to the frame due at the target (1010).
	CHECK(ring.PinNextForDecode(0, 1015, 1015 + 3 * kDelta, kDelta, view));
	CHECK_EQ(view.captureNs, 1010u);
	ring.Unpin(view);
	// In order from there, skipping the dropped 1030.
	CHECK(ring.PinNextForDecode(1010, 1015, 1045, kDelta, view));
	CHECK_EQ(view.captureNs, 1020u);
	ring.Unpin(view);
	CHECK(ring.PinNextForDecode(1020, 1015, 1045, kDelta, view));
	CHECK_EQ(view.captureNs, 1040u);
	ring.Unpin(view);
	// Window end: 1050 is past 1045.
	CHECK(!ring.PinNextForDecode(1040, 1015, 1045, kDelta, view));
	// 1060 still encoding and in the future (target 1055): wait, don't skip.
	CHECK(ring.PinNextForDecode(1040, 1055, 1085, kDelta, view));
	CHECK_EQ(view.captureNs, 1050u);
	ring.Unpin(view);
	CHECK(!ring.PinNextForDecode(1050, 1055, 1085, kDelta, view));
	// Same, but now 1060 and 1070 are already due (target 1075): skipped.
	CHECK(ring.PinNextForDecode(1050, 1075, 1105, kDelta, view));
	CHECK_EQ(view.captureNs, 1080u);
	ring.Unpin(view);
	// Far behind: jump to the frame due now, not the backlog.
	CHECK(ring.PinNextForDecode(1000, 1105, 1135, kDelta, view));
	CHECK_EQ(view.captureNs, 1100u);
	CHECK_EQ(view.data[0], 10u);
	ring.Unpin(view);
}

void TestPayloadsFreedOnReuse()
{
	testcheck::g_currentTest = "payloads freed on reuse";
	FrameRing ring(4, ArenaConfig());
	for (int lap = 0; lap < 50; ++lap) {
		FrameRing::Reservation r;
		CHECK(ring.Reserve(static_cast<uint64_t>(lap) * kDelta, r));
		CHECK(PublishTagged(ring, r, static_cast<uint8_t>(lap)));
	}
	// Only the last 4 payloads (128 bytes reserved each) are alive.
	CHECK_EQ(ring.Arena().GetStats().liveBytes, 4u * 128u);
	CHECK(ring.Arena().GetStats().committedBytes <= 2u * 4096u);
}

} // namespace

int main()
{
	TestPureSelection();
	TestStateMachine();
	TestReserveFailures();
	TestSelectionWithDrops();
	TestDecodeAhead();
	TestPayloadsFreedOnReuse();
	return testcheck::Finish("frame_ring_tests");
}
