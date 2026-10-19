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

// Unit tests for trigglow::degradation (src/degradation-policy.hpp): the
// ladder, the initial parallelism, every down trigger, the cooldown, the 30 s
// hysteresis going up, the recovery order and the absence of oscillation;
// since v0.7.0 the storage codec policy's hooks.

#include "check.hpp"
#include "degradation-policy.hpp"

#include <vector>

using namespace trigglow::degradation;

namespace {

constexpr uint64_t kSecond = 1000000000ULL;
constexpr uint64_t kInterval60 = 16666667ULL;
constexpr double kIntervalMs = 16.666667;

// Healthy defaults: plenty of samples, everything far under every threshold.
Sample Healthy(uint64_t nowNs)
{
	Sample s;
	s.nowNs = nowNs;
	s.frameIntervalNs = kInterval60;
	s.graphicsSamples = 120;
	s.graphicsP95Ms = 0.5;
	s.graphicsShortSamples = 30;
	s.graphicsShortP50Ms = 0.4;
	s.encodeSamples = 120;
	s.encodeP95Ms = 4.0;
	s.decodeSamples = 120;
	s.decodeP95Ms = 3.0;
	s.captures = 120;
	s.dueFrames = 120;
	return s;
}

// Runs `policy` from t0 to t1 in 250 ms steps with sample(t); returns the decisions taken.
template<typename SampleFn> std::vector<Decision> Run(Policy &policy, uint64_t t0, uint64_t t1, SampleFn sample)
{
	std::vector<Decision> out;
	for (uint64_t t = t0; t <= t1; t += kSecond / 4) {
		const Decision d = policy.Evaluate(sample(t));
		if (d.action != Action::None)
			out.push_back(d);
	}
	return out;
}

Policy Threaded1080(size_t initial = 0)
{
	return Policy(BuildLadder(1920, 1080, 720, true), BuildParallelismSteps(true), initial, true);
}

void TestLadder()
{
	testcheck::g_currentTest = "ladder";
	std::vector<Rung> l = BuildLadder(1920, 1080, 720, true);
	CHECK_EQ(l.size(), 5u);
	CHECK(l[0].width == 1920 && l[0].height == 1080 && l[0].captureDivisor == 1);
	// Widths: the nearest multiple of 8 (1593.6 -> 1592, 1322.7 -> 1320), so
	// every plane row is whole 4-byte units on OpenGL.
	CHECK(l[1].width == 1592 && l[1].height == 896);
	CHECK(l[2].width == 1320 && l[2].height == 744);
	CHECK(l[3].width == 1280 && l[3].height == 720 && l[3].captureDivisor == 1);
	CHECK(l[4].width == 1280 && l[4].height == 720 && l[4].captureDivisor == 2);
	for (size_t i = 0; i < l.size(); ++i)
		CHECK(l[i].width % 8 == 0 && l[i].height % 2 == 0 && l[i].height >= 720);

	l = BuildLadder(2560, 1440, 720, true);
	CHECK_EQ(l.size(), 6u);
	CHECK(l[4].height == 720 && l[5].captureDivisor == 2);

	// Floor at (or above) the full size: only the frame-rate rung.
	l = BuildLadder(1280, 720, 720, true);
	CHECK_EQ(l.size(), 2u);
	CHECK(l[1].height == 720 && l[1].captureDivisor == 2);
	// 30fps output: no half-rate rung.
	l = BuildLadder(1280, 720, 1080, false);
	CHECK_EQ(l.size(), 1u);
	CHECK(BuildLadder(0, 0, 720, true).empty());

	// The 1440p (2K) and 2160p (4K) floors: at an output that size the CPU
	// may only halve the frame rate, never drop below the floor.
	l = BuildLadder(2560, 1440, 1440, true);
	CHECK_EQ(l.size(), 2u);
	CHECK(l[0].width == 2560 && l[0].height == 1440 && l[0].captureDivisor == 1);
	CHECK(l[1].width == 2560 && l[1].height == 1440 && l[1].captureDivisor == 2);
	l = BuildLadder(3840, 2160, 2160, true);
	CHECK_EQ(l.size(), 2u);
	CHECK(l[1].width == 3840 && l[1].height == 2160 && l[1].captureDivisor == 2);
	l = BuildLadder(3840, 2160, 1440, true);
	CHECK(l.size() >= 3u);
	CHECK(l[l.size() - 2].width == 2560 && l[l.size() - 2].height == 1440);
	for (const Rung &rung : l)
		CHECK(rung.height >= 1440);

	// Every rung of every ladder bufferfit can start from: Y rows a multiple of
	// 8 bytes, U/V rows of 4 (QA: 1594x896, 2124x1194 with 1062-byte chroma
	// rows and 852x480 sheared the picture on OpenGL).
	const uint32_t starts[][3] = {{1920, 1080, 720},  {2560, 1440, 720}, {1280, 720, 480},   {3840, 2160, 1080},
				      {1360, 768, 480},   {1600, 900, 720},  {856, 480, 480},    {2560, 1440, 480},
				      {3440, 1440, 720},  {2560, 1080, 480}, {2560, 1440, 1440}, {3840, 2160, 1440},
				      {3840, 2160, 2160}, {3440, 1440, 1440}};
	for (const auto &start : starts) {
		for (const Rung &rung : BuildLadder(start[0], start[1], start[2], true)) {
			CHECK(rung.width % 8 == 0 && (rung.width / 2) % 4 == 0);
			CHECK(rung.height % 2 == 0 && rung.height >= 2);
			CHECK(rung.width <= start[0] && rung.height <= start[1]);
		}
	}
}

void TestInitialParallelism()
{
	testcheck::g_currentTest = "initial parallelism";
	// At most 2 slices per context since v0.7.0 (RAM): 1x1 -> 2x1 -> 2x2.
	const std::vector<Parallelism> threaded = BuildParallelismSteps(true);
	CHECK_EQ(threaded.size(), 3u);
	CHECK(threaded[1].contexts == 2 && threaded[1].slices == 1);
	CHECK(threaded.back().contexts == 2 && threaded.back().slices == 2);
	for (const Parallelism &step : threaded)
		CHECK(step.slices <= 2 && step.contexts <= 2);
	CHECK_EQ(InitialParallelism(threaded, true, 1920, 1080, kInterval60, 2), 0u);     // 1080p60: one context.
	CHECK_EQ(InitialParallelism(threaded, true, 2560, 1440, kInterval60, 2), 0u);     // 1440p60: one context.
	CHECK_EQ(InitialParallelism(threaded, true, 3840, 2160, kInterval60, 2), 1u);     // 4K60: two (2x1).
	CHECK_EQ(InitialParallelism(threaded, true, 3840, 2160, 2 * kInterval60, 2), 0u); // 4K30: one.
	CHECK_EQ(InitialParallelism(threaded, true, 7680, 4320, kInterval60, 2), 2u);     // 8K60: everything.
	const std::vector<Parallelism> sync = BuildParallelismSteps(false);
	CHECK_EQ(sync.size(), 2u);
	CHECK(sync.back().contexts == 1 && sync.back().slices == 2);
	CHECK_EQ(InitialParallelism(sync, false, 1920, 1080, kInterval60, 2), 1u);
	CHECK_EQ(InitialParallelism(sync, false, 1920, 1080, kInterval60, 4), 1u); // Never past the last step.
	CHECK_EQ(InitialParallelism(sync, false, 1920, 1080, kInterval60, 1), 0u);
	CHECK_NEAR(SliceSpeedup(4), 2.56, 1e-9);
	CHECK(PredictEncodeP95Ms(2560, 1440) > 9.0 && PredictEncodeP95Ms(2560, 1440) < 10.0);
}

void TestHealthyNeverMoves()
{
	testcheck::g_currentTest = "healthy never moves";
	Policy p = Threaded1080();
	CHECK(Run(p, kSecond, 120 * kSecond, Healthy).empty());
	CHECK_EQ(p.RungIndex(), 0u);
	CHECK_EQ(p.ParallelismIndex(), 0u);
	// Busy but under every threshold (encode at 75%, decode at 70%): still nothing.
	Policy q = Threaded1080();
	CHECK(Run(q, kSecond, 120 * kSecond, [](uint64_t t) {
		      Sample s = Healthy(t);
		      s.encodeP95Ms = 0.75 * kIntervalMs;
		      s.decodeP95Ms = 0.70 * kIntervalMs;
		      return s;
	      }).empty());
}

void TestEncodeOverload()
{
	testcheck::g_currentTest = "encode overload";
	Policy p = Threaded1080();
	// Encode at 95% of one context: more parallelism, one step per cooldown,
	// then lower rungs once maxed out.
	auto overloaded = [&](uint64_t t) {
		Sample s = Healthy(t);
		s.encodeP95Ms = 0.95 * kIntervalMs; // Per-frame time doesn't improve (worst case).
		s.encodeP95Ms *= p.CurrentParallelism().contexts;
		return s;
	};
	// Cooldown: nothing in the first 2 s.
	CHECK(Run(p, kSecond, kSecond + kSecond * 7 / 4, overloaded).empty());
	const Decision first = p.Evaluate(overloaded(3 * kSecond));
	CHECK(first.action == Action::MoreParallelism);
	CHECK(first.cause == Cause::Encode);
	CHECK(first.load > 0.8);
	CHECK_EQ(p.ParallelismIndex(), 1u);
	// Next change only after another full cooldown.
	CHECK(p.Evaluate(overloaded(4 * kSecond)).action == Action::None);
	CHECK(p.Evaluate(overloaded(5 * kSecond)).action == Action::MoreParallelism);
	CHECK_EQ(p.ParallelismIndex(), 2u); // 2x2: the last step.
	CHECK(p.Evaluate(overloaded(7 * kSecond)).action == Action::LowerQuality);
	CHECK_EQ(p.RungIndex(), 1u);
	// All the way down, then nothing more to do (but never a crash or a move up).
	Run(p, 10 * kSecond, 60 * kSecond, overloaded);
	CHECK_EQ(p.RungIndex(), p.Ladder().size() - 1);
	CHECK(p.CurrentRung().captureDivisor == 2);
	CHECK(Run(p, 61 * kSecond, 200 * kSecond, overloaded).empty());
}

void TestDropRate()
{
	testcheck::g_currentTest = "drop rate";
	Policy p = Threaded1080();
	auto dropping = [](uint64_t t) {
		Sample s = Healthy(t);
		s.encodeDrops = 4; // 3.3% of 120 captures.
		return s;
	};
	const std::vector<Decision> d = Run(p, kSecond, 3 * kSecond, dropping);
	CHECK_EQ(d.size(), 1u);
	CHECK(d.size() == 1 && d[0].action == Action::MoreParallelism && d[0].cause == Cause::Encode);
	// 1% is fine.
	Policy q = Threaded1080();
	CHECK(Run(q, kSecond, 30 * kSecond, [](uint64_t t) {
		      Sample s = Healthy(t);
		      s.encodeDrops = 1;
		      return s;
	      }).empty());
}

void TestDecodeOverload()
{
	testcheck::g_currentTest = "decode overload";
	Policy p = Threaded1080();
	auto slowDecode = [](uint64_t t) {
		Sample s = Healthy(t);
		s.decodeP95Ms = 0.9 * kIntervalMs;
		return s;
	};
	const std::vector<Decision> d = Run(p, kSecond, 3 * kSecond, slowDecode);
	CHECK(d.size() == 1 && d[0].action == Action::LowerQuality && d[0].cause == Cause::Decode);
	CHECK_EQ(p.ParallelismIndex(), 0u); // Decode can't use more encode threads.
	// Missed decode-ahead frames trigger it too.
	Policy q = Threaded1080();
	const std::vector<Decision> m = Run(q, kSecond, 3 * kSecond, [](uint64_t t) {
		Sample s = Healthy(t);
		s.decodeMisses = 10; // 8%.
		return s;
	});
	CHECK(m.size() == 1 && m[0].cause == Cause::Decode);
}

void TestGraphicsRules()
{
	testcheck::g_currentTest = "graphics rules";
	// p95 > 40% for the window: threaded -> lower rung.
	Policy p = Threaded1080();
	std::vector<Decision> d = Run(p, kSecond, 3 * kSecond, [](uint64_t t) {
		Sample s = Healthy(t);
		s.graphicsP95Ms = 0.45 * kIntervalMs;
		return s;
	});
	CHECK(d.size() == 1 && d[0].action == Action::LowerQuality && d[0].cause == Cause::Graphics);
	// p95 at 30% with OBS lagging frames: also down.
	Policy lag = Threaded1080();
	d = Run(lag, kSecond, 3 * kSecond, [](uint64_t t) {
		Sample s = Healthy(t);
		s.graphicsP95Ms = 0.30 * kIntervalMs;
		s.obsLaggedFrames = 3;
		return s;
	});
	CHECK_EQ(d.size(), 1u);
	// 30% without lag: fine.
	Policy fine = Threaded1080();
	CHECK(Run(fine, kSecond, 30 * kSecond, [](uint64_t t) {
		      Sample s = Healthy(t);
		      s.graphicsP95Ms = 0.30 * kIntervalMs;
		      return s;
	      }).empty());
	// p50 over the short window > 50%: immediate (0.5 s after the start, not 2 s).
	Policy now = Threaded1080();
	CHECK(now.Evaluate(Healthy(kSecond)).action == Action::None);
	Sample spike = Healthy(kSecond + kSecond / 2);
	spike.graphicsShortP50Ms = 0.6 * kIntervalMs;
	const Decision immediate = now.Evaluate(spike);
	CHECK(immediate.action == Action::LowerQuality && immediate.cause == Cause::Graphics);
	CHECK(immediate.load > 0.5);
}

void TestSyncMode()
{
	testcheck::g_currentTest = "sync mode";
	// Codec on G: more slices first, then lower rungs. Worker metrics ignored.
	Policy p(BuildLadder(1920, 1080, 720, true), BuildParallelismSteps(false), 0, false);
	auto heavy = [](uint64_t t) {
		Sample s = Healthy(t);
		s.graphicsP95Ms = 0.6 * kIntervalMs;
		s.encodeP95Ms = 100.0; // Would trip the threaded rules; ignored here.
		return s;
	};
	std::vector<Decision> d = Run(p, kSecond, 10 * kSecond, heavy);
	CHECK(d.size() >= 2);
	if (d.size() >= 2) {
		CHECK(d[0].action == Action::MoreParallelism && d[0].cause == Cause::Graphics);
		CHECK(d[1].action == Action::LowerQuality);
	}
	CHECK(p.CurrentParallelism().contexts == 1 && p.CurrentParallelism().slices == 2);
}

void TestHysteresisUp()
{
	testcheck::g_currentTest = "hysteresis up";
	Policy p = Threaded1080();
	// Down one rung for decode.
	Run(p, kSecond, 3 * kSecond, [](uint64_t t) {
		Sample s = Healthy(t);
		s.decodeP95Ms = 0.9 * kIntervalMs;
		return s;
	});
	CHECK_EQ(p.RungIndex(), 1u);
	// Load drops: the prediction one rung up (x1.45 pixels x1.1) is 3*1.6 = ~4.8ms
	// of 16.7 (29%): fine, but only after 30 s straight.
	auto calm = [](uint64_t t) {
		return Healthy(t);
	};
	CHECK(Run(p, 4 * kSecond, 30 * kSecond, calm).empty());
	// A bad moment in between restarts the 30 s.
	Sample blip = Healthy(31 * kSecond);
	blip.decodeP95Ms = 0.6 * kIntervalMs; // Not a down trigger, but predicts too much one rung up.
	CHECK(p.Evaluate(blip).action == Action::None);
	CHECK(Run(p, 31 * kSecond + kSecond / 4, 60 * kSecond, calm).empty());
	const std::vector<Decision> up = Run(p, 60 * kSecond + kSecond / 4, 63 * kSecond, calm);
	CHECK(up.size() == 1 && up[0].action == Action::RaiseQuality && up[0].cause == Cause::Recovered);
	CHECK_EQ(p.RungIndex(), 0u);
}

void TestNoOscillation()
{
	testcheck::g_currentTest = "no oscillation";
	// Decode at 85% at full size: one rung down (31% fewer pixels -> ~59%);
	// predicted back up: 59% * 1.45 * 1.1 = 94% > 50%, so it stays down forever.
	Policy p = Threaded1080();
	std::vector<Decision> all;
	for (uint64_t t = kSecond; t <= 300 * kSecond; t += kSecond / 4) {
		Sample s = Healthy(t);
		const Rung &rung = p.CurrentRung();
		const double pixels = static_cast<double>(rung.width) * rung.height / (1920.0 * 1080.0);
		s.decodeP95Ms = 0.85 * kIntervalMs * pixels;
		const Decision d = p.Evaluate(s);
		if (d.action != Action::None)
			all.push_back(d);
	}
	CHECK_EQ(all.size(), 1u);
	CHECK_EQ(p.RungIndex(), 1u);
}

// The same, as the pipeline really behaves: the decoder works `delay`
// seconds behind the capture, so after a rung change it keeps decoding the
// previous rung's frames (and reporting their load) for `delay` seconds. QA's
// simulation (decode at 84% of a frame at 1440p60, delay 30/60 s) cascaded
// to 720p at half rate within 10 s, then climbed back on stale numbers. With
// the metrics labelled by the rung they were measured on (Sample::decodeRung)
// it takes one step down and stays there.
void TestNoOscillationDelayedDecode(uint64_t delaySeconds)
{
	testcheck::g_currentTest = delaySeconds == 30 ? "no oscillation, decoder 30 s behind"
						      : "no oscillation, decoder 60 s behind";
	const std::vector<Rung> ladder = BuildLadder(2560, 1440, 720, true);
	Policy p(ladder, BuildParallelismSteps(true), 0, true);
	struct Change {
		uint64_t t;
		size_t rung;
	};
	std::vector<Change> history{{0, 0}};
	auto rungAt = [&](uint64_t t) {
		size_t rung = 0;
		for (const Change &c : history) {
			if (c.t <= t)
				rung = c.rung;
		}
		return rung;
	};
	auto pixels = [&](size_t rung) {
		return static_cast<double>(ladder[rung].width) * ladder[rung].height / (2560.0 * 1440.0);
	};
	const uint64_t delayNs = delaySeconds * kSecond;
	const uint64_t start = delayNs + kSecond; // Active: the decoder is already busy.
	std::vector<Decision> changes;
	for (uint64_t t = start; t <= start + 400 * kSecond; t += kSecond / 4) {
		const size_t decoded = rungAt(t - delayNs); // The rung of the frames due now.
		Sample s = Healthy(t);
		s.encodeP95Ms = 9.0 * pixels(p.RungIndex());
		s.decodeRung = decoded;
		s.decodeSamples = 120 / ladder[decoded].captureDivisor;
		s.decodeP95Ms = 14.0 * pixels(decoded); // 84% of a frame at 2560x1440.
		s.dueFrames = s.decodeSamples;
		const Decision d = p.Evaluate(s);
		if (d.action == Action::LowerQuality || d.action == Action::RaiseQuality) {
			changes.push_back(d);
			history.push_back({t, p.RungIndex()});
		} else {
			CHECK(d.action == Action::None);
		}
	}
	// 2128x1194 decodes in 9.6 ms (58% of a frame): enough. Back up would
	// predict 9.6 * 1.45 * 1.1 = 15.3 ms (92%) > 50%: never.
	CHECK_EQ(changes.size(), 1u);
	CHECK(!changes.empty() && changes[0].action == Action::LowerQuality && changes[0].cause == Cause::Decode);
	CHECK_EQ(p.RungIndex(), 1u);

	// Decode metrics of another rung than the current one are ignored outright,
	// however bad (and however good: no going up on them either).
	Policy q(ladder, BuildParallelismSteps(true), 0, true);
	Run(q, kSecond, 3 * kSecond, [](uint64_t t) {
		Sample s = Healthy(t);
		s.decodeP95Ms = 0.9 * kIntervalMs;
		return s;
	});
	CHECK_EQ(q.RungIndex(), 1u);
	CHECK(Run(q, 4 * kSecond, 100 * kSecond, [](uint64_t t) {
		      Sample s = Healthy(t);
		      s.decodeRung = 0; // Still the full-size frames.
		      s.decodeP95Ms = 2.0 * kIntervalMs;
		      s.decodeMisses = s.dueFrames;
		      return s;
	      }).empty());
	CHECK(Run(q, 101 * kSecond, 200 * kSecond, [](uint64_t t) {
		      Sample s = Healthy(t);
		      s.decodeRung = 0;
		      s.decodeP95Ms = 0.1;
		      return s;
	      }).empty());
	CHECK_EQ(q.RungIndex(), 1u);
}

void TestRecoveryOrder()
{
	testcheck::g_currentTest = "recovery order";
	Policy p = Threaded1080();
	// Encode overload: 2 parallelism steps (to 2x2) and one rung down.
	Run(p, kSecond, 7 * kSecond, [&](uint64_t t) {
		Sample s = Healthy(t);
		s.encodeP95Ms = 0.95 * kIntervalMs * p.CurrentParallelism().contexts;
		return s;
	});
	CHECK_EQ(p.ParallelismIndex(), 2u);
	CHECK_EQ(p.RungIndex(), 1u);
	// Calm (cheap frames even single-threaded): quality first, then threads,
	// one step per 30 s, never below the initial parallelism.
	const std::vector<Decision> d = Run(p, 8 * kSecond, 400 * kSecond, [](uint64_t t) {
		Sample s = Healthy(t);
		s.encodeP95Ms = 1.0;
		return s;
	});
	CHECK_EQ(d.size(), 3u);
	if (d.size() == 3) {
		CHECK(d[0].action == Action::RaiseQuality);
		CHECK(d[1].action == Action::LessParallelism);
		CHECK(d[2].action == Action::LessParallelism);
	}
	CHECK_EQ(p.RungIndex(), 0u);
	CHECK_EQ(p.ParallelismIndex(), 0u);

	// Started at 2 contexts (4K60): never goes below that.
	Policy k(BuildLadder(3840, 2160, 720, true), BuildParallelismSteps(true), 1, true);
	CHECK(Run(k, kSecond, 200 * kSecond, [](uint64_t t) {
		      Sample s = Healthy(t);
		      s.encodeP95Ms = 1.0;
		      return s;
	      }).empty());
	CHECK_EQ(k.ParallelismIndex(), 1u);
}

void TestNotEnoughSamples()
{
	testcheck::g_currentTest = "not enough samples";
	Policy p = Threaded1080();
	// Terrible numbers, but from a handful of frames (e.g. filter just shown): ignored.
	CHECK(Run(p, kSecond, 20 * kSecond, [](uint64_t t) {
		      Sample s = Healthy(t);
		      s.encodeSamples = 5;
		      s.encodeP95Ms = 100.0;
		      s.decodeSamples = 5;
		      s.decodeP95Ms = 100.0;
		      s.graphicsSamples = 5;
		      s.graphicsP95Ms = 100.0;
		      s.graphicsShortSamples = 2;
		      s.graphicsShortP50Ms = 100.0;
		      s.captures = 5;
		      s.encodeDrops = 5;
		      return s;
	      }).empty());
}

// v0.7.0: the storage codec policy's hooks -- the hardware steps, a forced
// rung down, the veto on going up, steps switched with the codec, a new
// ladder -- and the worker's own time as the only encode load (I6).
void TestCodecHooks()
{
	testcheck::g_currentTest = "codec hooks";
	const std::vector<Parallelism> hw = BuildParallelismSteps(true, true);
	CHECK_EQ(hw.size(), 1u);
	CHECK(hw[0].contexts == 1 && hw[0].slices == 1);
	CHECK_EQ(BuildParallelismSteps(false, true).size(), 1u);

	// ForceLower: one rung right away; none at the last rung.
	Policy p = Threaded1080();
	const Decision d = p.ForceLower(Cause::Encode, 2.4, 10 * kSecond);
	CHECK(d.action == Action::LowerQuality && d.cause == Cause::Encode);
	CHECK_EQ(p.RungIndex(), 1u);
	while (p.RungIndex() + 1 < p.Ladder().size())
		p.ForceLower(Cause::Encode, 2.4, 11 * kSecond);
	CHECK(p.ForceLower(Cause::Encode, 2.4, 12 * kSecond).action == Action::None);

	// The veto: calm for a minute, no going up while blockUp; up once it's lifted.
	Policy q = Threaded1080();
	q.ForceLower(Cause::Encode, 2.4, kSecond);
	auto calm = [](uint64_t t) {
		Sample s = Healthy(t);
		s.graphicsP95Ms = 0.2;
		s.encodeP95Ms = 1.0;
		s.decodeP95Ms = 1.0;
		return s;
	};
	CHECK(Run(q, 2 * kSecond, 90 * kSecond, [&](uint64_t t) {
		      Sample s = calm(t);
		      s.blockUp = true;
		      return s;
	      }).empty());
	const std::vector<Decision> up = Run(q, 90 * kSecond + kSecond / 4, 130 * kSecond, calm);
	CHECK(!up.empty() && up.front().action == Action::RaiseQuality);

	// Steps switched with the codec: 2x2 MJPEG -> 1x1 hardware, rung kept.
	Policy r = Threaded1080(2);
	r.ForceLower(Cause::Encode, 1.0, kSecond);
	r.SetParallelism(BuildParallelismSteps(true, true), 0);
	CHECK_EQ(r.RungIndex(), 1u);
	CHECK_EQ(r.ParallelismIndex(), 0u);
	CHECK(r.CurrentParallelism().contexts == 1 && r.CurrentParallelism().slices == 1);
	// An encode overload on hardware steps can only lower a rung.
	const std::vector<Decision> over = Run(r, 2 * kSecond, 6 * kSecond, [](uint64_t t) {
		Sample s = Healthy(t);
		s.encodeP95Ms = 16.0;
		return s;
	});
	CHECK(!over.empty() && over.front().action == Action::LowerQuality);
	// Back to MJPEG's steps, at its initial step.
	r.SetParallelism(BuildParallelismSteps(true), 1);
	CHECK_EQ(r.ParallelismIndex(), 1u);
	CHECK(r.CurrentParallelism().contexts == 2);

	// A new ladder (the encoders' ceiling), on a given rung, parallelism kept.
	Policy e = Threaded1080(1);
	e.Rebase(BuildLadder(1280, 720, 720, true), 0);
	CHECK_EQ(e.Ladder().size(), 2u); // 720p, then 720p at half rate.
	CHECK_EQ(e.RungIndex(), 0u);
	CHECK_EQ(e.ParallelismIndex(), 1u);
	e.Rebase(BuildLadder(1920, 1080, 720, true), 9);
	CHECK_EQ(e.RungIndex(), e.Ladder().size() - 1);

	// I6: a hardware encoder's ~20 ms latency never reaches the policy; the
	// worker's own few milliseconds per frame don't move it.
	Policy g(BuildLadder(1920, 1080, 720, true), BuildParallelismSteps(true, true), 0, true);
	CHECK(Run(g, kSecond, 60 * kSecond, [](uint64_t t) {
		      Sample s = Healthy(t);
		      s.encodeP95Ms = 3.0; // Send(), copies, Publish -- not the 19-21 ms to the packet.
		      return s;
	      }).empty());
}

} // namespace

int main()
{
	TestLadder();
	TestInitialParallelism();
	TestHealthyNeverMoves();
	TestEncodeOverload();
	TestDropRate();
	TestDecodeOverload();
	TestGraphicsRules();
	TestSyncMode();
	TestHysteresisUp();
	TestNoOscillation();
	TestNoOscillationDelayedDecode(30);
	TestNoOscillationDelayedDecode(60);
	TestRecoveryOrder();
	TestNotEnoughSamples();
	TestCodecHooks();
	return testcheck::Finish("degradation_policy_tests");
}
