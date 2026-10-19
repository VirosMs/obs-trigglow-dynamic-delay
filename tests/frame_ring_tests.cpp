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
// dropped slots in the way, decode-ahead, and payload freeing; since v0.7.0
// the per-packet storage metadata (codec, key, chain, seq) with several codecs
// in one ring, ringselect::Decodable(), PlanDecodeStart() and
// ChooseDecodeStart() (RAM design 2.4, invariants I2/I5).

#include "check.hpp"
#include "frame-ring.hpp"

#include <cstring>
#include <vector>

using trigglow::FrameArena;
using trigglow::FrameRing;
using trigglow::SlotState;
namespace ringselect = trigglow::ringselect;
namespace storage = trigglow::storage;

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
	payload.codec = trigglow::storage::CodecId::Mjpeg;
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

// The caller's tag (the degradation rung) follows the slot: reservation,
// every view of it and Due(); a reuse retags it.
void TestTags()
{
	testcheck::g_currentTest = "tags";
	FrameRing ring(2, ArenaConfig());
	FrameRing::Reservation a, b;
	CHECK(ring.Reserve(100, a, 3));
	CHECK(ring.Reserve(110, b));
	CHECK_EQ(a.tag, 3u);
	CHECK_EQ(b.tag, 0u);
	CHECK(PublishTagged(ring, a, 1));
	CHECK(ring.Due(105).tag == 3u && ring.Due(115).tag == 0u);
	FrameRing::ReadView view;
	CHECK(ring.PinForDisplay(105, 0, view));
	CHECK_EQ(view.tag, 3u);
	ring.Unpin(view);
	CHECK(ring.PinNextForDecode(0, 105, UINT64_MAX, 1000, view));
	CHECK_EQ(view.tag, 3u);
	ring.Unpin(view);
	FrameRing::Reservation c;
	CHECK(ring.Reserve(120, c, 7)); // Reuses a's slot.
	CHECK_EQ(c.index, a.index);
	CHECK_EQ(ring.Due(125).tag, 7u);
}

// A stored packet's metadata, as an encoder publishes it.
struct Meta {
	storage::CodecId codec = storage::CodecId::Hevc;
	bool key = false;
	uint32_t chain = 0;
	uint32_t seq = 0;
};

bool PublishMeta(FrameRing &ring, const FrameRing::Reservation &reservation, const Meta &meta)
{
	if (!ring.BeginEncode(reservation.index, reservation.generation))
		return false;
	FrameRing::Payload payload;
	payload.allocation = ring.Arena().Allocate(64);
	payload.bytes = 64;
	payload.width = 4;
	payload.height = 2;
	payload.codec = meta.codec;
	payload.key = meta.key;
	payload.chain = meta.chain;
	payload.seq = meta.seq;
	if (!ring.Publish(reservation.index, reservation.generation, payload)) {
		ring.Arena().Free(payload.allocation);
		return false;
	}
	return true;
}

// Codec, key, chain and seq travel with the payload into every view of the
// slot (decode, display, Due(), Snapshot()); one ring holds several codecs; a
// reused slot starts over from the defaults (raw, key, no chain).
void TestPayloadMetadata()
{
	testcheck::g_currentTest = "payload metadata";
	FrameRing ring(8, ArenaConfig());
	const Meta metas[5] = {{storage::CodecId::Hevc, true, 7, 0},
			       {storage::CodecId::Hevc, false, 7, 1},
			       {storage::CodecId::Mjpeg, true, 8, 0},
			       {storage::CodecId::Raw, true, storage::kNoChain, 0},
			       {storage::CodecId::H264, false, 9, 41}};
	std::vector<FrameRing::Reservation> r(5);
	for (int i = 0; i < 5; ++i) {
		CHECK(ring.Reserve(1000 + i * kDelta, r[i]));
		CHECK(PublishMeta(ring, r[i], metas[i]));
	}
	for (int i = 0; i < 5; ++i) {
		FrameRing::ReadView view;
		CHECK(ring.PinForDisplay(1000 + i * kDelta, 0, view));
		CHECK(view.codec == metas[i].codec && view.key == metas[i].key && view.chain == metas[i].chain &&
		      view.seq == metas[i].seq);
		ring.Unpin(view);
		const FrameRing::DueSlot due = ring.Due(1000 + i * kDelta);
		CHECK(due.state == SlotState::Ready && due.codec == metas[i].codec && due.key == metas[i].key &&
		      due.chain == metas[i].chain && due.seq == metas[i].seq);
	}
	FrameRing::ReadView view;
	CHECK(ring.PinNextForDecode(1000, 1010, UINT64_MAX, 1000, view));
	CHECK(view.captureNs == 1010 && view.codec == storage::CodecId::Hevc && !view.key && view.seq == 1);
	ring.Unpin(view);
	const std::vector<FrameRing::SlotSummary> snap = ring.Snapshot();
	CHECK_EQ(snap.size(), 5u);
	for (size_t i = 0; i < snap.size() && i < 5; ++i)
		CHECK(snap[i].state == SlotState::Ready && snap[i].codec == metas[i].codec &&
		      snap[i].chain == metas[i].chain && snap[i].seq == metas[i].seq &&
		      snap[i].captureNs == 1000 + i * kDelta);
	// A plain raw payload (what v0.6.0 called compressed = false): the defaults.
	const FrameRing::Payload plain;
	CHECK(plain.codec == storage::CodecId::Raw && plain.key && plain.chain == storage::kNoChain);
	// Wrap around: a slot reused by Reserve() reports nothing of its old packet.
	for (int i = 0; i < 8; ++i) {
		FrameRing::Reservation w;
		CHECK(ring.Reserve(2000 + i * kDelta, w));
	}
	const FrameRing::DueSlot reused = ring.Due(2000);
	CHECK(reused.state == SlotState::Reserved && reused.codec == storage::CodecId::Raw && reused.key &&
	      reused.chain == storage::kNoChain);
}

// Capture times only grow from the oldest slot to the newest with payloads
// of several codecs published out of order (two encoders, one of them with
// frames in flight), so the binary searches stay valid.
void TestMixedCodecsMonotonic()
{
	testcheck::g_currentTest = "mixed codecs keep capture order";
	FrameRing ring(32, ArenaConfig());
	std::vector<FrameRing::Reservation> r(32);
	for (int i = 0; i < 32; ++i)
		CHECK(ring.Reserve(5000 + i * kDelta, r[i]));
	// Publish odd slots (MJPEG) first, then the even ones (HEVC) late.
	for (int i = 1; i < 32; i += 2)
		CHECK(PublishMeta(ring, r[i], {storage::CodecId::Mjpeg, true, 100, static_cast<uint32_t>(i / 2)}));
	for (int i = 0; i < 32; i += 2)
		CHECK(PublishMeta(ring, r[i], {storage::CodecId::Hevc, i % 8 == 0, 200, static_cast<uint32_t>(i / 2)}));
	const std::vector<FrameRing::SlotSummary> snap = ring.Snapshot();
	bool increasing = true;
	for (size_t i = 1; i < snap.size(); ++i)
		increasing = increasing && snap[i - 1].captureNs < snap[i].captureNs;
	CHECK(increasing);
	FrameRing::ReadView view;
	CHECK(ring.PinForDisplay(5000 + 17 * kDelta + 3, 0, view));
	CHECK(view.captureNs == 5000 + 17 * kDelta && view.codec == storage::CodecId::Mjpeg);
	ring.Unpin(view);
}

// The decoder's rule (invariant I2): a key always; a P-frame only right
// after its reference -- same chain, next seq.
void TestDecodable()
{
	testcheck::g_currentTest = "decodable";
	ringselect::ChainCursor none;
	CHECK(ringselect::Decodable(true, 5, 9, none));   // A key: always.
	CHECK(!ringselect::Decodable(false, 5, 1, none)); // A P-frame with nothing decoded yet.
	ringselect::ChainCursor at{true, 5, 3};
	CHECK(ringselect::Decodable(false, 5, 4, at));  // The next one.
	CHECK(!ringselect::Decodable(false, 5, 5, at)); // A gap (4 lost).
	CHECK(!ringselect::Decodable(false, 5, 3, at)); // The same one again.
	CHECK(!ringselect::Decodable(false, 6, 4, at)); // Another chain.
	CHECK(ringselect::Decodable(true, 6, 0, at));   // Another chain's key.
	ringselect::ChainCursor wrap{true, 5, UINT32_MAX};
	CHECK(ringselect::Decodable(false, 5, 0, wrap)); // seq wraps around.
}

// One position of a fake ring for PlanDecodeStart(): Ready with its packet's
// metadata, or not Ready (dropped before its encode, still encoding).
ringselect::ChainSlot K(uint32_t chain, uint32_t seq)
{
	return {true, true, chain, seq};
}
ringselect::ChainSlot P(uint32_t chain, uint32_t seq)
{
	return {true, false, chain, seq};
}
ringselect::ChainSlot D()
{
	return {false, false, 0, 0};
}

ringselect::DecodeStart Plan(const std::vector<ringselect::ChainSlot> &slots, size_t due)
{
	return ringselect::PlanDecodeStart(slots.size(), due, [&](size_t p) { return slots[p]; });
}

void TestPlanDecodeStart()
{
	testcheck::g_currentTest = "plan decode start";
	// The due frame is a key: nothing to catch up (raw and MJPEG, always).
	ringselect::DecodeStart plan = Plan({K(1, 0), K(2, 0), K(3, 0)}, 2);
	CHECK(plan.found && plan.keyPos == 2 && plan.catchUp == 0);
	CHECK_EQ(plan.nextKeyPos, ringselect::kNoPosition);
	// Its key 3 frames back: decode 3 unshown (K, P1, P2), then show P3.
	plan = Plan({K(7, 0), P(7, 1), P(7, 2), P(7, 3), P(7, 4), K(7, 5)}, 3);
	CHECK(plan.found && plan.keyPos == 0 && plan.catchUp == 3);
	CHECK_EQ(plan.nextKeyPos, 5u);
	// Slots dropped before their encode (no seq) aren't gaps.
	plan = Plan({K(7, 0), D(), P(7, 1), D(), D(), P(7, 2)}, 5);
	CHECK(plan.found && plan.keyPos == 0 && plan.catchUp == 2);
	// A packet lost after its encode (seq 2 never stored): wait for the next key.
	plan = Plan({K(7, 0), P(7, 1), D(), P(7, 3), K(7, 4)}, 3);
	CHECK(!plan.found);
	CHECK_EQ(plan.nextKeyPos, 4u);
	// Its key already overwritten (the oldest slot is a P-frame): not found.
	plan = Plan({P(7, 5), P(7, 6), P(7, 7)}, 2);
	CHECK(!plan.found);
	// All MJPEG (every frame its own key): always the due frame itself.
	plan = Plan({K(10, 0), K(10, 1), K(11, 0), K(10, 2)}, 3);
	CHECK(plan.found && plan.keyPos == 3 && plan.catchUp == 0);
	// HEVC -> MJPEG fallback -> HEVC again: the new chain starts on its own key.
	plan = Plan({K(1, 0), P(1, 1), K(2, 0), K(3, 0), K(4, 0), P(4, 1), P(4, 2)}, 6);
	CHECK(plan.found && plan.keyPos == 4 && plan.catchUp == 2);
	// A Ready frame of another chain right before the due P-frame: its chain
	// was cut there, so no key can be reached (the encoder never stores that).
	plan = Plan({K(1, 0), P(1, 1), K(2, 0), P(1, 2)}, 3);
	CHECK(!plan.found);
	// The due position not Ready, or past the end: nothing.
	CHECK(!Plan({K(1, 0), D()}, 1).found);
	CHECK(!Plan({K(1, 0)}, 3).found);
}

void TestChooseDecodeStart()
{
	testcheck::g_currentTest = "choose decode start";
	constexpr uint64_t kMs = 1000000;
	ringselect::DecodeStart plan;
	plan.found = true;
	plan.keyPos = 0;
	plan.catchUp = 3;
	plan.nextKeyPos = 5;
	const size_t due = 3;
	// 3 decodes of 4 ms (12 ms) vs. the next key 2 frames away (33 ms at 60 fps): catch up.
	CHECK(ringselect::ChooseDecodeStart(plan, due, 4 * kMs, 16666667) == ringselect::StartChoice::CatchUp);
	// 3 decodes of 20 ms (60 ms) vs. 33 ms: wait for the key.
	CHECK(ringselect::ChooseDecodeStart(plan, due, 20 * kMs, 16666667) == ringselect::StartChoice::WaitForKey);
	// No key in sight: catch up whatever it costs.
	plan.nextKeyPos = ringselect::kNoPosition;
	CHECK(ringselect::ChooseDecodeStart(plan, due, 20 * kMs, 16666667) == ringselect::StartChoice::CatchUp);
	// Nothing to catch up: the due frame is its own key.
	plan.catchUp = 0;
	plan.nextKeyPos = 5;
	CHECK(ringselect::ChooseDecodeStart(plan, due, 20 * kMs, 16666667) == ringselect::StartChoice::CatchUp);
	// No key to start from: wait.
	plan.found = false;
	CHECK(ringselect::ChooseDecodeStart(plan, due, 1, 16666667) == ringselect::StartChoice::WaitForKey);
}

// FrameRing::PlanDecodeStart() on a real ring with GOP packets: the frame
// shown for a target, its key, the catch-up count, the next key, and the
// choice -- across dropped slots and a chain change.
void TestRingPlan()
{
	testcheck::g_currentTest = "ring plan";
	FrameRing ring(16, ArenaConfig());
	std::vector<FrameRing::Reservation> r(12);
	for (int i = 0; i < 12; ++i)
		CHECK(ring.Reserve(1000 + i * kDelta, r[i]));
	// 0:K(5,0) 1:P(5,1) 2:dropped 3:P(5,2) 4:P(5,3) 5:K(5,4) 6:P(5,5)
	// 7:K(6,0) MJPEG 8:K(9,0) 9:P(9,1) 10:P(9,2) 11:still encoding.
	const struct {
		int i;
		Meta meta;
	} published[] = {{0, {storage::CodecId::Hevc, true, 5, 0}},  {1, {storage::CodecId::Hevc, false, 5, 1}},
			 {3, {storage::CodecId::Hevc, false, 5, 2}}, {4, {storage::CodecId::Hevc, false, 5, 3}},
			 {5, {storage::CodecId::Hevc, true, 5, 4}},  {6, {storage::CodecId::Hevc, false, 5, 5}},
			 {7, {storage::CodecId::Mjpeg, true, 6, 0}}, {8, {storage::CodecId::Hevc, true, 9, 0}},
			 {9, {storage::CodecId::Hevc, false, 9, 1}}, {10, {storage::CodecId::Hevc, false, 9, 2}}};
	for (const auto &p : published)
		CHECK(PublishMeta(ring, r[p.i], p.meta));
	CHECK(ring.MarkDropped(r[2].index, r[2].generation));
	CHECK(ring.BeginEncode(r[11].index, r[11].generation));

	constexpr uint64_t kInterval = kDelta;
	// Due 4 (P(5,3)): from the key at 0, 3 frames unshown (0, 1, 3); next key at 5.
	FrameRing::DecodePlan plan = ring.PlanDecodeStart(1000 + 4 * kDelta + 5, 4, 1, kInterval);
	CHECK(plan.due && plan.dueNs == 1040 && plan.found && plan.keyNs == 1000 && plan.catchUp == 3);
	CHECK(plan.nextKey && plan.framesToNextKey == 1);
	// 3 decodes of 1 ns vs. 1 frame: catch up; of 20 ns each: wait for the key.
	CHECK(plan.choice == ringselect::StartChoice::CatchUp);
	plan = ring.PlanDecodeStart(1000 + 4 * kDelta + 5, 4, 20, kInterval);
	CHECK(plan.choice == ringselect::StartChoice::WaitForKey);
	// Due 2 is dropped: the frame shown is 1, P(5,1), one back from its key.
	plan = ring.PlanDecodeStart(1000 + 2 * kDelta, 4, 1, kInterval);
	CHECK(plan.due && plan.dueNs == 1010 && plan.found && plan.keyNs == 1000 && plan.catchUp == 1);
	// Due 7 (MJPEG): its own key.
	plan = ring.PlanDecodeStart(1000 + 7 * kDelta, 4, 1, kInterval);
	CHECK(plan.found && plan.keyNs == 1070 && plan.catchUp == 0 && plan.choice == ringselect::StartChoice::CatchUp);
	// Due 10 (P(9,2)): key at 8, past the MJPEG frame; slot 11 still encoding.
	plan = ring.PlanDecodeStart(1000 + 11 * kDelta, 4, 1, kInterval);
	CHECK(plan.due && plan.dueNs == 1100 && plan.found && plan.keyNs == 1080 && plan.catchUp == 2);
	CHECK(!plan.nextKey);
	// Before the oldest frame: nothing due.
	CHECK(!ring.PlanDecodeStart(999, 4, 1, kInterval).due);
	CHECK_EQ(ring.GetCounts().pinned, 0u);
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
	TestTags();
	TestPayloadMetadata();
	TestMixedCodecsMonotonic();
	TestDecodable();
	TestPlanDecodeStart();
	TestChooseDecodeStart();
	TestRingPlan();
	return testcheck::Finish("frame_ring_tests");
}
