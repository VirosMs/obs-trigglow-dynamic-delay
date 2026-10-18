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

#include <array>
#include <cstddef>
#include <cstdint>

// Small, allocation-free timing helpers shared by the graphics thread
// (VideoDelayFilter) and the codec workers (CodecPipeline). Pure C++, no OBS,
// so the tests and the benchmarks link them as they are. None of these types
// lock: each one has a single owner, or its owner guards it with a mutex.
namespace trigglow {

// Wall-clock samples for one telemetry window (the "perf check" line). Fixed
// storage, so nothing is allocated per frame or per window; Summarize()
// reorders the samples with std::nth_element, so it's only called right
// before a Reset(). Past kCapacity samples in one window the oldest are
// overwritten (percentiles then cover the most recent kCapacity); max and
// total stay exact.
struct PerfSeries {
	static constexpr size_t kCapacity = 4096;    // > 10s of 3 views at 120fps.
	std::array<uint32_t, kCapacity> samplesNs{}; // Clamped to UINT32_MAX (~4.3s).
	uint64_t count = 0;
	uint64_t maxNs = 0;
	uint64_t totalNs = 0; // Sum of every sample: busy time, for utilization.
	void Push(uint64_t ns);
	void Reset();
	// p50/p95/max in milliseconds; all 0 for an empty series.
	void Summarize(double &p50Ms, double &p95Ms, double &maxMs);
};

// Timestamped samples kept for percentiles and counts over a TRAILING window
// (the degradation policy's "p95 over the last 2 s"). A ring of the most
// recent kCapacity samples: at 60 samples/s that's 17 s of history, far more
// than any window asked for. Timestamps must be pushed in non-decreasing
// order (they come from one thread's clock reads).
class RollingWindow {
public:
	static constexpr size_t kCapacity = 1024;

	struct Summary {
		uint32_t samples = 0; // Samples inside the window.
		double p50Ms = 0.0;
		double p95Ms = 0.0;
		double maxMs = 0.0;
		uint64_t sumNs = 0;
	};

	void Push(uint64_t tNs, uint64_t valueNs);
	void Clear();
	// Samples with nowNs - windowNs <= t <= nowNs. Cost O(samples in window).
	Summary Summarize(uint64_t nowNs, uint64_t windowNs) const;
	// Just the count over the same window (events: drops, misses...).
	uint32_t Count(uint64_t nowNs, uint64_t windowNs) const;
	// Timestamp of the oldest sample still stored (0 if empty).
	uint64_t OldestNs() const;

private:
	std::array<uint64_t, kCapacity> t_{};
	std::array<uint32_t, kCapacity> v_{};
	size_t next_ = 0; // Where the next sample goes.
	size_t size_ = 0;
};

} // namespace trigglow
