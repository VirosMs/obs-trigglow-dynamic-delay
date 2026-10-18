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

#include "frame-ring.hpp"

#include <algorithm>
#include <atomic>

namespace trigglow {

namespace {
// Generations are unique across EVERY ring, not just within one: a job that
// outlives its ring (Disable, hard resize) can then never match a slot of the
// ring that replaced it, even at the same index.
std::atomic<uint64_t> g_nextGeneration{1};
} // namespace

const char *SlotStateName(SlotState state)
{
	switch (state) {
	case SlotState::Free:
		return "free";
	case SlotState::Reserved:
		return "reserved";
	case SlotState::Encoding:
		return "encoding";
	case SlotState::Ready:
		return "ready";
	case SlotState::Dropped:
		return "dropped";
	}
	return "?";
}

FrameRing::FrameRing(size_t slotCount, const FrameArena::Config &arena)
	: arena_(arena),
	  headers_(std::max<size_t>(1, slotCount))
{
}

bool FrameRing::Reserve(uint64_t captureNs, Reservation &out, uint32_t tag)
{
	FrameArena::Allocation previous;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Header &header = headers_[writeIndex_];
		if (header.state == SlotState::Reserved || header.state == SlotState::Encoding || header.pins > 0) {
			++reserveFailures_;
			return false;
		}
		previous = header.payload.allocation;
		header.payload = Payload{};
		// Never below the previous reservation: the binary searches rely on it.
		captureNs = std::max(captureNs, lastCaptureNs_);
		lastCaptureNs_ = captureNs;
		header.captureNs = captureNs;
		header.generation = g_nextGeneration.fetch_add(1, std::memory_order_relaxed);
		header.state = SlotState::Reserved;
		header.tag = tag;

		out.index = writeIndex_;
		out.generation = header.generation;
		out.captureNs = captureNs;
		out.tag = tag;

		writeIndex_ = (writeIndex_ + 1) % headers_.size();
		if (buffered_ < headers_.size())
			++buffered_;
		oldest_ = (writeIndex_ + headers_.size() - buffered_) % headers_.size();
	}
	// The overwritten frame's bytes, outside the ring lock (FrameArena::Free
	// is O(1) and never calls the OS).
	arena_.Free(previous);
	return true;
}

bool FrameRing::MarkDropped(size_t index, uint64_t generation)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (index >= headers_.size())
		return false;
	Header &header = headers_[index];
	if (header.generation != generation ||
	    (header.state != SlotState::Reserved && header.state != SlotState::Encoding))
		return false;
	header.state = SlotState::Dropped;
	return true;
}

bool FrameRing::BeginEncode(size_t index, uint64_t generation)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (index >= headers_.size())
		return false;
	Header &header = headers_[index];
	if (header.generation != generation || header.state != SlotState::Reserved)
		return false;
	header.state = SlotState::Encoding;
	return true;
}

bool FrameRing::Publish(size_t index, uint64_t generation, const Payload &payload)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (index >= headers_.size())
		return false;
	Header &header = headers_[index];
	if (header.generation != generation || header.state != SlotState::Encoding)
		return false;
	header.payload = payload;
	header.state = SlotState::Ready;
	return true;
}

void FrameRing::FillViewLocked(size_t index, ReadView &out)
{
	Header &header = headers_[index];
	++header.pins;
	out.index = index;
	out.generation = header.generation;
	out.captureNs = header.captureNs;
	out.data = header.payload.allocation.data;
	out.bytes = header.payload.bytes;
	out.width = header.payload.width;
	out.height = header.payload.height;
	out.compressed = header.payload.compressed;
	out.tag = header.tag;
}

bool FrameRing::PinNextForDecode(uint64_t afterNs, uint64_t targetNs, uint64_t windowEndNs, uint64_t lateNs,
				 ReadView &out)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const size_t count = buffered_;
	if (count == 0)
		return false;
	auto captureAt = [&](size_t p) {
		return headers_[IndexAt(p)].captureNs;
	};
	auto readyAt = [&](size_t p) {
		return headers_[IndexAt(p)].state == SlotState::Ready;
	};

	if (afterNs + lateNs < targetNs) {
		// Behind (or first call): the frame due now, never the backlog.
		const size_t p = ringselect::SelectForDisplay(count, targetNs, 8, captureAt, readyAt);
		if (p != count && captureAt(p) > afterNs) {
			FillViewLocked(IndexAt(p), out);
			return true;
		}
		// Nothing Ready at or before the target: fall through to the frames after it.
	}

	// First position strictly after afterNs.
	const size_t last = ringselect::LastAtOrBefore(count, afterNs, captureAt);
	size_t p = last == count ? 0 : last + 1;
	for (; p < count; ++p) {
		const Header &header = headers_[IndexAt(p)];
		if (header.captureNs > windowEndNs)
			return false;
		switch (header.state) {
		case SlotState::Ready:
			FillViewLocked(IndexAt(p), out);
			return true;
		case SlotState::Dropped:
			continue;
		case SlotState::Reserved:
		case SlotState::Encoding:
			if (header.captureNs <= targetNs)
				continue; // Already due and still not encoded: skip it, playback repeats a frame.
			return false;     // Future frame still encoding: wait for it.
		case SlotState::Free:
			return false;
		}
	}
	return false;
}

bool FrameRing::PinForDisplay(uint64_t targetNs, size_t maxBack, ReadView &out)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const size_t count = buffered_;
	auto captureAt = [&](size_t p) {
		return headers_[IndexAt(p)].captureNs;
	};
	auto readyAt = [&](size_t p) {
		return headers_[IndexAt(p)].state == SlotState::Ready;
	};
	const size_t p = ringselect::SelectForDisplay(count, targetNs, maxBack, captureAt, readyAt);
	if (p == count)
		return false;
	FillViewLocked(IndexAt(p), out);
	return true;
}

bool FrameRing::DisplayCandidateNs(uint64_t targetNs, size_t maxBack, uint64_t &captureNs) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const size_t count = buffered_;
	auto captureAt = [&](size_t p) {
		return headers_[IndexAt(p)].captureNs;
	};
	auto readyAt = [&](size_t p) {
		return headers_[IndexAt(p)].state == SlotState::Ready;
	};
	const size_t p = ringselect::SelectForDisplay(count, targetNs, maxBack, captureAt, readyAt);
	if (p == count)
		return false;
	captureNs = captureAt(p);
	return true;
}

void FrameRing::Unpin(const ReadView &view)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (view.index >= headers_.size())
		return;
	Header &header = headers_[view.index];
	// A pinned slot can't be reserved again, so the generation always matches;
	// checked anyway so a bogus view can never underflow someone else's pin.
	if (header.generation == view.generation && header.pins > 0)
		--header.pins;
}

FrameRing::DueSlot FrameRing::Due(uint64_t targetNs) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	DueSlot due;
	const size_t count = buffered_;
	auto captureAt = [&](size_t p) {
		return headers_[IndexAt(p)].captureNs;
	};
	const size_t p = ringselect::LastAtOrBefore(count, targetNs, captureAt);
	if (p == count)
		return due;
	const Header &header = headers_[IndexAt(p)];
	due.exists = true;
	due.captureNs = header.captureNs;
	due.state = header.state;
	due.tag = header.tag;
	return due;
}

bool FrameRing::OldestCaptureNs(uint64_t &out) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (buffered_ == 0)
		return false;
	out = headers_[oldest_].captureNs;
	return true;
}

FrameRing::Counts FrameRing::GetCounts() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	Counts counts;
	counts.buffered = buffered_;
	counts.reserveFailures = reserveFailures_;
	for (const Header &header : headers_) {
		switch (header.state) {
		case SlotState::Reserved:
			++counts.reserved;
			break;
		case SlotState::Encoding:
			++counts.encoding;
			break;
		case SlotState::Ready:
			++counts.ready;
			break;
		case SlotState::Dropped:
			++counts.dropped;
			break;
		case SlotState::Free:
			break;
		}
		if (header.pins > 0)
			++counts.pinned;
	}
	return counts;
}

} // namespace trigglow
