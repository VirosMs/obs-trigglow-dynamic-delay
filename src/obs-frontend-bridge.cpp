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
#include <obs-module.h> // obs_module_file()
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
// Text source shown over the wrapper scene while the delay is Active. Kept
// after Disable (just hidden) so user restyling survives; same "no tocar"
// naming convention as the other auto-managed objects.
constexpr const char *kDelayOverlaySourceName = "Trigglow Delay Overlay Text (auto, no tocar)";
constexpr const char *kDelayBadgeSourceName = "Trigglow Delay Overlay Badge (auto, no tocar)";
// Names of earlier builds of the overlay (text only, then a full-size badge);
// removed on sight so users who tried them don't keep a stale second overlay.
constexpr const char *kLegacyOverlayNames[] = {"Trigglow Delay Overlay (auto, no tocar)",
					       "Trigglow Delay Text (auto, no tocar)",
					       "Trigglow Delay Badge (auto, no tocar)"};
// data/images/delay-badge.png is 480x144 (drawn at 2.5x the size it is shown,
// so it stays sharp); both items are scaled by kOverlayScale. The logo sits on
// the left of the badge, the text is centered in a 290x80 box to its right.
// Distance from the chosen canvas corner to the image edge. The badge PNG already has ~9px (at
// the shown size) of transparent glow margin, so 0 puts the visible frame ~9px from the screen edge.
constexpr float kOverlayMargin = 0.0f;
constexpr float kOverlayBadgeW = 480.0f;
constexpr float kOverlayBadgeH = 144.0f;
constexpr float kOverlayScale = 0.4f;
constexpr float kOverlayTextOffsetX = 160.0f; // In badge pixels, before scaling.
constexpr float kOverlayTextOffsetY = 32.0f;
constexpr int kOverlayTextW = 290;
constexpr int kOverlayTextH = 80;

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

	// Match the item BY NAME, not "the first item": the wrapper scene is
	// reused across sessions and can still hold a previously chosen live
	// scene (found live, 2026-10-04: live scene set to "In Game" but the
	// wrapper kept showing the older "Talk").
	struct FindCtx {
		const std::string *liveName;
		obs_source_t *itemSource = nullptr;
	} ctx{&liveSceneName};
	obs_scene_enum_items(
		wrapperScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *c = static_cast<FindCtx *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);
			const char *name = source ? obs_source_get_name(source) : nullptr;
			if (name && *c->liveName == name) {
				c->itemSource = source;
				return false;
			}
			return true;
		},
		&ctx);

	if (!ctx.itemSource) {
		TRIGGLOW_LOG_WARN(kComponent, "FindBufferFilter: wrapper scene %p has no item for live scene \"%s\"",
				  static_cast<void *>(wrapperSource), liveSceneName.c_str());
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

	// The wrapper scene is reused across sessions, so it may still contain a
	// live scene chosen earlier. Keep the item for the CURRENT live scene (this
	// does NOT duplicate liveSource -- obs_sceneitem_get_source() returns the
	// SAME object, see the header comment on this function) and remove every
	// other scene item, switching off the delay filter left on its source.
	obs_source_t *itemSource = nullptr;
	struct FindCtx {
		const std::string *liveName;
		obs_source_t *itemSource = nullptr;
		std::vector<obs_sceneitem_t *> stale;
	} ctx{&liveSceneName};
	obs_scene_enum_items(
		wrapperScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *c = static_cast<FindCtx *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);
			const char *name = source ? obs_source_get_name(source) : nullptr;
			if (!name)
				return true;
			// The delay overlay lives in this scene too; it is never the
			// live scene the filter attaches to.
			if (strcmp(name, kDelayOverlaySourceName) == 0 || strcmp(name, kDelayBadgeSourceName) == 0)
				return true;
			for (const char *legacy : kLegacyOverlayNames) {
				if (strcmp(name, legacy) == 0)
					return true;
			}
			if (*c->liveName == name) {
				c->itemSource = source;
			} else {
				c->stale.push_back(item);
			}
			return true;
		},
		&ctx);
	for (obs_sceneitem_t *staleItem : ctx.stale) {
		obs_source_t *oldSource = obs_sceneitem_get_source(staleItem);
		if (obs_source_t *oldFilter = obs_source_get_filter_by_name(oldSource, kBufferFilterInstanceName)) {
			obs_source_set_enabled(oldFilter, false);
			obs_source_release(oldFilter);
		}
		TRIGGLOW_LOG_INFO(kComponent, "buffer mode: removing previous live scene \"%s\" from the wrapper",
				  obs_source_get_name(oldSource));
		obs_sceneitem_remove(staleItem);
	}
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

	if (!enabled) {
		// OBS bypasses a disabled filter's video_render entirely (that's
		// the whole point of disabling it here on Disable()) -- meaning
		// VideoDelayFilter::Render() never runs again to shrink/free its
		// RAM ring on its own. Without this, a 30s@1080p buffer just sits
		// there in memory even after the user presses Disable (found live,
		// 2026-08-26). "release_buffers" is a proc handler VideoDelayFilter
		// registers on itself for exactly this -- see its Create()/
		// ReleaseBuffersProc for why it hops onto the graphics thread
		// instead of clearing the ring right here.
		calldata_t cd = {};
		proc_handler_t *procHandler = obs_source_get_proc_handler(filter);
		if (!procHandler || !proc_handler_call(procHandler, "release_buffers", &cd)) {
			TRIGGLOW_LOG_WARN(kComponent,
					  "SetBufferFilterEnabled(false): release_buffers proc call failed, "
					  "RAM ring may not have been freed");
		}
		calldata_free(&cd);
	}

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

uint32_t ObsFrontendBridge::GetVideoEffectiveDelaySeconds(const std::string &liveSceneName) const
{
	obs_source_t *filter = FindBufferFilter(liveSceneName);
	if (!filter)
		return 0;

	proc_handler_t *procHandler = obs_source_get_proc_handler(filter);
	if (!procHandler)
		return 0;

	calldata_t cd = {};
	if (!proc_handler_call(procHandler, "get_effective_delay_seconds", &cd)) {
		calldata_free(&cd);
		return 0;
	}
	auto seconds = static_cast<uint32_t>(calldata_int(&cd, "seconds"));
	calldata_free(&cd);
	return seconds;
}

void ObsFrontendBridge::DisableAllDelayFilters() const
{
	// The filters are persisted with the scene collection, so closing OBS
	// while the delay was Active leaves them enabled in the saved collection:
	// the next start would delay the scene permanently with the plugin
	// showing Inactive. Called once the collection has finished loading.
	struct Ctx {
		int disabled = 0;
	} ctx;
	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			const char *id = obs_source_get_id(source);
			if (id &&
			    (strcmp(id, VideoDelayFilter::Id()) == 0 || strcmp(id, AudioDelayFilter::Id()) == 0) &&
			    obs_source_enabled(source)) {
				obs_source_set_enabled(source, false);
				++static_cast<Ctx *>(param)->disabled;
			}
			return true;
		},
		&ctx);
	if (ctx.disabled > 0)
		TRIGGLOW_LOG_INFO(kComponent, "switched off %d delay filter(s) left enabled by a previous session",
				  ctx.disabled);
}

bool ObsFrontendBridge::SetDelayOverlay(bool visible, const std::string &text, uint32_t corner) const
{
	obs_source_t *wrapperSource = obs_get_source_by_name(kBufferWrapperSceneName);
	obs_scene_t *wrapperScene = wrapperSource ? obs_scene_from_source(wrapperSource) : nullptr;
	if (!wrapperScene) {
		if (wrapperSource)
			obs_source_release(wrapperSource);
		return false;
	}

	// The first overlay build was a bare text source; replace it so users who
	// tried that version don't keep a second, unstyled "Delay Ns" on screen.
	for (const char *legacyName : kLegacyOverlayNames) {
		if (obs_sceneitem_t *legacy = obs_scene_find_source(wrapperScene, legacyName))
			obs_sceneitem_remove(legacy);
	}

	obs_sceneitem_t *badgeItem = obs_scene_find_source(wrapperScene, kDelayBadgeSourceName);
	obs_sceneitem_t *textItem = obs_scene_find_source(wrapperScene, kDelayOverlaySourceName);
	if (!visible && !badgeItem && !textItem) {
		obs_source_release(wrapperSource);
		return true; // Nothing to hide.
	}

	// Created once, then only toggled/updated, so restyling or moving them in
	// OBS sticks. The badge goes in first (drawn below) and the text on top.
	if (visible && !badgeItem) {
		char *badgePath = obs_module_file("images/delay-badge.png");
		if (badgePath) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "file", badgePath);
			obs_source_t *badgeSource =
				obs_source_create("image_source", kDelayBadgeSourceName, settings, nullptr);
			obs_data_release(settings);
			if (badgeSource) {
				badgeItem = obs_scene_add(wrapperScene, badgeSource);
				obs_source_release(badgeSource);
				if (badgeItem) {
					vec2 pos, scale;
					vec2_set(&pos, kOverlayMargin, kOverlayMargin);
					vec2_set(&scale, kOverlayScale, kOverlayScale);
					obs_sceneitem_set_pos(badgeItem, &pos);
					obs_sceneitem_set_scale(badgeItem, &scale);
				}
			}
			bfree(badgePath);
		} else {
			TRIGGLOW_LOG_WARN(kComponent, "delay overlay: images/delay-badge.png not found, text only");
		}
	}

	if (visible && !textItem) {
		// Windows uses GDI+ text, macOS/Linux FreeType2; try the newest
		// version of each first since older OBS builds lack the newer ids.
		static const char *const kTextSourceIds[] = {"text_gdiplus_v3", "text_gdiplus_v2", "text_gdiplus",
							     "text_ft2_source_v2"};
		const char *textId = nullptr;
		for (const char *id : kTextSourceIds) {
			if (obs_get_source_output_flags(id) != 0) {
				textId = id;
				break;
			}
		}
		if (!textId) {
			TRIGGLOW_LOG_WARN(kComponent, "delay overlay: no text source type available in this OBS");
		} else {
			obs_data_t *settings = obs_data_create();
			obs_data_t *font = obs_data_create();
			obs_data_set_string(font, "face", "Segoe UI");
			obs_data_set_int(font, "size", 60);
			obs_data_set_string(font, "style", "Bold Italic");
			obs_data_set_int(font, "flags", 3); // OBS_FONT_BOLD | OBS_FONT_ITALIC
			obs_data_set_obj(settings, "font", font);
			obs_data_release(font);
			obs_data_set_string(settings, "text", text.c_str());
			// Colors are ABGR. White fading to light cyan, like the badge border.
			obs_data_set_int(settings, "color", 0xFFFFFFFF);
			obs_data_set_bool(settings, "gradient", true);
			obs_data_set_int(settings, "gradient_color", 0xFFFDE6BA);
			obs_data_set_int(settings, "gradient_dir", 90);
			obs_data_set_int(settings, "gradient_opacity", 100);
			obs_data_set_int(settings, "color1", 0xFFFFFFFF); // FreeType2 equivalents
			obs_data_set_int(settings, "color2", 0xFFFDE6BA);
			// Fixed, centered box so "Delay 5s" and "Delay 60s" both sit in
			// the middle of the badge (GDI+ only; FreeType2 ignores these).
			obs_data_set_string(settings, "align", "center");
			obs_data_set_string(settings, "valign", "center");
			obs_data_set_bool(settings, "extents", true);
			obs_data_set_int(settings, "extents_cx", kOverlayTextW);
			obs_data_set_int(settings, "extents_cy", kOverlayTextH);
			obs_data_set_bool(settings, "extents_wrap", false);

			obs_source_t *textSource =
				obs_source_create(textId, kDelayOverlaySourceName, settings, nullptr);
			obs_data_release(settings);
			if (textSource) {
				textItem = obs_scene_add(wrapperScene, textSource);
				obs_source_release(textSource);
				if (textItem) {
					vec2 pos, scale;
					vec2_set(&pos, kOverlayMargin + kOverlayTextOffsetX * kOverlayScale,
						 kOverlayMargin + kOverlayTextOffsetY * kOverlayScale);
					vec2_set(&scale, kOverlayScale, kOverlayScale);
					obs_sceneitem_set_pos(textItem, &pos);
					obs_sceneitem_set_scale(textItem, &scale);
					TRIGGLOW_LOG_INFO(kComponent, "delay overlay: created (%s)", textId);
				}
			} else {
				TRIGGLOW_LOG_WARN(kComponent, "delay overlay: could not create the text source (%s)",
						  textId);
			}
		}
	} else if (visible && textItem) {
		obs_source_t *textSource = obs_sceneitem_get_source(textItem);
		obs_data_t *settings = obs_source_get_settings(textSource);
		obs_data_set_string(settings, "text", text.c_str());
		obs_source_update(textSource, settings);
		obs_data_release(settings);
	}

	if (visible) {
		// Position/scale are re-applied on every show so the chosen corner takes
		// effect immediately (and follows a canvas resolution change).
		obs_video_info ovi = {};
		float canvasW = 1920.0f, canvasH = 1080.0f;
		if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
			canvasW = static_cast<float>(ovi.base_width);
			canvasH = static_cast<float>(ovi.base_height);
		}
		const float badgeW = kOverlayBadgeW * kOverlayScale;
		const float badgeH = kOverlayBadgeH * kOverlayScale;
		const bool right = corner == 1 || corner == 3;
		const bool bottom = corner == 2 || corner == 3;
		const float x = right ? canvasW - badgeW - kOverlayMargin : kOverlayMargin;
		const float y = bottom ? canvasH - badgeH - kOverlayMargin : kOverlayMargin;
		vec2 scale;
		vec2_set(&scale, kOverlayScale, kOverlayScale);
		if (badgeItem) {
			vec2 pos;
			vec2_set(&pos, x, y);
			obs_sceneitem_set_pos(badgeItem, &pos);
			obs_sceneitem_set_scale(badgeItem, &scale);
		}
		if (textItem) {
			vec2 pos;
			vec2_set(&pos, x + kOverlayTextOffsetX * kOverlayScale,
				 y + kOverlayTextOffsetY * kOverlayScale);
			obs_sceneitem_set_pos(textItem, &pos);
			obs_sceneitem_set_scale(textItem, &scale);
		}
		// The live scene can be (re)added to the wrapper AFTER the overlay was
		// created (e.g. after switching live scene), which would put it on top
		// and hide the overlay. Always bring the overlay back above everything.
		if (badgeItem)
			obs_sceneitem_set_order(badgeItem, OBS_ORDER_MOVE_TOP);
		if (textItem)
			obs_sceneitem_set_order(textItem, OBS_ORDER_MOVE_TOP);
	}

	if (badgeItem)
		obs_sceneitem_set_visible(badgeItem, visible);
	if (textItem)
		obs_sceneitem_set_visible(textItem, visible);

	obs_source_release(wrapperSource);
	return badgeItem != nullptr || textItem != nullptr;
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
		if (enabled) {
			// The ring still holds whatever this source sent before the
			// last Disable; flush it so the new Filling window starts
			// clean instead of replaying old audio.
			obs_data_t *settings = obs_source_get_settings(filter);
			obs_data_set_int(settings, "reset_token", obs_data_get_int(settings, "reset_token") + 1);
			obs_source_update(filter, settings);
			obs_data_release(settings);
		}
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

VideoDelayFilter::BufferFitEstimate ObsFrontendBridge::EstimateBufferFit(const std::string &liveSceneName,
									 uint32_t requestedDelaySeconds,
									 uint32_t minResolutionHeight) const
{
	obs_source_t *liveSource = obs_get_source_by_name(liveSceneName.c_str());
	if (!liveSource)
		return {};

	// base_width/height, not the filtered width/height -- matches what
	// VideoDelayFilter::Render() itself queries, so this predicts the exact
	// same thing EnsureRingSized() will actually compute once Enable() runs.
	uint32_t width = obs_source_get_base_width(liveSource);
	uint32_t height = obs_source_get_base_height(liveSource);
	obs_source_release(liveSource);

	obs_video_info ovi = {};
	uint32_t fps = 30;
	if (obs_get_video_info(&ovi) && ovi.fps_den > 0)
		fps = ovi.fps_num / ovi.fps_den;

	return VideoDelayFilter::EstimateBufferFit(requestedDelaySeconds, minResolutionHeight, width, height, fps);
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
