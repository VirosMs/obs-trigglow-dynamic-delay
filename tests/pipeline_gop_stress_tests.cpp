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

// Concurrency stress test of FrameRing + FrameArena + CodecPipeline with a
// fake INTER-FRAME storage codec (tests/fake-gop-codec.hpp; v0.7.0 RAM design
// 8.2). Same simulated graphics thread as pipeline_stress_tests.cpp (250 fps
// captures with a 2-frame staging delay, playback by age), but the encoder
// keeps frames in flight (0-3 frames of latency, polled by the worker), makes
// P-frames that reference the frame before them, a key every 2-8 frames or
// when forced, and now and then fails or is busy; the factory alternates it
// with an intra codec, so rings hold both. Chaos on top: Disable/Enable and
// hard resizes, frame-size changes (new encoder, new chain), capture-rate
// changes, rings whose arena is too small (packets lost after their encode:
// chain breaks), render pauses and slow-decoder phases.
//
// Correctness checks, always strict: the fake decoder is never handed a
// P-frame without its reference (invariant I2) and no picture shown is
// corrupt; no Ready P-frame in the ring follows a lost packet of its chain,
// and every chain starts on a key (I3, by inspecting ring snapshots); every
// codec object is used by one thread only, never the graphics thread; shown
// frames are never younger than the target, only ever get newer and match
// their stamp; frames keep being shown; the arena never leaks and no ring is
// freed on the graphics thread. Timing checks (looser with TRIGGLOW_STRESS_CI=1,
// like pipeline_stress_tests): the p99.9 of the graphics thread's calls into
// the pipeline, and how many due frames are shown exactly in a steady run.

#include "check.hpp"
#include "codec-pipeline.hpp"
#include "fake-gop-codec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace trigglow;

namespace {

using Clock = std::chrono::steady_clock;

uint64_t NowNs()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

bool EnvFlag(const char *name)
{
#ifdef _MSC_VER
	char *value = nullptr;
	size_t length = 0;
	if (_dupenv_s(&value, &length, name) != 0 || !value)
		return false;
	const bool on = value[0] != '\0' && std::strcmp(value, "0") != 0;
	std::free(value);
	return on;
#else
	const char *value = std::getenv(name);
	return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
#endif
}

bool OnCi()
{
	static const bool onCi = EnvFlag("TRIGGLOW_STRESS_CI");
	return onCi;
}

struct TimingLimits {
	uint64_t graphicsCallP999Ns; // The graphics thread's calls into the pipeline and ring.
	double steadyExactFraction;  // Steady run (no losses): due frames shown exactly.
};

const TimingLimits &Limits()
{
	static const TimingLimits strict{2000000, 0.85};
	static const TimingLimits ci{20000000, 0.40};
	return OnCi() ? ci : strict;
}

std::atomic<uint64_t> g_ringsCreated{0};
std::atomic<uint64_t> g_ringsFreed{0};
std::atomic<uint64_t> g_ringsFreedOnGraphics{0};
std::thread::id g_graphicsThread;

// Stamped frames, like pipeline_stress_tests.cpp: Y bytes 0-7 the capture time.
void FillStamped(RawFrame &frame, uint64_t stamp)
{
	std::memset(frame.plane[0], static_cast<int>(stamp & 0xFF),
		    static_cast<size_t>(frame.stride[0]) * frame.height);
	std::memset(frame.plane[1], static_cast<int>((stamp >> 8) & 0xFF),
		    static_cast<size_t>(frame.stride[1]) * (frame.height / 2));
	std::memset(frame.plane[2], static_cast<int>((stamp >> 8) & 0xFF),
		    static_cast<size_t>(frame.stride[2]) * (frame.height / 2));
	std::memcpy(frame.plane[0], &stamp, sizeof(stamp));
}

struct Scenario {
	const char *name = "";
	uint32_t seconds = 4;
	uint64_t periodNs = 4000000;  // 250 fps.
	uint64_t delayNs = 200000000; // 200 ms.
	bool chaos = true;
	bool mixIntra = true;          // Every third encoder the factory opens is intra.
	bool twoContexts = false;      // Allow 2 encode contexts (inter chains interleave: skips, never corruption).
	uint32_t maxLatencyFrames = 3; // Latency per encoder: 0..this many capture periods...
	bool fixedLatency = false;     // ...or always this many.
	uint32_t fixedGop = 0;         // 0: 2-8 per encoder.
	bool injectFailures = true;    // Failed and Busy sends.
	bool tightArenas = true;       // Some rings' arenas too small: packets lost after their encode.
	bool expectLosses = false;     // The run must have exercised chain breaks and orphan drops.
	bool checkExact = false;       // Timing check on the exact frame shown (steady runs).
};

// A ring of slots for the delay; tight: its arena only fits ~3/4 of the packets.
std::shared_ptr<FrameRing> MakeRing(const Scenario &s, uint64_t captureIntervalNs, bool tight)
{
	FrameArena::Config arena;
	arena.blockBytes = 4 * 1024;
	arena.maxSpareBlocks = 1;
	const size_t slots = static_cast<size_t>(s.delayNs / captureIntervalNs) + 16;
	if (tight)
		arena.limitBytes = slots * 512 * 3 / 4; // Packets average ~470 bytes (64-byte aligned: ~512).
	++g_ringsCreated;
	return std::shared_ptr<FrameRing>(new FrameRing(slots, arena), [](FrameRing *ring) {
		++g_ringsFreed;
		if (std::this_thread::get_id() == g_graphicsThread)
			++g_ringsFreedOnGraphics;
		delete ring;
	});
}

// Invariant I3 checked on the ring itself: inside one chain, a Ready P-frame
// always comes right after its predecessor (seq + 1) among the chain's Ready
// slots -- a packet lost after its encode is never followed by a stored
// P-frame of its chain -- and a chain seen for the first time starts on a key.
struct ChainBook {
	std::map<uint32_t, bool> seen;
	uint64_t orphanReady = 0;
	uint64_t chainsNotOnKey = 0;
	uint64_t snapshots = 0;
	uint64_t readyInter = 0;

	void Inspect(const FrameRing &ring)
	{
		++snapshots;
		std::map<uint32_t, uint32_t> last;
		for (const FrameRing::SlotSummary &slot : ring.Snapshot()) {
			if (slot.state != SlotState::Ready || slot.chain == storage::kNoChain)
				continue;
			if (storage::ClassOf(slot.codec) == storage::StorageClass::Inter)
				++readyInter;
			auto it = last.find(slot.chain);
			if (it == last.end()) {
				// The oldest Ready packet of this chain still in the ring.
				if (seen.emplace(slot.chain, slot.key).second && !slot.key)
					++chainsNotOnKey;
				last.emplace(slot.chain, slot.seq);
				continue;
			}
			if (!slot.key && slot.seq != it->second + 1)
				++orphanReady;
			it->second = slot.seq;
		}
	}
};

struct RunStats {
	uint64_t displayed = 0;
	uint64_t dueFrames = 0;
	uint64_t exact = 0;
	uint64_t younger = 0;
	uint64_t notIncreasing = 0;
	uint64_t stampMismatch = 0;
	uint64_t corruptShown = 0;
	uint64_t resets = 0;
	uint64_t pauses = 0;
	uint64_t encodersOpened = 0;
	CodecPipeline::WindowStats total;
	std::vector<uint32_t> callNs; // Every timed graphics-thread call.
};

void Accumulate(CodecPipeline::WindowStats &total, const CodecPipeline::WindowStats &w)
{
	total.encodedBytes += w.encodedBytes;
	total.rawFallbacks += w.rawFallbacks;
	total.encodeFailures += w.encodeFailures;
	total.arenaDrops += w.arenaDrops;
	total.staleEncodes += w.staleEncodes;
	total.chainBreaks += w.chainBreaks;
	total.orphanDrops += w.orphanDrops;
	total.busyDrops += w.busyDrops;
	total.encoderResets += w.encoderResets;
	total.noKeySkips += w.noKeySkips;
	total.decodeFailures += w.decodeFailures;
	total.inFlightMax = std::max(total.inFlightMax, w.inFlightMax);
}

RunStats RunScenario(const Scenario &s)
{
	testcheck::g_currentTest = s.name;
	g_graphicsThread = std::this_thread::get_id();
	g_ringsFreedOnGraphics = 0;
	const uint64_t ringsCreatedBefore = g_ringsCreated.load();
	const uint64_t ringsFreedBefore = g_ringsFreed.load();
	fakegop::Counters counters;
	counters.graphicsThread = g_graphicsThread;
	std::atomic<uint32_t> opened{0};

	CodecPipeline::Options options;
	options.threaded = true;
	options.clockNs = &NowNs;
	options.lookaheadFrames = 3;
	options.encoderFactory = [&](const storage::EncoderConfig &config) -> std::unique_ptr<storage::Encoder> {
		// Called on the encode workers: everything it decides comes from n.
		const uint32_t n = opened++;
		fakegop::EncoderOptions o;
		o.inter = !s.mixIntra || n % 3 != 2;
		o.codec = o.inter ? storage::CodecId::Hevc : storage::CodecId::Mjpeg;
		o.gop = s.fixedGop ? s.fixedGop : 2 + (n * 5) % 7;
		const uint32_t latency = s.fixedLatency ? s.maxLatencyFrames : (n * 3) % (s.maxLatencyFrames + 1);
		o.latencyNs = static_cast<uint64_t>(latency) * s.periodNs;
		o.busyAbove = s.injectFailures && n % 4 == 1 ? 2 : 0;
		o.failOneIn = s.injectFailures ? 150 : 0;
		o.fillerBytes = 380;
		o.seed = n + 1;
		o.clockNs = &NowNs;
		o.counters = &counters;
		return std::make_unique<fakegop::FakeGopEncoder>(config, o);
	};
	options.decoderFactory = [&](storage::CodecId codec) -> std::unique_ptr<storage::Decoder> {
		return std::make_unique<fakegop::FakeGopDecoder>(codec, &counters);
	};
	auto pipeline = std::make_unique<CodecPipeline>(options);
	pipeline->SetEncodeParallelism(1, 1);

	std::mt19937 rng(4321);
	auto chance = [&](int oneIn) {
		return std::uniform_int_distribution<int>(0, oneIn - 1)(rng) == 0;
	};
	const uint32_t sizes[3][2] = {{64, 32}, {48, 24}, {32, 16}};
	uint32_t sizeIndex = 0;
	uint32_t divisor = 1;
	uint64_t captureIntervalNs = s.periodNs;

	RunStats stats;
	auto timed = [&](auto &&call) {
		const uint64_t t0 = NowNs();
		call();
		stats.callNs.push_back(static_cast<uint32_t>(std::min<uint64_t>(NowNs() - t0, UINT32_MAX)));
	};

	std::shared_ptr<FrameRing> ring = MakeRing(s, s.periodNs, false);
	pipeline->Attach(ring, 64, 32, captureIntervalNs);
	struct Pending {
		FrameRing::Reservation reservation;
		uint32_t width, height;
	};
	std::deque<Pending> staging;
	ChainBook book;
	uint64_t lastShown = 0, lastDue = 0, lastCell = 0, nextInspect = 0, nextStats = 0;
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
			bool reserved = false;
			timed([&] { reserved = ring->Reserve(now, reservation); });
			if (reserved)
				staging.push_back({reservation, sizes[sizeIndex][0], sizes[sizeIndex][1]});
			while (staging.size() > 2) {
				const Pending p = staging.front();
				staging.pop_front();
				RawFrame *frame = nullptr;
				timed([&] { frame = pipeline->AcquireRawFrame(p.width, p.height); });
				if (!frame) {
					ring->MarkDropped(p.reservation.index, p.reservation.generation);
					pipeline->NoteBackpressureDrop();
					continue;
				}
				FillStamped(*frame, p.reservation.captureNs);
				timed([&] { pipeline->SubmitEncode(ring, frame, p.reservation); });
			}
		}

		// --- playback by age ---
		uint64_t oldest = 0;
		if (ring->OldestCaptureNs(oldest) && now > s.delayNs) {
			uint64_t target = now - s.delayNs;
			bool play = oldest <= target;
			if (!play && oldest <= target + 2 * captureIntervalNs) {
				target = oldest;
				play = true;
			} else if (!play && oldest <= target + s.delayNs / 4) {
				timed([&] { pipeline->PrimeDecode(oldest); });
			}
			if (play) {
				DecodedLease lease;
				bool got = false;
				timed([&] { got = pipeline->AcquireForDisplay(target, lastShown, lease); });
				if (got) {
					++stats.displayed;
					const uint64_t stamp = fakegop::ReadStamp(lease.image.plane[0]);
					if (lease.captureNs > target)
						++stats.younger;
					if (lease.captureNs <= lastShown)
						++stats.notIncreasing;
					if ((stamp ^ fakegop::kCorruptMask) == lease.captureNs)
						++stats.corruptShown;
					else if (stamp != lease.captureNs)
						++stats.stampMismatch;
					lastShown = lease.captureNs;
					pipeline->ReleaseDisplay(lease);
				}
				const FrameRing::DueSlot due = ring->Due(target);
				if (due.exists && due.captureNs != lastDue && due.state != SlotState::Dropped) {
					lastDue = due.captureNs;
					++stats.dueFrames;
					if (lastShown == due.captureNs)
						++stats.exact;
				}
			}
		}

		// --- inspection: ring snapshots and the pipeline's counters ---
		if (now >= nextInspect) {
			nextInspect = now + 20000000;
			book.Inspect(*ring);
		}
		if (now >= nextStats) {
			nextStats = now + 500000000;
			CodecPipeline::WindowStats w;
			pipeline->TakeWindowStats(w);
			Accumulate(stats.total, w);
		}

		// --- chaos ---
		if (s.chaos) {
			if (chance(400)) {
				// Disable/Enable or a hard resize: a whole new ring (sometimes with
				// an arena too small for it).
				const bool disable = chance(2);
				timed([&] { pipeline->Detach(disable, std::move(ring)); });
				ring = MakeRing(s, captureIntervalNs, s.tightArenas && chance(2));
				staging.clear();
				lastShown = 0;
				lastDue = 0;
				timed([&] { pipeline->Attach(ring, 64, 32, captureIntervalNs); });
				++stats.resets;
			}
			if (s.twoContexts && chance(150))
				pipeline->SetEncodeParallelism(chance(2) ? 1 : 2, 1 + static_cast<uint32_t>(rng() % 2));
			else if (chance(150))
				pipeline->SetEncodeParallelism(
					1, 1 + static_cast<uint32_t>(rng() % 2)); // New encoder, new chain.
			if (chance(120))
				sizeIndex = static_cast<uint32_t>(rng() % 3);
			if (chance(500)) {
				divisor = divisor == 1 ? 2 : 1;
				captureIntervalNs = s.periodNs * divisor;
				pipeline->SetCaptureInterval(captureIntervalNs);
			}
			if (chance(300))
				counters.decodeExtraUs = counters.decodeExtraUs.load()
								 ? 0
								 : static_cast<uint32_t>(s.periodNs * 5 / 2 / 1000);
			if (chance(700)) {
				// A render pause: no call at all for 100-250 ms.
				std::this_thread::sleep_for(std::chrono::milliseconds(100 + rng() % 150));
				++stats.pauses;
			}
		}

		nextTick += s.periodNs;
		if (NowNs() > nextTick)
			nextTick = NowNs();
		while (NowNs() < nextTick)
			std::this_thread::yield();
	}
	const uint64_t freedOnGraphics = g_ringsFreedOnGraphics.load();
	CodecPipeline::WindowStats w;
	pipeline->TakeWindowStats(w);
	Accumulate(stats.total, w);
	pipeline->Shutdown();
	const FrameArena::Stats arenaStats = ring->Arena().GetStats();
	CHECK(arenaStats.liveBytes <= ring->SlotCount() * (I420FrameBytes(64, 32) + 64));
	ring.reset();
	pipeline.reset();
	counters.decodeExtraUs = 0;
	stats.encodersOpened = opened.load();

	std::vector<uint32_t> sorted = stats.callNs;
	std::sort(sorted.begin(), sorted.end());
	const uint64_t p999 = sorted.empty() ? 0 : sorted[std::min(sorted.size() - 1, sorted.size() * 999 / 1000)];
	const uint64_t maxCall = sorted.empty() ? 0 : sorted.back();
	std::printf("  %s: displayed %llu of %llu due (exact %.1f%%), resets %llu, pauses %llu, encoders %llu, "
		    "encodes %llu (keys %llu, forced %llu), decodes %llu, chain breaks %llu, orphan drops %llu, "
		    "no-key skips %llu, busy %llu, raw fallbacks %llu, arena drops %llu, in flight max %u, snapshots "
		    "%llu (inter Ready %llu), G calls p99.9 %.3f ms max %.3f ms\n",
		    s.name, static_cast<unsigned long long>(stats.displayed),
		    static_cast<unsigned long long>(stats.dueFrames),
		    stats.dueFrames ? 100.0 * static_cast<double>(stats.exact) / static_cast<double>(stats.dueFrames)
				    : 0.0,
		    static_cast<unsigned long long>(stats.resets), static_cast<unsigned long long>(stats.pauses),
		    static_cast<unsigned long long>(stats.encodersOpened),
		    static_cast<unsigned long long>(counters.encodes.load()),
		    static_cast<unsigned long long>(counters.keys.load()),
		    static_cast<unsigned long long>(counters.forcedKeys.load()),
		    static_cast<unsigned long long>(counters.decodes.load()),
		    static_cast<unsigned long long>(stats.total.chainBreaks),
		    static_cast<unsigned long long>(stats.total.orphanDrops),
		    static_cast<unsigned long long>(stats.total.noKeySkips),
		    static_cast<unsigned long long>(stats.total.busyDrops),
		    static_cast<unsigned long long>(stats.total.rawFallbacks),
		    static_cast<unsigned long long>(stats.total.arenaDrops), stats.total.inFlightMax,
		    static_cast<unsigned long long>(book.snapshots), static_cast<unsigned long long>(book.readyInter),
		    static_cast<double>(p999) / 1e6, static_cast<double>(maxCall) / 1e6);

	// Correctness: always strict.
	CHECK_EQ(counters.corruptDecodes.load(), 0u); // I2: never a P-frame without its reference.
	CHECK_EQ(stats.corruptShown, 0u);
	CHECK_EQ(counters.badPackets.load(), 0u);
	CHECK_EQ(counters.wrongSize.load(), 0u);
	CHECK_EQ(counters.threadViolations.load(), 0u); // One thread per codec object, never G.
	CHECK_EQ(book.orphanReady, 0u);                 // I3.
	CHECK_EQ(book.chainsNotOnKey, 0u);
	CHECK_EQ(stats.total.encoderResets, 0u); // Packets always came back in Send() order.
	CHECK(book.readyInter > 0);
	CHECK(stats.displayed > 0);
	CHECK(stats.displayed * 4 >= stats.dueFrames); // Frames keep being shown.
	CHECK_EQ(stats.younger, 0u);
	CHECK_EQ(stats.notIncreasing, 0u);
	CHECK_EQ(stats.stampMismatch, 0u);
	CHECK_EQ(freedOnGraphics, 0u);
	CHECK_EQ(g_ringsFreed.load() - ringsFreedBefore, g_ringsCreated.load() - ringsCreatedBefore);
	if (s.maxLatencyFrames > 0)
		CHECK(stats.total.inFlightMax >= 2); // Frames really stayed in flight.
	if (s.expectLosses) {
		CHECK(stats.total.chainBreaks > 0);
		CHECK(stats.total.orphanDrops > 0);
		CHECK(counters.forcedKeys.load() > stats.encodersOpened);
	}
	// Timing.
	CHECK(p999 <= Limits().graphicsCallP999Ns);
	if (s.checkExact)
		CHECK(static_cast<double>(stats.exact) >=
		      Limits().steadyExactFraction * static_cast<double>(stats.dueFrames));
	return stats;
}

} // namespace

int main(int argc, char **argv)
{
	const uint32_t scale = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 1;
	std::printf("pipeline_gop_stress_tests: %s timing limits (graphics calls p99.9 <= %.0f ms, steady exact >= "
		    "%.0f%%)\n",
		    OnCi() ? "CI (TRIGGLOW_STRESS_CI)" : "strict",
		    static_cast<double>(Limits().graphicsCallP999Ns) / 1e6, Limits().steadyExactFraction * 100.0);
	std::atomic<bool> done{false};
	std::thread watchdog([&] {
		for (uint32_t i = 0; i < 1800 * std::max<uint32_t>(scale, 1) && !done; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!done) {
			std::printf("pipeline_gop_stress_tests: watchdog fired (deadlock?)\n");
			std::fflush(stdout);
			std::abort();
		}
	});

	// Inter-frame packets in flight, no losses, no chaos: every frame stored,
	// shown in order like an intra codec.
	Scenario steady;
	steady.name = "steady inter, latency 2, GOP 6";
	steady.seconds = 2 * scale;
	steady.chaos = false;
	steady.mixIntra = false;
	steady.maxLatencyFrames = 2;
	steady.fixedLatency = true;
	steady.fixedGop = 6;
	steady.injectFailures = false;
	steady.tightArenas = false;
	steady.checkExact = true;
	const RunStats steadyStats = RunScenario(steady);
	CHECK_EQ(steadyStats.total.chainBreaks, 0u);
	CHECK_EQ(steadyStats.total.orphanDrops, 0u);
	CHECK_EQ(steadyStats.total.arenaDrops, 0u);

	Scenario chaos;
	chaos.name = "chaos, inter + intra, one context";
	chaos.seconds = 5 * scale;
	chaos.expectLosses = true;
	RunScenario(chaos);

	Scenario two;
	two.name = "chaos, two contexts (interleaved chains)";
	two.seconds = 3 * scale;
	two.twoContexts = true;
	RunScenario(two);

	Scenario quick;
	quick.name = "chaos, no latency, 10-frame delay";
	quick.seconds = 2 * scale;
	quick.delayNs = 40000000;
	quick.maxLatencyFrames = 0;
	RunScenario(quick);

	done = true;
	watchdog.join();
	return testcheck::Finish("pipeline_gop_stress_tests");
}
