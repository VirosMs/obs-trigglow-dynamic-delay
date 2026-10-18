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
// hysteresis going up, the recovery order and the absence of oscillation.

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
	CHECK(l[1].width == 1594 && l[1].height == 896);
	CHECK(l[2].width == 1322 && l[2].height == 744);
	CHECK(l[3].width == 1280 && l[3].height == 720 && l[3].captureDivisor == 1);
	CHECK(l[4].width == 1280 && l[4].height == 720 && l[4].captureDivisor == 2);
	for (size_t i = 0; i < l.size(); ++i)
		CHECK(l[i].width % 2 == 0 && l[i].height % 2 == 0 && l[i].height >= 720);

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
}

void TestInitialParallelism()
{
	testcheck::g_currentTest = "initial parallelism";
	const std::vector<Parallelism> threaded = BuildParallelismSteps(true);
	CHECK_EQ(threaded.size(), 4u);
	CHECK(threaded.back().contexts == 2 && threaded.back().slices == 4);
	CHECK_EQ(InitialParallelism(threaded, true, 1920, 1080, kInterval60, 4), 0u);     // 1080p60: one context.
	CHECK_EQ(InitialParallelism(threaded, true, 2560, 1440, kInterval60, 4), 0u);     // 1440p60: one context.
	CHECK_EQ(InitialParallelism(threaded, true, 3840, 2160, kInterval60, 4), 1u);     // 4K60: two.
	CHECK_EQ(InitialParallelism(threaded, true, 3840, 2160, 2 * kInterval60, 4), 0u); // 4K30: one.
	CHECK_EQ(InitialParallelism(threaded, true, 7680, 4320, kInterval60, 4), 3u);     // 8K60: everything.
	const std::vector<Parallelism> sync = BuildParallelismSteps(false);
	CHECK_EQ(sync.size(), 3u);
	CHECK_EQ(InitialParallelism(sync, false, 1920, 1080, kInterval60, 4), 2u);
	CHECK_EQ(InitialParallelism(sync, false, 1920, 1080, kInterval60, 2), 1u);
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
	CHECK(p.Evaluate(overloaded(7 * kSecond)).action == Action::MoreParallelism);
	CHECK_EQ(p.ParallelismIndex(), 3u);
	CHECK(p.Evaluate(overloaded(9 * kSecond)).action == Action::LowerQuality);
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
	CHECK(d.size() >= 3);
	if (d.size() >= 3) {
		CHECK(d[0].action == Action::MoreParallelism && d[0].cause == Cause::Graphics);
		CHECK(d[1].action == Action::MoreParallelism);
		CHECK(d[2].action == Action::LowerQuality);
	}
	CHECK(p.CurrentParallelism().contexts == 1 && p.CurrentParallelism().slices == 4);
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

void TestRecoveryOrder()
{
	testcheck::g_currentTest = "recovery order";
	Policy p = Threaded1080();
	// Encode overload: 3 parallelism steps and one rung down.
	Run(p, kSecond, 9 * kSecond, [&](uint64_t t) {
		Sample s = Healthy(t);
		s.encodeP95Ms = 0.95 * kIntervalMs * p.CurrentParallelism().contexts;
		return s;
	});
	CHECK_EQ(p.ParallelismIndex(), 3u);
	CHECK_EQ(p.RungIndex(), 1u);
	// Calm (cheap frames even single-threaded): quality first, then threads,
	// one step per 30 s, never below the initial parallelism.
	const std::vector<Decision> d = Run(p, 10 * kSecond, 400 * kSecond, [](uint64_t t) {
		Sample s = Healthy(t);
		s.encodeP95Ms = 1.0;
		return s;
	});
	CHECK_EQ(d.size(), 4u);
	if (d.size() == 4) {
		CHECK(d[0].action == Action::RaiseQuality);
		CHECK(d[1].action == Action::LessParallelism);
		CHECK(d[2].action == Action::LessParallelism);
		CHECK(d[3].action == Action::LessParallelism);
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
	TestRecoveryOrder();
	TestNotEnoughSamples();
	return testcheck::Finish("degradation_policy_tests");
}
