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
#include <functional>
#include <string>
#include <vector>

#include "obs-frontend-bridge.hpp"

// delay-controller owns the plugin's ENTIRE state machine. Both the dock UI
// (settings-ui) and the OBS hotkeys (hotkeys) call the exact same three
// public methods (Enable/Disable/Toggle) instead of having two separate code
// paths that could drift out of sync — see docs/SPEC.md §5.
//
// Deliberately has zero Qt dependency, so it can be built and exercised in a
// plain unit test without linking Qt or a running OBS process (only
// ObsFrontendBridge would need to be mocked/faked for that — a real next
// step for v0.2, not included in this MVP).
namespace trigglow {

enum class DelayState {
	Inactive, // Delay disabled; nothing configured on the output right now.
	Applying, // Enable/Disable/Toggle was requested and (if live) a reconnect is in flight.
	Active,   // Delay is enabled and confirmed applied.
	Error,    // Something went wrong; see DelayStatus::message.
};

struct DelayStatus {
	DelayState state = DelayState::Inactive;
	uint32_t configuredSeconds = 10;
	uint32_t activeSeconds = 0;
	bool safeMode = true;
	// Empty = disabled (current behavior: no scene switch during reconnect).
	// Scene shown to viewers while Applying, in place of OBS's own brief
	// black/frozen frame during the reconnect (issue #173). Switched back to
	// whatever was active once StreamingStarted confirms the reconnect.
	std::string reconnectSceneName;
	std::string message; // Human-readable (Spanish), empty outside of Error/info states.
};

using StatusChangedCallback = std::function<void(const DelayStatus &)>;

class DelayController {
public:
	explicit DelayController(ObsFrontendBridge &bridge);

	// Wires up the frontend event handler on the bridge. Call once from
	// obs_module_load(), after the bridge itself has been constructed.
	void Init();

	void SetStatusChangedCallback(StatusChangedCallback callback) { onStatusChanged_ = std::move(callback); }

	// --- The three actions shared by the dock buttons AND the hotkeys ---
	void Enable();
	void Disable();
	void Toggle();

	// --- Settings ---
	void SetDelaySeconds(uint32_t seconds);
	void SetSafeMode(bool enabled);
	// `sceneName` empty disables the feature (default): no scene switch
	// during reconnect, same behavior as before issue #173.
	void SetReconnectScene(std::string sceneName);
	DelayStatus GetStatus() const { return status_; }

	// Every scene in the current OBS scene collection, for the dock's scene
	// picker. Passthrough to ObsFrontendBridge so settings-ui never needs to
	// touch obs-frontend-api.h directly.
	std::vector<std::string> ListAvailableScenes() const;

	// Persistence: called by plugin-main.cpp with the obs_data_t* loaded
	// from/about to be saved to obs_module_get_config_path(). Kept as plain
	// getters/setters here (not obs_data_t directly) so this header doesn't
	// need to include obs.h.
	void LoadSettings(uint32_t delaySeconds, bool safeMode, bool wasEnabled, std::string reconnectSceneName = {});
	struct SettingsSnapshot {
		uint32_t delaySeconds;
		bool safeMode;
		bool enabled;
		std::string reconnectSceneName;
	};
	SettingsSnapshot SaveSettings() const;

	// Called by settings-ui's watchdog QTimer (see docs/SPEC.md §3, "modo
	// seguro"): if we're still in Applying after a reasonable timeout, give
	// up cleanly instead of leaving the UI stuck forever.
	void OnApplyTimeout();

	// Forwarded from ObsFrontendBridge's event handler.
	void OnFrontendEvent(FrontendEvent event);

private:
	void SetState(DelayState state, std::string message = {});
	void ApplyAndMaybeReconnect();
	void RestoreSceneBeforeReconnect();
	void NotifyStatusChanged();

	ObsFrontendBridge &bridge_;
	DelayStatus status_;
	bool enabled_ = false;
	bool pendingReconnect_ = false;
	// Scene active right before we switched to reconnectSceneName for a live
	// reconnect; empty when we haven't switched anything. Restored once the
	// reconnect resolves (success or safe-mode giveup) — see ApplyAndMaybeReconnect(),
	// OnFrontendEvent(), and OnApplyTimeout().
	std::string sceneBeforeReconnect_;
	StatusChangedCallback onStatusChanged_;
};

} // namespace trigglow
