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

#include "delay-controller.hpp"
#include "logging.hpp"

namespace trigglow {

namespace {
constexpr const char *kComponent = "delay-controller";
}

DelayController::DelayController(ObsFrontendBridge &bridge) : bridge_(bridge) {}

void DelayController::Init()
{
	bridge_.Init([this](FrontendEvent event) { OnFrontendEvent(event); });
	TRIGGLOW_LOG_INFO(kComponent, "initialized (default delay=%us, safeMode=%s)", status_.configuredSeconds,
			  status_.safeMode ? "on" : "off");
}

void DelayController::Enable()
{
	enabled_ = true;
	ApplyAndMaybeReconnect();
}

void DelayController::Disable()
{
	enabled_ = false;
	// Disabling means "configure 0s delay". Same reconnect trade-off as
	// enabling: if we're live, it only takes full effect after a brief
	// reconnect (see docs/SPEC.md §2.2).
	ApplyAndMaybeReconnect();
}

void DelayController::Toggle()
{
	if (enabled_)
		Disable();
	else
		Enable();
}

void DelayController::SetDelaySeconds(uint32_t seconds)
{
	// Sanity clamp: 0 is meaningless as an "enabled" delay, and OBS/most
	// destinations don't need more than 30 minutes of buffered stream.
	if (seconds > 1800)
		seconds = 1800;
	status_.configuredSeconds = seconds;
	NotifyStatusChanged();

	// If already enabled, re-apply immediately with the new value so the
	// dock reflects reality without requiring a separate button press.
	if (enabled_)
		ApplyAndMaybeReconnect();
}

void DelayController::SetSafeMode(bool enabled)
{
	status_.safeMode = enabled;
	NotifyStatusChanged();
}

void DelayController::LoadSettings(uint32_t delaySeconds, bool safeMode, bool wasEnabled)
{
	status_.configuredSeconds = delaySeconds;
	status_.safeMode = safeMode;
	enabled_ = wasEnabled;
	// Deliberately NOT auto-applying on load: OBS may not have a valid
	// streaming service configured yet at obs_module_load() time, and this
	// plugin should never silently touch stream state on startup. The user
	// (or a hotkey/Stream Deck press) must explicitly (re)enable it.
	SetState(enabled_ ? DelayState::Active : DelayState::Inactive);
}

DelayController::SettingsSnapshot DelayController::SaveSettings() const
{
	return {status_.configuredSeconds, status_.safeMode, enabled_};
}

void DelayController::OnApplyTimeout()
{
	if (status_.state != DelayState::Applying)
		return;

	if (!status_.safeMode) {
		// Without safe mode we trust that the reconnect is just slow
		// (bad network, slow handshake with the platform) and leave the
		// state as-is rather than surfacing a false error.
		TRIGGLOW_LOG_WARN(kComponent, "apply timeout reached but safe mode is off; leaving state as Applying");
		return;
	}

	TRIGGLOW_LOG_ERROR(kComponent, "apply timeout reached in safe mode; giving up and reporting Error");
	pendingReconnect_ = false;
	SetState(DelayState::Error, "OBS no confirmo la reconexion a tiempo. Revisa tu conexion y el estado del "
				    "stream en OBS, y vuelve a intentarlo manualmente.");
}

void DelayController::OnFrontendEvent(FrontendEvent event)
{
	switch (event) {
	case FrontendEvent::StreamingStarted:
		if (pendingReconnect_) {
			pendingReconnect_ = false;
			uint32_t active = bridge_.GetActiveDelaySeconds();
			status_.activeSeconds = active;
			SetState(enabled_ ? DelayState::Active : DelayState::Inactive);
			TRIGGLOW_LOG_INFO(kComponent, "reconnect confirmed, active delay now %us", active);
		}
		break;
	case FrontendEvent::StreamingStopped:
		if (pendingReconnect_) {
			// We stopped the stream ourselves to apply a new delay; now
			// restart it. If the user manually stopped streaming at the
			// same time, this still just starts it back up with the
			// already-applied configured delay, which is the documented
			// (if slightly surprising) trade-off of this MVP strategy.
			TRIGGLOW_LOG_INFO(kComponent, "streaming stopped for reconnect, requesting restart");
			bridge_.RequestStreamingStart();
		} else {
			status_.activeSeconds = 0;
		}
		break;
	default:
		break;
	}
}

void DelayController::ApplyAndMaybeReconnect()
{
	uint32_t targetSeconds = enabled_ ? status_.configuredSeconds : 0;

	std::string error;
	if (!bridge_.ApplyConfiguredDelay(targetSeconds, /*preserveOnDisconnect=*/status_.safeMode, error)) {
		SetState(DelayState::Error, error);
		return;
	}

	if (!bridge_.IsStreamingActive()) {
		// Not live: the new value is armed cleanly for next time, no
		// reconnect needed. See docs/SPEC.md §2.2.
		status_.activeSeconds = 0;
		SetState(enabled_ ? DelayState::Active : DelayState::Inactive);
		TRIGGLOW_LOG_INFO(kComponent, "%s while offline, armed for next stream start (no reconnect needed)",
				  enabled_ ? "enabled" : "disabled");
		return;
	}

	// Live: the only real way to make the new delay take effect is to
	// restart the streaming output (see docs/SPEC.md §2). Mark ourselves as
	// "Applying" and let OnFrontendEvent() take it from here once OBS
	// confirms the stop/start cycle.
	pendingReconnect_ = true;
	SetState(DelayState::Applying, "Aplicando cambios: reconectando el stream para que el nuevo delay tenga "
				       "efecto. Esto puede causar un corte breve visible para tu audiencia.");
	bridge_.RequestStreamingStop();
}

void DelayController::SetState(DelayState state, std::string message)
{
	status_.state = state;
	status_.message = std::move(message);
	NotifyStatusChanged();
}

void DelayController::NotifyStatusChanged()
{
	if (onStatusChanged_)
		onStatusChanged_(status_);
}

} // namespace trigglow
