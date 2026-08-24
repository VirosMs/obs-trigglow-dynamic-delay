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

extern "C" {
#include <obs-frontend-api.h> // needed here too: obs_frontend_event is the exact callback param type
}

// obs-frontend-bridge.hpp/.cpp are the ONLY files in this plugin allowed to include
// <obs-frontend-api.h> / <obs.h> output functions directly. Every other file
// (delay-controller, settings-ui, hotkeys, plugin-main) talks to OBS only
// through this narrow interface. That keeps a single, auditable place to
// check against future changes to OBS's frontend/output API, and lets
// delay-controller stay unit-testable without a running OBS process.
//
// Ground truth for every function called here (verified 2026-08-20 against
// libobs source, not just docs — see docs/SPEC.md §2):
//   - obs_frontend_get_streaming_output() / obs_output_release()
//   - obs_output_set_delay() / obs_output_get_delay() / obs_output_get_active_delay()
//   - obs_frontend_streaming_start() / obs_frontend_streaming_stop() / obs_frontend_streaming_active()
//   - obs_frontend_add_event_callback() / obs_frontend_remove_event_callback()
//   - obs_frontend_add_dock_by_id()
//   - obs_frontend_get_scenes() / obs_frontend_source_list_free()
//   - obs_frontend_get_current_scene() / obs_frontend_set_current_scene()
//   - obs_source_get_name() / obs_source_release()
namespace trigglow {

// Mirrors the subset of obs_frontend_event we care about, so callers outside
// this file never need to include obs-frontend-api.h themselves.
enum class FrontendEvent {
	StreamingStarting,
	StreamingStarted,
	StreamingStopping,
	StreamingStopped,
	FinishedLoading,
	Other,
};

using FrontendEventHandler = std::function<void(FrontendEvent)>;

class ObsFrontendBridge {
public:
	ObsFrontendBridge() = default;
	~ObsFrontendBridge();

	ObsFrontendBridge(const ObsFrontendBridge &) = delete;
	ObsFrontendBridge &operator=(const ObsFrontendBridge &) = delete;

	// Registers this plugin's frontend event callback. Call once from
	// obs_module_load(). Safe to call only once per process lifetime.
	void Init(FrontendEventHandler handler);

	// Unregisters the event callback. Call from obs_module_unload().
	void Shutdown();

	bool IsStreamingActive() const;
	void RequestStreamingStart() const;
	void RequestStreamingStop() const;

	// Applies `delaySeconds` (and the "preserve on disconnect" flag) to the
	// current streaming output's *configured* delay. Per docs/SPEC.md §2,
	// this only changes what OBS will use the NEXT time the output starts —
	// it does not affect an output that is already running.
	//
	// Returns false and fills `outError` with a user-facing (Spanish)
	// message if there is currently no valid streaming output to configure.
	bool ApplyConfiguredDelay(uint32_t delaySeconds, bool preserveOnDisconnect, std::string &outError) const;

	// Currently configured delay (what will be used on next start), in seconds.
	// Returns 0 if there is no streaming output available right now.
	uint32_t GetConfiguredDelaySeconds() const;

	// Delay actually in effect on the running output right now, in seconds.
	// Returns 0 if the output isn't active or has no delay in effect.
	uint32_t GetActiveDelaySeconds() const;

	// Adds a Qt widget as a native OBS dock. `widget` must be a QWidget*;
	// passed as void* here so this header stays Qt-free and can be included
	// from delay-controller.hpp / hotkeys.hpp without pulling in Qt.
	void AddDock(const char *id, const char *title, void *qWidget) const;

	// --- Scene switching (optional "reconnect placeholder" scene, see
	// docs/product roadmap issue #173) ---

	// Names of every scene in the current scene collection, in OBS's own order.
	std::vector<std::string> ListSceneNames() const;

	// Name of the currently active (program) scene, or empty if none.
	std::string GetCurrentSceneName() const;

	// Switches the active scene by name. Returns false (no-op) if `name` is
	// empty or doesn't match any current scene — callers should treat that as
	// "couldn't switch," not a crash-worthy error.
	bool SetCurrentSceneByName(const std::string &name) const;

private:
	// Must match obs_frontend_event_cb exactly: void(*)(enum obs_frontend_event, void*).
	static void FrontendEventCallback(enum obs_frontend_event event, void *privateData);

	FrontendEventHandler handler_;
	bool initialized_ = false;
};

} // namespace trigglow
