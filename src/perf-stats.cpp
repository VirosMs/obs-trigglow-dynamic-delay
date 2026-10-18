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

#include "perf-stats.hpp"

#include <algorithm>

namespace trigglow {

namespace {
// p50/p95 of samples[0, n), reordering them. n > 0.
void Percentiles(uint32_t *samples, size_t n, double &p50Ms, double &p95Ms)
{
	const size_t i50 = n / 2;
	const size_t i95 = std::min(n - 1, (n * 95) / 100);
	std::nth_element(samples, samples + i50, samples + n);
	const uint32_t p50Ns = samples[i50];
	// Everything after i50 is now >= the median: p95 only needs that part.
	std::nth_element(samples + i50, samples + i95, samples + n);
	p50Ms = static_cast<double>(p50Ns) / 1e6;
	p95Ms = static_cast<double>(samples[i95]) / 1e6;
}

uint32_t Clamp32(uint64_t ns)
{
	return static_cast<uint32_t>(std::min<uint64_t>(ns, UINT32_MAX));
}
} // namespace

void PerfSeries::Push(uint64_t ns)
{
	samplesNs[static_cast<size_t>(count % kCapacity)] = Clamp32(ns);
	++count;
	maxNs = std::max(maxNs, ns);
	totalNs += ns;
}

void PerfSeries::Reset()
{
	count = 0;
	maxNs = 0;
	totalNs = 0;
}

void PerfSeries::Summarize(double &p50Ms, double &p95Ms, double &maxMs)
{
	const auto n = static_cast<size_t>(std::min<uint64_t>(count, kCapacity));
	if (n == 0) {
		p50Ms = 0.0;
		p95Ms = 0.0;
		maxMs = 0.0;
		return;
	}
	Percentiles(samplesNs.data(), n, p50Ms, p95Ms);
	maxMs = static_cast<double>(maxNs) / 1e6;
}

void RollingWindow::Push(uint64_t tNs, uint64_t valueNs)
{
	t_[next_] = tNs;
	v_[next_] = Clamp32(valueNs);
	next_ = (next_ + 1) % kCapacity;
	size_ = std::min(size_ + 1, kCapacity);
}

void RollingWindow::Clear()
{
	next_ = 0;
	size_ = 0;
}

RollingWindow::Summary RollingWindow::Summarize(uint64_t nowNs, uint64_t windowNs) const
{
	Summary summary;
	const uint64_t fromNs = nowNs > windowNs ? nowNs - windowNs : 0;
	std::array<uint32_t, kCapacity> scratch;
	size_t n = 0;
	uint32_t maxNs = 0;
	// Newest first; stop at the first sample older than the window (pushes are
	// in time order).
	for (size_t k = 0; k < size_; ++k) {
		const size_t i = (next_ + kCapacity - 1 - k) % kCapacity;
		if (t_[i] < fromNs)
			break;
		if (t_[i] > nowNs)
			continue;
		scratch[n++] = v_[i];
		summary.sumNs += v_[i];
		maxNs = std::max(maxNs, v_[i]);
	}
	summary.samples = static_cast<uint32_t>(n);
	if (n > 0) {
		Percentiles(scratch.data(), n, summary.p50Ms, summary.p95Ms);
		summary.maxMs = static_cast<double>(maxNs) / 1e6;
	}
	return summary;
}

uint32_t RollingWindow::Count(uint64_t nowNs, uint64_t windowNs) const
{
	const uint64_t fromNs = nowNs > windowNs ? nowNs - windowNs : 0;
	uint32_t n = 0;
	for (size_t k = 0; k < size_; ++k) {
		const size_t i = (next_ + kCapacity - 1 - k) % kCapacity;
		if (t_[i] < fromNs)
			break;
		if (t_[i] <= nowNs)
			++n;
	}
	return n;
}

uint64_t RollingWindow::OldestNs() const
{
	if (size_ == 0)
		return 0;
	return t_[(next_ + kCapacity - size_) % kCapacity];
}

} // namespace trigglow
