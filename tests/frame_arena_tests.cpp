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

// Unit tests for trigglow::FrameArena (src/frame-arena.hpp): block bump
// allocation, out-of-order frees, block recycling, the limit, oversized
// payloads, Trim(), a FIFO steady state like the ring's, and a small
// multi-threaded run with content checks.

#include "check.hpp"
#include "frame-arena.hpp"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using trigglow::FrameArena;

namespace {

constexpr size_t kKiB = 1024;

FrameArena::Config SmallConfig(size_t blockBytes = 64 * kKiB, uint64_t limit = UINT64_MAX, size_t spare = 2)
{
	FrameArena::Config config;
	config.blockBytes = blockBytes;
	config.limitBytes = limit;
	config.maxSpareBlocks = spare;
	return config;
}

void TestBasics()
{
	testcheck::g_currentTest = "basics";
	FrameArena arena(SmallConfig());
	FrameArena::Allocation a = arena.Allocate(100);
	CHECK(a);
	CHECK_EQ(a.bytes, 128u); // Rounded to the 64-byte alignment.
	CHECK_EQ(reinterpret_cast<uintptr_t>(a.data) % FrameArena::kAlignment, 0u);
	FrameArena::Allocation b = arena.Allocate(1);
	CHECK_EQ(b.bytes, 64u);
	CHECK_EQ(static_cast<size_t>(b.data - a.data), 128u); // Bump: back to back.
	CHECK_EQ(arena.Allocate(0).bytes, 64u);               // 0 is treated as 1.
	FrameArena::Stats stats = arena.GetStats();
	CHECK_EQ(stats.blocks, 1u);
	CHECK_EQ(stats.committedBytes, 64u * kKiB);
	CHECK_EQ(stats.liveBytes, 256u);
	arena.Free(a);
	CHECK(!a);     // Free() resets the handle.
	arena.Free(a); // And a second Free() of it is a no-op.
	CHECK_EQ(arena.GetStats().liveBytes, 128u);
}

void TestBlockRecycling()
{
	testcheck::g_currentTest = "block recycling";
	FrameArena arena(SmallConfig(64 * kKiB));
	std::vector<FrameArena::Allocation> first;
	for (int i = 0; i < 4; ++i)
		first.push_back(arena.Allocate(16 * kKiB)); // Fills block 0 exactly.
	CHECK_EQ(arena.GetStats().blocks, 1u);
	const uint32_t firstBlock = first[0].block;
	FrameArena::Allocation second = arena.Allocate(16 * kKiB); // Doesn't fit: block 1.
	CHECK(second.block != firstBlock);
	CHECK_EQ(arena.GetStats().blocks, 2u);
	// Out of order: the block only empties with its last allocation.
	arena.Free(first[2]);
	arena.Free(first[0]);
	arena.Free(first[3]);
	CHECK_EQ(arena.GetStats().spareBlocks, 0u);
	arena.Free(first[1]);
	CHECK_EQ(arena.GetStats().spareBlocks, 1u);
	// Filling block 1 moves on to the spare instead of a new block.
	for (int i = 0; i < 3; ++i)
		arena.Allocate(16 * kKiB);
	FrameArena::Allocation reused = arena.Allocate(16 * kKiB);
	CHECK_EQ(reused.block, firstBlock);
	CHECK_EQ(arena.GetStats().blocks, 2u);
	CHECK_EQ(arena.GetStats().spareBlocks, 0u);
}

void TestCurrentBlockRestarts()
{
	testcheck::g_currentTest = "empty current block restarts";
	FrameArena arena(SmallConfig(64 * kKiB));
	FrameArena::Allocation a = arena.Allocate(40 * kKiB);
	uint8_t *start = a.data;
	arena.Free(a);
	FrameArena::Allocation b = arena.Allocate(40 * kKiB); // Wouldn't fit after a, but the block is empty.
	CHECK(b.data == start);
	CHECK_EQ(arena.GetStats().blocks, 1u);
}

void TestLimit()
{
	testcheck::g_currentTest = "limit";
	FrameArena arena(SmallConfig(64 * kKiB, 128 * kKiB));
	int ok = 0;
	for (int i = 0; i < 10; ++i) {
		if (arena.Allocate(32 * kKiB))
			++ok;
	}
	CHECK_EQ(ok, 4); // Two blocks of two.
	FrameArena::Stats stats = arena.GetStats();
	CHECK(stats.committedBytes <= 128 * kKiB);
	CHECK_EQ(stats.failedAllocations, 6u);
	// Oversized ones respect the limit too.
	CHECK(!arena.Allocate(256 * kKiB));
}

void TestOversizedAndTrim()
{
	testcheck::g_currentTest = "oversized and trim";
	FrameArena arena(SmallConfig(64 * kKiB, UINT64_MAX, 1));
	FrameArena::Allocation big = arena.Allocate(200 * kKiB);
	CHECK(big);
	CHECK_EQ(arena.GetStats().committedBytes, 200u * kKiB);
	std::memset(big.data, 0x5A, 200 * kKiB);
	FrameArena::Allocation small = arena.Allocate(1 * kKiB);
	CHECK(small.block != big.block); // Never shares the dedicated block.
	arena.Free(big);
	CHECK_EQ(arena.GetStats().spareBlocks, 0u); // Oversized blocks never become spares...
	arena.Trim();                               // ...Trim() gives them back.
	CHECK_EQ(arena.GetStats().committedBytes, 64u * kKiB);
	CHECK_EQ(arena.GetStats().releasedBlocks, 1u);

	// Five emptied standard blocks, one spare kept.
	std::vector<FrameArena::Allocation> fills;
	for (int i = 0; i < 6 * 2; ++i)
		fills.push_back(arena.Allocate(32 * kKiB));
	arena.Free(small);
	for (auto &a : fills)
		arena.Free(a);
	const uint32_t spares = arena.GetStats().spareBlocks;
	CHECK(spares >= 5u);
	arena.Trim();
	CHECK_EQ(arena.GetStats().spareBlocks, 1u);
	// Released ids are reused.
	for (int i = 0; i < 20; ++i)
		fills.push_back(arena.Allocate(32 * kKiB));
	CHECK(arena.GetStats().blocks >= 10u);
}

// The ring's pattern: free the oldest, allocate the newest, sizes varying.
// Committed memory must track the live window, not grow, and every payload
// must keep its bytes until freed.
void TestFifoSteadyState()
{
	testcheck::g_currentTest = "FIFO steady state";
	FrameArena arena(SmallConfig(256 * kKiB));
	std::mt19937 rng(7);
	std::uniform_int_distribution<size_t> size(4 * kKiB, 24 * kKiB);
	struct Entry {
		FrameArena::Allocation a;
		uint8_t tag;
		size_t bytes;
	};
	std::deque<Entry> window;
	uint64_t maxLive = 0, maxCommitted = 0;
	int corrupt = 0;
	for (int i = 0; i < 20000; ++i) {
		if (window.size() == 100) {
			Entry &old = window.front();
			for (size_t k = 0; k < old.bytes; k += 997) {
				if (old.a.data[k] != old.tag)
					++corrupt;
			}
			arena.Free(old.a);
			window.pop_front();
		}
		Entry e{arena.Allocate(size(rng)), static_cast<uint8_t>(i), 0};
		e.bytes = e.a.bytes;
		std::memset(e.a.data, e.tag, e.bytes);
		window.push_back(e);
		const FrameArena::Stats s = arena.GetStats();
		maxLive = std::max(maxLive, s.liveBytes);
		maxCommitted = std::max(maxCommitted, s.committedBytes);
		if (i % 1000 == 0)
			arena.Trim();
	}
	CHECK_EQ(corrupt, 0);
	// The window plus a few blocks (current, partially freed one, spares).
	CHECK(maxCommitted <= maxLive + 4 * 256 * kKiB);
	std::printf("  fifo: max live %llu KiB, max committed %llu KiB\n",
		    static_cast<unsigned long long>(maxLive / kKiB),
		    static_cast<unsigned long long>(maxCommitted / kKiB));
}

// Two allocating threads, one freeing thread (frees out of order), content
// checked at free time.
void TestConcurrent()
{
	testcheck::g_currentTest = "concurrent";
	FrameArena arena(SmallConfig(128 * kKiB));
	std::mutex m;
	std::vector<FrameArena::Allocation> live;
	std::atomic<bool> stop{false};
	std::atomic<int> corrupt{0};
	auto producer = [&](int id) {
		std::mt19937 rng(static_cast<unsigned>(id));
		std::uniform_int_distribution<size_t> size(1, 20 * kKiB);
		for (int i = 0; i < 20000; ++i) {
			FrameArena::Allocation a = arena.Allocate(size(rng));
			if (!a)
				continue;
			std::memset(a.data, static_cast<int>(a.bytes & 0xFF), a.bytes);
			std::lock_guard<std::mutex> lock(m);
			live.push_back(a);
		}
	};
	std::thread consumer([&] {
		std::mt19937 rng(99);
		while (!stop || !live.empty()) {
			FrameArena::Allocation a;
			{
				std::lock_guard<std::mutex> lock(m);
				if (live.empty())
					continue;
				const size_t i = std::uniform_int_distribution<size_t>(0, live.size() - 1)(rng);
				a = live[i];
				live[i] = live.back();
				live.pop_back();
			}
			for (size_t k = 0; k < a.bytes; k += 61) {
				if (a.data[k] != static_cast<uint8_t>(a.bytes & 0xFF)) {
					++corrupt;
					break;
				}
			}
			arena.Free(a);
			arena.Trim();
		}
	});
	std::thread p1(producer, 1), p2(producer, 2);
	p1.join();
	p2.join();
	stop = true;
	consumer.join();
	CHECK_EQ(corrupt.load(), 0);
	CHECK_EQ(arena.GetStats().liveBytes, 0u);
}

} // namespace

int main()
{
	TestBasics();
	TestBlockRecycling();
	TestCurrentBlockRestarts();
	TestLimit();
	TestOversizedAndTrim();
	TestFifoSteadyState();
	TestConcurrent();
	return testcheck::Finish("frame_arena_tests");
}
