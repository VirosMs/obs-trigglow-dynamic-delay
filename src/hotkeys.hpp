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

#include "delay-controller.hpp"

extern "C" {
#include <obs.h> // obs_hotkey_id, obs_hotkey_t, obs_hotkey_func — core libobs, not the frontend API.
}

// Registers the plugin's 3 native OBS hotkeys via obs_hotkey_register_frontend
// (libobs/obs-hotkey.h). Registered hotkeys show up in OBS's own
// Settings -> Hotkeys list under this plugin's display name, exactly like
// any other native OBS hotkey (Start Streaming, Mute, etc). This is what
// makes the Stream Deck flow work with zero custom integration: Stream Deck's
// own "Hotkey" / "System Hotkey" action can trigger ANY OBS hotkey by name,
// this plugin's included, without a dedicated Stream Deck plugin — see
// docs/STREAM_DECK.md.
//
// Frontend hotkeys registered this way are automatically persisted by OBS's
// own hotkey config (keyed by the string name passed to
// obs_hotkey_register_frontend) — this plugin does not need to save/load
// key bindings itself, only unregister them cleanly on unload.
namespace trigglow {

class DelayHotkeys {
public:
	explicit DelayHotkeys(DelayController &controller);
	~DelayHotkeys();

	DelayHotkeys(const DelayHotkeys &) = delete;
	DelayHotkeys &operator=(const DelayHotkeys &) = delete;

	// Registers all 3 hotkeys. Call once from obs_module_load().
	void Init();

	// Unregisters all 3 hotkeys. Call from obs_module_unload().
	void Shutdown();

private:
	static void ToggleCallback(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void EnableCallback(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void DisableCallback(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

	DelayController &controller_;
	obs_hotkey_id toggleId_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id enableId_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id disableId_ = OBS_INVALID_HOTKEY_ID;
};

} // namespace trigglow
