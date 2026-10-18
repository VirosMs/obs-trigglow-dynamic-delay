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

// Concurrency stress test for FrameRing + FrameArena + CodecPipeline, without
// OBS or FFmpeg. A simulated graphics thread captures at 250 fps (with a
// 2-frame staging delay like the real one), plays back by age and, at random
// moments, detaches/reattaches (Disable, hard resize), changes the encode
// parallelism, the frame size (soft rung change) and the capture rate. The
// codec is fake but stamps every frame with its capture time, so any frame
// shown in the wrong place, torn, or read after being recycled is caught.
//
// Hard checks (any violation fails): a shown frame is never younger than the
// playback target (the delay never shrinks), shown capture times only ever
// increase, every frame's content matches its timestamp, nothing deadlocks
// (watchdog), the arena never leaks, every ring is eventually freed and none
// is ever freed on the simulated graphics thread (the reaper thread does it,
// whichever reference is the last). Soft check: the frame shown is the exact
// one due for >= 90% of due frames (CI machines are noisy; a quiet one gets
// ~100%).

#include "check.hpp"
#include "codec-pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <thread>

using namespace trigglow;

namespace {

using Clock = std::chrono::steady_clock;

uint64_t NowNs()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

std::atomic<uint64_t> g_tornFrames{0};
std::atomic<uint64_t> g_badPackets{0};
std::atomic<uint64_t> g_encodes{0};
std::atomic<uint64_t> g_decodes{0};

// Ring lifetimes: which thread frees each one (the simulated graphics thread
// must never be the one).
std::thread::id g_graphicsThread;
std::atomic<uint64_t> g_ringsCreated{0};
std::atomic<uint64_t> g_ringsFreed{0};
std::atomic<uint64_t> g_ringsFreedOnGraphics{0};

constexpr uint32_t kMagic = 0x54524947; // "TRIG"

void SpinFor(uint32_t micros)
{
	const auto end = Clock::now() + std::chrono::microseconds(micros);
	while (Clock::now() < end) {
	}
}

// Stamp layout in every frame: Y bytes [0, 8) = captureNs; every other Y byte =
// captureNs & 0xFF; every U/V byte = (captureNs >> 8) & 0xFF.
void FillStamped(RawFrame &frame, uint64_t stamp)
{
	const uint8_t y = static_cast<uint8_t>(stamp & 0xFF);
	const uint8_t c = static_cast<uint8_t>((stamp >> 8) & 0xFF);
	std::memset(frame.plane[0], y, static_cast<size_t>(frame.stride[0]) * frame.height);
	std::memset(frame.plane[1], c, static_cast<size_t>(frame.stride[1]) * (frame.height / 2));
	std::memset(frame.plane[2], c, static_cast<size_t>(frame.stride[2]) * (frame.height / 2));
	std::memcpy(frame.plane[0], &stamp, sizeof(stamp));
}

uint64_t ReadStamp(const uint8_t *y)
{
	uint64_t stamp;
	std::memcpy(&stamp, y, sizeof(stamp));
	return stamp;
}

bool CheckStamped(const I420View &f, uint64_t &stamp)
{
	stamp = ReadStamp(f.plane[0]);
	const uint8_t y = static_cast<uint8_t>(stamp & 0xFF);
	const uint8_t c = static_cast<uint8_t>((stamp >> 8) & 0xFF);
	for (uint32_t row = 0; row < f.height; ++row) {
		for (uint32_t col = row == 0 ? 8 : 0; col < f.width; ++col) {
			if (f.plane[0][static_cast<size_t>(row) * f.stride[0] + col] != y)
				return false;
		}
	}
	for (int p = 1; p < 3; ++p) {
		for (uint32_t row = 0; row < f.height / 2; ++row) {
			for (uint32_t col = 0; col < f.width / 2; ++col) {
				if (f.plane[p][static_cast<size_t>(row) * f.stride[p] + col] != c)
					return false;
			}
		}
	}
	return true;
}

// Filler after the 20-byte header: depends on the stamp (so the arena sees
// varying sizes), always well under the raw frame size (so it's "compressed").
size_t FillerBytes(uint64_t stamp, uint32_t width, uint32_t height)
{
	return 16 + static_cast<size_t>(stamp % (I420FrameBytes(width, height) / 2));
}

// "Compresses" a stamped frame into a header + filler; burns some CPU; fails
// now and then (the pipeline then stores the frame raw).
class StampCodec final : public VideoCodec {
public:
	explicit StampCodec(uint32_t seed) : rng_(seed) {}

	bool Encode(const I420View &frame, uint32_t /*slices*/, const uint8_t *&packet, size_t &bytes,
		    bool &compressed) override
	{
		uint64_t stamp = 0;
		if (!CheckStamped(frame, stamp))
			++g_tornFrames;
		SpinFor(std::uniform_int_distribution<uint32_t>(100, 700)(rng_));
		++g_encodes;
		if (std::uniform_int_distribution<int>(0, 99)(rng_) == 0)
			return false; // 1%: raw fallback path.
		const size_t filler = FillerBytes(stamp, frame.width, frame.height);
		packet_.resize(4 * 3 + 8 + filler);
		uint8_t *p = packet_.data();
		std::memcpy(p, &kMagic, 4);
		std::memcpy(p + 4, &frame.width, 4);
		std::memcpy(p + 8, &frame.height, 4);
		std::memcpy(p + 12, &stamp, 8);
		std::memset(p + 20, static_cast<int>(stamp & 0xFF), filler);
		packet = packet_.data();
		bytes = packet_.size();
		compressed = true;
		return true;
	}

	bool Decode(const uint8_t *packet, size_t bytes, DecodedImage &out) override
	{
		SpinFor(std::uniform_int_distribution<uint32_t>(50, 400)(rng_));
		++g_decodes;
		uint32_t magic = 0, width = 0, height = 0;
		uint64_t stamp = 0;
		if (bytes < 20) {
			++g_badPackets;
			return false;
		}
		std::memcpy(&magic, packet, 4);
		std::memcpy(&width, packet + 4, 4);
		std::memcpy(&height, packet + 8, 4);
		std::memcpy(&stamp, packet + 12, 8);
		bool ok = magic == kMagic && width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
			  bytes == 20 + FillerBytes(stamp, width, height);
		for (size_t k = 20; ok && k < bytes; k += 37)
			ok = packet[k] == static_cast<uint8_t>(stamp & 0xFF);
		if (!ok) {
			++g_badPackets;
			return false;
		}
		const size_t need = I420FrameBytes(width, height);
		std::shared_ptr<uint8_t> buffer(new uint8_t[need], std::default_delete<uint8_t[]>());
		std::memset(buffer.get(), static_cast<int>(stamp & 0xFF), need);
		std::memcpy(buffer.get(), &stamp, 8);
		const I420View view = PackedI420View(buffer.get(), width, height);
		for (int i = 0; i < 3; ++i) {
			out.plane[i] = view.plane[i];
			out.stride[i] = view.stride[i];
		}
		out.width = width;
		out.height = height;
		out.owner = buffer;
		return true;
	}

	void Release() override { packet_ = std::vector<uint8_t>(); }

private:
	std::mt19937 rng_;
	std::vector<uint8_t> packet_;
};

struct RunStats {
	uint64_t displayed = 0;
	uint64_t dueFrames = 0;
	uint64_t exact = 0;
	uint64_t younger = 0;       // Shown frame captured after the target: the delay shrank. Must be 0.
	uint64_t notIncreasing = 0; // Must be 0.
	uint64_t stampMismatch = 0; // Must be 0.
	uint64_t resets = 0;
	uint64_t backpressure = 0;
	uint64_t reserveFailures = 0;
};

struct Scenario {
	const char *name = "";
	bool threaded = true;
	bool rawCodec = false;
	uint32_t seconds = 3;
	uint64_t periodNs = 4000000;  // 250 fps.
	uint64_t delayNs = 200000000; // 200 ms.
	bool chaos = true;            // Random resets / geometry / parallelism / rate changes.
};

std::shared_ptr<FrameRing> MakeRing(const Scenario &s, uint64_t captureIntervalNs)
{
	FrameArena::Config arena;
	arena.blockBytes = 64 * 1024;
	arena.maxSpareBlocks = 2;
	const size_t slots = static_cast<size_t>(s.delayNs / captureIntervalNs) + 16;
	++g_ringsCreated;
	return std::shared_ptr<FrameRing>(new FrameRing(slots, arena), [](FrameRing *ring) {
		++g_ringsFreed;
		if (std::this_thread::get_id() == g_graphicsThread)
			++g_ringsFreedOnGraphics;
		delete ring;
	});
}

RunStats RunScenario(const Scenario &s)
{
	testcheck::g_currentTest = s.name;
	g_graphicsThread = std::this_thread::get_id();
	g_ringsFreedOnGraphics = 0;
	const uint64_t ringsCreatedBefore = g_ringsCreated.load();
	const uint64_t ringsFreedBefore = g_ringsFreed.load();
	CodecPipeline::Options options;
	options.threaded = s.threaded;
	options.clockNs = &NowNs;
	options.lookaheadFrames = 3;
	std::atomic<uint32_t> seeds{1};
	if (s.rawCodec)
		options.codecFactory = &MakeRawCodec;
	else
		options.codecFactory = [&seeds] {
			return std::unique_ptr<VideoCodec>(new StampCodec(seeds++));
		};
	auto pipeline = std::make_unique<CodecPipeline>(options);

	std::mt19937 rng(1234);
	auto chance = [&](int oneIn) {
		return std::uniform_int_distribution<int>(0, oneIn - 1)(rng) == 0;
	};
	const uint32_t sizes[3][2] = {{64, 32}, {48, 24}, {32, 16}};
	uint32_t sizeIndex = 0;
	uint32_t divisor = 1;
	uint64_t captureIntervalNs = s.periodNs;

	std::shared_ptr<FrameRing> ring = MakeRing(s, s.periodNs);
	pipeline->Attach(ring, 64, 32, captureIntervalNs);
	struct Pending {
		FrameRing::Reservation reservation;
		uint32_t width, height;
	};
	std::deque<Pending> staging;

	RunStats stats;
	uint64_t lastShown = 0;
	uint64_t lastDue = 0;
	uint64_t lastCell = 0;
	const uint64_t start = NowNs();
	const uint64_t end = start + static_cast<uint64_t>(s.seconds) * 1000000000ULL;
	uint64_t nextTick = start;
	while (NowNs() < end) {
		const uint64_t now = NowNs();

		// --- capture (one per cell), harvested two captures later ---
		const uint64_t cell = now / captureIntervalNs;
		if (cell != lastCell) {
			lastCell = cell;
			FrameRing::Reservation reservation;
			if (ring->Reserve(now, reservation))
				staging.push_back({reservation, sizes[sizeIndex][0], sizes[sizeIndex][1]});
			else
				++stats.reserveFailures;
			while (staging.size() > 2) {
				const Pending p = staging.front();
				staging.pop_front();
				RawFrame *frame = pipeline->AcquireRawFrame(p.width, p.height);
				if (!frame) {
					ring->MarkDropped(p.reservation.index, p.reservation.generation);
					pipeline->NoteBackpressureDrop();
					++stats.backpressure;
					continue;
				}
				FillStamped(*frame, p.reservation.captureNs);
				pipeline->SubmitEncode(ring, frame, p.reservation);
			}
		}

		// --- playback by age ---
		uint64_t oldest = 0;
		if (ring->OldestCaptureNs(oldest) && now > s.delayNs) {
			uint64_t target = now - s.delayNs;
			bool play = oldest <= target;
			if (!play && oldest <= target + 2 * captureIntervalNs) {
				target = oldest; // Fill boundary, like the filter.
				play = true;
			} else if (!play && oldest <= target + s.delayNs / 4) {
				pipeline->PrimeDecode(oldest); // Like the filter's last quarter second of fill.
			}
			if (play) {
				DecodedLease lease;
				if (pipeline->AcquireForDisplay(target, lastShown, lease)) {
					++stats.displayed;
					if (lease.captureNs > target)
						++stats.younger;
					if (lease.captureNs <= lastShown)
						++stats.notIncreasing;
					if (!lease.image.plane[0] || ReadStamp(lease.image.plane[0]) != lease.captureNs)
						++stats.stampMismatch;
					lastShown = lease.captureNs;
					pipeline->ReleaseDisplay(lease);
				}
				const FrameRing::DueSlot due = ring->Due(target);
				if (due.exists && due.captureNs != lastDue && due.state != SlotState::Dropped) {
					// Judged right after the acquire: with the decode-ahead the
					// frame due now must already be decoded and shown.
					lastDue = due.captureNs;
					++stats.dueFrames;
					if (lastShown == due.captureNs)
						++stats.exact;
				}
			}
		}

		// --- chaos ---
		if (s.chaos) {
			if (chance(400)) {
				// Disable/re-enable or a hard resize: a whole new ring. Our
				// reference goes along, like VideoDelayFilter::StopCycle().
				pipeline->Detach(chance(2), std::move(ring));
				ring = MakeRing(s, captureIntervalNs);
				staging.clear();
				lastShown = 0;
				lastDue = 0;
				pipeline->Attach(ring, 64, 32, captureIntervalNs);
				++stats.resets;
			}
			if (chance(150)) {
				const uint32_t slices[3] = {1, 2, 4};
				pipeline->SetEncodeParallelism(chance(2) ? 1 : 2, slices[rng() % 3]);
			}
			if (chance(120))
				sizeIndex = static_cast<uint32_t>(rng() % 3);
			if (chance(500)) {
				divisor = divisor == 1 ? 2 : 1;
				captureIntervalNs = s.periodNs * divisor;
				pipeline->SetCaptureInterval(captureIntervalNs);
			}
		}

		// Paced by spinning (yield): sleep_for() can't do 4 ms steps with
		// Windows' default 15.6 ms timer.
		nextTick += s.periodNs;
		if (NowNs() > nextTick)
			nextTick = NowNs(); // Fell behind: don't try to catch up.
		while (NowNs() < nextTick)
			std::this_thread::yield();
	}
	// Tear down with work still in flight (on OBS's destruction thread, where
	// freeing is fine: only count what was freed before).
	const uint64_t freedOnGraphics = g_ringsFreedOnGraphics.load();
	pipeline->Shutdown();
	const FrameArena::Stats arenaStats = ring->Arena().GetStats();
	CHECK(arenaStats.liveBytes <= ring->SlotCount() * (I420FrameBytes(64, 32) + 64));
	ring.reset();
	pipeline.reset();
	CHECK_EQ(freedOnGraphics, 0u);
	CHECK_EQ(g_ringsFreed.load() - ringsFreedBefore, g_ringsCreated.load() - ringsCreatedBefore);

	std::printf(
		"  %s: displayed %llu, due %llu, exact %.2f%%, resets %llu, backpressure %llu, reserve "
		"failures %llu, encodes %llu, decodes %llu, rings freed on the graphics thread %llu\n",
		s.name, static_cast<unsigned long long>(stats.displayed),
		static_cast<unsigned long long>(stats.dueFrames),
		stats.dueFrames ? 100.0 * static_cast<double>(stats.exact) / static_cast<double>(stats.dueFrames) : 0.0,
		static_cast<unsigned long long>(stats.resets), static_cast<unsigned long long>(stats.backpressure),
		static_cast<unsigned long long>(stats.reserveFailures),
		static_cast<unsigned long long>(g_encodes.load()), static_cast<unsigned long long>(g_decodes.load()),
		static_cast<unsigned long long>(freedOnGraphics));
	CHECK(stats.displayed > 0);
	CHECK_EQ(stats.younger, 0u);
	CHECK_EQ(stats.notIncreasing, 0u);
	CHECK_EQ(stats.stampMismatch, 0u);
	CHECK(stats.dueFrames > 0 && static_cast<double>(stats.exact) >= 0.90 * static_cast<double>(stats.dueFrames));
	return stats;
}

// RollingWindow's tags (src/perf-stats.hpp, linked here): the degradation
// policy reads the encode and decode load per capture rung.
void TestRollingWindowTags()
{
	testcheck::g_currentTest = "rolling window tags";
	RollingWindow w;
	CHECK_EQ(w.NewestTag(1000), RollingWindow::kAnyTag);
	for (uint64_t t = 1; t <= 10; ++t)
		w.Push(t * 100, t * 1000000, t <= 6 ? 0u : 1u); // 6 samples of rung 0, then 4 of rung 1.
	CHECK_EQ(w.Count(1000, 1000), 10u);
	CHECK_EQ(w.Count(1000, 1000, 0), 6u);
	CHECK_EQ(w.Count(1000, 1000, 1), 4u);
	CHECK_EQ(w.Count(1000, 1000, 2), 0u);
	const RollingWindow::Summary rung1 = w.Summarize(1000, 1000, 1);
	CHECK_EQ(rung1.samples, 4u);
	CHECK(rung1.maxMs > 9.9 && rung1.maxMs < 10.1);
	const RollingWindow::Summary rung0 = w.Summarize(1000, 1000, 0);
	CHECK(rung0.maxMs > 5.9 && rung0.maxMs < 6.1);
	CHECK_EQ(w.NewestTag(1000), 1u);
	CHECK_EQ(w.NewestTag(650), 0u); // Newest at or before t = 650: the 6th sample.
	CHECK_EQ(w.NewestTag(50), RollingWindow::kAnyTag);
}

// Rendering stops for 300 ms (OBS stalled, the scene hidden): the decoder had
// decoded a few frames ahead of the old target, and those are now far older
// than the frame due. The first frame shown afterwards must be (about) the due
// one, never one of those (QA: one frame ~280 ms older than due).
void RunPauseScenario()
{
	testcheck::g_currentTest = "threaded, 300 ms render pause";
	CodecPipeline::Options options;
	options.threaded = true;
	options.clockNs = &NowNs;
	options.lookaheadFrames = 3;
	std::atomic<uint32_t> seeds{100};
	options.codecFactory = [&seeds] {
		return std::unique_ptr<VideoCodec>(new StampCodec(seeds++));
	};
	auto pipeline = std::make_unique<CodecPipeline>(options);
	const uint64_t periodNs = 4000000, delayNs = 1000000000;
	auto ring = std::make_shared<FrameRing>(static_cast<size_t>(delayNs / periodNs) + 32, FrameArena::Config{});
	pipeline->Attach(ring, 64, 32, periodNs);
	std::deque<FrameRing::Reservation> staging;
	uint64_t lastShown = 0, lastCell = 0, worstLagNs = 0, shownAfter = 0;
	auto step = [&](bool measure) {
		const uint64_t now = NowNs();
		const uint64_t cell = now / periodNs;
		if (cell != lastCell) {
			lastCell = cell;
			FrameRing::Reservation reservation;
			if (ring->Reserve(now, reservation))
				staging.push_back(reservation);
			while (staging.size() > 2) {
				const FrameRing::Reservation r = staging.front();
				staging.pop_front();
				RawFrame *frame = pipeline->AcquireRawFrame(64, 32);
				if (!frame) {
					ring->MarkDropped(r.index, r.generation);
					continue;
				}
				FillStamped(*frame, r.captureNs);
				pipeline->SubmitEncode(ring, frame, r);
			}
		}
		uint64_t oldest = 0;
		if (ring->OldestCaptureNs(oldest) && now > delayNs && oldest <= now - delayNs) {
			DecodedLease lease;
			if (pipeline->AcquireForDisplay(now - delayNs, lastShown, lease)) {
				if (measure) {
					worstLagNs = std::max(worstLagNs, (now - delayNs) - lease.captureNs);
					++shownAfter;
				}
				lastShown = lease.captureNs;
				pipeline->ReleaseDisplay(lease);
			}
		}
	};
	auto runFor = [&](uint64_t ns, bool measure) {
		const uint64_t end = NowNs() + ns;
		uint64_t nextTick = NowNs();
		while (NowNs() < end) {
			step(measure);
			nextTick += periodNs;
			while (NowNs() < nextTick)
				std::this_thread::yield();
		}
	};
	runFor(1500000000ULL, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(300)); // No Render() at all.
	runFor(100000000ULL, true);
	CodecPipeline::WindowStats stats;
	pipeline->TakeWindowStats(stats);
	pipeline->Shutdown();
	std::printf("  %s: %llu frames shown in the 100 ms after it, worst %.1f ms older than due (%llu stale "
		    "decoded frames dropped)\n",
		    testcheck::g_currentTest, static_cast<unsigned long long>(shownAfter),
		    static_cast<double>(worstLagNs) / 1e6, static_cast<unsigned long long>(stats.staleDecoded));
	CHECK(shownAfter > 0);
	// The due frame, or one at most kStaleIntervals captures older (plus a
	// capture interval of jitter between the two clock reads).
	CHECK(worstLagNs <= (CodecPipeline::kStaleIntervals + 1) * periodNs);
}

} // namespace

int main(int argc, char **argv)
{
	const uint32_t scale = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 1;
	std::atomic<bool> done{false};
	std::thread watchdog([&] {
		for (uint32_t i = 0; i < 1200 * std::max<uint32_t>(scale, 1) && !done; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!done) {
			std::printf("pipeline_stress_tests: watchdog fired (deadlock?)\n");
			std::fflush(stdout);
			std::abort();
		}
	});

	Scenario chaos;
	chaos.name = "threaded, chaos";
	chaos.seconds = 4 * scale;
	RunScenario(chaos);

	Scenario tight;
	tight.name = "threaded, 10-frame delay";
	tight.delayNs = 40000000; // 10 frames: the decoder works right behind the encoders.
	tight.seconds = 2 * scale;
	tight.chaos = false;
	RunScenario(tight);

	Scenario raw;
	raw.name = "threaded, raw codec";
	raw.rawCodec = true;
	raw.seconds = 2 * scale;
	RunScenario(raw);

	Scenario sync;
	sync.name = "sync (kill switch)";
	sync.threaded = false;
	sync.seconds = 2 * scale;
	sync.periodNs = 8000000; // The fake codec runs on this thread: leave it room.
	RunScenario(sync);

	RunPauseScenario();
	TestRollingWindowTags();

	CHECK_EQ(g_tornFrames.load(), 0u);
	CHECK_EQ(g_badPackets.load(), 0u);
	done = true;
	watchdog.join();
	return testcheck::Finish("pipeline_stress_tests");
}
