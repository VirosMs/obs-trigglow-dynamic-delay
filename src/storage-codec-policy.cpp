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

#include "storage-codec-policy.hpp"

namespace trigglow::storagepolicy {

const char *StateName(State state)
{
	switch (state) {
	case State::Mjpeg:
		return "Mjpeg";
	case State::Probing:
		return "Probing";
	case State::Hw:
		return "Hw";
	case State::HwGuarded:
		return "HwGuarded";
	case State::Yielded:
		return "Yielded";
	case State::Evacuated:
		return "Evacuated";
	}
	return "?";
}

const char *ActionName(Action action)
{
	switch (action) {
	case Action::None:
		return "none";
	case Action::Adopted:
		return "adopted";
	case Action::NoHardware:
		return "no hardware";
	case Action::LowerRung:
		return "lower rung";
	case Action::Evacuate:
		return "evacuate";
	case Action::Yield:
		return "yield";
	case Action::Resume:
		return "resume";
	case Action::VetoLifted:
		return "veto lifted";
	}
	return "?";
}

const char *ReasonText(Reason reason)
{
	switch (reason) {
	case Reason::None:
		return "none";
	case Reason::HardError:
		return "the hardware codec failed";
	case Reason::ObsSkipped:
		return "OBS skipped frames for encoding lag, on an engine the buffer shares";
	case Reason::Latency:
		return "the buffer encoder's latency rose on an engine OBS shares";
	case Reason::InFlight:
		return "frames queued up in the buffer encoder, on an engine OBS shares";
	case Reason::LatencyAtFloor:
		return "the buffer encoder's engine warned at the lowest resolution allowed";
	case Reason::NoEncoder:
		return "no hardware encoder works for this frame size";
	case Reason::ObsEncodeError:
		return "an OBS output stopped on an encoder error, on an engine the buffer shares";
	}
	return "?";
}

Policy::Policy(bool hardwareAllowed, const Thresholds &thresholds)
	: state_(hardwareAllowed ? State::Probing : State::Mjpeg),
	  t_(thresholds)
{
}

Decision Policy::Go(State state, Action action, Reason reason, double value, uint64_t nowNs)
{
	state_ = state;
	lastTransitionNs_ = nowNs;
	warnSinceNs_ = 0;
	if (action != Action::VetoLifted)
		++transitions_;
	return Decision{action, reason, value};
}

Decision Policy::Evaluate(const Sample &s)
{
	if (state_ == State::Mjpeg || state_ == State::Evacuated) {
		obsEncodeErrorPending_ = false;
		return {};
	}
	// An OBS encoder error while probing or yielded (the hardware encoder not
	// storing yet, its engine not known) is kept until it is: then, with the
	// engine shared, it evacuates like one reported then.
	if (s.obsEncodeError)
		obsEncodeErrorPending_ = true;
	// A hard error wins over everything, at once.
	if (s.hardErrors > 0) {
		vetoUntilNs_ = 0;
		return Go(State::Evacuated, Action::Evacuate, Reason::HardError, s.hardErrors, s.nowNs);
	}
	switch (state_) {
	case State::Probing:
		if (s.yielded)
			return Go(State::Yielded, Action::Yield, Reason::None, 0.0, s.nowNs);
		if (s.interActive)
			return Go(State::Hw, Action::Adopted, Reason::None, 0.0, s.nowNs);
		if (s.setupFailed)
			return Go(State::Mjpeg, Action::NoHardware, Reason::NoEncoder, 0.0, s.nowNs);
		return {};
	case State::Yielded:
		if (!s.yielded)
			return Go(State::Probing, Action::Resume, Reason::None, 0.0, s.nowNs);
		return {};
	case State::Hw:
	case State::HwGuarded:
		break;
	case State::Mjpeg:
	case State::Evacuated:
		return {};
	}

	// OBS's own encoder failed on an engine the buffer shares: at once, like a
	// hard error. Not shared: not the buffer's doing, forgotten.
	const bool obsEncodeError = obsEncodeErrorPending_;
	obsEncodeErrorPending_ = false;
	if (obsEncodeError && s.engineShared) {
		vetoUntilNs_ = 0;
		return Go(State::Evacuated, Action::Evacuate, Reason::ObsEncodeError, 0.0, s.nowNs);
	}
	if (s.yielded)
		return Go(State::Yielded, Action::Yield, Reason::None, 0.0, s.nowNs);
	if (s.setupFailed) {
		// A new rung's size no encoder opens at: the bridge would stay for good.
		vetoUntilNs_ = 0;
		return Go(State::Evacuated, Action::Evacuate, Reason::NoEncoder, 0.0, s.nowNs);
	}
	const bool settled = s.nowNs >= lastTransitionNs_ + t_.cooldownNs;
	// The symptom: OBS already skipped frames, on an engine the buffer shares.
	// At once, never after the cooldown: the stream is lagging now, and a rung
	// just lowered or an encoder just adopted (or back from a yield) is no
	// reason to let it lag for 2 s more.
	if (s.engineShared && s.obsSkipped >= t_.skippedFrames) {
		vetoUntilNs_ = 0;
		return Go(State::Evacuated, Action::Evacuate, Reason::ObsSkipped, s.obsSkipped, s.nowNs);
	}
	// The early warning: the engine saturates before OBS skips anything.
	const double intervalMs = static_cast<double>(s.captureIntervalNs) / 1e6;
	const bool latency = s.latencySamples >= t_.minLatencySamples && intervalMs > 0.0 &&
			     s.latencyP95Ms > t_.latencyFactor * intervalMs;
	const bool queued = s.inFlightMax >= t_.inFlightWarn;
	const bool warn = s.engineShared && (latency || queued);
	if (!warn) {
		warnSinceNs_ = 0;
	} else if (warnSinceNs_ == 0) {
		warnSinceNs_ = s.nowNs;
	}
	if (warn && settled && s.nowNs >= warnSinceNs_ + t_.warnHoldNs) {
		const Reason why = latency ? Reason::Latency : Reason::InFlight;
		const double value = latency ? s.latencyP95Ms : static_cast<double>(s.inFlightMax);
		if (s.canLowerRung) {
			vetoUntilNs_ = s.nowNs + t_.vetoNs;
			return Go(State::HwGuarded, Action::LowerRung, why, value, s.nowNs);
		}
		vetoUntilNs_ = 0;
		return Go(State::Evacuated, Action::Evacuate, Reason::LatencyAtFloor, value, s.nowNs);
	}
	if (state_ == State::HwGuarded && !warn && s.nowNs >= vetoUntilNs_) {
		vetoUntilNs_ = 0;
		return Go(State::Hw, Action::VetoLifted, Reason::None, 0.0, s.nowNs);
	}
	return {};
}

} // namespace trigglow::storagepolicy
