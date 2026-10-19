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

// Adversarial tests of CodecPipeline with the inter-frame (hardware) storage
// codec path, from the v0.7.0 QA: the fake inter-frame codec of
// tests/fake-gop-codec.hpp posing as a hardware one (a setup thread opens it,
// AMF-like latency: a packet out only once the next frame is in), against
// the same simulated graphics thread as pipeline_gop_stress_tests.cpp. Each
// test aims at one edge: evacuations at random points inside a GOP, packets
// lost after their encode, Disable/Enable and shutdown while a setup is
// blocked, frame sizes flapping, yields (while capturing, during a render
// pause, with a setup's result waiting), half frame rate, render pauses with
// frames held in the encoder, an evacuation plus a slow software decode, and
// an arena too small for the ring.
//
// Correctness checks are strict (never a P-frame decoded without its
// reference, nothing corrupt or younger than the target shown, one thread per
// codec object, never the graphics thread); checks on how many due frames are
// shown exactly are looser with TRIGGLOW_STRESS_CI=1, like the stress tests.

#include "check.hpp"
#include "codec-pipeline.hpp"
#include "fake-gop-codec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

// Shared CI runners don't keep the 4 ms steps: the checks on exact frames get looser there.
bool OnCi()
{
	static const bool onCi = EnvFlag("TRIGGLOW_STRESS_CI");
	return onCi;
}

std::atomic<int> g_liveHw{0};    // Fake "hardware" encoders alive.
std::atomic<int> g_maxLiveHw{0}; // Max seen.
std::atomic<int> g_liveHwDec{0};

void NoteLive(int v)
{
	int prev = g_maxLiveHw.load();
	while (v > prev && !g_maxLiveHw.compare_exchange_weak(prev, v)) {
	}
}

// Wraps a FakeGopEncoder: counts live "hardware" encoders (like HwEncoder's openEncoders),
// can corrupt one packet (unusable size: a packet lost after its encode).
class HwFake final : public storage::Encoder {
public:
	HwFake(std::unique_ptr<fakegop::FakeGopEncoder> inner, std::shared_ptr<storage::InterControl> control)
		: inner_(std::move(inner)),
		  control_(std::move(control))
	{
		NoteLive(++g_liveHw);
		if (control_)
			control_->openEncoders.fetch_add(1);
	}
	~HwFake() override
	{
		--g_liveHw;
		if (control_)
			control_->openEncoders.fetch_sub(1);
	}
	storage::CodecId Codec() const override { return inner_->Codec(); }
	storage::Backend GetBackend() const override { return storage::Backend::Nvenc; }
	const char *Name() const override { return "fake_nvenc"; }
	bool InterFrame() const override { return true; }
	const storage::EncoderConfig &Config() const override { return inner_->Config(); }
	storage::SendResult Send(const I420View &frame, uint64_t seq, bool forceKey) override
	{
		lastForce_ = forceKey;
		if (forceKey)
			++forced_;
		return inner_->Send(frame, seq, forceKey);
	}
	bool Receive(storage::Packet &out) override
	{
		if (!inner_->Receive(out))
			return false;
		Tamper(out);
		return true;
	}
	void Drain(uint64_t deadlineNs, const std::function<void(const storage::Packet &)> &sink) override
	{
		inner_->Drain(deadlineNs, [&](const storage::Packet &p) {
			storage::Packet q = p;
			Tamper(q);
			sink(q);
		});
	}
	uint32_t InFlight() const override { return inner_->InFlight(); }
	bool Broken() const override { return inner_->Broken(); }

	std::atomic<int64_t> corruptNext{-1}; // Make the next packet (any seq) >= this counter unusable.
	std::atomic<uint64_t> lost{0};
	std::atomic<uint64_t> forced_{0};
	bool lastForce_ = false;

private:
	void Tamper(storage::Packet &out)
	{
		if (corruptNext.load() == 1) {
			corruptNext = -1;
			out.bytes = 1u << 30; // Past the raw size: unusable -> lost after its encode.
			++lost;
		}
	}
	std::unique_ptr<fakegop::FakeGopEncoder> inner_;
	std::shared_ptr<storage::InterControl> control_;
};

class CountingDecoder final : public storage::Decoder {
public:
	explicit CountingDecoder(std::unique_ptr<storage::Decoder> inner) : inner_(std::move(inner)) { ++g_liveHwDec; }
	~CountingDecoder() override { --g_liveHwDec; }
	storage::CodecId Codec() const override { return inner_->Codec(); }
	const char *Name() const override { return "fake-hwdec"; }
	storage::DecodeStatus Decode(const storage::EncodedFrame &in, bool wantImage, DecodedImage &out,
				     uint64_t &outSeq) override
	{
		return inner_->Decode(in, wantImage, out, outSeq);
	}
	void Flush() override { inner_->Flush(); }
	bool Hardware() const override { return true; }
	bool Broken() const override { return inner_->Broken(); }

private:
	std::unique_ptr<storage::Decoder> inner_;
};

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

struct HwConfig {
	uint32_t gop = 30;
	uint32_t latencyFrames = 1; // AMF-like: a packet out only after the next Send().
	uint64_t latencyNs = 0;
	uint32_t setupMinMs = 0;
	uint32_t setupMaxMs = 0;
	bool setupBlocksOnGate = false;
	uint32_t stallAfter = 0;
};

struct Harness {
	uint64_t periodNs = 4000000;  // 250 fps
	uint64_t delayNs = 400000000; // 400 ms = 100 frames
	uint32_t width = 64, height = 32;
	bool tight = false;
	fakegop::Counters counters;
	std::shared_ptr<storage::InterControl> control = std::make_shared<storage::InterControl>();
	HwConfig hw;
	std::atomic<uint32_t> setups{0};
	std::mutex gateMutex;
	std::condition_variable gateCv;
	bool gateOpen = true;
	std::mutex lastMutex;
	HwFake *lastHw = nullptr; // The newest "hardware" encoder (for tampering); only valid while alive.
	std::unique_ptr<CodecPipeline> pipeline;
	std::shared_ptr<FrameRing> ring;
	uint64_t captureIntervalNs = 4000000;
	struct Pending {
		FrameRing::Reservation reservation;
		uint32_t width, height;
	};
	std::deque<Pending> staging;
	uint64_t lastShown = 0, lastDue = 0, lastCell = 0;
	// Stats
	uint64_t displayed = 0, younger = 0, notIncreasing = 0, corruptShown = 0, stampMismatch = 0;
	uint64_t dueFrames = 0, exact = 0;
	std::vector<uint32_t> callNs;
	// Per-due record: captureNs -> shown exactly?
	struct DueRec {
		uint64_t captureNs;
		uint64_t atNs;
		bool exact;
	};
	std::vector<DueRec> dues;

	size_t Slots() const { return static_cast<size_t>(delayNs / periodNs) + hw.gop + 16; }

	std::shared_ptr<FrameRing> MakeRing()
	{
		FrameArena::Config arena;
		arena.blockBytes = 4 * 1024;
		arena.maxSpareBlocks = 1;
		if (tight)
			arena.limitBytes = Slots() * 512 * 3 / 4;
		return std::make_shared<FrameRing>(Slots(), arena);
	}

	void Build(bool hardware)
	{
		counters.graphicsThread = std::this_thread::get_id();
		CodecPipeline::Options options;
		options.threaded = true;
		options.clockNs = &NowNs;
		options.lookaheadFrames = 3;
		options.interControl = control;
		if (hardware) {
			// Behaves like hwstorage::Provider::Setup() as far as the pipeline
			// can tell: the encoder counts as open from before its "open" (the
			// setupMinMs-setupMaxMs sleep), and `cancelled` is asked before
			// and after it, and while blocked on the gate; what was opened is
			// closed right here and the session comes back cancelled.
			options.interSetup = [this](const storage::EncoderConfig &requested,
						    const storage::SetupCancelled &cancelled) -> storage::InterSession {
				const uint32_t n = setups++;
				std::mt19937 local(n + 99);
				storage::InterSession stopped;
				stopped.cancelled = true;
				if (cancelled())
					return stopped;
				if (hw.setupBlocksOnGate) {
					std::unique_lock<std::mutex> lock(gateMutex);
					while (!gateCv.wait_for(lock, std::chrono::milliseconds(2),
								[&] { return gateOpen; })) {
						if (cancelled())
							return stopped;
					}
				}
				fakegop::EncoderOptions o;
				o.inter = true;
				o.codec = storage::CodecId::Hevc;
				o.gop = hw.gop;
				o.latencyFrames = hw.latencyFrames;
				o.latencyNs = hw.latencyNs;
				o.fillerBytes = 380;
				o.seed = n + 1000;
				o.clockNs = &NowNs;
				o.counters = &counters;
				o.handover = true;
				o.stallAfter = hw.stallAfter;
				storage::EncoderConfig config = requested;
				config.keyframeInterval = o.gop;
				auto inner = std::make_unique<fakegop::FakeGopEncoder>(config, o);
				auto enc = std::make_unique<HwFake>(std::move(inner), control); // Counted from here...
				if (hw.setupMaxMs > 0)                                          // ...through the open.
					std::this_thread::sleep_for(std::chrono::milliseconds(
						hw.setupMinMs + local() % (hw.setupMaxMs - hw.setupMinMs + 1)));
				if (cancelled())
					return stopped; // `enc` closes here, on the setup thread.
				{
					std::lock_guard<std::mutex> lock(lastMutex);
					lastHw = enc.get();
				}
				storage::InterSession session;
				session.encoder = std::move(enc);
				session.decoder = std::make_unique<CountingDecoder>(
					std::make_unique<fakegop::FakeGopDecoder>(storage::CodecId::Hevc, &counters,
										  true, true));
				return session;
			};
			options.softwareDecoderFactory =
				[this](storage::CodecId codec) -> std::unique_ptr<storage::Decoder> {
				return std::make_unique<fakegop::FakeGopDecoder>(codec, &counters);
			};
		}
		options.encoderFactory =
			[this](const storage::EncoderConfig &config) -> std::unique_ptr<storage::Encoder> {
			fakegop::EncoderOptions o;
			o.inter = false; // The MJPEG bridge / fallback.
			o.codec = storage::CodecId::Mjpeg;
			o.fillerBytes = 380;
			o.clockNs = &NowNs;
			o.counters = &counters;
			return std::make_unique<fakegop::FakeGopEncoder>(config, o);
		};
		options.decoderFactory = [this](storage::CodecId codec) -> std::unique_ptr<storage::Decoder> {
			return std::make_unique<fakegop::FakeGopDecoder>(codec, &counters, false,
									 codec == storage::CodecId::Hevc);
		};
		pipeline = std::make_unique<CodecPipeline>(options);
		pipeline->SetEncodeParallelism(1, 1);
		if (hardware)
			pipeline->SetStorageTarget(true);
		ring = MakeRing();
		pipeline->Attach(ring, 64, 32, captureIntervalNs);
	}

	template<typename F> void Timed(F &&f)
	{
		const uint64_t t0 = NowNs();
		f();
		callNs.push_back(static_cast<uint32_t>(std::min<uint64_t>(NowNs() - t0, UINT32_MAX)));
	}

	void Reset(bool disable)
	{
		Timed([&] { pipeline->Detach(disable, std::move(ring)); });
		ring = MakeRing();
		staging.clear();
		lastShown = 0;
		lastDue = 0;
		Timed([&] { pipeline->Attach(ring, 64, 32, captureIntervalNs); });
	}

	// One G tick; returns after sleeping until the next period.
	void Tick(uint64_t &nextTick, bool capture = true)
	{
		const uint64_t now = NowNs();
		const uint64_t cell = now / captureIntervalNs;
		if (capture && cell != lastCell) {
			lastCell = cell;
			FrameRing::Reservation reservation;
			bool reserved = false;
			Timed([&] { reserved = ring->Reserve(now, reservation); });
			if (reserved)
				staging.push_back({reservation, width, height});
			while (staging.size() > 2) {
				const Pending p = staging.front();
				staging.pop_front();
				RawFrame *frame = nullptr;
				Timed([&] { frame = pipeline->AcquireRawFrame(p.width, p.height); });
				if (!frame) {
					ring->MarkDropped(p.reservation.index, p.reservation.generation);
					pipeline->NoteBackpressureDrop();
					continue;
				}
				FillStamped(*frame, p.reservation.captureNs);
				Timed([&] { pipeline->SubmitEncode(ring, frame, p.reservation); });
			}
		}
		uint64_t oldest = 0;
		if (capture && ring->OldestCaptureNs(oldest) && now > delayNs) {
			uint64_t target = now - delayNs;
			bool play = oldest <= target;
			if (!play && oldest <= target + 2 * captureIntervalNs) {
				target = oldest;
				play = true;
			} else if (!play && oldest <= target + delayNs / 4) {
				Timed([&] { pipeline->PrimeDecode(oldest); });
			}
			if (play) {
				DecodedLease lease;
				bool got = false;
				Timed([&] { got = pipeline->AcquireForDisplay(target, lastShown, lease); });
				if (got) {
					++displayed;
					const uint64_t stamp = fakegop::ReadStamp(lease.image.plane[0]);
					if (lease.captureNs > target)
						++younger;
					if (lease.captureNs <= lastShown)
						++notIncreasing;
					if ((stamp ^ fakegop::kCorruptMask) == lease.captureNs)
						++corruptShown;
					else if (stamp != lease.captureNs)
						++stampMismatch;
					lastShown = lease.captureNs;
					pipeline->ReleaseDisplay(lease);
				}
				const FrameRing::DueSlot due = ring->Due(target);
				if (due.exists && due.captureNs != lastDue && due.state != SlotState::Dropped) {
					lastDue = due.captureNs;
					++dueFrames;
					const bool ex = lastShown == due.captureNs;
					if (ex)
						++exact;
					dues.push_back({due.captureNs, now, ex});
				}
			}
		}
		nextTick += periodNs;
		if (NowNs() > nextTick)
			nextTick = NowNs();
		while (NowNs() < nextTick)
			std::this_thread::yield();
	}

	void RunFor(uint64_t ns, bool capture = true)
	{
		const uint64_t end = NowNs() + ns;
		uint64_t nextTick = NowNs();
		while (NowNs() < end)
			Tick(nextTick, capture);
	}

	bool WaitInter(uint64_t timeoutNs)
	{
		const uint64_t end = NowNs() + timeoutNs;
		uint64_t nextTick = NowNs();
		while (NowNs() < end) {
			if (pipeline->GetCodecStatus().interActive)
				return true;
			Tick(nextTick);
		}
		return pipeline->GetCodecStatus().interActive;
	}

	void CheckCommon(const char *name)
	{
		std::printf(
			"  [%s] displayed %llu due %llu exact %.1f%% corruptDecodes %llu corruptShown %llu younger %llu "
			"notInc %llu stampMismatch %llu threadViol %llu badPackets %llu\n",
			name, (unsigned long long)displayed, (unsigned long long)dueFrames,
			dueFrames ? 100.0 * exact / dueFrames : 0.0, (unsigned long long)counters.corruptDecodes.load(),
			(unsigned long long)corruptShown, (unsigned long long)younger,
			(unsigned long long)notIncreasing, (unsigned long long)stampMismatch,
			(unsigned long long)counters.threadViolations.load(),
			(unsigned long long)counters.badPackets.load());
		CHECK_EQ(counters.corruptDecodes.load(), 0u);
		CHECK_EQ(corruptShown, 0u);
		CHECK_EQ(younger, 0u);
		CHECK_EQ(notIncreasing, 0u);
		CHECK_EQ(stampMismatch, 0u);
		CHECK_EQ(counters.threadViolations.load(), 0u);
		CHECK_EQ(counters.badPackets.load(), 0u);
		CHECK_EQ(counters.wrongSize.load(), 0u);
	}

	// Non-exact due frames with atNs in [from, to).
	uint64_t MissesBetween(uint64_t from, uint64_t to) const
	{
		uint64_t n = 0;
		for (const DueRec &d : dues)
			if (d.atNs >= from && d.atNs < to && !d.exact)
				++n;
		return n;
	}
	uint64_t DuesBetween(uint64_t from, uint64_t to) const
	{
		uint64_t n = 0;
		for (const DueRec &d : dues)
			if (d.atNs >= from && d.atNs < to)
				++n;
		return n;
	}

	void Finish()
	{
		pipeline->Shutdown();
		ring.reset();
		pipeline.reset();
	}
};

uint64_t P999(std::vector<uint32_t> v)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, v.size() * 999 / 1000)];
}

// T1: evacuation (inter -> intra) at random points inside a GOP, 8 times; the delayed
// view keeps its exact frame across the switch (the drain hands over what is in flight).
void TestEvacuationMidGop()
{
	testcheck::g_currentTest = "T1 evacuation mid-GOP";
	Harness h;
	h.hw.gop = 30;
	h.hw.latencyFrames = 1;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(h.delayNs + 300000000ULL);
	std::mt19937 rng(5);
	uint64_t worstMisses = 0, totalMisses = 0, totalDues = 0;
	for (int i = 0; i < 8; ++i) {
		// A random offset inside the GOP.
		h.RunFor((rng() % 30) * h.periodNs);
		const uint64_t evacAt = NowNs();
		h.pipeline->SetStorageTarget(false);
		h.RunFor(h.delayNs + 200000000ULL); // The switch reaches the delayed view.
		// Window around when the switch point is due on screen.
		const uint64_t dueAt = evacAt + h.delayNs;
		const uint64_t m = h.MissesBetween(dueAt - 60000000ULL, dueAt + 100000000ULL);
		totalMisses += m;
		totalDues += h.DuesBetween(dueAt - 60000000ULL, dueAt + 100000000ULL);
		worstMisses = std::max(worstMisses, m);
		h.pipeline->SetStorageTarget(true); // Back (a new Enable would do this).
		CHECK(h.WaitInter(2000000000ULL));
		h.RunFor(h.delayNs + 300000000ULL);
	}
	const CodecPipeline::CodecStatus st = h.pipeline->GetCodecStatus();
	std::printf("  [T1] evacuations 8: due frames around the switch %llu, not exact %llu (worst %llu per switch); "
		    "adoptions %llu hardErrors %llu\n",
		    (unsigned long long)totalDues, (unsigned long long)totalMisses, (unsigned long long)worstMisses,
		    (unsigned long long)st.adoptions, (unsigned long long)st.hardErrors);
	CHECK(worstMisses <= (OnCi() ? 30u : 3u)); // At most 3 frames not shown exactly per evacuation.
	CHECK_EQ(st.hardErrors, 0u);
	h.CheckCommon("T1");
	h.Finish();
	CHECK_EQ(g_liveHw.load(), 0);
}

// T2: a packet lost after its encode, mid-GOP (unusable bytes); the next Send forces a
// key, the orphan P-frames are never stored, the view holds a bounded number of frames.
void TestPacketLoss()
{
	testcheck::g_currentTest = "T2 packet loss";
	Harness h;
	h.hw.gop = 30;
	h.hw.latencyFrames = 2; // Two frames in flight when the loss is noticed.
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(h.delayNs + 300000000ULL);
	std::mt19937 rng(9);
	uint64_t worst = 0;
	uint64_t lostTotal = 0;
	uint64_t forcedBefore = 0, forcedAfter = 0;
	for (int i = 0; i < 8; ++i) {
		h.RunFor((5 + rng() % 20) * h.periodNs);
		HwFake *enc = nullptr;
		{
			std::lock_guard<std::mutex> lock(h.lastMutex);
			enc = h.lastHw;
		}
		forcedBefore = h.counters.forcedKeys.load();
		const uint64_t lossAt = NowNs();
		enc->corruptNext = 1;
		h.RunFor(h.delayNs + 200000000ULL);
		forcedAfter = h.counters.forcedKeys.load();
		CHECK(forcedAfter > forcedBefore); // The next Send() was a forced key.
		const uint64_t dueAt = lossAt + h.delayNs;
		const uint64_t m = h.MissesBetween(dueAt - 40000000ULL, dueAt + 100000000ULL);
		worst = std::max(worst, m);
		lostTotal += enc->lost.load();
	}
	CodecPipeline::WindowStats w;
	h.pipeline->TakeWindowStats(w);
	std::printf(
		"  [T2] losses %llu: worst not-exact due frames around a loss %llu (in flight 2 -> expect <= 2+1+1)\n",
		(unsigned long long)lostTotal, (unsigned long long)worst);
	CHECK(lostTotal >= 8);
	CHECK(worst <= (OnCi() ? 30u : 5u));
	h.CheckCommon("T2");
	h.Finish();
}

// T3: Disable/Enable (and hard resizes) while the setup thread is blocked opening an encoder;
// then Shutdown while a setup is still blocked. No deadlock, no stale encoder adopted for
// a dead cycle, every fake hardware encoder closed in the end, adoption still happens.
void TestDisableDuringSetup()
{
	testcheck::g_currentTest = "T3 disable during setup";
	Harness h;
	h.hw.setupBlocksOnGate = true;
	{
		std::lock_guard<std::mutex> lock(h.gateMutex);
		h.gateOpen = false;
	}
	h.Build(true);
	h.RunFor(100000000ULL); // A setup is requested and blocks.
	for (int i = 0; i < 10; ++i) {
		h.Reset(i % 2 == 0); // Disable/Enable or a hard resize while it's blocked.
		if (i % 2 == 0)
			h.pipeline->SetStorageTarget(true); // A new Enable re-targets hardware.
		h.RunFor(30000000ULL);
	}
	CHECK(!h.pipeline->GetCodecStatus().interActive);
	{
		std::lock_guard<std::mutex> lock(h.gateMutex);
		h.gateOpen = true;
	}
	h.gateCv.notify_all();
	CHECK(h.WaitInter(3000000000ULL)); // It still gets there.
	h.RunFor(h.delayNs + 200000000ULL);
	// Now block again and shut down mid-setup: the setup is cancelled, Shutdown
	// doesn't wait for the gate (opened 300 ms later).
	{
		std::lock_guard<std::mutex> lock(h.gateMutex);
		h.gateOpen = false;
	}
	h.width = 48;
	h.height = 24; // A new size: a new setup, which blocks.
	h.RunFor(100000000ULL);
	std::thread opener([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		{
			std::lock_guard<std::mutex> lock(h.gateMutex);
			h.gateOpen = true;
		}
		h.gateCv.notify_all();
	});
	const uint64_t t0 = NowNs();
	h.CheckCommon("T3");
	h.Finish();
	const double shutdownMs = (NowNs() - t0) / 1e6;
	opener.join();
	std::printf(
		"  [T3] setups %u, shutdown with a blocked setup took %.0f ms, live hw encoders after %d, max live %d, "
		"live hw decoders %d\n",
		h.setups.load(), shutdownMs, g_liveHw.load(), g_maxLiveHw.load(), g_liveHwDec.load());
	CHECK(shutdownMs < (OnCi() ? 280.0 : 200.0));
	CHECK_EQ(g_liveHw.load(), 0);
	CHECK_EQ(g_liveHwDec.load(), 0);
}

// T4: frame size changes in a loop (rung flapping): new GOP each time, bridge meanwhile.
void TestResolutionLoop()
{
	testcheck::g_currentTest = "T4 resolution loop";
	g_maxLiveHw = 0;
	Harness h;
	h.hw.setupMinMs = 0;
	h.hw.setupMaxMs = 30;
	h.hw.gop = 12;
	h.hw.latencyFrames = 1;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	std::mt19937 rng(17);
	const uint32_t sizes[3][2] = {{64, 32}, {48, 24}, {32, 16}};
	const uint64_t end = NowNs() + 4000000000ULL;
	uint64_t nextTick = NowNs();
	int changes = 0;
	while (NowNs() < end) {
		if (rng() % 6 == 0) {
			const auto s = rng() % 3;
			h.width = sizes[s][0];
			h.height = sizes[s][1];
			++changes;
		}
		h.Tick(nextTick);
	}
	h.width = 64;
	h.height = 32;
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(h.delayNs + 300000000ULL);
	const CodecPipeline::CodecStatus st = h.pipeline->GetCodecStatus();
	std::printf("  [T4] size changes %d, setups %u, adoptions %llu, max live hw encoders %d, G p99.9 %.3f ms\n",
		    changes, h.setups.load(), (unsigned long long)st.adoptions, g_maxLiveHw.load(),
		    P999(h.callNs) / 1e6);
	CHECK(g_maxLiveHw.load() <= 3);
	h.CheckCommon("T4");
	h.Finish();
	CHECK_EQ(g_liveHw.load(), 0);
}

// T5: a yield from "the UI" (like YieldHwEncoderProc: set yielded, Wake, wait up to 100 ms
// for openEncoders == 0): while frames flow, during a render pause, and with a setup
// result waiting in the mailbox during a render pause.
void TestYieldLatency()
{
	testcheck::g_currentTest = "T5 yield";
	Harness h;
	h.hw.latencyFrames = 1;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(300000000ULL);
	auto yieldWait = [&](const char *what) {
		const uint64_t t0 = NowNs();
		h.control->yielded = true;
		h.control->Wake();
		while (h.control->openEncoders.load() > 0 && NowNs() - t0 < 100000000ULL)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		const double ms = (NowNs() - t0) / 1e6;
		std::printf("  [T5] yield %s: open encoders %d after %.1f ms\n", what, h.control->openEncoders.load(),
			    ms);
		return h.control->openEncoders.load();
	};
	// (a) frames flowing: G keeps ticking on another thread? No: G is this thread; run the UI wait on a thread.
	{
		std::atomic<int> left{-1};
		std::thread ui([&] { left = yieldWait("while capturing"); });
		h.RunFor(200000000ULL);
		ui.join();
		CHECK_EQ(left.load(), 0);
		h.control->yielded = false;
		h.control->Wake();
		CHECK(h.WaitInter(2000000000ULL));
		h.RunFor(300000000ULL);
	}
	// (b) render paused (no captures at all), the encoder open.
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const int left = yieldWait("during a render pause");
		CHECK_EQ(left, 0);
		h.control->yielded = false;
		h.control->Wake();
		CHECK(h.WaitInter(2000000000ULL));
		h.RunFor(300000000ULL);
	}
	// (c) a setup result in the mailbox (not adopted yet) when rendering pauses.
	{
		h.hw.setupMinMs = 60;
		h.hw.setupMaxMs = 60;
		h.width = 48;
		h.height = 24; // New size: the encoder retires (closed on the reaper), a setup starts.
		h.RunFor(20000000ULL);
		// Pause now: the setup finishes ~40 ms later into the mailbox, no job adopts it.
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		const int left = yieldWait("render paused, setup result waiting in the mailbox");
		CHECK_EQ(left, 0); // Worker 0 closes it: no job comes to adopt or drop it.
		std::printf("  [T5c] -> %s\n", left == 0 ? "closed" : "STILL OPEN after 100 ms (session held)");
		h.control->yielded = false;
		h.control->Wake();
		h.RunFor(300000000ULL);
	}
	h.CheckCommon("T5");
	h.Finish();
	CHECK_EQ(g_liveHw.load(), 0);
}

// T6: half frame rate toggles (capture interval x2) with the inter encoder on, plus pauses.
void TestHalfRate()
{
	testcheck::g_currentTest = "T6 half rate";
	Harness h;
	h.hw.gop = 30;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	std::mt19937 rng(23);
	for (int i = 0; i < 12; ++i) {
		h.captureIntervalNs = h.captureIntervalNs == h.periodNs ? 2 * h.periodNs : h.periodNs;
		h.pipeline->SetCaptureInterval(h.captureIntervalNs);
		h.RunFor(150000000ULL + (rng() % 200) * 1000000ULL);
		if (i % 3 == 1)
			std::this_thread::sleep_for(std::chrono::milliseconds(150 + rng() % 200)); // Render pause.
	}
	h.captureIntervalNs = h.periodNs;
	h.pipeline->SetCaptureInterval(h.captureIntervalNs);
	h.RunFor(h.delayNs + 300000000ULL);
	CodecPipeline::WindowStats w;
	h.pipeline->TakeWindowStats(w);
	std::printf("  [T6] catch-ups %llu (frames %llu, max %.1f ms) noKeyWaits %llu\n",
		    (unsigned long long)w.catchUps, (unsigned long long)w.catchUpFrames, w.catchUpMaxNs / 1e6,
		    (unsigned long long)w.noKeyWaits);
	h.CheckCommon("T6");
	CHECK_EQ(h.pipeline->GetCodecStatus().hardErrors, 0u);
	h.Finish();
}

// T7: stall detection vs. render pauses. AMF hands a frame's packet out only once the next
// frame is in. (a) 1 frame in flight + 700 ms pause, (b) 2 frames held (a busy engine) +
// 700 ms pause: neither is a stall (before, (b) failed the encoder, and the policy then
// evacuated and blacklisted it). (c) a real stall while frames keep coming still fails it.
void TestStallVsPause()
{
	for (uint32_t held : {1u, 2u}) {
		testcheck::g_currentTest = held == 1 ? "T7a pause, 1 in flight" : "T7b pause, 2 in flight";
		Harness h;
		h.hw.latencyFrames = held;
		h.Build(true);
		CHECK(h.WaitInter(2000000000ULL));
		h.RunFor(h.delayNs + 200000000ULL);
		for (int i = 0; i < 3; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(700)); // No Render() at all.
			h.RunFor(500000000ULL);
		}
		const CodecPipeline::CodecStatus st = h.pipeline->GetCodecStatus();
		std::printf("  [T7] %u held + 3 pauses of 700 ms: hardErrors %llu, interActive %d, lastError '%s'\n",
			    held, (unsigned long long)st.hardErrors, st.interActive ? 1 : 0, st.lastError);
		CHECK_EQ(st.hardErrors, 0u); // Frames held over a render pause are no stall.
		CHECK(st.interActive);
		h.CheckCommon(held == 1 ? "T7a" : "T7b");
		h.Finish();
	}
	// T7c: a real stall -- no packet any more while frames keep coming -- still fails the encoder.
	testcheck::g_currentTest = "T7c stall while capturing";
	Harness h;
	h.hw.latencyFrames = 1;
	h.hw.stallAfter = 150;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(1500000000ULL); // 150 frames (0.6 s), then 500 ms of frames without a packet.
	const CodecPipeline::CodecStatus st = h.pipeline->GetCodecStatus();
	std::printf("  [T7c] stalled encoder: hardErrors %llu, interActive %d, lastError '%s'\n",
		    static_cast<unsigned long long>(st.hardErrors), st.interActive ? 1 : 0, st.lastError);
	CHECK_EQ(st.hardErrors, 1u);
	CHECK(!st.interActive);
	CHECK(std::strstr(st.lastError, "stalled") != nullptr);
	h.CheckCommon("T7c");
	h.Finish();
}

// T8: evacuation + software decode switch at once (AMD/Intel decode shares the engine),
// software decoder slower than real time for a while.
void TestEvacuateSoftwareDecode()
{
	testcheck::g_currentTest = "T8 evacuate + sw decode";
	Harness h;
	h.hw.gop = 30;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(h.delayNs + 300000000ULL);
	const uint64_t at = NowNs();
	h.pipeline->SetStorageTarget(false);
	h.pipeline->SetSoftwareDecode(true);
	h.counters.decodeExtraUs = static_cast<uint32_t>(h.periodNs * 3 / 2 / 1000); // 1.5x slower than real time.
	h.RunFor(h.delayNs / 2);
	h.counters.decodeExtraUs = 0;
	h.RunFor(h.delayNs);
	const uint64_t m = h.MissesBetween(at, at + 2 * h.delayNs);
	const uint64_t d = h.DuesBetween(at, at + 2 * h.delayNs);
	CodecPipeline::WindowStats w;
	h.pipeline->TakeWindowStats(w);
	std::printf("  [T8] due %llu not exact %llu; catch-ups %llu noKeyWaits %llu noKeySkips %llu decFail %llu\n",
		    (unsigned long long)d, (unsigned long long)m, (unsigned long long)w.catchUps,
		    (unsigned long long)w.noKeyWaits, (unsigned long long)w.noKeySkips,
		    (unsigned long long)w.decodeFailures);
	h.CheckCommon("T8");
	h.Finish();
}

// T9: tight arena (RAM clip) with a long GOP: ring reuse with partial GOPs and repeated
// losses; never an orphan P shown, frames keep coming.
void TestTightArena()
{
	testcheck::g_currentTest = "T9 tight arena";
	Harness h;
	h.hw.gop = 30;
	h.tight = true;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(3000000000ULL);
	CodecPipeline::WindowStats w;
	h.pipeline->TakeWindowStats(w);
	std::printf("  [T9] displayed %llu of %llu due; chainBreaks %llu orphan %llu arenaDrops %llu forcedKeys %llu\n",
		    (unsigned long long)h.displayed, (unsigned long long)h.dueFrames, (unsigned long long)w.chainBreaks,
		    (unsigned long long)w.orphanDrops, (unsigned long long)w.arenaDrops,
		    (unsigned long long)h.counters.forcedKeys.load());
	CHECK(h.displayed * 4 >= h.dueFrames);
	h.CheckCommon("T9");
	h.Finish();
}

// T10: the hardware decoder fails mid-run (a lost device): counted once in
// the codec status -- what makes the codec policy take the encoder off the
// hardware too -- and the decoding goes on in software, nothing corrupt shown.
void TestHardwareDecoderFailure()
{
	testcheck::g_currentTest = "T10 hardware decoder failure";
	Harness h;
	h.hw.gop = 30;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(h.delayNs + 300000000ULL);
	CHECK_EQ(h.pipeline->GetCodecStatus().decoderFailures, 0u);
	h.counters.breakHardwareDecoders = true;
	h.RunFor(100000000ULL);
	const uint64_t shownBefore = h.displayed;
	h.RunFor(h.delayNs + 300000000ULL);
	const CodecPipeline::CodecStatus st = h.pipeline->GetCodecStatus();
	std::printf("  [T10] decoder failures %llu, software decode %d, shown after the failure %llu, lastError '%s'\n",
		    static_cast<unsigned long long>(st.decoderFailures), st.softwareDecode ? 1 : 0,
		    static_cast<unsigned long long>(h.displayed - shownBefore), st.lastError);
	CHECK_EQ(st.decoderFailures, 1u); // Once: the decoders after it are software ones.
	CHECK(st.softwareDecode);
	CHECK(!st.hardwareDecode);
	CHECK(h.displayed > shownBefore + 10); // Frames keep coming, decoded in software.
	CHECK_EQ(st.hardErrors, 0u);           // The encoder itself is fine...
	CHECK(st.interActive);                 // ...and it's the policy's call to evacuate it.
	h.CheckCommon("T10");
	h.Finish();
}

// T11: a yield while the setup thread is opening an encoder (an open of 150
// ms): the encoder counts from before its open, so the yield sees it, and the
// setup closes it the moment its open returns -- never handed over, and no
// setup failure (which would keep the hardware off for the cycle). Once the
// yield is over, the hardware comes back.
void TestYieldDuringSetupOpen()
{
	testcheck::g_currentTest = "T11 yield during a setup's open";
	Harness h;
	h.hw.latencyFrames = 1;
	h.Build(true);
	CHECK(h.WaitInter(2000000000ULL));
	h.RunFor(300000000ULL);
	h.hw.setupMinMs = 150;
	h.hw.setupMaxMs = 150;
	const uint32_t setupsBefore = h.setups.load();
	h.width = 48;
	h.height = 24; // A new size: the old encoder retires, a setup starts opening the next one.
	h.RunFor(60000000ULL);
	CHECK(h.setups.load() > setupsBefore);
	CHECK(h.control->openEncoders.load() >= 1); // The encoder being opened counts already.
	std::atomic<int64_t> closedUs{-1};
	std::thread ui([&] {
		const uint64_t t0 = NowNs();
		h.control->yielded = true;
		h.control->Wake();
		while (h.control->openEncoders.load() > 0 && NowNs() - t0 < 1000000000ULL)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		if (h.control->openEncoders.load() == 0)
			closedUs = static_cast<int64_t>((NowNs() - t0) / 1000);
	});
	h.RunFor(400000000ULL); // G keeps rendering through the yield.
	ui.join();
	const CodecPipeline::CodecStatus st = h.pipeline->GetCodecStatus();
	std::printf("  [T11] yield during a 150 ms open: no session left after %.1f ms, interActive %d, setup failures "
		    "%llu\n",
		    static_cast<double>(closedUs.load()) / 1000.0, st.interActive ? 1 : 0,
		    static_cast<unsigned long long>(st.setupFailures));
	CHECK(closedUs.load() >= 0);                         // Closed...
	CHECK(closedUs.load() < (OnCi() ? 400000 : 150000)); // ...right after its open (~90 ms left).
	CHECK(!st.interActive);                              // Never adopted while yielded.
	CHECK_EQ(st.setupFailures, 0u);                      // A cancelled setup is no failure.
	CHECK_EQ(st.hardErrors, 0u);
	h.hw.setupMinMs = 0;
	h.hw.setupMaxMs = 0;
	h.control->yielded = false;
	h.control->Wake();
	CHECK(h.WaitInter(2000000000ULL)); // Set up again at the new size.
	h.RunFor(h.delayNs + 200000000ULL);
	h.CheckCommon("T11");
	h.Finish();
	CHECK_EQ(g_liveHw.load(), 0);
}

} // namespace

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : nullptr;
	std::atomic<bool> done{false};
	std::thread watchdog([&] {
		for (int i = 0; i < 3000 && !done; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!done) {
			std::printf("adversarial: watchdog fired (deadlock?)\n");
			std::fflush(stdout);
			std::abort();
		}
	});
	auto run = [&](const char *id, void (*fn)()) {
		if (!only || std::strcmp(only, id) == 0) {
			std::printf("%s\n", id);
			std::fflush(stdout);
			fn();
		}
	};
	run("T1", &TestEvacuationMidGop);
	run("T2", &TestPacketLoss);
	run("T3", &TestDisableDuringSetup);
	run("T4", &TestResolutionLoop);
	run("T5", &TestYieldLatency);
	run("T6", &TestHalfRate);
	run("T7", &TestStallVsPause);
	run("T8", &TestEvacuateSoftwareDecode);
	run("T9", &TestTightArena);
	run("T10", &TestHardwareDecoderFailure);
	run("T11", &TestYieldDuringSetupOpen);
	done = true;
	watchdog.join();
	return testcheck::Finish("adversarial_gop_tests");
}
