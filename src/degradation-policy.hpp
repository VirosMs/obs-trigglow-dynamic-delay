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

#include <cstddef>
#include <cstdint>
#include <vector>

// Adaptive degradation of the delay buffer (v0.6.0, buffer pipeline design
// B.8, thresholds from the perf2 measurements). Pure: it only sees numbers,
// so tests/degradation_policy_tests.cpp drives it with synthetic metrics.
//
// Two independent knobs, never cutting the delay:
//   - Parallelism: encode contexts x slice threads. Threaded (codec workers):
//     1x1 -> 2x1 -> 2x2 (at most 2 encode workers and, since v0.7.0, 2
//     slices per context: each slice adds an encoder's worth of buffers).
//     Sync (kill switch, codec on the graphics thread): slices 1 -> 2 on the
//     one context.
//   - Quality rung: the fitted buffer size, then smaller sizes in x0.83 steps
//     down to the user's quality floor, then that floor captured at half the
//     frame rate (only when the output runs at 48fps or more). Every rung is a
//     "soft" change: same ring, same slot count, so the delay never moves.
//
// Going down (all over a trailing ~2 s window, after a 2 s cooldown since the
// last change):
//   - Graphics thread (the plugin's own work in Render() -- conversion,
//     readback and copy, upload, and the codec in sync mode; not rendering
//     the scene itself, which OBS does anyway -- p95 > 40% of the frame
//     interval, or its p50 over the last 0.5 s > 50%, or OBS lagging frames
//     while p95 > 25%): sync -> more slices first, then a lower rung;
//     threaded -> a lower rung (what's left on G scales with pixels).
//   - Encode (threaded): p95 per frame above 80% of what the contexts have
//     per frame, or > 2% of captures dropped for backpressure -> more
//     parallelism first, a lower rung once it's maxed out.
//   - Decode (threaded): p95 above 80% of the capture interval, or > 5% of
//     due frames missed by the decode-ahead -> a lower rung (the MJPEG decoder
//     has no threading). The decoder works `delay` seconds behind the capture,
//     so it only meets a new rung's frames that long after the change: decode
//     metrics only count when they were measured on frames captured at the
//     current rung (Sample::decodeRung). Before that they're ignored, both
//     going down (no second step for a load the first one hasn't reached the
//     decoder yet) and going up (no prediction from the old rung's numbers).
// Going up: only when the PREDICTED load one step up stays under the targets
// (G p95 <= 25% of the interval; workers <= 50%) for 30 s straight. Quality
// is restored before parallelism is given back. The prediction scales the
// measured p95 by the pixel ratio (and by 1.6 per halving of slices), plus 10%.
namespace trigglow::degradation {

struct Rung {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t captureDivisor = 1; // 1 = every output frame, 2 = every other one.
};

struct Parallelism {
	uint32_t contexts = 1;
	uint32_t slices = 1;
};

// fullWidth x fullHeight (rung 0) down to floorHeight rows in `step` scale
// steps, then the floor again at half rate when allowHalfRate. floorHeight >=
// fullHeight gives no resolution rungs. Widths are multiples of 8
// (bufferfit::AlignedWidth, whole 4-byte rows in every plane for OpenGL), heights
// even (bufferfit::EvenDim); pass a fullWidth that already is one (bufferfit's).
std::vector<Rung> BuildLadder(uint32_t fullWidth, uint32_t fullHeight, uint32_t floorHeight, bool allowHalfRate,
			      double step = 0.83);

std::vector<Parallelism> BuildParallelismSteps(bool threaded);

// Predicted encode p95 (ms) of ONE single-threaded MJPEG context at
// width x height (huffman=default), from the perf measurements with a 10%
// margin: 1.1 * (0.3 + 2.3 ms per megapixel).
double PredictEncodeP95Ms(uint32_t width, uint32_t height);
// Wall time of one frame with `slices` slice threads relative to one: 1.6x
// faster per doubling (measured 1.63x for 1->2 and 1.56x for 2->4).
double SliceSpeedup(uint32_t slices);

// Threaded: the first step whose predicted encode load is <= 70% of what the
// contexts have per frame at this capture interval (so 4K60 starts with two
// contexts, 1440p60 with one). Sync: the step with preferredSyncSlices slices.
size_t InitialParallelism(const std::vector<Parallelism> &steps, bool threaded, uint32_t width, uint32_t height,
			  uint64_t captureIntervalNs, uint32_t preferredSyncSlices);

struct Thresholds {
	double graphicsTarget = 0.25;    // Up: predicted G p95 <= 25% of the interval.
	double graphicsDown = 0.40;      // Down: G p95 > 40%.
	double graphicsImmediate = 0.50; // Down now: G p50 (short window) > 50%.
	double workerDown = 0.80;        // Down: encode/decode p95 > 80% of the budget.
	double workerUp = 0.50;          // Up: predicted worker load <= 50%.
	double dropRateDown = 0.02;      // Down (encode): > 2% of captures dropped.
	double missRateDown = 0.05;      // Down (decode): > 5% of due frames missed.
	double predictionMargin = 1.10;
	uint64_t cooldownNs = 2000000000ULL;       // Min time between changes (= the window length).
	uint64_t immediateSettleNs = 500000000ULL; // The p50 rule waits only this long.
	uint64_t upHoldNs = 30000000000ULL;        // Up only after 30 s of good predictions.
	uint32_t minSamples = 20;                  // Per metric, or the metric is ignored.
};

// One evaluation's input. Percentiles over the trailing window (2 s), the
// short p50 over the last 0.5 s, counts over the trailing window.
struct Sample {
	uint64_t nowNs = 0;
	uint64_t frameIntervalNs = 0; // OBS output frame interval (1/fps).
	// The plugin's own work per Render() call on the graphics thread (not the
	// scene's render), see the header comment.
	uint32_t graphicsSamples = 0;
	double graphicsP95Ms = 0.0;
	uint32_t graphicsShortSamples = 0;
	double graphicsShortP50Ms = 0.0;
	uint32_t encodeSamples = 0;
	double encodeP95Ms = 0.0;
	uint32_t decodeSamples = 0;
	double decodeP95Ms = 0.0;
	uint32_t captures = 0;     // Captures attempted.
	uint32_t encodeDrops = 0;  // Of those, dropped (no RawFrame, no free slot, arena full).
	uint32_t dueFrames = 0;    // Playback frames that had a new frame due.
	uint32_t decodeMisses = 0; // Of those, Ready in the ring but not decoded in time.
	uint32_t obsLaggedFrames = 0;
	// The rung the frames behind decodeSamples/decodeP95Ms, dueFrames and
	// decodeMisses were captured at. Any other than the policy's current rung:
	// those metrics are ignored. kSameRung: assume the current one.
	static constexpr size_t kSameRung = SIZE_MAX;
	size_t decodeRung = kSameRung;
};

enum class Action : uint8_t { None, MoreParallelism, LessParallelism, LowerQuality, RaiseQuality };
enum class Cause : uint8_t { None, Graphics, Encode, Decode, Recovered };

const char *ActionName(Action action);
const char *CauseName(Cause cause);

struct Decision {
	Action action = Action::None;
	Cause cause = Cause::None;
	double load = 0.0; // The fraction that triggered it (or the predicted one, going up).
};

class Policy {
public:
	Policy() = default;
	Policy(std::vector<Rung> ladder, std::vector<Parallelism> steps, size_t initialParallelism, bool threaded,
	       const Thresholds &thresholds = Thresholds());

	// Applies and returns at most one step. Call every few hundred ms.
	Decision Evaluate(const Sample &sample);

	bool Valid() const { return !ladder_.empty() && !steps_.empty(); }
	size_t RungIndex() const { return rung_; }
	size_t ParallelismIndex() const { return step_; }
	const Rung &CurrentRung() const { return ladder_[rung_]; }
	const Parallelism &CurrentParallelism() const { return steps_[step_]; }
	const std::vector<Rung> &Ladder() const { return ladder_; }
	const std::vector<Parallelism> &Steps() const { return steps_; }
	const Thresholds &GetThresholds() const { return thresholds_; }

private:
	Decision Apply(Action action, Cause cause, double load, uint64_t nowNs);

	std::vector<Rung> ladder_;
	std::vector<Parallelism> steps_;
	size_t rung_ = 0;
	size_t step_ = 0;
	size_t initialStep_ = 0;
	bool threaded_ = true;
	Thresholds thresholds_;
	uint64_t lastChangeNs_ = 0; // 0 until the first evaluation (then the cycle start).
	uint64_t upSinceNs_ = 0;    // When the "one step up looks fine" streak began (0 = no streak).
};

} // namespace trigglow::degradation
