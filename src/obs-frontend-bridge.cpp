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

#include "obs-frontend-bridge.hpp"
#include "logging.hpp"

extern "C" {
#include <obs.h>
#include <obs-frontend-api.h>
}

namespace {
constexpr const char *kComponent = "frontend-bridge";

// OBS_OUTPUT_DELAY_PRESERVE (1 << 0): keep buffered-but-unsent frames instead
// of dropping them when the delay shrinks or the connection hiccups. Real
// flag, defined in libobs/obs.h. We mirror it here so callers don't need to
// include obs.h just to pass this bit through.
constexpr uint32_t kObsOutputDelayPreserve = 1u << 0;
} // namespace

namespace trigglow {

ObsFrontendBridge::~ObsFrontendBridge()
{
	Shutdown();
}

void ObsFrontendBridge::Init(FrontendEventHandler handler)
{
	if (initialized_) {
		TRIGGLOW_LOG_WARN(kComponent, "Init() called twice, ignoring second call");
		return;
	}
	handler_ = std::move(handler);
	obs_frontend_add_event_callback(&ObsFrontendBridge::FrontendEventCallback, this);
	initialized_ = true;
}

void ObsFrontendBridge::Shutdown()
{
	if (!initialized_)
		return;
	obs_frontend_remove_event_callback(&ObsFrontendBridge::FrontendEventCallback, this);
	initialized_ = false;
}

bool ObsFrontendBridge::IsStreamingActive() const
{
	return obs_frontend_streaming_active();
}

void ObsFrontendBridge::RequestStreamingStart() const
{
	obs_frontend_streaming_start();
}

void ObsFrontendBridge::RequestStreamingStop() const
{
	obs_frontend_streaming_stop();
}

bool ObsFrontendBridge::ApplyConfiguredDelay(uint32_t delaySeconds, bool preserveOnDisconnect,
					     std::string &outError) const
{
	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output) {
		// OBS only creates the streaming output once a service is configured
		// under Ajustes -> Emision. This is the expected state for anyone
		// testing the plugin via local recording instead of a real stream
		// (Recording/Replay Buffer never had a delay, so there is nothing to
		// arm) - not a plugin bug, so the message says so explicitly instead
		// of just "Error".
		outError = "El delay solo aplica al streaming (no a grabar en local): configura un "
			   "servicio en Ajustes -> Emision para poder activarlo. Si solo estas "
			   "probando el plugin grabando en local, esto es normal.";
		return false;
	}

	uint32_t flags = preserveOnDisconnect ? kObsOutputDelayPreserve : 0;
	obs_output_set_delay(output, delaySeconds, flags);
	obs_output_release(output);

	TRIGGLOW_LOG_INFO(kComponent, "configured delay set to %us (preserve=%s)", delaySeconds,
			  preserveOnDisconnect ? "on" : "off");
	return true;
}

uint32_t ObsFrontendBridge::GetConfiguredDelaySeconds() const
{
	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output)
		return 0;
	uint32_t seconds = obs_output_get_delay(output);
	obs_output_release(output);
	return seconds;
}

uint32_t ObsFrontendBridge::GetActiveDelaySeconds() const
{
	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output)
		return 0;
	uint32_t seconds = obs_output_get_active_delay(output);
	obs_output_release(output);
	return seconds;
}

std::vector<std::string> ObsFrontendBridge::ListSceneNames() const
{
	std::vector<std::string> names;
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		const char *name = obs_source_get_name(scenes.sources.array[i]);
		if (name)
			names.emplace_back(name);
	}
	obs_frontend_source_list_free(&scenes);
	return names;
}

std::string ObsFrontendBridge::GetCurrentSceneName() const
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		return {};
	const char *name = obs_source_get_name(scene);
	std::string result = name ? name : "";
	obs_source_release(scene);
	return result;
}

bool ObsFrontendBridge::SetCurrentSceneByName(const std::string &name) const
{
	if (name.empty())
		return false;

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	bool found = false;
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *src = scenes.sources.array[i];
		const char *srcName = obs_source_get_name(src);
		if (srcName && name == srcName) {
			obs_frontend_set_current_scene(src);
			found = true;
			break;
		}
	}
	obs_frontend_source_list_free(&scenes);
	return found;
}

void ObsFrontendBridge::AddDock(const char *id, const char *title, void *qWidget) const
{
	// obs_frontend_add_dock_by_id() takes ownership of the widget's lifetime
	// within OBS's dock system once this call returns true. We must NOT
	// delete qWidget ourselves afterward.
	if (!obs_frontend_add_dock_by_id(id, title, qWidget)) {
		TRIGGLOW_LOG_ERROR(kComponent, "obs_frontend_add_dock_by_id(\"%s\") failed", id);
	}
}

void ObsFrontendBridge::FrontendEventCallback(enum obs_frontend_event event, void *privateData)
{
	auto *self = static_cast<ObsFrontendBridge *>(privateData);
	if (!self || !self->handler_)
		return;

	FrontendEvent mapped = FrontendEvent::Other;
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		mapped = FrontendEvent::StreamingStarting;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		mapped = FrontendEvent::StreamingStarted;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		mapped = FrontendEvent::StreamingStopping;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		mapped = FrontendEvent::StreamingStopped;
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		mapped = FrontendEvent::FinishedLoading;
		break;
	default:
		return; // Don't spam the handler with events we don't act on.
	}

	self->handler_(mapped);
}

} // namespace trigglow
