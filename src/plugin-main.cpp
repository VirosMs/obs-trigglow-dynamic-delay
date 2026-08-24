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

#include "delay-controller.hpp"
#include "hotkeys.hpp"
#include "logging.hpp"
#include "obs-frontend-bridge.hpp"
#include "settings-ui.hpp"
#include "video-delay-filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kComponent = "plugin-main";
constexpr const char *kDockId = "trigglow_dynamic_delay_dock";
constexpr const char *kDockTitle = "Trigglow Dynamic Delay";
constexpr const char *kSettingsFile = "settings.json";

// All plugin-lifetime state lives in one place, constructed in
// obs_module_load() and torn down in obs_module_unload(). No globals spread
// across the other files — every other module gets what it needs by
// reference/pointer from here.
struct PluginState {
	trigglow::ObsFrontendBridge bridge;
	std::unique_ptr<trigglow::DelayController> controller;
	std::unique_ptr<trigglow::DelayHotkeys> hotkeys;
	// Not owned past AddDock(): OBS's dock system takes ownership of the
	// QWidget once obs_frontend_add_dock_by_id() returns true. Kept here
	// only as a non-owning reference for potential future use (e.g. forcing
	// a refresh), never deleted from this side.
	trigglow::TrigglowDelayDock *dock = nullptr;
};

PluginState *g_state = nullptr;

QString SettingsFilePath()
{
	char *path = obs_module_get_config_path(obs_current_module(), kSettingsFile);
	QString qpath = path ? QString::fromUtf8(path) : QString();
	bfree(path);
	return qpath;
}

void LoadSettings(trigglow::DelayController &controller)
{
	QString path = SettingsFilePath();
	if (path.isEmpty()) {
		TRIGGLOW_LOG_WARN(kComponent, "could not resolve settings path, using defaults");
		return;
	}

	obs_data_t *data = obs_data_create_from_json_file(path.toUtf8().constData());
	if (!data) {
		TRIGGLOW_LOG_INFO(kComponent, "no existing settings file at %s, using defaults",
				  path.toUtf8().constData());
		return;
	}

	uint32_t delaySeconds = static_cast<uint32_t>(obs_data_get_int(data, "delay_seconds"));
	bool safeMode = obs_data_get_bool(data, "safe_mode");
	bool wasEnabled = obs_data_get_bool(data, "enabled");
	const char *reconnectScene = obs_data_get_string(data, "reconnect_scene");

	// Fresh installs (empty obs_data_t defaults) would give us 0/false/false;
	// treat a totally-empty file as "use built-in defaults" instead of a
	// literal 0-second delay.
	if (delaySeconds == 0 && !obs_data_has_user_value(data, "delay_seconds"))
		delaySeconds = 10;

	controller.LoadSettings(delaySeconds, safeMode, wasEnabled, reconnectScene ? reconnectScene : "");
	obs_data_release(data);

	TRIGGLOW_LOG_INFO(kComponent, "settings loaded (delay=%us, safeMode=%s, enabled=%s, reconnectScene=%s)",
			  delaySeconds, safeMode ? "on" : "off", wasEnabled ? "yes" : "no",
			  (reconnectScene && *reconnectScene) ? reconnectScene : "(none)");
}

void SaveSettings(const trigglow::DelayController &controller)
{
	QString path = SettingsFilePath();
	if (path.isEmpty())
		return;

	// obs_module_get_config_path() does not guarantee the directory exists
	// yet on a fresh install; ensure it does before writing.
	QFileInfo info(path);
	QDir().mkpath(info.absolutePath());

	auto snapshot = controller.SaveSettings();

	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "delay_seconds", snapshot.delaySeconds);
	obs_data_set_bool(data, "safe_mode", snapshot.safeMode);
	obs_data_set_bool(data, "enabled", snapshot.enabled);
	obs_data_set_string(data, "reconnect_scene", snapshot.reconnectSceneName.c_str());
	obs_data_save_json(data, path.toUtf8().constData());
	obs_data_release(data);

	TRIGGLOW_LOG_INFO(kComponent, "settings saved to %s", path.toUtf8().constData());
}

} // namespace

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "loading Trigglow Dynamic Delay v%s", PLUGIN_VERSION);

	// Phase 1 (issue #173, video-only): a standalone OBS filter the user
	// attaches manually via the Filters dialog. Not wired into
	// DelayController/the dock yet -- see video-delay-filter.hpp.
	trigglow::VideoDelayFilter::Register();

	g_state = new PluginState();
	g_state->controller = std::make_unique<trigglow::DelayController>(g_state->bridge);
	g_state->controller->Init();

	LoadSettings(*g_state->controller);

	g_state->hotkeys = std::make_unique<trigglow::DelayHotkeys>(*g_state->controller);
	g_state->hotkeys->Init();

	auto *dock = new trigglow::TrigglowDelayDock(*g_state->controller);
	g_state->dock = dock;
	g_state->bridge.AddDock(kDockId, kDockTitle, dock);

	TRIGGLOW_LOG_INFO(kComponent, "loaded successfully");
	return true;
}

void obs_module_unload(void)
{
	if (!g_state)
		return;

	SaveSettings(*g_state->controller);

	// Hotkeys and the frontend event callback must be torn down before we
	// free the controller they point back into.
	if (g_state->hotkeys)
		g_state->hotkeys->Shutdown();
	g_state->bridge.Shutdown();

	// g_state->dock is intentionally NOT deleted here — see PluginState's
	// comment: OBS's dock system owns it after AddDock().
	delete g_state;
	g_state = nullptr;

	obs_log(LOG_INFO, "unloaded");
}
