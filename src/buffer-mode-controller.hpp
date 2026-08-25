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

// buffer-mode-controller owns the state machine for the "no reconnect"
// delay (issue #173 phase 2, video-only): unlike delay-controller (which
// touches the streaming output's own native delay), this mode NEVER
// touches streaming at all — it only orchestrates a wrapper scene + the
// trigglow_video_delay_filter (video-delay-filter.hpp) and switches scenes
// via ObsFrontendBridge, exactly like delay-controller's optional
// reconnect-scene picker already does.
//
// Deliberately a separate class from DelayController rather than folded
// into it: DelayController's DelayState (Inactive/Applying/Active/Error)
// and its whole Enable/Disable/Toggle contract are specifically about
// reconnect semantics. This mode has a different lifecycle (Filling has a
// real duration the user waits out, not a brief "Applying...") and talks
// to a completely different part of OBS (scenes/filters, not the streaming
// output) — keeping them separate avoids overloading one state machine
// with two unrelated meanings.
//
// Qt-free like delay-controller, for the same reason: unit-testable
// without a running OBS process (only ObsFrontendBridge would need
// mocking), and settings-ui.cpp stays the only Qt-facing file.
namespace trigglow {

enum class BufferModeState {
	Inactive, // Not running; live scene shown directly and unmodified.
	Filling,  // Enable() just ran; loading scene showing, filter still disabled, waiting delaySeconds.
	Active,   // Buffer full; wrapper scene showing delayed content, filter enabled.
	Error,
};

struct BufferModeStatus {
	BufferModeState state = BufferModeState::Inactive;
	uint32_t delaySeconds = 5;
	// Floor on the delayed video's resolution -- VideoDelayFilter never
	// captures shorter than this, shortening the ACTUAL buffered duration
	// instead if delaySeconds doesn't fit the VRAM budget at this height.
	// See VideoDelayFilter::EnsureRingSized. 720 matches
	// kDefaultMinResolutionHeight.
	uint32_t minResolutionHeight = 720;
	std::string liveSceneName;    // Required to Enable(); the streamer's real content.
	std::string loadingSceneName; // Optional; shown during Filling. Empty = no scene switch while filling.
	std::string message;          // Human-readable (Spanish), empty outside Error/info states.
};

using BufferStatusChangedCallback = std::function<void(const BufferModeStatus &)>;

class BufferModeController {
public:
	explicit BufferModeController(ObsFrontendBridge &bridge);

	void SetStatusChangedCallback(BufferStatusChangedCallback callback) { onStatusChanged_ = std::move(callback); }

	// Ensures the wrapper scene/filter exist for the configured live scene,
	// switches to the loading scene (if set), and moves to Filling. The
	// dock is responsible for arming a single-shot timer for
	// status_.delaySeconds and calling OnFillTimerElapsed() once it fires
	// (same pattern as DelayController's apply watchdog — see
	// settings-ui.cpp's ArmApplyWatchdog).
	void Enable();

	// Disables the filter and switches back to whatever scene was active
	// right before Enable() (falls back to the live scene itself if that's
	// unavailable). Safe to call from any state, including mid-Filling.
	void Disable();

	void Toggle();

	void SetLiveScene(std::string sceneName);
	void SetLoadingScene(std::string sceneName);
	void SetDelaySeconds(uint32_t seconds);
	void SetMinResolutionHeight(uint32_t heightPixels);

	BufferModeStatus GetStatus() const { return status_; }

	// Passthrough so settings-ui never needs to touch ObsFrontendBridge
	// directly — mirrors DelayController::ListAvailableScenes().
	std::vector<std::string> ListAvailableScenes() const { return bridge_.ListSceneNames(); }

	// Informational only (bytes) -- what the video buffer's VRAM budget
	// actually is on this machine right now, detected from real hardware
	// where possible. Never restricts delaySeconds/minResolutionHeight,
	// just lets the dock show the user what their choices are working with.
	uint64_t GetBufferBudgetBytes() const { return bridge_.GetBufferBudgetBytes(); }

	// Called once by the dock's fill timer after status_.delaySeconds has
	// elapsed since Enable(). No-op if we're no longer Filling (e.g. the
	// user pressed Disable before the timer fired).
	void OnFillTimerElapsed();

	// Persistence, mirroring DelayController::LoadSettings/SaveSettings.
	// Deliberately never auto-Enable()s on load, same reasoning as
	// DelayController: this creates/modifies OBS scenes, which should never
	// happen silently at obs_module_load() time.
	void LoadSettings(uint32_t delaySeconds, uint32_t minResolutionHeight, std::string liveSceneName,
			  std::string loadingSceneName);
	struct SettingsSnapshot {
		uint32_t delaySeconds;
		uint32_t minResolutionHeight;
		std::string liveSceneName;
		std::string loadingSceneName;
	};
	SettingsSnapshot SaveSettings() const;

private:
	void SetState(BufferModeState state, std::string message = {});
	void NotifyStatusChanged();

	ObsFrontendBridge &bridge_;
	BufferModeStatus status_;
	// Scene that was on Program right before Enable() switched away from
	// it, restored by Disable(). Empty when not currently enabled.
	std::string sceneBeforeEnable_;
	// True between a successful AcquireLiveSceneRendering() (in Enable())
	// and the matching ReleaseLiveSceneRendering() (in OnFillTimerElapsed()
	// once the wrapper scene takes over showing it naturally, or in
	// Disable() if we're interrupted mid-Filling). Tracked so we never
	// double-release or leak the keep-alive.
	bool liveSceneRenderingHeld_ = false;
	BufferStatusChangedCallback onStatusChanged_;
};

} // namespace trigglow
