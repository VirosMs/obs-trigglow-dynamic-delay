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
#include "audio-delay-filter.hpp"
#include "logging.hpp"
#include "video-delay-filter.hpp"

#include <cstring>

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
	// Defensive: normally BufferModeController's Enable()/Disable() pairing
	// already released this before unload, but don't leave a dangling main
	// render callback + source ref behind if the plugin is unloaded mid-Filling.
	if (liveSceneRenderSource_)
		ReleaseLiveSceneRendering({});
	if (liveSceneRenderTarget_) {
		// Unlike a source's own destroy callback, Shutdown() runs on the
		// plugin-unload path with no graphics context already entered for
		// us — gs_texrender_destroy() needs one explicitly.
		obs_enter_graphics();
		gs_texrender_destroy(liveSceneRenderTarget_);
		obs_leave_graphics();
		liveSceneRenderTarget_ = nullptr;
	}

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

namespace {
// Fixed, well-known names so EnsureBufferWrapperScene()/FindBufferFilter()
// can relocate the same wrapper scene and filter instance across calls
// without tracking any extra IDs themselves.
//
// kBufferFilterInstanceName is DELIBERATELY NOT "Trigglow Video Delay
// Buffer" (VideoDelayFilter::GetName()'s return value, i.e. the name OBS's
// own Filters dialog would default to if a user manually adds this filter
// type, which is exactly what happened during this plugin's own Phase 1
// testing on "Multimedia"). Found live, 2026-08-25: OBS enforces globally
// unique SOURCE names (filters are sources), so obs_source_create() with
// that name silently produced a differently-named object whenever a
// same-named filter already existed anywhere else in the scene collection
// -- every FindBufferFilter() lookup by the original name then failed,
// SetBufferFilterEnabled/SetBufferFilterDelaySeconds silently no-op'd
// (neither had any failure logging until today), the filter stayed
// permanently disabled at its creation default, and every Enable() press
// created ANOTHER orphaned duplicate instead of reusing the existing one.
// Using a name no manual "Add Filter" would ever produce avoids the
// collision entirely, regardless of what else the user has attached
// elsewhere.
constexpr const char *kBufferWrapperSceneName = "Trigglow Delay Buffer (no tocar)";
constexpr const char *kBufferFilterInstanceName = "Trigglow Buffer Mode Delay (auto, no tocar)";

// Prefix for AudioDelayFilter instance names -- one per audio-capable leaf
// source inside the live scene, so each needs its own globally-unique name
// (see the name-collision comment on kBufferFilterInstanceName above; same
// constraint applies here). Suffixing with the leaf source's own name keeps
// it both unique and recognizable if a curious user opens that source's
// Filters dialog.
constexpr const char *kAudioDelayFilterPrefix = "Trigglow Audio Delay (auto, no tocar) - ";
} // namespace

obs_source_t *ObsFrontendBridge::FindBufferFilter(const std::string &liveSceneName) const
{
	// obs_source_get_filter_by_name() and obs_scene_from_source() are plain
	// accessors into structures already owned elsewhere (no "increments the
	// reference counter, use obs_source_release" doc comment, unlike
	// obs_get_source_by_name()) - the only owned reference in this function
	// is wrapperSource itself.
	obs_source_t *wrapperSource = obs_get_source_by_name(kBufferWrapperSceneName);
	if (!wrapperSource) {
		TRIGGLOW_LOG_WARN(kComponent, "FindBufferFilter: obs_get_source_by_name(\"%s\") found nothing",
				  kBufferWrapperSceneName);
		return nullptr;
	}

	obs_scene_t *wrapperScene = obs_scene_from_source(wrapperSource);
	if (!wrapperScene) {
		TRIGGLOW_LOG_WARN(kComponent, "FindBufferFilter: \"%s\" exists but isn't a scene (%p)",
				  kBufferWrapperSceneName, static_cast<void *>(wrapperSource));
		obs_source_release(wrapperSource);
		return nullptr;
	}

	struct FindCtx {
		obs_source_t *itemSource = nullptr;
	} ctx;
	obs_scene_enum_items(
		wrapperScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			static_cast<FindCtx *>(param)->itemSource = obs_sceneitem_get_source(item);
			return false; // We only ever add one item (the live scene); stop immediately.
		},
		&ctx);

	if (!ctx.itemSource) {
		TRIGGLOW_LOG_WARN(kComponent, "FindBufferFilter: wrapper scene %p has no scene-items",
				  static_cast<void *>(wrapperSource));
		obs_source_release(wrapperSource);
		return nullptr;
	}

	obs_source_t *filter = obs_source_get_filter_by_name(ctx.itemSource, kBufferFilterInstanceName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "FindBufferFilter: wrapper's item source %p (\"%s\") has no filter named \"%s\"",
				  static_cast<void *>(ctx.itemSource), obs_source_get_name(ctx.itemSource),
				  kBufferFilterInstanceName);
	}

	obs_source_release(wrapperSource);
	(void)liveSceneName; // Reserved: current design assumes a single live scene at a time (see header comment).
	return filter;
}

bool ObsFrontendBridge::EnsureBufferWrapperScene(const std::string &liveSceneName) const
{
	if (liveSceneName.empty())
		return false;

	if (FindBufferFilter(liveSceneName) != nullptr)
		return true; // Already fully set up.

	obs_source_t *liveSource = obs_get_source_by_name(liveSceneName.c_str());
	if (!liveSource) {
		TRIGGLOW_LOG_WARN(kComponent, "buffer mode: live scene \"%s\" not found", liveSceneName.c_str());
		return false;
	}

	obs_source_t *wrapperSource = obs_get_source_by_name(kBufferWrapperSceneName);
	obs_scene_t *wrapperScene = wrapperSource ? obs_scene_from_source(wrapperSource) : nullptr;
	bool wrapperWasFound = wrapperSource != nullptr;
	if (!wrapperSource) {
		wrapperScene = obs_scene_create(kBufferWrapperSceneName);
		wrapperSource = wrapperScene ? obs_scene_get_source(wrapperScene) : nullptr;
	}
	if (!wrapperScene || !wrapperSource) {
		TRIGGLOW_LOG_ERROR(kComponent, "buffer mode: could not create/find the wrapper scene");
		obs_source_release(liveSource);
		return false;
	}
	TRIGGLOW_LOG_INFO(kComponent, "buffer mode: wrapper scene %s, source=%p liveSource=%p",
			  wrapperWasFound ? "found existing" : "created new", static_cast<void *>(wrapperSource),
			  static_cast<void *>(liveSource));

	// Reuse an existing scene-item if the wrapper already has one (e.g. from
	// a previous session with a different live scene selected), otherwise
	// add the live scene now. NOTE: this does NOT duplicate liveSource -
	// obs_sceneitem_get_source() on the result returns the SAME object, see
	// the header comment on this function.
	obs_source_t *itemSource = nullptr;
	struct FindCtx {
		obs_source_t *itemSource = nullptr;
	} ctx;
	obs_scene_enum_items(
		wrapperScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			static_cast<FindCtx *>(param)->itemSource = obs_sceneitem_get_source(item);
			return false;
		},
		&ctx);
	itemSource = ctx.itemSource;

	if (!itemSource) {
		obs_sceneitem_t *newItem = obs_scene_add(wrapperScene, liveSource);
		itemSource = newItem ? obs_sceneitem_get_source(newItem) : nullptr;
	}

	obs_source_release(liveSource);

	if (!itemSource) {
		TRIGGLOW_LOG_ERROR(kComponent, "buffer mode: could not add \"%s\" to the wrapper scene",
				   liveSceneName.c_str());
		obs_source_release(wrapperSource);
		return false;
	}
	TRIGGLOW_LOG_INFO(kComponent,
			  "buffer mode: itemSource=%p (name=\"%s\", filters.num check via get_filter_by_name next)",
			  static_cast<void *>(itemSource), obs_source_get_name(itemSource));

	bool ok = true;
	obs_source_t *existingFilter = obs_source_get_filter_by_name(itemSource, kBufferFilterInstanceName);
	if (!existingFilter) {
		obs_data_t *filterSettings = obs_data_create();
		obs_source_t *filter =
			obs_source_create(VideoDelayFilter::Id(), kBufferFilterInstanceName, filterSettings, nullptr);
		obs_data_release(filterSettings);
		if (filter) {
			// Defensive: OBS enforces globally unique source names, so a
			// name collision with something the user created elsewhere
			// would make obs_source_create() silently return an object
			// under a DIFFERENT actual name -- exactly what caused this
			// whole lookup chain to fail live, 2026-08-24/25, before
			// kBufferFilterInstanceName was changed to something a manual
			// "Add Filter" could never produce. Kept as a loud safety net
			// in case that ever happens again for some other reason.
			const char *actualName = obs_source_get_name(filter);
			if (!actualName || strcmp(actualName, kBufferFilterInstanceName) != 0) {
				TRIGGLOW_LOG_ERROR(kComponent,
						   "buffer mode: created filter got renamed to \"%s\" (wanted \"%s\") "
						   "-- likely a name collision with something else in this scene "
						   "collection; buffer mode will NOT be able to find it again",
						   actualName ? actualName : "(null)", kBufferFilterInstanceName);
			}
			obs_source_filter_add(itemSource, filter);
			// Disabled by default: this filter stays permanently attached
			// to the live scene's OWN source object (see header comment),
			// so it must be inert unless SetBufferFilterEnabled(true) is
			// called for an actively-showing wrapper.
			obs_source_set_enabled(filter, false);
			obs_source_release(filter);
			TRIGGLOW_LOG_INFO(kComponent, "buffer mode: attached delay filter to \"%s\"",
					  liveSceneName.c_str());
		} else {
			TRIGGLOW_LOG_ERROR(kComponent, "buffer mode: obs_source_create for the delay filter failed");
			ok = false;
		}
	}

	obs_source_release(wrapperSource);
	return ok;
}

bool ObsFrontendBridge::SetBufferFilterEnabled(const std::string &liveSceneName, bool enabled) const
{
	obs_source_t *filter = FindBufferFilter(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterEnabled(%s): FindBufferFilter returned null, no-op",
				  enabled ? "true" : "false");
		return false;
	}
	obs_source_set_enabled(filter, enabled);
	TRIGGLOW_LOG_INFO(kComponent, "SetBufferFilterEnabled(%s) applied to filter %p", enabled ? "true" : "false",
			  static_cast<void *>(filter));
	return true;
}

bool ObsFrontendBridge::SetBufferFilterDelaySeconds(const std::string &liveSceneName, uint32_t seconds) const
{
	obs_source_t *filter = FindBufferFilter(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterDelaySeconds(%us): FindBufferFilter returned null, no-op",
				  seconds);
		return false;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "delay_seconds", seconds);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	return true;
}

bool ObsFrontendBridge::SetBufferFilterMinResolutionHeight(const std::string &liveSceneName,
							   uint32_t heightPixels) const
{
	obs_source_t *filter = FindBufferFilter(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "SetBufferFilterMinResolutionHeight(%u): FindBufferFilter returned null, no-op",
				  heightPixels);
		return false;
	}
	// obs_source_update() merges onto the filter's existing settings
	// (obs_data_apply), so this doesn't disturb delay_seconds.
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "min_resolution_height", heightPixels);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	return true;
}

bool ObsFrontendBridge::ShowBufferWrapperScene() const
{
	return SetCurrentSceneByName(kBufferWrapperSceneName);
}

std::vector<obs_source_t *> ObsFrontendBridge::GetAudioCapableChildren(const std::string &liveSceneName) const
{
	std::vector<obs_source_t *> result;

	obs_source_t *liveSource = obs_get_source_by_name(liveSceneName.c_str());
	if (!liveSource)
		return result;

	obs_scene_t *liveScene = obs_scene_from_source(liveSource);
	if (liveScene) {
		obs_scene_enum_items(
			liveScene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
				obs_source_t *itemSource = obs_sceneitem_get_source(item);
				if (itemSource && (obs_source_get_output_flags(itemSource) & OBS_SOURCE_AUDIO) != 0)
					static_cast<std::vector<obs_source_t *> *>(param)->push_back(itemSource);
				return true; // Keep enumerating -- unlike FindBufferFilter, we want ALL matches.
			},
			&result);
	}

	obs_source_release(liveSource);
	return result;
}

bool ObsFrontendBridge::EnsureAudioDelayFilters(const std::string &liveSceneName) const
{
	auto children = GetAudioCapableChildren(liveSceneName);
	if (children.empty()) {
		// Not a warning: plenty of scenes legitimately have no direct
		// audio-capable children (audio routed some other way, or a
		// video-only scene) -- buffer mode still works fine for video only.
		TRIGGLOW_LOG_INFO(kComponent, "audio delay: \"%s\" has no direct audio-capable children",
				  liveSceneName.c_str());
		return false;
	}

	for (obs_source_t *child : children) {
		const char *childName = obs_source_get_name(child);
		std::string filterName = std::string(kAudioDelayFilterPrefix) + (childName ? childName : "?");

		if (obs_source_get_filter_by_name(child, filterName.c_str()))
			continue; // Already attached.

		obs_data_t *filterSettings = obs_data_create();
		obs_source_t *filter =
			obs_source_create(AudioDelayFilter::Id(), filterName.c_str(), filterSettings, nullptr);
		obs_data_release(filterSettings);
		if (!filter) {
			TRIGGLOW_LOG_ERROR(kComponent, "audio delay: obs_source_create failed for \"%s\"",
					   filterName.c_str());
			continue;
		}

		const char *actualName = obs_source_get_name(filter);
		if (!actualName || filterName != actualName) {
			// Same defensive check as EnsureBufferWrapperScene's video
			// filter creation -- see that comment for the full story on
			// why a name mismatch here means it'll never be found again.
			TRIGGLOW_LOG_ERROR(kComponent,
					   "audio delay: created filter got renamed to \"%s\" (wanted \"%s\") -- "
					   "name collision, this instance will not be manageable",
					   actualName ? actualName : "(null)", filterName.c_str());
		}

		obs_source_filter_add(child, filter);
		obs_source_set_enabled(filter, false); // Disabled by default, same as the video filter.
		obs_source_release(filter);
		TRIGGLOW_LOG_INFO(kComponent, "audio delay: attached \"%s\" to \"%s\"", filterName.c_str(),
				  childName ? childName : "?");
	}

	return true;
}

bool ObsFrontendBridge::SetAudioDelayFiltersEnabled(const std::string &liveSceneName, bool enabled) const
{
	auto children = GetAudioCapableChildren(liveSceneName);
	bool foundAny = false;

	for (obs_source_t *child : children) {
		const char *childName = obs_source_get_name(child);
		std::string filterName = std::string(kAudioDelayFilterPrefix) + (childName ? childName : "?");
		obs_source_t *filter = obs_source_get_filter_by_name(child, filterName.c_str());
		if (!filter)
			continue;
		obs_source_set_enabled(filter, enabled);
		foundAny = true;
	}

	TRIGGLOW_LOG_INFO(kComponent, "audio delay: SetAudioDelayFiltersEnabled(%s) applied to %s",
			  enabled ? "true" : "false", foundAny ? "at least one filter" : "no filters (none found)");
	return foundAny;
}

bool ObsFrontendBridge::SetAudioDelayFiltersDelaySeconds(const std::string &liveSceneName, uint32_t seconds) const
{
	auto children = GetAudioCapableChildren(liveSceneName);
	bool foundAny = false;

	for (obs_source_t *child : children) {
		const char *childName = obs_source_get_name(child);
		std::string filterName = std::string(kAudioDelayFilterPrefix) + (childName ? childName : "?");
		obs_source_t *filter = obs_source_get_filter_by_name(child, filterName.c_str());
		if (!filter)
			continue;
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "delay_seconds", seconds);
		obs_source_update(filter, settings);
		obs_data_release(settings);
		foundAny = true;
	}

	return foundAny;
}

bool ObsFrontendBridge::AcquireLiveSceneRendering(const std::string &liveSceneName)
{
	if (liveSceneRenderSource_) {
		TRIGGLOW_LOG_WARN(kComponent, "AcquireLiveSceneRendering called while already holding one, ignoring");
		return false;
	}

	// obs_get_source_by_name() gives us the +1 ref this function keeps
	// until ReleaseLiveSceneRendering() releases it.
	obs_source_t *liveSource = obs_get_source_by_name(liveSceneName.c_str());
	if (!liveSource)
		return false;

	liveSceneRenderSource_ = liveSource;
	// inc_showing ONLY -- not inc_active. Tried adding inc_active on top
	// (hoping it would also keep audio flowing) TWICE now (2026-08-25) and
	// both times it broke video with no visible error: our draw callback
	// still fired (confirmed via loggedFirstRenderCallback_ below), but
	// VideoDelayFilter::Render() never ran at all -- not even its
	// null-target/zero-size diagnostic branches, meaning render_video()'s
	// internal reentrancy guard (obs-source.c's `rendering_filter` flag) is
	// the likely culprit: making the scene "active" (not just "showing")
	// appears to make some OTHER part of libobs also try to render it the
	// same frame, and that guard -- a plain bool, not a per-call-stack
	// flag -- ends up skipping our filter chain entirely, falling back to
	// obs_source_main_render() which renders the scene's raw children with
	// no filters applied. inc_showing alone was proven working before any
	// of this; reverted to just that rather than keep stacking primitives
	// we can't fully verify from the public headers alone. Audio's own
	// keep-alive needs, if any, are a separate problem to solve without
	// touching this proven-working video path again.
	obs_source_inc_showing(liveSceneRenderSource_);
	obs_add_main_render_callback(&ObsFrontendBridge::RenderLiveSceneCallback, this);
	loggedFirstRenderCallback_ = false;
	TRIGGLOW_LOG_INFO(kComponent, "buffer mode: keep-alive acquired for \"%s\" (source=%p, filters.num=%zu)",
			  liveSceneName.c_str(), static_cast<void *>(liveSceneRenderSource_),
			  obs_source_filter_count(liveSceneRenderSource_));

	return true;
}

bool ObsFrontendBridge::ReleaseLiveSceneRendering(const std::string & /*liveSceneName*/)
{
	if (!liveSceneRenderSource_)
		return false;

	obs_remove_main_render_callback(&ObsFrontendBridge::RenderLiveSceneCallback, this);
	obs_source_dec_showing(liveSceneRenderSource_);
	TRIGGLOW_LOG_INFO(kComponent, "buffer mode: keep-alive released");
	obs_source_release(liveSceneRenderSource_);
	liveSceneRenderSource_ = nullptr;
	return true;
}

void ObsFrontendBridge::RenderLiveSceneCallback(void *param, uint32_t /*cx*/, uint32_t /*cy*/)
{
	auto *self = static_cast<ObsFrontendBridge *>(param);
	if (!self->liveSceneRenderSource_)
		return;

	uint32_t width = obs_source_get_base_width(self->liveSceneRenderSource_);
	uint32_t height = obs_source_get_base_height(self->liveSceneRenderSource_);
	if (width == 0 || height == 0)
		return;

	if (!self->loggedFirstRenderCallback_) {
		TRIGGLOW_LOG_INFO(kComponent, "buffer mode: RenderLiveSceneCallback firing (%ux%u)", width, height);
		self->loggedFirstRenderCallback_ = true;
	}

	if (!self->liveSceneRenderTarget_)
		self->liveSceneRenderTarget_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	// Render into our own throwaway texture, NOT the real Program output —
	// see the header comment on AcquireLiveSceneRendering for why (draw
	// callbacks run with the render target already pointed at OBS's actual
	// output texture).
	gs_texrender_reset(self->liveSceneRenderTarget_);
	if (gs_texrender_begin(self->liveSceneRenderTarget_, width, height)) {
		struct vec4 clearColor = {};
		gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
		gs_matrix_push();
		gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
		obs_source_video_render(self->liveSceneRenderSource_);
		gs_matrix_pop();
		gs_texrender_end(self->liveSceneRenderTarget_);
	}
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

uint64_t ObsFrontendBridge::GetBufferBudgetBytes() const
{
	return VideoDelayFilter::GetBufferBudgetBytes();
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
