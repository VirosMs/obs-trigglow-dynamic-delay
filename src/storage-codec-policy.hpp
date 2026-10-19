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

#include <cstdint>

// The delay buffer's storage codec policy (v0.7.0 RAM design 4.2): when the
// hardware inter-frame codec may keep running and when the buffer goes back to
// MJPEG, so that it never costs OBS's own encoders their engine ("Encoding
// overloaded"). Pure: it only sees numbers (tests/storage_policy_tests.cpp);
// VideoDelayFilter evaluates it on the graphics thread every 250 ms, next to
// the degradation policy, and acts on its decisions.
//
//   Mjpeg      buffer_codec=mjpeg, the sync kill switch, no hardware: nothing to do.
//   Probing    the setup thread looks for an encoder (MJPEG bridges meanwhile).
//   Hw         the hardware encoder stores the frames.
//   HwGuarded  its engine warned (latency): one rung down, no going back up for 5 min.
//   Yielded    an OBS output starting took the engine (NVENC sessions).
//   Evacuated  back on MJPEG until the next Enable (the user's decision: no
//              second try in the same cycle).
//
// Two speeds (design 4.2): with no visible symptom yet -- the encoder's
// latency or its queue growing on an engine OBS also encodes on -- one rung
// down (fewer pixels: -31% of the engine per rung) and on with hardware; with
// a symptom -- OBS skipped frames for encoding lag, with the engine shared --
// or a warning at the last rung, or any hard error, MJPEG, which never touches
// the engine. Hard errors act at once; everything else waits 2 s after the
// previous transition, and a warning must hold for 1 s with enough samples.
namespace trigglow::storagepolicy {

enum class State : uint8_t { Mjpeg, Probing, Hw, HwGuarded, Yielded, Evacuated };

enum class Action : uint8_t {
	None,
	Adopted,    // Probing -> Hw: the hardware encoder took over.
	NoHardware, // Probing -> Mjpeg: no encoder works; stop asking.
	LowerRung,  // Early warning: one rung down, going up vetoed (Hw -> HwGuarded).
	Evacuate,   // Back to MJPEG for the rest of the cycle (-> Evacuated).
	Yield,      // An OBS output took the engine (-> Yielded; the pipeline already closed it).
	Resume,     // Yielded -> Probing: the hardware encoder may come back.
	VetoLifted, // HwGuarded -> Hw: 5 minutes without a warning.
};

enum class Reason : uint8_t {
	None,
	HardError,      // The encoder or the hardware decoder failed.
	ObsSkipped,     // OBS skipped frames for encoding lag, the engine shared.
	Latency,        // The encoder's latency p95 above 2.5 capture intervals.
	InFlight,       // Frames queued in the encoder.
	LatencyAtFloor, // A warning with no resolution rung left to lower.
	NoEncoder,      // The setup found no encoder (for this frame size).
};

const char *StateName(State state);
const char *ActionName(Action action);
const char *ReasonText(Reason reason);

struct Thresholds {
	uint64_t cooldownNs = 2000000000ULL; // Between transitions (hard errors excepted).
	double latencyFactor = 2.5;          // x the capture interval (42 ms at 60 fps).
	uint64_t warnHoldNs = 1000000000ULL; // A warning must hold this long...
	uint32_t minLatencySamples = 20;     // ...over this many packets.
	uint32_t inFlightWarn = 3;           // AMF runs at 1-2.
	uint32_t skippedFrames = 1;          // OBS frames skipped for encoding lag in the window.
	uint64_t vetoNs = 300000000000ULL;   // No going up for 5 minutes after a warning.
};

// One evaluation's input, over the trailing windows the filter keeps.
struct Sample {
	uint64_t nowNs = 0;
	uint64_t captureIntervalNs = 0;
	bool interActive = false;    // The hardware encoder stores the frames right now.
	bool setupFailed = false;    // A setup since the last sample found no encoder.
	uint32_t hardErrors = 0;     // Since the last sample: encoder or hardware decoder failures.
	bool yielded = false;        // The UI yielded the engine (InterControl::yielded).
	bool engineShared = true;    // storage::EngineShare::Any(): shared, or unknown.
	uint32_t obsSkipped = 0;     // Encoding-lag frames OBS skipped (main mix + its encoders' mixes), last 2 s.
	uint32_t latencySamples = 0; // Packets in the latency window (the last second).
	double latencyP95Ms = 0.0;
	uint32_t inFlightMax = 0;  // Most frames in flight at a send, last second.
	bool canLowerRung = false; // A resolution rung is left above the user's floor.
};

struct Decision {
	Action action = Action::None;
	Reason reason = Reason::None;
	double value = 0.0; // What triggered it (frames, ms, frames in flight).
};

class Policy {
public:
	Policy() = default; // Mjpeg: no hardware.
	explicit Policy(bool hardwareAllowed, const Thresholds &thresholds = Thresholds());

	Decision Evaluate(const Sample &sample);

	State GetState() const { return state_; }
	bool Hardware() const { return state_ == State::Probing || state_ == State::Hw || state_ == State::HwGuarded; }
	// The degradation policy must not go up (degradation::Sample::blockUp).
	bool BlockUp(uint64_t nowNs) const { return vetoUntilNs_ != 0 && nowNs < vetoUntilNs_; }
	// Decisions that changed what the pipeline does (VetoLifted aside).
	uint64_t Transitions() const { return transitions_; }

private:
	Decision Go(State state, Action action, Reason reason, double value, uint64_t nowNs);

	State state_ = State::Mjpeg;
	Thresholds t_;
	uint64_t lastTransitionNs_ = 0;
	uint64_t warnSinceNs_ = 0;
	uint64_t vetoUntilNs_ = 0;
	uint64_t transitions_ = 0;
};

} // namespace trigglow::storagepolicy
