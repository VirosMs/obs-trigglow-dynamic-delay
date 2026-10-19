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
// Correctness checks, always strict (any violation fails): a shown frame is
// never younger than the playback target (the delay never shrinks), shown
// capture times only ever increase, every frame's content matches its
// timestamp, frames keep being shown, nothing deadlocks (watchdog), the arena
// never leaks, every ring is eventually freed and none is ever freed on the
// simulated graphics thread (the reaper thread does it, whichever reference
// is the last).
//
// Timing checks: how often the frame shown is the exact one due (>= 90%; a
// quiet machine gets ~100%), how stale the first frame after a render pause
// can be, and how a decoder slower than the capture keeps up. They depend on
// the machine keeping 4 ms steps, which shared CI runners (macOS above all)
// and ThreadSanitizer's slowdown don't always do, and the unit-test job gates
// the release. With TRIGGLOW_STRESS_CI=1 in the environment -- set by the
// unit-tests and ThreadSanitizer jobs of .github/workflows/build-project.yaml
// -- they use the looser CI limits of TimingLimits below, still far from what
// the bugs they guard against looked like (a ~280 ms stale frame after a
// pause, a frozen view behind a slow decoder). Run without it locally.

#include "check.hpp"
#include "codec-pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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
// Added to every fake decode (microseconds): a decoder slower than the capture.
std::atomic<uint32_t> g_decodeExtraUs{0};
std::atomic<uint64_t> g_ringsCreated{0};
std::atomic<uint64_t> g_ringsFreed{0};
std::atomic<uint64_t> g_ringsFreedOnGraphics{0};

constexpr uint32_t kMagic = 0x54524947; // "TRIG"

// Thresholds of the timing checks (see the header comment): strict by default,
// looser on CI (TRIGGLOW_STRESS_CI=1). Periods are capture periods (4 ms).
struct TimingLimits {
	double exactFraction;     // Due frames shown exactly, at least.
	uint64_t pauseLagPeriods; // After a 300 ms pause: first frame at most this much older than due.
	uint64_t slowShownMin;    // Slow decoder: frames shown in the measured second, at least.
	uint64_t slowStaleMax;    // Slow decoder: decoded frames dropped as stale, at most.
	uint64_t slowLagPeriods;  // Slow decoder: frame shown at most this much older than due.
};

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

const TimingLimits &Limits()
{
	static const TimingLimits strict{0.90, 3, 30, 5, 8};
	static const TimingLimits ci{0.50, 25, 10, 25, 25};
	return OnCi() ? ci : strict;
}

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
// now and then (the pipeline then stores the frame raw). A synchronous, all-
// intra storage::Encoder, like MJPEG (it poses as one: the codec id is only
// the payload's dispatch tag).
class StampEncoder final : public storage::Encoder {
public:
	StampEncoder(const storage::EncoderConfig &config, uint32_t seed) : config_(config), rng_(seed) {}

	storage::CodecId Codec() const override { return storage::CodecId::Mjpeg; }
	storage::Backend GetBackend() const override { return storage::Backend::Cpu; }
	const char *Name() const override { return "stamp"; }
	bool InterFrame() const override { return false; }
	const storage::EncoderConfig &Config() const override { return config_; }

	storage::SendResult Send(const I420View &frame, uint64_t seq, bool /*forceKey*/) override
	{
		pending_ = false;
		uint64_t stamp = 0;
		if (!CheckStamped(frame, stamp))
			++g_tornFrames;
		SpinFor(std::uniform_int_distribution<uint32_t>(100, 700)(rng_));
		++g_encodes;
		if (frame.width != config_.width || frame.height != config_.height)
			++g_badPackets; // The pipeline opens an encoder per frame size.
		if (std::uniform_int_distribution<int>(0, 99)(rng_) == 0)
			return storage::SendResult::Failed; // 1%: raw fallback path.
		const size_t filler = FillerBytes(stamp, frame.width, frame.height);
		packet_.resize(4 * 3 + 8 + filler);
		uint8_t *p = packet_.data();
		std::memcpy(p, &kMagic, 4);
		std::memcpy(p + 4, &frame.width, 4);
		std::memcpy(p + 8, &frame.height, 4);
		std::memcpy(p + 12, &stamp, 8);
		std::memset(p + 20, static_cast<int>(stamp & 0xFF), filler);
		pending_ = true;
		seq_ = seq;
		return storage::SendResult::Ok;
	}
	bool Receive(storage::Packet &out) override
	{
		if (!pending_)
			return false;
		pending_ = false;
		out = storage::Packet{};
		out.data = packet_.data();
		out.bytes = packet_.size();
		out.seq = seq_;
		out.key = true;
		out.codec = storage::CodecId::Mjpeg;
		return true;
	}
	void Drain(uint64_t /*deadlineNs*/, const std::function<void(const storage::Packet &)> &sink) override
	{
		storage::Packet packet;
		if (Receive(packet))
			sink(packet);
	}
	uint32_t InFlight() const override { return pending_ ? 1 : 0; }

private:
	storage::EncoderConfig config_;
	std::mt19937 rng_;
	std::vector<uint8_t> packet_;
	bool pending_ = false;
	uint64_t seq_ = 0;
};

class StampDecoder final : public storage::Decoder {
public:
	explicit StampDecoder(uint32_t seed) : rng_(seed) {}

	storage::CodecId Codec() const override { return storage::CodecId::Mjpeg; }
	const char *Name() const override { return "stamp"; }
	void Flush() override {}

	storage::DecodeStatus Decode(const storage::EncodedFrame &in, bool /*wantImage*/, DecodedImage &out,
				     uint64_t &outSeq) override
	{
		outSeq = in.seq;
		return DecodeBytes(in.data, in.bytes, out) ? storage::DecodeStatus::Frame
							   : storage::DecodeStatus::Failed;
	}

private:
	bool DecodeBytes(const uint8_t *packet, size_t bytes, DecodedImage &out)
	{
		SpinFor(std::uniform_int_distribution<uint32_t>(50, 400)(rng_) + g_decodeExtraUs.load());
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

	std::mt19937 rng_;
};

// The stamp codec's factories, as VideoDelayFilter sets MJPEG's up.
void UseStampCodec(CodecPipeline::Options &options, std::atomic<uint32_t> &seeds)
{
	options.encoderFactory = [&seeds](const storage::EncoderConfig &config) {
		return std::unique_ptr<storage::Encoder>(new StampEncoder(config, seeds++));
	};
	options.decoderFactory = [&seeds](storage::CodecId codec) -> std::unique_ptr<storage::Decoder> {
		if (codec != storage::CodecId::Mjpeg)
			return nullptr;
		return std::unique_ptr<storage::Decoder>(new StampDecoder(seeds++));
	};
}

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
		options.encoderFactory = &storage::MakeRawEncoder;
	else
		UseStampCodec(options, seeds);
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
				const uint32_t slices[3] = {1, 2, 4}; // 4: clamped to kMaxSlicesPerContext.
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
	CHECK(stats.dueFrames > 0 &&
	      static_cast<double>(stats.exact) >= Limits().exactFraction * static_cast<double>(stats.dueFrames));
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

// pause: rendering stops for 300 ms (OBS stalled, the scene hidden): the
// decoder had decoded a few frames ahead of the old target, and those are now
// far older than the frame due. The first frame shown afterwards must be
// (about) the due one, never one of those (QA: one frame ~280 ms older than
// due). !pause: no pause, but a decoder 2.5 captures slow: it can't decode
// every frame, and it jumps to the due one each time -- every frame it does
// decode must still be shown (dropping late frames as stale would freeze the
// delayed view).
void RunPlaybackScenario(bool pause)
{
	testcheck::g_currentTest = pause ? "threaded, 300 ms render pause" : "threaded, decoder slower than capture";
	CodecPipeline::Options options;
	options.threaded = true;
	options.clockNs = &NowNs;
	options.lookaheadFrames = 3;
	std::atomic<uint32_t> seeds{100};
	UseStampCodec(options, seeds);
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
	g_decodeExtraUs = pause ? 0 : static_cast<uint32_t>(periodNs * 5 / 2 / 1000);
	runFor(1500000000ULL, false);
	if (pause)
		std::this_thread::sleep_for(std::chrono::milliseconds(300)); // No Render() at all.
	const uint64_t measureNs = pause ? 100000000ULL : 1000000000ULL;
	runFor(measureNs, true);
	CodecPipeline::WindowStats stats;
	pipeline->TakeWindowStats(stats);
	pipeline->Shutdown();
	g_decodeExtraUs = 0;
	std::printf("  %s: %llu frames shown in the %llu ms measured, worst %.1f ms older than due (%llu stale "
		    "decoded frames dropped)\n",
		    testcheck::g_currentTest, static_cast<unsigned long long>(shownAfter),
		    static_cast<unsigned long long>(measureNs / 1000000), static_cast<double>(worstLagNs) / 1e6,
		    static_cast<unsigned long long>(stats.staleDecoded));
	const TimingLimits &limits = Limits();
	if (pause) {
		CHECK(shownAfter > 0);
		// The due frame, or one at most a couple of captures older (the decoder
		// jumps to the due frame and decodes it while this loop renders on).
		CHECK(worstLagNs <= limits.pauseLagPeriods * periodNs);
	} else {
		// ~10.2 ms per decode: up to ~98 frames a second, each shown late
		// but fresh. Nothing is dropped as stale (a few at most, if this
		// loop itself stalls past 4 captures: that IS a render pause).
		CHECK(shownAfter > 0);
		CHECK(shownAfter >= limits.slowShownMin);
		CHECK(stats.staleDecoded <= limits.slowStaleMax);
		CHECK(worstLagNs <= limits.slowLagPeriods * periodNs);
	}
}

} // namespace

int main(int argc, char **argv)
{
	const uint32_t scale = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 1;
	const TimingLimits &limits = Limits();
	std::printf("pipeline_stress_tests: %s timing limits (exact >= %.0f%%, pause lag <= %llu periods, slow "
		    "decoder: >= %llu shown, <= %llu stale, lag <= %llu periods)\n",
		    OnCi() ? "CI (TRIGGLOW_STRESS_CI)" : "strict", limits.exactFraction * 100.0,
		    static_cast<unsigned long long>(limits.pauseLagPeriods),
		    static_cast<unsigned long long>(limits.slowShownMin),
		    static_cast<unsigned long long>(limits.slowStaleMax),
		    static_cast<unsigned long long>(limits.slowLagPeriods));
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

	RunPlaybackScenario(true);
	RunPlaybackScenario(false);
	TestRollingWindowTags();

	CHECK_EQ(g_tornFrames.load(), 0u);
	CHECK_EQ(g_badPackets.load(), 0u);
	done = true;
	watchdog.join();
	return testcheck::Finish("pipeline_stress_tests");
}
