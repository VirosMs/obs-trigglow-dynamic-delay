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

BufferModeController::BufferModeController(ObsFrontendBridge &bridge) : bridge_(bridge) {}

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
	if (status_.state == BufferModeState::Active)
		bridge_.SetBufferFilterDelaySeconds(status_.liveSceneName, seconds);
}

void BufferModeController::Enable()
{
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

	bridge_.SetBufferFilterEnabled(status_.liveSceneName, true);
	bridge_.ShowBufferWrapperScene();
	SetState(BufferModeState::Active);
	TRIGGLOW_LOG_INFO(kComponent, "buffer full, now showing delayed content");
}

void BufferModeController::Disable()
{
	bool wasActive = status_.state != BufferModeState::Inactive;

	bridge_.SetBufferFilterEnabled(status_.liveSceneName, false);
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

void BufferModeController::LoadSettings(uint32_t delaySeconds, std::string liveSceneName, std::string loadingSceneName)
{
	status_.delaySeconds = delaySeconds;
	status_.liveSceneName = std::move(liveSceneName);
	status_.loadingSceneName = std::move(loadingSceneName);
	// Deliberately NOT calling Enable() here even if the user left it on
	// last session — see header comment. Always starts Inactive.
	SetState(BufferModeState::Inactive);
}

BufferModeController::SettingsSnapshot BufferModeController::SaveSettings() const
{
	return {status_.delaySeconds, status_.liveSceneName, status_.loadingSceneName};
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

} // namespace trigglow
