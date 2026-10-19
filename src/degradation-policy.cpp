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

#include "degradation-policy.hpp"

#include "buffer-fit.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace trigglow::degradation {

std::vector<Rung> BuildLadder(uint32_t fullWidth, uint32_t fullHeight, uint32_t floorHeight, bool allowHalfRate,
			      double step)
{
	std::vector<Rung> ladder;
	if (fullWidth == 0 || fullHeight == 0)
		return ladder;
	ladder.push_back(Rung{fullWidth, fullHeight, 1});
	if (step > 0.0 && step < 1.0 && floorHeight < fullHeight) {
		const double floorScale = static_cast<double>(std::max<uint32_t>(floorHeight, 2)) / fullHeight;
		for (double scale = step;; scale *= step) {
			if (scale <= floorScale + 1e-9) {
				// The floor itself is the last resolution rung (unless the
				// previous step already landed on it after rounding).
				const Rung floorRung{bufferfit::AlignedWidth(fullWidth, floorScale),
						     bufferfit::EvenDim(fullHeight, floorScale), 1};
				if (floorRung.height < ladder.back().height)
					ladder.push_back(floorRung);
				break;
			}
			const Rung rung{bufferfit::AlignedWidth(fullWidth, scale),
					bufferfit::EvenDim(fullHeight, scale), 1};
			if (rung.height < ladder.back().height)
				ladder.push_back(rung);
		}
	}
	if (allowHalfRate) {
		Rung half = ladder.back();
		half.captureDivisor = 2;
		ladder.push_back(half);
	}
	return ladder;
}

std::vector<Parallelism> BuildParallelismSteps(bool threaded)
{
	// At most 2 slice threads per context since v0.7.0 (design 6.2): every
	// extra slice costs the encoder ~15 MB at 1440p and ~33 MB at 2160p, and
	// 2 contexts x 4 slices held ~60/130 MB more than 2 x 2.
	if (threaded)
		return {{1, 1}, {2, 1}, {2, 2}};
	return {{1, 1}, {1, 2}};
}

double PredictEncodeP95Ms(uint32_t width, uint32_t height)
{
	const double megapixels = static_cast<double>(width) * height / 1e6;
	return 1.1 * (0.3 + 2.3 * megapixels);
}

double SliceSpeedup(uint32_t slices)
{
	if (slices <= 1)
		return 1.0;
	return std::pow(1.6, std::log2(static_cast<double>(slices)));
}

size_t InitialParallelism(const std::vector<Parallelism> &steps, bool threaded, uint32_t width, uint32_t height,
			  uint64_t captureIntervalNs, uint32_t preferredSyncSlices)
{
	if (steps.empty())
		return 0;
	if (!threaded) {
		size_t best = 0;
		for (size_t i = 0; i < steps.size(); ++i) {
			if (steps[i].slices <= preferredSyncSlices)
				best = i;
		}
		return best;
	}
	const double intervalMs = static_cast<double>(captureIntervalNs) / 1e6;
	const double perFrameMs = PredictEncodeP95Ms(width, height);
	for (size_t i = 0; i < steps.size(); ++i) {
		const double load = perFrameMs / SliceSpeedup(steps[i].slices) / (intervalMs * steps[i].contexts);
		if (load <= 0.70)
			return i;
	}
	return steps.size() - 1;
}

const char *ActionName(Action action)
{
	switch (action) {
	case Action::None:
		return "none";
	case Action::MoreParallelism:
		return "more encode threads";
	case Action::LessParallelism:
		return "fewer encode threads";
	case Action::LowerQuality:
		return "lower rung";
	case Action::RaiseQuality:
		return "higher rung";
	}
	return "?";
}

const char *CauseName(Cause cause)
{
	switch (cause) {
	case Cause::None:
		return "none";
	case Cause::Graphics:
		return "graphics thread";
	case Cause::Encode:
		return "encode";
	case Cause::Decode:
		return "decode";
	case Cause::Recovered:
		return "load back under target";
	}
	return "?";
}

Policy::Policy(std::vector<Rung> ladder, std::vector<Parallelism> steps, size_t initialParallelism, bool threaded,
	       const Thresholds &thresholds)
	: ladder_(std::move(ladder)),
	  steps_(std::move(steps)),
	  threaded_(threaded),
	  thresholds_(thresholds)
{
	if (steps_.empty())
		steps_.push_back(Parallelism{});
	if (ladder_.empty())
		ladder_.push_back(Rung{});
	step_ = std::min(initialParallelism, steps_.size() - 1);
	initialStep_ = step_;
}

Decision Policy::Apply(Action action, Cause cause, double load, uint64_t nowNs)
{
	switch (action) {
	case Action::MoreParallelism:
		++step_;
		break;
	case Action::LessParallelism:
		--step_;
		break;
	case Action::LowerQuality:
		++rung_;
		break;
	case Action::RaiseQuality:
		--rung_;
		break;
	case Action::None:
		break;
	}
	lastChangeNs_ = nowNs;
	upSinceNs_ = 0;
	return Decision{action, cause, load};
}

Decision Policy::Evaluate(const Sample &s)
{
	if (!Valid() || s.frameIntervalNs == 0)
		return {};
	if (lastChangeNs_ == 0)
		lastChangeNs_ = s.nowNs; // Cycle start: the windows fill from here.
	const Thresholds &t = thresholds_;
	const uint64_t sinceChange = s.nowNs >= lastChangeNs_ ? s.nowNs - lastChangeNs_ : 0;
	const bool settled = sinceChange >= t.cooldownNs;
	const double intervalMs = static_cast<double>(s.frameIntervalNs) / 1e6;
	const double captureMs = intervalMs * ladder_[rung_].captureDivisor;
	const Parallelism &par = steps_[step_];
	const bool canLower = rung_ + 1 < ladder_.size();
	const bool canParallelize = step_ + 1 < steps_.size();

	// --- Down ---
	const double graphicsLoad = s.graphicsP95Ms / intervalMs;
	const double graphicsShort = s.graphicsShortP50Ms / intervalMs;
	const bool graphicsWindow = settled && s.graphicsSamples >= t.minSamples;
	bool graphicsDown = graphicsWindow && (graphicsLoad > t.graphicsDown ||
					       (graphicsLoad > t.graphicsTarget && s.obsLaggedFrames > 0));
	double graphicsTrigger = graphicsLoad;
	if (!graphicsDown && sinceChange >= t.immediateSettleNs && s.graphicsShortSamples >= t.minSamples / 2 &&
	    graphicsShort > t.graphicsImmediate) {
		graphicsDown = true;
		graphicsTrigger = graphicsShort;
	}

	// Decode metrics come from frames captured `delay` seconds ago: only those
	// captured at the current rung say anything about it.
	const bool decodeCurrent = s.decodeRung == Sample::kSameRung || s.decodeRung == rung_;
	const uint32_t decodeSamples = decodeCurrent ? s.decodeSamples : 0;
	const uint32_t dueFrames = decodeCurrent ? s.dueFrames : 0;

	bool encodeDown = false, decodeDown = false;
	double encodeLoad = 0.0, decodeLoad = 0.0;
	if (threaded_) {
		encodeLoad = s.encodeP95Ms / (captureMs * par.contexts);
		decodeLoad = s.decodeP95Ms / captureMs;
		const double dropRate = s.captures > 0 ? static_cast<double>(s.encodeDrops) / s.captures : 0.0;
		const double missRate = dueFrames > 0 ? static_cast<double>(s.decodeMisses) / dueFrames : 0.0;
		encodeDown = settled && ((s.encodeSamples >= t.minSamples && encodeLoad > t.workerDown) ||
					 (s.captures >= t.minSamples && dropRate > t.dropRateDown));
		decodeDown = settled && ((decodeSamples >= t.minSamples && decodeLoad > t.workerDown) ||
					 (dueFrames >= t.minSamples && missRate > t.missRateDown));
		if (encodeDown && encodeLoad <= t.workerDown)
			encodeLoad = dropRate; // Report what actually triggered it.
		if (decodeDown && decodeLoad <= t.workerDown)
			decodeLoad = missRate;
	}

	if (encodeDown) {
		if (canParallelize)
			return Apply(Action::MoreParallelism, Cause::Encode, encodeLoad, s.nowNs);
		if (canLower)
			return Apply(Action::LowerQuality, Cause::Encode, encodeLoad, s.nowNs);
	}
	if (decodeDown && canLower)
		return Apply(Action::LowerQuality, Cause::Decode, decodeLoad, s.nowNs);
	if (graphicsDown) {
		// Sync: the codec runs on G, and slices cut its wall time there.
		if (!threaded_ && canParallelize)
			return Apply(Action::MoreParallelism, Cause::Graphics, graphicsTrigger, s.nowNs);
		if (canLower)
			return Apply(Action::LowerQuality, Cause::Graphics, graphicsTrigger, s.nowNs);
	}
	if (encodeDown || decodeDown || graphicsDown) {
		upSinceNs_ = 0; // Overloaded at the last rung: nothing left to do, and certainly no going up.
		return {};
	}

	// --- Up (one step, predicted) ---
	if (!settled || s.graphicsSamples < t.minSamples ||
	    (threaded_ && (s.encodeSamples < t.minSamples || decodeSamples < t.minSamples))) {
		upSinceNs_ = 0;
		return {};
	}
	Action up = Action::None;
	double predicted = 0.0;
	bool ok = false;
	const double margin = t.predictionMargin;
	if (rung_ > 0) {
		const Rung &now = ladder_[rung_];
		const Rung &next = ladder_[rung_ - 1];
		const double pixels = static_cast<double>(next.width) * next.height /
				      std::max(1.0, static_cast<double>(now.width) * now.height);
		const double nextCaptureMs = intervalMs * next.captureDivisor;
		const double graphicsNext = graphicsLoad * pixels * margin;
		ok = graphicsNext <= t.graphicsTarget;
		predicted = graphicsNext;
		if (threaded_) {
			const double encodeNext = s.encodeP95Ms * pixels * margin / (nextCaptureMs * par.contexts);
			const double decodeNext = s.decodeP95Ms * pixels * margin / nextCaptureMs;
			ok = ok && encodeNext <= t.workerUp && decodeNext <= t.workerUp;
			predicted = std::max(predicted, std::max(encodeNext, decodeNext));
		}
		up = Action::RaiseQuality;
	} else if (step_ > initialStep_) {
		const Parallelism &next = steps_[step_ - 1];
		const double slower = SliceSpeedup(par.slices) / SliceSpeedup(next.slices);
		if (threaded_) {
			predicted = s.encodeP95Ms * slower * margin / (captureMs * next.contexts);
			ok = predicted <= t.workerUp;
		} else {
			// The whole G time is assumed to slow down (encode is only part of
			// it): errs towards staying put.
			predicted = graphicsLoad * slower * margin;
			ok = predicted <= t.graphicsTarget;
		}
		up = Action::LessParallelism;
	}
	if (up == Action::None || !ok) {
		upSinceNs_ = 0;
		return {};
	}
	if (upSinceNs_ == 0) {
		upSinceNs_ = s.nowNs;
		return {};
	}
	if (s.nowNs - upSinceNs_ < t.upHoldNs)
		return {};
	return Apply(up, Cause::Recovered, predicted, s.nowNs);
}

} // namespace trigglow::degradation
