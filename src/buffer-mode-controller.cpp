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

#include "buffer-mode-controller.hpp"
#include "logging.hpp"

namespace trigglow {

namespace {
constexpr const char *kComponent = "buffer-mode-controller";
}

BufferModeController::BufferModeController(ObsFrontendBridge &bridge) : bridge_(bridge)
{
	// The only subscriber to ObsFrontendBridge's frontend-event callback in
	// the currently-active (buffer-mode) code path -- DelayController owned
	// this before buffer mode replaced it as the only instantiated
	// controller (see plugin-main.cpp), leaving it wired to nothing. See
	// SetSceneListRefreshCallback's comment for why this is needed.
	bridge_.Init([this](FrontendEvent event) { OnFrontendEvent(event); });
}

void BufferModeController::SetLiveScene(std::string sceneName)
{
	status_.liveSceneName = std::move(sceneName);
	NotifyStatusChanged();
}

void BufferModeController::SetLoadingScene(std::string sceneName)
{
	status_.loadingSceneName = std::move(sceneName);
	NotifyStatusChanged();
}

void BufferModeController::SetDelaySeconds(uint32_t seconds)
{
	status_.delaySeconds = seconds;
	NotifyStatusChanged();
	if (status_.state == BufferModeState::Active) {
		bridge_.SetBufferFilterDelaySeconds(status_.liveSceneName, seconds);
		bridge_.SetAudioDelayFiltersDelaySeconds(status_.liveSceneName, seconds);
	}
}

void BufferModeController::SetMinResolutionHeight(uint32_t heightPixels)
{
	status_.minResolutionHeight = heightPixels;
	NotifyStatusChanged();
	if (status_.state == BufferModeState::Active)
		bridge_.SetBufferFilterMinResolutionHeight(status_.liveSceneName, heightPixels);
}

void BufferModeController::Enable()
{
	// Guard against re-entry: pressing Enable again while already
	// Filling/Active must be a no-op, not re-run the whole sequence. Found
	// live (2026-08-24): re-running it while Active overwrote
	// sceneBeforeEnable_ with the WRAPPER scene itself (since that's what's
	// on Program at that point), not the true original live scene — Disable()
	// would then "restore" to the wrapper instead of the real live content.
	// The observed symptom was Program getting stuck on the loading scene /
	// black after a second Enable press.
	if (status_.state == BufferModeState::Filling || status_.state == BufferModeState::Active)
		return;

	if (status_.liveSceneName.empty()) {
		SetState(BufferModeState::Error, "Elige primero una escena en directo.");
		return;
	}

	if (!bridge_.EnsureBufferWrapperScene(status_.liveSceneName)) {
		SetState(BufferModeState::Error,
			 "No se pudo preparar la escena auxiliar de buffer (revisa el log de OBS).");
		return;
	}
	bridge_.SetBufferFilterDelaySeconds(status_.liveSceneName, status_.delaySeconds);
	bridge_.SetBufferFilterMinResolutionHeight(status_.liveSceneName, status_.minResolutionHeight);

	// Audio: unlike the video filter, these attach to individual leaf audio
	// sources inside the live scene (Mic, Desktop, etc.), not the scene
	// itself -- see ObsFrontendBridge::EnsureAudioDelayFilters's comment for
	// why. No keep-alive needed for these (verified live, 2026-08-25): a
	// leaf source's own audio pipeline runs continuously regardless of
	// Program visibility, unlike video rendering. Not treated as fatal if
	// the live scene simply has no audio sources.
	bridge_.EnsureAudioDelayFilters(status_.liveSceneName);
	bridge_.SetAudioDelayFiltersDelaySeconds(status_.liveSceneName, status_.delaySeconds);
	bridge_.SetAudioDelayFiltersEnabled(status_.liveSceneName, true);

	// Enable the filter AND force the live scene to keep rendering in the
	// background right now, not once the fill timer elapses: Program is
	// about to switch to the loading scene, and without both of these the
	// live scene stops rendering entirely (nothing shows it), so the
	// filter's ring buffer would sit empty for the whole Filling window
	// instead of actually accumulating delayFrames worth of history. See
	// ObsFrontendBridge::AcquireLiveSceneRendering's comment for the full
	// story (found live, 2026-08-24: the delay collapsed to ~0 within a
	// second of switching to the delayed view).
	bridge_.SetBufferFilterEnabled(status_.liveSceneName, true);
	liveSceneRenderingHeld_ = bridge_.AcquireLiveSceneRendering(status_.liveSceneName);

	sceneBeforeEnable_ = bridge_.GetCurrentSceneName();
	if (!status_.loadingSceneName.empty())
		bridge_.SetCurrentSceneByName(status_.loadingSceneName);

	SetState(BufferModeState::Filling,
		 "Llenando el buffer (" + std::to_string(status_.delaySeconds) + "s)... el dock arma el temporizador.");
	TRIGGLOW_LOG_INFO(kComponent, "enabled: live=\"%s\" loading=\"%s\" delay=%us", status_.liveSceneName.c_str(),
			  status_.loadingSceneName.c_str(), status_.delaySeconds);
}

void BufferModeController::OnFillTimerElapsed()
{
	if (status_.state != BufferModeState::Filling)
		return;

	// The filter was already enabled back in Enable(); the wrapper scene
	// nesting the live scene now naturally keeps it rendering once it's on
	// Program, so the manual keep-alive from Enable() can be released.
	if (liveSceneRenderingHeld_) {
		bridge_.ReleaseLiveSceneRendering(status_.liveSceneName);
		liveSceneRenderingHeld_ = false;
	}
	bridge_.ShowBufferWrapperScene();
	SetState(BufferModeState::Active);
	TRIGGLOW_LOG_INFO(kComponent, "buffer full, now showing delayed content");
}

void BufferModeController::Disable()
{
	bool wasActive = status_.state != BufferModeState::Inactive;

	// Covers Disable() interrupting mid-Filling, before OnFillTimerElapsed()
	// had a chance to release it.
	if (liveSceneRenderingHeld_) {
		bridge_.ReleaseLiveSceneRendering(status_.liveSceneName);
		liveSceneRenderingHeld_ = false;
	}

	bridge_.SetBufferFilterEnabled(status_.liveSceneName, false);
	bridge_.SetAudioDelayFiltersEnabled(status_.liveSceneName, false);
	if (!sceneBeforeEnable_.empty()) {
		bridge_.SetCurrentSceneByName(sceneBeforeEnable_);
		sceneBeforeEnable_.clear();
	} else if (!status_.liveSceneName.empty()) {
		bridge_.SetCurrentSceneByName(status_.liveSceneName);
	}

	SetState(BufferModeState::Inactive);
	if (wasActive)
		TRIGGLOW_LOG_INFO(kComponent, "disabled");
}

void BufferModeController::Toggle()
{
	if (status_.state == BufferModeState::Inactive)
		Enable();
	else
		Disable();
}

void BufferModeController::LoadSettings(uint32_t delaySeconds, uint32_t minResolutionHeight, std::string liveSceneName,
					std::string loadingSceneName)
{
	status_.delaySeconds = delaySeconds;
	status_.minResolutionHeight = minResolutionHeight;
	status_.liveSceneName = std::move(liveSceneName);
	status_.loadingSceneName = std::move(loadingSceneName);
	// Deliberately NOT calling Enable() here even if the user left it on
	// last session — see header comment. Always starts Inactive.
	SetState(BufferModeState::Inactive);
}

BufferModeController::SettingsSnapshot BufferModeController::SaveSettings() const
{
	return {status_.delaySeconds, status_.minResolutionHeight, status_.liveSceneName, status_.loadingSceneName};
}

void BufferModeController::SetState(BufferModeState state, std::string message)
{
	status_.state = state;
	status_.message = std::move(message);
	NotifyStatusChanged();
}

void BufferModeController::NotifyStatusChanged()
{
	if (onStatusChanged_)
		onStatusChanged_(status_);
}

void BufferModeController::OnFrontendEvent(FrontendEvent event)
{
	if (event == FrontendEvent::FinishedLoading && onSceneListRefresh_)
		onSceneListRefresh_();
}

} // namespace trigglow
