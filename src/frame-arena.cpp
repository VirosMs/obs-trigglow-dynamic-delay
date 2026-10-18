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

#include "frame-arena.hpp"

#include <algorithm>
#include <new>

namespace trigglow {

namespace {
size_t RoundUp(size_t bytes, size_t to)
{
	return (bytes + to - 1) / to * to;
}
} // namespace

FrameArena::FrameArena(const Config &config) : config_(config)
{
	// Blocks shorter than one alignment unit would make every allocation
	// "oversized"; keep the arithmetic sane whatever the caller passes.
	config_.blockBytes = RoundUp(std::max(config_.blockBytes, kAlignment), kAlignment);
}

FrameArena::~FrameArena() = default;

uint32_t FrameArena::NewBlockLocked(size_t capacity, bool oversized)
{
	if (stats_.committedBytes + capacity > config_.limitBytes) {
		++stats_.failedAllocations;
		return kNoBlock;
	}
	// Default-init: no page is touched until a payload is written into it.
	// kAlignment extra bytes so the usable start can sit on a cache line.
	std::unique_ptr<uint8_t[]> memory(new (std::nothrow) uint8_t[capacity + kAlignment]);
	if (!memory) {
		++stats_.failedAllocations;
		return kNoBlock;
	}
	uint32_t id;
	if (!vacant_.empty()) {
		id = vacant_.back();
		vacant_.pop_back();
	} else {
		id = static_cast<uint32_t>(blocks_.size());
		blocks_.emplace_back();
	}
	Block &block = blocks_[id];
	const auto address = reinterpret_cast<uintptr_t>(memory.get());
	block.base = memory.get() + (kAlignment - address % kAlignment) % kAlignment;
	block.memory = std::move(memory);
	block.capacity = capacity;
	block.used = 0;
	block.live = 0;
	block.oversized = oversized;
	block.spare = false;
	stats_.committedBytes += capacity;
	stats_.peakCommittedBytes = std::max(stats_.peakCommittedBytes, stats_.committedBytes);
	++stats_.blocks;
	return id;
}

void FrameArena::RetireIfEmptyLocked(uint32_t id)
{
	Block &block = blocks_[id];
	if (block.live != 0 || id == current_ || block.spare || !block.memory)
		return;
	block.used = 0;
	if (block.oversized)
		return; // Trim() gives it back to the OS; never reused for small payloads.
	block.spare = true;
	spare_.push_back(id);
	++stats_.spareBlocks;
}

FrameArena::Allocation FrameArena::Allocate(size_t bytes)
{
	const size_t size = RoundUp(std::max<size_t>(bytes, 1), kAlignment);
	std::lock_guard<std::mutex> lock(mutex_);

	Allocation out;
	if (size > config_.blockBytes) {
		// A payload bigger than a whole block (raw frames at very high
		// resolutions with small blocks): a dedicated block of its own.
		const uint32_t id = NewBlockLocked(size, true);
		if (id == kNoBlock)
			return out;
		Block &block = blocks_[id];
		block.used = size;
		block.live = 1;
		out.data = block.base;
		out.block = id;
		out.bytes = static_cast<uint32_t>(size);
		stats_.liveBytes += size;
		return out;
	}

	if (current_ == kNoBlock || blocks_[current_].used + size > blocks_[current_].capacity) {
		// Doesn't fit the current block: move on. The old block stays alive
		// until its last allocation is freed (or is recycled right away if
		// it's already empty).
		const uint32_t previous = current_;
		current_ = kNoBlock;
		if (previous != kNoBlock)
			RetireIfEmptyLocked(previous);
		if (!spare_.empty()) {
			current_ = spare_.back();
			spare_.pop_back();
			blocks_[current_].spare = false;
			--stats_.spareBlocks;
		} else {
			current_ = NewBlockLocked(config_.blockBytes, false);
			if (current_ == kNoBlock)
				return out;
		}
	}

	Block &block = blocks_[current_];
	out.data = block.base + block.used;
	out.block = current_;
	out.bytes = static_cast<uint32_t>(size);
	block.used += size;
	++block.live;
	stats_.liveBytes += size;
	return out;
}

void FrameArena::Free(Allocation &allocation)
{
	if (!allocation)
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (allocation.block < blocks_.size()) {
			Block &block = blocks_[allocation.block];
			if (block.live > 0) {
				--block.live;
				stats_.liveBytes -= std::min<uint64_t>(stats_.liveBytes, allocation.bytes);
				if (block.live == 0) {
					if (allocation.block == current_)
						block.used = 0; // Empty current block: start over from its beginning.
					else
						RetireIfEmptyLocked(allocation.block);
				}
			}
		}
	}
	allocation = Allocation{};
}

void FrameArena::Trim()
{
	std::vector<std::unique_ptr<uint8_t[]>> release;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto releaseBlock = [&](uint32_t id) {
			Block &block = blocks_[id];
			stats_.committedBytes -= block.capacity;
			--stats_.blocks;
			++stats_.releasedBlocks;
			release.push_back(std::move(block.memory));
			block = Block{};
			vacant_.push_back(id);
		};
		while (spare_.size() > config_.maxSpareBlocks) {
			const uint32_t id = spare_.front();
			spare_.erase(spare_.begin());
			--stats_.spareBlocks;
			releaseBlock(id);
		}
		for (uint32_t id = 0; id < blocks_.size(); ++id) {
			const Block &block = blocks_[id];
			if (block.memory && block.oversized && block.live == 0)
				releaseBlock(id);
		}
	}
	// `release` goes out of scope here: the OS calls happen without the lock.
}

FrameArena::Stats FrameArena::GetStats() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return stats_;
}

} // namespace trigglow
