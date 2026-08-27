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

#include <memory>

#include <QDir>
#include <QFileInfo>

extern "C" {
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
}

#ifdef TRIGGLOW_HAVE_FFMPEG
extern "C" {
#include <libavutil/avutil.h>
}
#endif

#include "audio-delay-filter.hpp"
#include "buffer-mode-controller.hpp"
#include "hotkeys.hpp"
#include "logging.hpp"
#include "obs-frontend-bridge.hpp"
#include "settings-ui.hpp"
#include "video-delay-filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("Trigglow (VirosMs)")

// Required by OBS's plugin listing (Help > About, and the plugins manager) --
// was missing entirely before the OBS store submission pass (2026-08-26).
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Delays your stream's video and audio by a configurable number of seconds without ever "
	       "reconnecting the stream -- built for stream snipers, spoiler-safe reactions, and chat-timing.";
}

namespace {

constexpr const char *kComponent = "plugin-main";
constexpr const char *kDockId = "trigglow_dynamic_delay_dock";
constexpr const char *kDockTitle = "Trigglow Dynamic Delay";
constexpr const char *kBufferSettingsFile = "buffer-mode-settings.json";

// All plugin-lifetime state lives in one place, constructed in
// obs_module_load() and torn down in obs_module_unload(). No globals spread
// across the other files — every other module gets what it needs by
// reference/pointer from here.
//
// Buffer mode (BufferModeController) only, as of 2026-08-24. DelayController
// (native obs_output_set_delay reconnect mode) and its hotkeys.hpp/cpp
// wiring aren't instantiated here anymore -- live testing confirmed buffer
// mode works well and having two mechanisms + a mode selector was confusing
// for a non-technical streamer. DelayController's source stays in the repo
// (may come back) but is now dead code from the module's point of view;
// hotkeys.hpp/cpp was repointed to drive BufferModeController instead of
// deleted, since Stream Deck hotkey compatibility is a flagship feature
// that still needs a home.
struct PluginState {
	trigglow::ObsFrontendBridge bridge;
	std::unique_ptr<trigglow::BufferModeController> bufferController;
	std::unique_ptr<trigglow::DelayHotkeys> hotkeys;
	// Not owned past AddDock(): OBS's dock system takes ownership of the
	// QWidget once obs_frontend_add_dock_by_id() returns true. Kept here
	// only as a non-owning reference for potential future use (e.g. forcing
	// a refresh), never deleted from this side.
	trigglow::TrigglowDelayDock *dock = nullptr;
};

PluginState *g_state = nullptr;

void LoadBufferSettings(trigglow::BufferModeController &controller)
{
	char *rawPath = obs_module_get_config_path(obs_current_module(), kBufferSettingsFile);
	QString path = rawPath ? QString::fromUtf8(rawPath) : QString();
	bfree(rawPath);
	if (path.isEmpty())
		return;

	obs_data_t *data = obs_data_create_from_json_file(path.toUtf8().constData());
	if (!data) {
		TRIGGLOW_LOG_INFO(kComponent, "no existing settings file, using defaults");
		return;
	}

	uint32_t delaySeconds = static_cast<uint32_t>(obs_data_get_int(data, "delay_seconds"));
	if (delaySeconds == 0 && !obs_data_has_user_value(data, "delay_seconds"))
		delaySeconds = 5;
	uint32_t minResolutionHeight = static_cast<uint32_t>(obs_data_get_int(data, "min_resolution_height"));
	if (minResolutionHeight == 0)
		minResolutionHeight = 720;
	const char *liveScene = obs_data_get_string(data, "live_scene");
	const char *loadingScene = obs_data_get_string(data, "loading_scene");

	controller.LoadSettings(delaySeconds, minResolutionHeight, liveScene ? liveScene : "",
				loadingScene ? loadingScene : "");
	obs_data_release(data);

	TRIGGLOW_LOG_INFO(kComponent, "settings loaded (delay=%us, min_res=%up, live=\"%s\", loading=\"%s\")",
			  delaySeconds, minResolutionHeight, liveScene ? liveScene : "",
			  loadingScene ? loadingScene : "");
}

void SaveBufferSettings(const trigglow::BufferModeController &controller)
{
	char *rawPath = obs_module_get_config_path(obs_current_module(), kBufferSettingsFile);
	QString path = rawPath ? QString::fromUtf8(rawPath) : QString();
	bfree(rawPath);
	if (path.isEmpty())
		return;

	QFileInfo info(path);
	QDir().mkpath(info.absolutePath());

	auto snapshot = controller.SaveSettings();

	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "delay_seconds", snapshot.delaySeconds);
	obs_data_set_int(data, "min_resolution_height", snapshot.minResolutionHeight);
	obs_data_set_string(data, "live_scene", snapshot.liveSceneName.c_str());
	obs_data_set_string(data, "loading_scene", snapshot.loadingSceneName.c_str());
	obs_data_save_json(data, path.toUtf8().constData());
	obs_data_release(data);

	TRIGGLOW_LOG_INFO(kComponent, "settings saved to %s", path.toUtf8().constData());
}

} // namespace

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "loading Trigglow Dynamic Delay v%s", PLUGIN_VERSION);

#ifdef TRIGGLOW_HAVE_FFMPEG
	// v0.3.0 Phase 0 smoke test (see docs/ROADMAP.md) -- proves FFmpeg
	// actually links AND loads correctly at runtime inside a real OBS
	// process, not just that it compiles. A static-link mistake or a
	// missing/mismatched runtime DLL would otherwise only surface as an
	// opaque plugin-load failure with no clue why. No codec logic uses
	// FFmpeg yet; this is purely a "the dependency is wired up" check.
	obs_log(LOG_INFO, "FFmpeg linked: %s", av_version_info());
#endif

	// Registers the OBS filters that buffer mode orchestrates under the hood
	// (see obs-frontend-bridge.cpp's EnsureBufferWrapperScene /
	// EnsureAudioDelayFilters). The user never adds either manually via the
	// Filters dialog -- see video-delay-filter.hpp / audio-delay-filter.hpp.
	trigglow::VideoDelayFilter::Register();
	trigglow::AudioDelayFilter::Register();

	g_state = new PluginState();
	g_state->bufferController = std::make_unique<trigglow::BufferModeController>(g_state->bridge);

	LoadBufferSettings(*g_state->bufferController);

	g_state->hotkeys = std::make_unique<trigglow::DelayHotkeys>(*g_state->bufferController);
	g_state->hotkeys->Init();

	auto *dock = new trigglow::TrigglowDelayDock(*g_state->bufferController);
	g_state->dock = dock;
	g_state->bridge.AddDock(kDockId, kDockTitle, dock);

	TRIGGLOW_LOG_INFO(kComponent, "loaded successfully");
	return true;
}

void obs_module_unload(void)
{
	if (!g_state)
		return;

	SaveBufferSettings(*g_state->bufferController);

	// Hotkeys must be torn down before we free the controller they point
	// back into.
	if (g_state->hotkeys)
		g_state->hotkeys->Shutdown();
	g_state->bridge.Shutdown();

	// g_state->dock is intentionally NOT deleted here — see PluginState's
	// comment: OBS's dock system owns it after AddDock().
	delete g_state;
	g_state = nullptr;

	obs_log(LOG_INFO, "unloaded");
}
