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

#include "hotkeys.hpp"
#include "logging.hpp"

namespace trigglow {

namespace {
constexpr const char *kComponent = "hotkeys";

// obs_hotkey_register_frontend callbacks fire on OBS's internal hotkey
// thread, never the Qt UI thread — that's true for a real key press AND for
// a Stream Deck press via System > Hotkey, which is this plugin's advertised
// main control path (see docs/SPEC.md). Calling into BufferModeController (and,
// through its status callback, straight into the Qt dock's widgets) from
// that thread is a data race / Qt thread-affinity violation. Route the
// actual call through OBS's own UI task queue instead.
void ToggleOnUiThread(void *param)
{
	static_cast<BufferModeController *>(param)->Toggle();
}

void EnableOnUiThread(void *param)
{
	static_cast<BufferModeController *>(param)->Enable();
}

void DisableOnUiThread(void *param)
{
	static_cast<BufferModeController *>(param)->Disable();
}
} // namespace

DelayHotkeys::DelayHotkeys(BufferModeController &controller) : controller_(controller) {}

DelayHotkeys::~DelayHotkeys()
{
	Shutdown();
}

void DelayHotkeys::Init()
{
	toggleId_ = obs_hotkey_register_frontend("trigglow_dynamic_delay.toggle", "Trigglow: Toggle Dynamic Delay",
						 &DelayHotkeys::ToggleCallback, this);
	enableId_ = obs_hotkey_register_frontend("trigglow_dynamic_delay.enable", "Trigglow: Enable Dynamic Delay",
						 &DelayHotkeys::EnableCallback, this);
	disableId_ = obs_hotkey_register_frontend("trigglow_dynamic_delay.disable", "Trigglow: Disable Dynamic Delay",
						  &DelayHotkeys::DisableCallback, this);

	TRIGGLOW_LOG_INFO(kComponent, "registered 3 hotkeys (toggle/enable/disable) — assign them in "
				      "Settings > Hotkeys");
}

void DelayHotkeys::Shutdown()
{
	if (toggleId_ != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(toggleId_);
		toggleId_ = OBS_INVALID_HOTKEY_ID;
	}
	if (enableId_ != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(enableId_);
		enableId_ = OBS_INVALID_HOTKEY_ID;
	}
	if (disableId_ != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(disableId_);
		disableId_ = OBS_INVALID_HOTKEY_ID;
	}
}

void DelayHotkeys::ToggleCallback(void *data, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
	// Hotkey callbacks fire on both key-down (pressed=true) and key-up
	// (pressed=false); we only want to act once, on press, exactly like
	// OBS's own built-in hotkeys (Start/Stop Streaming, Mute, etc).
	if (!pressed)
		return;
	auto *self = static_cast<DelayHotkeys *>(data);
	obs_queue_task(OBS_TASK_UI, ToggleOnUiThread, &self->controller_, false);
}

void DelayHotkeys::EnableCallback(void *data, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
	if (!pressed)
		return;
	auto *self = static_cast<DelayHotkeys *>(data);
	obs_queue_task(OBS_TASK_UI, EnableOnUiThread, &self->controller_, false);
}

void DelayHotkeys::DisableCallback(void *data, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
	if (!pressed)
		return;
	auto *self = static_cast<DelayHotkeys *>(data);
	obs_queue_task(OBS_TASK_UI, DisableOnUiThread, &self->controller_, false);
}

} // namespace trigglow
