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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Byte arena for the delay ring's frame payloads (v0.6.0, buffer pipeline
// design B1 + perf2 #3). Replaces one heap buffer per ring slot.
//
// Before: every slot owned a buffer sized raw/5 up front (grow-only), so the
// commit charge was slots x raw/5 no matter what the frames really weighed,
// and freeing the ring meant one VirtualFree per slot (1816 at 1440p60).
// Now: payloads are bump-allocated, back to back, in large blocks (32 MiB by
// default); a payload never straddles two blocks. Every block counts its live
// allocations; when the last one in a block is freed (the ring overwrote all
// the frames it held) the block is recycled whole. Since the ring frees frames
// in roughly the order it wrote them, committed memory stays close to the real
// window of packets plus one or two blocks, and freeing the ring is one call
// per block.
//
// Allocations and frees can come in any order and from any thread (the codec
// workers allocate, the graphics thread frees when it overwrites a slot): the
// per-block counter makes out-of-order frees safe. Free() never calls the OS:
// emptied blocks go to a spare list, and Trim() (called off the graphics
// thread) releases the spares beyond maxSpareBlocks. Pure C++, no OBS.
namespace trigglow {

class FrameArena {
public:
	static constexpr uint32_t kNoBlock = UINT32_MAX;
	static constexpr size_t kAlignment = 64; // Every allocation starts on a cache line.

	struct Config {
		size_t blockBytes = 32ULL * 1024 * 1024;
		// Cap on committed bytes (all blocks, spares included). An allocation
		// that would need a new block past it fails instead (the caller drops
		// that frame): the ring can never grow without bound.
		uint64_t limitBytes = UINT64_MAX;
		size_t maxSpareBlocks = 2; // Empty blocks kept for reuse by Trim().
	};

	// A live allocation. Trivially copyable handle; Free() takes it back.
	struct Allocation {
		uint8_t *data = nullptr;
		uint32_t block = kNoBlock;
		uint32_t bytes = 0; // Reserved (rounded up to kAlignment).
		explicit operator bool() const { return data != nullptr; }
	};

	struct Stats {
		uint64_t committedBytes = 0;     // Every block currently allocated from the OS.
		uint64_t peakCommittedBytes = 0; // Since construction.
		uint64_t liveBytes = 0;          // Reserved by live allocations.
		uint32_t blocks = 0;             // Blocks allocated from the OS (spares included).
		uint32_t spareBlocks = 0;
		uint64_t failedAllocations = 0; // Refused by limitBytes.
		uint64_t releasedBlocks = 0;    // Given back to the OS by Trim().
	};

	explicit FrameArena(const Config &config);
	~FrameArena();
	FrameArena(const FrameArena &) = delete;
	FrameArena &operator=(const FrameArena &) = delete;

	// Uninitialized storage for `bytes` (0 is treated as 1). Empty Allocation
	// when the limit is reached. May allocate a new block (one heap call,
	// pages untouched until written).
	Allocation Allocate(size_t bytes);
	// Returns the allocation and resets the handle. O(1); never calls the OS.
	void Free(Allocation &allocation);
	// Releases empty blocks beyond maxSpareBlocks (and empty oversized ones) to
	// the OS. The only call that frees memory: keep it off the graphics thread.
	void Trim();

	Stats GetStats() const;
	const Config &GetConfig() const { return config_; }

private:
	struct Block {
		std::unique_ptr<uint8_t[]> memory;
		uint8_t *base = nullptr; // memory, rounded up to kAlignment.
		size_t capacity = 0;
		size_t used = 0;
		uint32_t live = 0;      // Live allocations inside.
		bool oversized = false; // Dedicated block for one allocation > blockBytes.
		bool spare = false;     // Empty and on spare_.
	};

	// All under mutex_.
	uint32_t NewBlockLocked(size_t capacity, bool oversized); // kNoBlock past the limit.
	void RetireIfEmptyLocked(uint32_t id);

	Config config_; // Normalized by the constructor, then never changed.
	mutable std::mutex mutex_;
	std::vector<Block> blocks_;
	std::vector<uint32_t> spare_;  // Empty standard blocks, reusable.
	std::vector<uint32_t> vacant_; // Ids in blocks_ with no memory (released), reusable.
	uint32_t current_ = kNoBlock;  // Block being bump-allocated.
	Stats stats_;
};

} // namespace trigglow
