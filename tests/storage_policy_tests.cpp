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

// Unit tests for trigglow::storagepolicy (src/storage-codec-policy.hpp, v0.7.0
// RAM design 4.2 and 8.1): the triggers and their priorities (a hard error
// over everything), evacuation only with the engine shared, the early warning
// lowering a rung and evacuating at the floor, the 2 s cooldown, the 5-minute
// veto, yields and the way back, the minimum of samples, Evacuated as terminal,
// and no oscillation over an hour of metrics at the edge of the thresholds.

#include "check.hpp"
#include "storage-codec-policy.hpp"

#include <cstdint>
#include <vector>

using namespace trigglow::storagepolicy;

namespace {

constexpr uint64_t kSecond = 1000000000ULL;
constexpr uint64_t kInterval60 = 16666667ULL; // 2.5x = 41.7 ms.

// The hardware encoder running fine on a shared engine (the user's NVENC case).
Sample Fine(uint64_t nowNs)
{
	Sample s;
	s.nowNs = nowNs;
	s.captureIntervalNs = kInterval60;
	s.interActive = true;
	s.engineShared = true;
	s.latencySamples = 60;
	s.latencyP95Ms = 20.5;
	s.inFlightMax = 1;
	s.canLowerRung = true;
	return s;
}

// A policy already on hardware (adopted at 1 s).
Policy OnHardware()
{
	Policy p(true);
	CHECK(p.Evaluate(Fine(kSecond)).action == Action::Adopted);
	return p;
}

template<typename SampleFn> std::vector<Decision> Run(Policy &p, uint64_t t0, uint64_t t1, SampleFn sample)
{
	std::vector<Decision> out;
	for (uint64_t t = t0; t <= t1; t += kSecond / 4) {
		const Decision d = p.Evaluate(sample(t));
		if (d.action != Action::None)
			out.push_back(d);
	}
	return out;
}

void TestStates()
{
	testcheck::g_currentTest = "states";
	Policy none;
	CHECK(none.GetState() == State::Mjpeg && !none.Hardware());
	Sample bad = Fine(kSecond);
	bad.hardErrors = 3;
	bad.obsSkipped = 50;
	CHECK(none.Evaluate(bad).action == Action::None); // buffer_codec=mjpeg: nothing ever happens.
	Policy forced(false);
	CHECK(forced.GetState() == State::Mjpeg);

	// Probing: adopted, no encoder, or yielded.
	Policy p(true);
	CHECK(p.GetState() == State::Probing && p.Hardware());
	Sample waiting = Fine(kSecond);
	waiting.interActive = false;
	CHECK(p.Evaluate(waiting).action == Action::None);
	CHECK(p.Evaluate(Fine(2 * kSecond)).action == Action::Adopted);
	CHECK(p.GetState() == State::Hw);
	Policy q(true);
	Sample failed = waiting;
	failed.setupFailed = true;
	const Decision d = q.Evaluate(failed);
	CHECK(d.action == Action::NoHardware && d.reason == Reason::NoEncoder);
	CHECK(q.GetState() == State::Mjpeg);
	Policy y(true);
	Sample yielded = waiting;
	yielded.yielded = true;
	CHECK(y.Evaluate(yielded).action == Action::Yield && y.GetState() == State::Yielded);
}

void TestHardErrors()
{
	testcheck::g_currentTest = "hard errors";
	// At once, whatever else is going on and whatever the cooldown.
	for (int from = 0; from < 3; ++from) {
		Policy p = OnHardware();
		if (from == 1) {
			Sample warn = Fine(2 * kSecond);
			warn.latencyP95Ms = 60.0;
			Run(p, 4 * kSecond, 6 * kSecond, [&](uint64_t t) {
				Sample s = warn;
				s.nowNs = t;
				return s;
			});
			CHECK(p.GetState() == State::HwGuarded);
		} else if (from == 2) {
			Sample yielded = Fine(2 * kSecond);
			yielded.yielded = true;
			p.Evaluate(yielded);
			CHECK(p.GetState() == State::Yielded);
		}
		Sample s = Fine(p.GetState() == State::Hw ? kSecond + 1 : 7 * kSecond); // Inside the cooldown.
		s.hardErrors = 1;
		s.obsSkipped = 10;
		s.latencyP95Ms = 80.0;
		s.yielded = from == 2;
		const Decision d = p.Evaluate(s);
		CHECK(d.action == Action::Evacuate && d.reason == Reason::HardError);
		CHECK(p.GetState() == State::Evacuated && !p.Hardware());
		// Terminal until the next Enable (a new Policy).
		CHECK(Run(p, 8 * kSecond, 600 * kSecond, [](uint64_t t) { return Fine(t); }).empty());
		CHECK(!p.BlockUp(9 * kSecond));
	}
	// A hard error while probing too.
	Policy probing(true);
	Sample s = Fine(kSecond);
	s.interActive = false;
	s.hardErrors = 1;
	CHECK(probing.Evaluate(s).action == Action::Evacuate);
}

void TestObsSkipped()
{
	testcheck::g_currentTest = "obs skipped frames";
	// Shared engine: one skipped frame evacuates (OBS warns at > 10 and 0.1%).
	Policy p = OnHardware();
	Sample s = Fine(4 * kSecond);
	s.obsSkipped = 1;
	const Decision d = p.Evaluate(s);
	CHECK(d.action == Action::Evacuate && d.reason == Reason::ObsSkipped && d.value == 1.0);
	// Not shared (x264 streams, or an iGPU): the buffer can't cause it -- the
	// degradation policy deals with CPU and the graphics thread.
	Policy q = OnHardware();
	CHECK(Run(q, 4 * kSecond, 120 * kSecond, [](uint64_t t) {
		      Sample n = Fine(t);
		      n.engineShared = false;
		      n.obsSkipped = 25;
		      return n;
	      }).empty());
	// Within the cooldown after adoption: it waits for it, then evacuates.
	Policy r = OnHardware();
	const std::vector<Decision> early = Run(r, kSecond + kSecond / 4, 4 * kSecond, [](uint64_t t) {
		Sample n = Fine(t);
		n.obsSkipped = 2;
		return n;
	});
	CHECK_EQ(early.size(), 1u);
	CHECK(early.size() == 1 && early[0].action == Action::Evacuate);
	// Zero skipped: nothing.
	Policy z = OnHardware();
	CHECK(Run(z, 4 * kSecond, 60 * kSecond, [](uint64_t t) { return Fine(t); }).empty());
}

void TestEarlyWarning()
{
	testcheck::g_currentTest = "early warning";
	auto slow = [](uint64_t t) {
		Sample s = Fine(t);
		s.latencyP95Ms = 45.0; // > 41.7 ms.
		return s;
	};
	// Held for a second: one rung down, the veto on.
	Policy p = OnHardware();
	const std::vector<Decision> d = Run(p, 4 * kSecond, 6 * kSecond, slow);
	CHECK_EQ(d.size(), 1u);
	CHECK(d.size() == 1 && d[0].action == Action::LowerRung && d[0].reason == Reason::Latency);
	CHECK(p.GetState() == State::HwGuarded);
	CHECK(p.BlockUp(7 * kSecond));
	// Still warning on the lower rung: another rung after the cooldown...
	const std::vector<Decision> again = Run(p, 6 * kSecond + kSecond / 4, 10 * kSecond, slow);
	CHECK(!again.empty() && again[0].action == Action::LowerRung);
	// ...and at the floor: MJPEG.
	const std::vector<Decision> floor = Run(p, 10 * kSecond + kSecond / 4, 15 * kSecond, [&](uint64_t t) {
		Sample s = slow(t);
		s.canLowerRung = false;
		return s;
	});
	CHECK(!floor.empty() && floor[0].action == Action::Evacuate && floor[0].reason == Reason::LatencyAtFloor);
	CHECK(p.GetState() == State::Evacuated);

	// Frames queued: the same warning.
	Policy q = OnHardware();
	const std::vector<Decision> queued = Run(q, 4 * kSecond, 6 * kSecond, [](uint64_t t) {
		Sample s = Fine(t);
		s.inFlightMax = 3;
		return s;
	});
	CHECK(!queued.empty() && queued[0].action == Action::LowerRung && queued[0].reason == Reason::InFlight);
	// Too few samples, or not shared, or not held a second: nothing.
	Policy few = OnHardware();
	CHECK(Run(few, 4 * kSecond, 30 * kSecond, [&](uint64_t t) {
		      Sample s = slow(t);
		      s.latencySamples = 19;
		      return s;
	      }).empty());
	Policy free = OnHardware();
	CHECK(Run(free, 4 * kSecond, 30 * kSecond, [&](uint64_t t) {
		      Sample s = slow(t);
		      s.engineShared = false;
		      return s;
	      }).empty());
	Policy brief = OnHardware();
	CHECK(Run(brief, 4 * kSecond, 4 * kSecond + 3 * kSecond / 4, slow).empty());
	CHECK(Run(brief, 5 * kSecond, 30 * kSecond, [](uint64_t t) { return Fine(t); }).empty());
	// At the floor straight away: MJPEG.
	Policy atFloor = OnHardware();
	const std::vector<Decision> e = Run(atFloor, 4 * kSecond, 6 * kSecond, [&](uint64_t t) {
		Sample s = slow(t);
		s.canLowerRung = false;
		return s;
	});
	CHECK(!e.empty() && e[0].action == Action::Evacuate && e[0].reason == Reason::LatencyAtFloor);
}

void TestVeto()
{
	testcheck::g_currentTest = "veto";
	Policy p = OnHardware();
	Run(p, 4 * kSecond, 6 * kSecond, [](uint64_t t) {
		Sample s = Fine(t);
		s.latencyP95Ms = 50.0;
		return s;
	});
	CHECK(p.GetState() == State::HwGuarded);
	const uint64_t warnedAt = 5 * kSecond; // Held from 4 s, acted at 5 s.
	CHECK(p.BlockUp(warnedAt + 299 * kSecond));
	// Quiet for 5 minutes: back to Hw, the veto lifted.
	const std::vector<Decision> d =
		Run(p, 6 * kSecond, warnedAt + 301 * kSecond, [](uint64_t t) { return Fine(t); });
	CHECK_EQ(d.size(), 1u);
	CHECK(d.size() == 1 && d[0].action == Action::VetoLifted);
	CHECK(p.GetState() == State::Hw);
	CHECK(!p.BlockUp(warnedAt + 302 * kSecond));
}

void TestYield()
{
	testcheck::g_currentTest = "yield";
	Policy p = OnHardware();
	Sample s = Fine(2 * kSecond);
	s.yielded = true;
	s.interActive = false;
	CHECK(p.Evaluate(s).action == Action::Yield);
	CHECK(p.GetState() == State::Yielded);
	// Still yielded: nothing; skipped frames meanwhile don't evacuate (MJPEG already).
	s.nowNs = 5 * kSecond;
	s.obsSkipped = 5;
	CHECK(p.Evaluate(s).action == Action::None);
	// The UI lets it go: Probing, then adopted again.
	Sample back = Fine(8 * kSecond);
	back.interActive = false;
	CHECK(p.Evaluate(back).action == Action::Resume);
	CHECK(p.GetState() == State::Probing);
	CHECK(p.Evaluate(Fine(9 * kSecond)).action == Action::Adopted);
	CHECK(p.GetState() == State::Hw);
	// A new frame size no encoder opens at, once on hardware: MJPEG for good.
	Sample failed = Fine(20 * kSecond);
	failed.interActive = false;
	failed.setupFailed = true;
	const Decision d = p.Evaluate(failed);
	CHECK(d.action == Action::Evacuate && d.reason == Reason::NoEncoder);
}

void TestNoOscillation()
{
	testcheck::g_currentTest = "no oscillation";
	const uint64_t hour = 3600 * kSecond;
	// Just under every threshold for an hour: nothing.
	Policy p = OnHardware();
	const uint64_t before = p.Transitions();
	CHECK(Run(p, 4 * kSecond, hour, [](uint64_t t) {
		      Sample s = Fine(t);
		      s.latencyP95Ms = 41.6;
		      s.inFlightMax = 2;
		      return s;
	      }).empty());
	CHECK_EQ(p.Transitions(), before);
	// Flickering around the latency threshold, never a full second above: nothing.
	Policy q = OnHardware();
	CHECK(Run(q, 4 * kSecond, hour, [](uint64_t t) {
		      Sample s = Fine(t);
		      s.latencyP95Ms = (t / (kSecond / 2)) % 2 == 0 ? 43.0 : 40.0;
		      return s;
	      }).empty());
	// One real episode (3 s), then fine for the rest of the hour: one rung, once.
	Policy r = OnHardware();
	const uint64_t start = r.Transitions();
	const std::vector<Decision> d = Run(r, 4 * kSecond, hour, [](uint64_t t) {
		Sample s = Fine(t);
		if (t >= 100 * kSecond && t < 103 * kSecond)
			s.latencyP95Ms = 55.0;
		return s;
	});
	CHECK_EQ(r.Transitions() - start, 1u);
	CHECK(d.size() == 2 && d[0].action == Action::LowerRung && d[1].action == Action::VetoLifted);
}

} // namespace

int main()
{
	TestStates();
	TestHardErrors();
	TestObsSkipped();
	TestEarlyWarning();
	TestVeto();
	TestYield();
	TestNoOscillation();
	return testcheck::Finish("storage_policy_tests");
}
