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
#include "auto-names.hpp"
#include "logging.hpp"
#include "overlay-layout.hpp"
#include "video-delay-filter.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

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

// The bridge BufferRestartedTask() may still deliver events to: set by Init(),
// cleared by Shutdown(), both on the UI thread like the task itself.
const trigglow::ObsFrontendBridge *g_eventBridge = nullptr;
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
	g_eventBridge = this;
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

	// Unguards them too: the "enable" handler must not outlive this bridge.
	ForgetActiveBufferFilter();
	ForgetActiveAudioFilters();
	UnwatchOutputStops();

	if (g_eventBridge == this)
		g_eventBridge = nullptr; // A BufferRestartedTask() still queued finds it gone.
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
// without tracking any extra IDs themselves. They live in auto-names.hpp
// (autonames::), together with the "(no tocar)" names v0.5.1 and earlier
// created: every lookup here tries the current name first, then those, and
// MigrateLegacyAutoNames() renames old collections once.
//
// autonames::kBufferFilter is DELIBERATELY NOT the filter type's display
// name (VideoDelayFilter::GetName(), i.e. the name OBS's own Filters dialog
// would default to if a user manually adds this filter type, which is
// exactly what happened during this plugin's own Phase 1 testing on
// "Multimedia"). Found live, 2026-08-25: OBS enforces globally unique SOURCE
// names (filters are sources), so obs_source_create() with that name
// silently produced a differently-named object whenever a same-named filter
// already existed anywhere else in the scene collection -- every
// FindBufferFilter() lookup by the original name then failed,
// SetBufferFilterEnabled/SetBufferFilterDelaySeconds silently no-op'd
// (neither had any failure logging until today), the filter stayed
// permanently disabled at its creation default, and every Enable() press
// created ANOTHER orphaned duplicate instead of reusing the existing one.
// Using a name no manual "Add Filter" would ever produce avoids the
// collision entirely, regardless of what else the user has attached
// elsewhere. The same global uniqueness is why the audio filters carry their
// source's name (autonames::AudioFilterName), and why a rename only happens
// when the new name is free (obs_source_set_name() would dedupe it to
// "... 2", which no lookup would find).
using autonames::Kind;

// Owned (+1) reference to the wrapper scene's source under its current or a
// legacy name, or null.
obs_source_t *GetWrapperSceneSourceRef()
{
	for (const std::string &name : autonames::LookupNames(Kind::WrapperScene)) {
		obs_source_t *source = obs_get_source_by_name(name.c_str());
		if (!source)
			continue;
		if (obs_scene_from_source(source))
			return source;
		obs_source_release(source); // Something else (an input) carries that name.
	}
	return nullptr;
}

// Owned reference to the first filter of `parent` called one of `names`.
OBSSourceAutoRelease FindFilterByNames(obs_source_t *parent, const std::vector<std::string> &names)
{
	for (const std::string &name : names) {
		OBSSourceAutoRelease filter = obs_source_get_filter_by_name(parent, name.c_str());
		if (filter)
			return filter;
	}
	return nullptr;
}

OBSSourceAutoRelease FindBufferFilterOn(obs_source_t *parent)
{
	return FindFilterByNames(parent, autonames::LookupNames(Kind::BufferFilter));
}

OBSSourceAutoRelease FindAudioFilterOn(obs_source_t *child)
{
	const char *childName = obs_source_get_name(child);
	return FindFilterByNames(child, autonames::AudioFilterLookupNames(childName ? childName : ""));
}

// Calls `source`'s "void <proc>(in int <param>)" proc handler with `value`.
// False if it has no such proc.
bool CallIntProc(obs_source_t *source, const char *proc, const char *param, long long value)
{
	proc_handler_t *handler = obs_source_get_proc_handler(source);
	if (!handler)
		return false;
	calldata_t cd = {};
	calldata_set_int(&cd, param, value);
	const bool ok = proc_handler_call(handler, proc, &cd);
	calldata_free(&cd);
	return ok;
}

// Keys SetDelayOverlay() keeps in the badge item's private settings, which OBS
// saves with the scene collection: a marker that finds the item whatever its
// source ends up called, and the layout last applied (overlay-layout.hpp's
// AppliedLayout), so a position or size the user gives the badge is kept.
constexpr const char *kImageSourceId = "image_source";
constexpr const char *kBadgeMarkerKey = "trigglow_overlay_badge";
constexpr const char *kBadgePlacedKey = "trigglow_overlay_placed";
constexpr const char *kBadgeCornerKey = "trigglow_overlay_corner";
constexpr const char *kBadgeCanvasWidthKey = "trigglow_overlay_canvas_width";
constexpr const char *kBadgeCanvasHeightKey = "trigglow_overlay_canvas_height";
constexpr const char *kBadgeXKey = "trigglow_overlay_x";
constexpr const char *kBadgeYKey = "trigglow_overlay_y";
constexpr const char *kBadgeScaleKey = "trigglow_overlay_scale";
constexpr const char *kBadgeDesignHeightKey = "trigglow_overlay_design_height";

bool HasBadgeMarker(obs_sceneitem_t *item)
{
	OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(item);
	return priv && obs_data_get_bool(priv, kBadgeMarkerKey);
}

// An overlay item of the wrapper scene (the badge, or an older overlay
// source): by name, or by the badge's marker when OBS had to name its source
// "Trigglow Delay Badge (auto) 2" or the user renamed it.
bool IsOverlayItem(obs_sceneitem_t *item, const char *sourceName)
{
	return (sourceName && autonames::IsOverlayName(sourceName)) || HasBadgeMarker(item);
}

// Borrowed: the overlay badge's item in the wrapper scene -- current or legacy
// name first, then the marker.
obs_sceneitem_t *FindOverlayBadgeItem(obs_scene_t *wrapperScene)
{
	for (const std::string &name : autonames::LookupNames(Kind::OverlayBadge)) {
		if (obs_sceneitem_t *item = obs_scene_find_source(wrapperScene, name.c_str()))
			return item;
	}
	obs_sceneitem_t *marked = nullptr;
	obs_scene_enum_items(
		wrapperScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			if (!HasBadgeMarker(item))
				return true;
			*static_cast<obs_sceneitem_t **>(param) = item;
			return false;
		},
		static_cast<void *>(&marked));
	return marked;
}

overlaylayout::AppliedLayout ReadAppliedLayout(obs_data_t *priv)
{
	overlaylayout::AppliedLayout applied;
	if (!priv || !obs_data_get_bool(priv, kBadgePlacedKey))
		return applied;
	applied.valid = true;
	applied.corner = static_cast<uint32_t>(obs_data_get_int(priv, kBadgeCornerKey));
	applied.canvasWidth = static_cast<float>(obs_data_get_double(priv, kBadgeCanvasWidthKey));
	applied.canvasHeight = static_cast<float>(obs_data_get_double(priv, kBadgeCanvasHeightKey));
	applied.x = static_cast<float>(obs_data_get_double(priv, kBadgeXKey));
	applied.y = static_cast<float>(obs_data_get_double(priv, kBadgeYKey));
	applied.scale = static_cast<float>(obs_data_get_double(priv, kBadgeScaleKey));
	applied.designHeight = static_cast<float>(obs_data_get_double(priv, kBadgeDesignHeightKey)); // 0 if absent.
	return applied;
}

void WriteAppliedLayout(obs_data_t *priv, uint32_t corner, float canvasWidth, float canvasHeight,
			const overlaylayout::Placement &placement)
{
	if (!priv)
		return;
	obs_data_set_bool(priv, kBadgePlacedKey, true);
	obs_data_set_int(priv, kBadgeCornerKey, corner);
	obs_data_set_double(priv, kBadgeCanvasWidthKey, canvasWidth);
	obs_data_set_double(priv, kBadgeCanvasHeightKey, canvasHeight);
	obs_data_set_double(priv, kBadgeXKey, placement.x);
	obs_data_set_double(priv, kBadgeYKey, placement.y);
	obs_data_set_double(priv, kBadgeScaleKey, placement.scale);
	obs_data_set_double(priv, kBadgeDesignHeightKey, overlaylayout::kDesignHeight);
}

// Removes the overlay sources earlier versions put in the wrapper scene and
// the generated image replaces (v0.5.x's "Delay Ns" text source and older
// prototypes), so nobody keeps a stale second overlay. Collected first and
// removed after the enumeration: obs_scene_enum_items() holds the scene's
// mutex. Removing the item drops the wrapper's reference to that source;
// libobs destroys it once nothing else (another scene) uses it.
int RemoveRetiredOverlayItems(obs_scene_t *wrapperScene)
{
	std::vector<obs_sceneitem_t *> retired; // Each with its own reference.
	obs_scene_enum_items(
		wrapperScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			obs_source_t *source = obs_sceneitem_get_source(item);
			const char *name = source ? obs_source_get_name(source) : nullptr;
			if (name && autonames::IsRetiredOverlayName(name)) {
				obs_sceneitem_addref(item);
				static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			}
			return true;
		},
		&retired);
	for (obs_sceneitem_t *item : retired) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		TRIGGLOW_LOG_INFO(kComponent, "delay overlay: removing the retired \"%s\" from the wrapper scene",
				  source ? obs_source_get_name(source) : "?");
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}
	return static_cast<int>(retired.size());
}
} // namespace

OBSSourceAutoRelease ObsFrontendBridge::FindBufferFilter(const std::string &liveSceneName) const
{
	// Owned references here (libobs 31.1.1): obs_get_source_by_name() and
	// obs_source_get_filter_by_name() both return the source with its refcount
	// incremented (the latter through obs_source_get_ref(), obs-source.c), so
	// both are wrapped and released automatically -- the filter's by the
	// caller. obs_scene_from_source() and obs_sceneitem_get_source() are
	// borrowed: the wrapper scene's own reference keeps them alive here.
	OBSSourceAutoRelease wrapperSource = GetWrapperSceneSourceRef();
	if (!wrapperSource) {
		TRIGGLOW_LOG_WARN(kComponent, "FindBufferFilter: no scene named \"%s\" (or a v0.5.1 name)",
				  autonames::kWrapperScene);
		return nullptr;
	}
	obs_scene_t *wrapperScene = obs_scene_from_source(wrapperSource); // Non-null: checked by the lookup.

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
				  static_cast<void *>(wrapperSource.Get()), liveSceneName.c_str());
		return nullptr;
	}

	OBSSourceAutoRelease filter = FindBufferFilterOn(ctx.itemSource);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "FindBufferFilter: wrapper's item source %p (\"%s\") has no filter named \"%s\"",
				  static_cast<void *>(ctx.itemSource), obs_source_get_name(ctx.itemSource),
				  autonames::kBufferFilter);
	}
	return filter;
}

OBSSourceAutoRelease ObsFrontendBridge::ActiveBufferFilter() const
{
	// +1 reference, or null if the filter was destroyed meanwhile.
	return activeBufferFilter_ ? obs_weak_source_get_source(activeBufferFilter_) : nullptr;
}

OBSSourceAutoRelease ObsFrontendBridge::BufferFilterFor(const std::string &liveSceneName) const
{
	if (activeBufferFilter_) {
		if (OBSSourceAutoRelease filter = ActiveBufferFilter())
			return filter;
	}
	return FindBufferFilter(liveSceneName);
}

void ObsFrontendBridge::ForgetActiveBufferFilter() const
{
	obs_weak_source_t *weak = nullptr;
	{
		std::lock_guard<std::mutex> lock(guardMutex_);
		std::swap(weak, activeBufferFilter_);
	}
	loggedBufferFilterGone_ = false;
	if (!weak)
		return;
	if (OBSSourceAutoRelease filter = obs_weak_source_get_source(weak))
		DisconnectGuard(filter);
	obs_weak_source_release(weak);
}

std::vector<OBSSourceAutoRelease> ObsFrontendBridge::AudioFiltersFor(const std::string &liveSceneName) const
{
	std::vector<OBSSourceAutoRelease> filters;
	// UI thread, the list's only writer: no lock needed to read it here.
	for (obs_weak_source_t *weak : activeAudioFilters_) {
		if (obs_source_t *filter = obs_weak_source_get_source(weak)) // +1, owned by the vector.
			filters.emplace_back(filter);
	}
	if (!filters.empty())
		return filters;
	for (const OBSSourceAutoRelease &child : GetAudioCapableChildren(liveSceneName)) {
		if (OBSSourceAutoRelease filter = FindAudioFilterOn(child.Get()))
			filters.push_back(std::move(filter));
	}
	return filters;
}

void ObsFrontendBridge::ForgetActiveAudioFilters() const
{
	std::vector<obs_weak_source_t *> weak;
	{
		std::lock_guard<std::mutex> lock(guardMutex_);
		weak.swap(activeAudioFilters_);
	}
	for (obs_weak_source_t *ref : weak) {
		if (OBSSourceAutoRelease filter = obs_weak_source_get_source(ref))
			DisconnectGuard(filter);
		obs_weak_source_release(ref);
	}
}

bool ObsFrontendBridge::IsGuardedFilter(obs_source_t *filter) const
{
	std::lock_guard<std::mutex> lock(guardMutex_);
	if (activeBufferFilter_ && obs_weak_source_references_source(activeBufferFilter_, filter))
		return true;
	return std::any_of(activeAudioFilters_.begin(), activeAudioFilters_.end(), [filter](obs_weak_source_t *weak) {
		return obs_weak_source_references_source(weak, filter);
	});
}

void ObsFrontendBridge::ConnectGuard(obs_source_t *filter) const
{
	// Connecting again is a no-op (libobs skips a callback/data pair already
	// connected).
	signal_handler_connect(obs_source_get_signal_handler(filter), "enable",
			       &ObsFrontendBridge::DelayFilterEnableSignal, const_cast<ObsFrontendBridge *>(this));
}

void ObsFrontendBridge::DisconnectGuard(obs_source_t *filter) const
{
	signal_handler_disconnect(obs_source_get_signal_handler(filter), "enable",
				  &ObsFrontendBridge::DelayFilterEnableSignal, const_cast<ObsFrontendBridge *>(this));
}

void ObsFrontendBridge::DelayFilterEnableSignal(void *param, calldata_t *data)
{
	if (calldata_bool(data, "enabled"))
		return;
	const auto *self = static_cast<const ObsFrontendBridge *>(param);
	auto *filter = static_cast<obs_source_t *>(calldata_ptr(data, "source"));
	if (!filter || !self->IsGuardedFilter(filter))
		return;
	// Back on at once. libobs flips the flag before emitting this signal, so
	// at most the frame being rendered right now skips the filter. This emits
	// "enable" again, with enabled = true: ignored above.
	obs_source_set_enabled(filter, true);
	if (!self->warnedFilterSwitchedOff_.exchange(true)) {
		const char *id = obs_source_get_id(filter);
		const bool video = id && strcmp(id, VideoDelayFilter::Id()) == 0;
		TRIGGLOW_LOG_WARN(kComponent,
				  "\"%s\" was switched off outside the Trigglow dock while the delay is on -- switched "
				  "back on: off, %s. Turn the delay off from the dock instead",
				  obs_source_get_name(filter),
				  video ? "the stream would show the live scene without delay"
					: "this source's audio would play ahead of the video");
	}
}

bool ObsFrontendBridge::EnsureBufferWrapperScene(const std::string &liveSceneName) const
{
	if (liveSceneName.empty())
		return false;

	if (FindBufferFilter(liveSceneName))
		return true; // Already fully set up (the temporary reference is released right here).

	obs_source_t *liveSource = obs_get_source_by_name(liveSceneName.c_str());
	if (!liveSource) {
		TRIGGLOW_LOG_WARN(kComponent, "buffer mode: live scene \"%s\" not found", liveSceneName.c_str());
		return false;
	}

	obs_source_t *wrapperSource = GetWrapperSceneSourceRef();
	obs_scene_t *wrapperScene = wrapperSource ? obs_scene_from_source(wrapperSource) : nullptr;
	bool wrapperWasFound = wrapperSource != nullptr;
	if (!wrapperSource) {
		wrapperScene = obs_scene_create(autonames::kWrapperScene);
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
	} ctx{&liveSceneName, nullptr, {}};
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
			if (IsOverlayItem(item, name))
				return true;
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
		if (OBSSourceAutoRelease oldFilter = FindBufferFilterOn(oldSource))
			obs_source_set_enabled(oldFilter, false);
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
	OBSSourceAutoRelease existingFilter = FindBufferFilterOn(itemSource);
	if (!existingFilter) {
		obs_data_t *filterSettings = obs_data_create();
		obs_source_t *filter =
			obs_source_create(VideoDelayFilter::Id(), autonames::kBufferFilter, filterSettings, nullptr);
		obs_data_release(filterSettings);
		if (filter) {
			// Defensive: OBS enforces globally unique source names, so a
			// name collision with something the user created elsewhere
			// would make obs_source_create() silently return an object
			// under a DIFFERENT actual name -- exactly what caused this
			// whole lookup chain to fail live, 2026-08-24/25, before
			// the filter's instance name was changed to something a manual
			// "Add Filter" could never produce. Kept as a loud safety net
			// in case that ever happens again for some other reason.
			const char *actualName = obs_source_get_name(filter);
			if (!actualName || strcmp(actualName, autonames::kBufferFilter) != 0) {
				TRIGGLOW_LOG_ERROR(kComponent,
						   "buffer mode: created filter got renamed to \"%s\" (wanted \"%s\") "
						   "-- likely a name collision with something else in this scene "
						   "collection; buffer mode will NOT be able to find it again",
						   actualName ? actualName : "(null)", autonames::kBufferFilter);
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
	// Held until this function returns: the "release_buffers" call below
	// blocks on a graphics task that uses the filter's private data. Switching
	// off reaches the filter that was switched on, wherever it is now (the
	// live scene may have been renamed meanwhile); by name otherwise.
	OBSSourceAutoRelease filter = enabled ? FindBufferFilter(liveSceneName) : BufferFilterFor(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterEnabled(%s): FindBufferFilter returned null, no-op",
				  enabled ? "true" : "false");
		if (!enabled)
			ForgetActiveBufferFilter();
		return false;
	}
	if (enabled) {
		obs_source_set_enabled(filter, true);
		ForgetActiveBufferFilter(); // The previous cycle's, normally released already.
		{
			std::lock_guard<std::mutex> lock(guardMutex_);
			activeBufferFilter_ = obs_source_get_weak_source(filter);
		}
		warnedFilterSwitchedOff_ = false;
		ConnectGuard(filter);
		// A hard resize while the delay is on (delay change, OBS video reset)
		// empties the ring: BufferModeController shows the loading scene until
		// it refills. Connecting again on every Enable is a no-op (libobs skips
		// a callback/data pair already connected).
		signal_handler_connect(obs_source_get_signal_handler(filter), VideoDelayFilter::kBufferRestartedSignal,
				       &ObsFrontendBridge::BufferRestartedSignal,
				       const_cast<ObsFrontendBridge *>(this));
	} else {
		// Unguarded before it's switched off, or DelayFilterEnableSignal()
		// would switch it straight back on.
		ForgetActiveBufferFilter();
		DisconnectGuard(filter);
		obs_source_set_enabled(filter, false);
	}

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
			  static_cast<void *>(filter.Get()));
	return true;
}

bool ObsFrontendBridge::SetBufferFilterDelaySeconds(const std::string &liveSceneName, uint32_t seconds) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterDelaySeconds(%us): FindBufferFilter returned null, no-op",
				  seconds);
		return false;
	}
	// The managed value first (applies at once, see the header); the update
	// after it carries the same value, and still runs the filter's Update()
	// (which starts each cycle's diagnostics afresh).
	if (!CallIntProc(filter, "set_managed_delay_seconds", "seconds", seconds))
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterDelaySeconds(%us): set_managed_delay_seconds proc failed",
				  seconds);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "delay_seconds", seconds);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	return true;
}

bool ObsFrontendBridge::SetBufferFilterOutputCap(const std::string &liveSceneName, bool capToOutput) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterOutputCap(%s): FindBufferFilter returned null, no-op",
				  capToOutput ? "true" : "false");
		return false;
	}
	// Merged onto the filter's existing settings, like the two setters around.
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "buffer_output_cap", capToOutput);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	return true;
}

bool ObsFrontendBridge::SetBufferFilterCodecWorkers(const std::string &liveSceneName, bool codecWorkers) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterCodecWorkers(%s): FindBufferFilter returned null, no-op",
				  codecWorkers ? "true" : "false");
		return false;
	}
	// Merged onto the filter's existing settings, like the setters around.
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "codec_workers", codecWorkers);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	return true;
}

bool ObsFrontendBridge::SetBufferFilterObsEncoders(const std::string &liveSceneName,
						   const VideoDelayFilter::ObsEncoderSet &encoders) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter)
		return false;
	proc_handler_t *procHandler = obs_source_get_proc_handler(filter);
	if (!procHandler)
		return false;
	VideoDelayFilter::ObsEncoderSet copy = encoders;
	calldata_t cd = {};
	calldata_set_ptr(&cd, "encoders", &copy);
	const bool ok = proc_handler_call(procHandler, "set_obs_encoders", &cd);
	calldata_free(&cd);
	return ok;
}

bool ObsFrontendBridge::ReportBufferEncoderLag(const std::string &liveSceneName, uint64_t skippedFrames) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter)
		return false;
	return CallIntProc(filter, "report_encoder_lag", "skipped",
			   static_cast<long long>(std::min<uint64_t>(skippedFrames, INT32_MAX)));
}

bool ObsFrontendBridge::YieldBufferHwEncoder(const std::string &liveSceneName, bool wait) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter)
		return false;
	proc_handler_t *procHandler = obs_source_get_proc_handler(filter);
	if (!procHandler)
		return false;
	calldata_t cd = {};
	calldata_set_bool(&cd, "wait", wait);
	const bool ok = proc_handler_call(procHandler, "yield_hw_encoder", &cd);
	calldata_free(&cd);
	return ok;
}

bool ObsFrontendBridge::ResumeBufferHwEncoder(const std::string &liveSceneName) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter)
		return false;
	proc_handler_t *procHandler = obs_source_get_proc_handler(filter);
	if (!procHandler)
		return false;
	calldata_t cd = {};
	const bool ok = proc_handler_call(procHandler, "resume_hw_encoder", &cd);
	calldata_free(&cd);
	return ok;
}

uint64_t ObsFrontendBridge::TakeMixSkippedFrames() const
{
	struct Ctx {
		const video_t *mainVideo;
		std::vector<std::pair<const void *, uint64_t>> mixes;
	} ctx{obs_get_video(), {}};
	// Under outputs_mutex. Each encoder is held (a reference of our own) while
	// it's read: the output could be stopping meanwhile, and stopping frees
	// an encoder's frame-rate divisor video_t. Its mix is the parent video
	// (obs_encoder_parent_video(), the canvas's own, which the canvas keeps):
	// never obs_encoder_video(), which for an encoder with a divisor is that
	// derived video_t -- not the main mix by pointer, yet counting the main
	// mix's skipped frames, which the filter already reads itself.
	obs_enum_outputs(
		[](void *param, obs_output_t *output) -> bool {
			auto *c = static_cast<Ctx *>(param);
			if (!obs_output_active(output))
				return true;
			for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; ++idx) {
				obs_encoder_t *encoder =
					obs_encoder_get_ref(obs_output_get_video_encoder2(output, idx));
				if (!encoder)
					continue;
				video_t *video = obs_encoder_parent_video(encoder);
				if (video && video != c->mainVideo &&
				    std::none_of(c->mixes.begin(), c->mixes.end(),
						 [&](const auto &mix) { return mix.first == video; }))
					c->mixes.emplace_back(video, video_output_get_skipped_frames(video));
				obs_encoder_release(encoder);
			}
			return true;
		},
		&ctx);
	uint64_t skipped = 0;
	for (const auto &mix : ctx.mixes) {
		for (const auto &before : mixSkippedBaselines_) {
			if (before.first == mix.first && mix.second > before.second)
				skipped += mix.second - before.second;
		}
	}
	mixSkippedBaselines_ = std::move(ctx.mixes);
	return skipped;
}

bool ObsFrontendBridge::ReportBufferObsEncodeError(const std::string &liveSceneName) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter)
		return false;
	proc_handler_t *procHandler = obs_source_get_proc_handler(filter);
	if (!procHandler)
		return false;
	calldata_t cd = {};
	const bool ok = proc_handler_call(procHandler, "report_obs_encode_error", &cd);
	calldata_free(&cd);
	return ok;
}

void ObsFrontendBridge::WatchOutputStops()
{
	// Outputs destroyed meanwhile: their signal handler went with them.
	for (auto it = watchedOutputs_.begin(); it != watchedOutputs_.end();) {
		if (obs_output_t *alive = obs_weak_output_get_output(*it)) {
			obs_output_release(alive);
			++it;
			continue;
		}
		obs_weak_output_release(*it);
		it = watchedOutputs_.erase(it);
	}
	// Active outputs with a video encoder, each with a reference of our own
	// (taken under outputs_mutex, connected after it).
	std::vector<obs_output_t *> active;
	obs_enum_outputs(
		[](void *param, obs_output_t *output) -> bool {
			auto *list = static_cast<std::vector<obs_output_t *> *>(param);
			if (!obs_output_active(output))
				return true;
			for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; ++idx) {
				if (obs_output_get_video_encoder2(output, idx)) {
					if (obs_output_t *ref = obs_output_get_ref(output))
						list->push_back(ref);
					break;
				}
			}
			return true;
		},
		&active);
	for (obs_output_t *output : active) {
		const bool watched =
			std::any_of(watchedOutputs_.begin(), watchedOutputs_.end(),
				    [&](obs_weak_output_t *w) { return obs_weak_output_references_output(w, output); });
		if (!watched) {
			signal_handler_connect(obs_output_get_signal_handler(output), "stop",
					       &ObsFrontendBridge::OutputStopSignal, this);
			watchedOutputs_.push_back(obs_output_get_weak_output(output));
		}
		obs_output_release(output);
	}
}

void ObsFrontendBridge::UnwatchOutputStops()
{
	for (obs_weak_output_t *weak : watchedOutputs_) {
		if (obs_output_t *output = obs_weak_output_get_output(weak)) {
			signal_handler_disconnect(obs_output_get_signal_handler(output), "stop",
						  &ObsFrontendBridge::OutputStopSignal, this);
			obs_output_release(output);
		}
		obs_weak_output_release(weak);
	}
	watchedOutputs_.clear();
}

void ObsFrontendBridge::OutputStopSignal(void *param, calldata_t *data)
{
	// The output's own thread: only an encoder error matters here (a network
	// drop or a user stop doesn't); never waits on the UI.
	if (calldata_int(data, "code") == OBS_OUTPUT_ENCODE_ERROR)
		obs_queue_task(OBS_TASK_UI, &ObsFrontendBridge::OutputEncodeErrorTask, param, false);
}

void ObsFrontendBridge::OutputEncodeErrorTask(void *param)
{
	auto *self = static_cast<ObsFrontendBridge *>(param);
	if (self != g_eventBridge || !self->handler_)
		return; // Shut down after the signal was queued.
	self->handler_(FrontendEvent::OutputEncodeError);
}

bool ObsFrontendBridge::SetBufferFilterEncoderCeiling(const std::string &liveSceneName, uint32_t width,
						      uint32_t height) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter)
		return false;
	proc_handler_t *procHandler = obs_source_get_proc_handler(filter);
	if (!procHandler)
		return false;
	calldata_t cd = {};
	calldata_set_int(&cd, "width", width);
	calldata_set_int(&cd, "height", height);
	const bool ok = proc_handler_call(procHandler, "set_encoder_ceiling", &cd);
	calldata_free(&cd);
	return ok;
}

bool ObsFrontendBridge::RawVideoOutputOnMainMix() const
{
	struct Ctx {
		const video_t *mainVideo;
		bool found;
	} ctx{obs_get_video(), false};
	obs_enum_outputs(
		[](void *param, obs_output_t *output) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const uint32_t flags = obs_output_get_flags(output);
			if (obs_output_active(output) && (flags & OBS_OUTPUT_VIDEO) && !(flags & OBS_OUTPUT_ENCODED) &&
			    obs_output_video(output) == c->mainVideo) {
				c->found = true;
				return false;
			}
			return true;
		},
		&ctx);
	return ctx.found;
}

bool ObsFrontendBridge::MainCanvasGeometry(encodercheck::Size &base, encodercheck::Size &output) const
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return false;
	base = {ovi.base_width, ovi.base_height};
	output = {ovi.output_width, ovi.output_height};
	return true;
}

bool ObsFrontendBridge::HardwareStorageAvailable() const
{
#ifdef TRIGGLOW_HAVE_HW_STORAGE
	return hwstorage::RegistryHasWorkingEncoder();
#else
	return false;
#endif
}

bool ObsFrontendBridge::HardwareStoragePossible() const
{
#ifdef TRIGGLOW_HAVE_HW_STORAGE
	// Once per session: the GPUs don't change under a running OBS, and the
	// dock asks on every change of its delay or quality controls.
	static const bool anyGpu = !QueryGpuAdapters().empty();
	return anyGpu;
#else
	return false;
#endif
}

bool ObsFrontendBridge::SetBufferFilterCodec(const std::string &liveSceneName, bool autoCodec) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterCodec(%s): FindBufferFilter returned null, no-op",
				  autoCodec ? "auto" : "mjpeg");
		return false;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "buffer_codec", autoCodec ? "auto" : "mjpeg");
	obs_source_update(filter, settings);
	obs_data_release(settings);
	return true;
}

bool ObsFrontendBridge::SetBufferFilterStorageClass(const std::string &liveSceneName,
						    bufferfit::RingClass ringClass) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent, "SetBufferFilterStorageClass(%s): FindBufferFilter returned null, no-op",
				  bufferfit::RingClassName(ringClass));
		return false;
	}
	// The proc applies it at once and writes it into the filter's settings:
	// an obs_source_update() would only reach the filter's Update() on the
	// next video tick, and its first ring could be sized with the previous
	// cycle's class meanwhile. The update is the fallback for a filter
	// without the proc.
	if (CallIntProc(filter, "set_storage_class", "ring_class", static_cast<long long>(ringClass)))
		return true;
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "storage_class", bufferfit::RingClassName(ringClass));
	obs_source_update(filter, settings);
	obs_data_release(settings);
	return true;
}

bool ObsFrontendBridge::GetBufferPipelineHealth(PipelineHealth &out) const
{
	// Through the weak reference only (see the header): no scene walk, no
	// WARN a second, nothing to lock against the graphics thread.
	OBSSourceAutoRelease filter = ActiveBufferFilter();
	if (!filter) {
		if (activeBufferFilter_ && !loggedBufferFilterGone_) {
			TRIGGLOW_LOG_WARN(kComponent, "the video delay filter switched on by Enable no longer exists "
						      "(removed, or the scene collection changed): no pipeline health "
						      "until the delay is turned on again");
			loggedBufferFilterGone_ = true;
		}
		return false;
	}
	proc_handler_t *procHandler = obs_source_get_proc_handler(filter);
	if (!procHandler)
		return false;
	PipelineHealth health;
	calldata_t cd = {};
	calldata_set_ptr(&cd, "health", &health);
	calldata_set_int(&cd, "size", static_cast<long long>(sizeof(health)));
	const bool ok = proc_handler_call(procHandler, "get_pipeline_state", &cd);
	calldata_free(&cd);
	if (ok)
		out = health;
	return ok;
}

void ObsFrontendBridge::BufferRestartedSignal(void *param, calldata_t * /*data*/)
{
	// Graphics thread (VideoDelayFilter::EnsureRingSized()); the controller
	// lives on the UI thread. Never waits: the graphics thread can't block on
	// the UI.
	obs_queue_task(OBS_TASK_UI, &ObsFrontendBridge::BufferRestartedTask, param, false);
}

void ObsFrontendBridge::BufferRestartedTask(void *param)
{
	auto *self = static_cast<ObsFrontendBridge *>(param);
	if (self != g_eventBridge || !self->handler_)
		return; // Shut down after the signal was queued.
	self->handler_(FrontendEvent::BufferRestarted);
}

bool ObsFrontendBridge::SetBufferFilterMinResolutionHeight(const std::string &liveSceneName,
							   uint32_t heightPixels) const
{
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
	if (!filter) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "SetBufferFilterMinResolutionHeight(%u): FindBufferFilter returned null, no-op",
				  heightPixels);
		return false;
	}
	// Managed, like SetBufferFilterDelaySeconds(). obs_source_update() merges
	// onto the filter's existing settings (obs_data_apply), so this doesn't
	// disturb delay_seconds.
	if (!CallIntProc(filter, "set_managed_min_resolution_height", "height", heightPixels))
		TRIGGLOW_LOG_WARN(
			kComponent,
			"SetBufferFilterMinResolutionHeight(%u): set_managed_min_resolution_height proc failed",
			heightPixels);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "min_resolution_height", heightPixels);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	return true;
}

uint32_t ObsFrontendBridge::GetVideoEffectiveDelaySeconds(const std::string &liveSceneName) const
{
	// Held until this function returns: the proc reads the filter's private
	// data (on this thread, from its current settings).
	OBSSourceAutoRelease filter = BufferFilterFor(liveSceneName);
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
	// showing Inactive. Called whenever a collection has finished loading: at
	// startup and after switching to another collection.
	//
	// Filters are never returned by obs_enum_sources() (inputs and groups
	// only), so they are reached through every source that can carry one
	// (libobs 31.1.1, see the header): inputs and groups (obs_enum_sources),
	// the main canvas's scenes, where the video filter lives
	// (obs_enum_scenes, which skips groups), and the global audio sources on
	// the output channels (obs_get_output_source). A source reached twice
	// (a global audio input is also an input) is harmless: its filters are
	// already off the second time.
	//
	// Three passes so nothing runs under a libobs lock: obs_source_set_enabled()
	// emits the "enable" signal, and its handlers must be free to take the
	// sources or filter mutexes themselves.
	std::vector<obs_source_t *> parents; // Strong refs.
	auto collectParent = [](void *param, obs_source_t *source) -> bool {
		if (obs_source_t *ref = obs_source_get_ref(source))
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
		return true;
	};
	obs_enum_sources(collectParent, &parents);
	obs_enum_scenes(collectParent, &parents);
	for (uint32_t channel = 0; channel < MAX_CHANNELS; ++channel) {
		if (obs_source_t *source = obs_get_output_source(channel)) // Already +1.
			parents.push_back(source);
	}

	std::vector<obs_source_t *> filters; // Strong refs.
	for (obs_source_t *parent : parents) {
		obs_source_enum_filters(
			parent,
			[](obs_source_t *, obs_source_t *filter, void *param) {
				const char *id = obs_source_get_id(filter);
				if (!id || (strcmp(id, VideoDelayFilter::Id()) != 0 &&
					    strcmp(id, AudioDelayFilter::Id()) != 0))
					return;
				if (obs_source_t *ref = obs_source_get_ref(filter))
					static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
			},
			&filters);
		obs_source_release(parent);
	}

	int disabled = 0;
	for (obs_source_t *filter : filters) {
		if (obs_source_enabled(filter)) {
			obs_source_set_enabled(filter, false);
			++disabled;
		}
		obs_source_release(filter);
	}
	if (disabled > 0)
		TRIGGLOW_LOG_INFO(kComponent, "switched off %d delay filter(s) left enabled by a previous session",
				  disabled);
}

void ObsFrontendBridge::SetOverlayImageProvider(OverlayImageProvider provider)
{
	overlayImageProvider_ = std::move(provider);
}

bool ObsFrontendBridge::SetDelayOverlay(bool visible, uint32_t seconds, uint32_t corner) const
{
	OBSSourceAutoRelease wrapperSource = GetWrapperSceneSourceRef();
	obs_scene_t *wrapperScene = wrapperSource ? obs_scene_from_source(wrapperSource) : nullptr;
	if (!wrapperScene)
		return false;

	// Before v0.6 the overlay was a badge PNG plus a separate text source; the
	// text (and older prototypes) go, the badge item is reused below.
	RemoveRetiredOverlayItems(wrapperScene);

	obs_sceneitem_t *badgeItem = FindOverlayBadgeItem(wrapperScene); // Borrowed.
	if (!visible) {
		if (badgeItem)
			obs_sceneitem_set_visible(badgeItem, false);
		return true;
	}

	// The generated PNG for this value (overlay-badge.hpp, rendered on this,
	// the UI, thread). Its path changes with the value, and obs_source_update()
	// below makes image_source reload it even when the path is the same.
	const std::string imagePath = overlayImageProvider_ ? overlayImageProvider_(seconds) : std::string();
	if (imagePath.empty()) {
		TRIGGLOW_LOG_WARN(kComponent, "delay overlay: no badge image for %us, overlay stays hidden", seconds);
		if (badgeItem)
			obs_sceneitem_set_visible(badgeItem, false);
		return false;
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "file", imagePath.c_str());
	if (!badgeItem) {
		// One badge source per collection. A source already called
		// kOverlayBadge outside this scene (the user also put it in one of
		// their scenes, or removed it from this one) is reused: creating
		// another one would make OBS call it "... 2", and -- before the
		// marker below -- add yet another copy on every appearance.
		OBSSourceAutoRelease badgeSource = obs_get_source_by_name(autonames::kOverlayBadge);
		const char *id = badgeSource ? obs_source_get_unversioned_id(badgeSource) : nullptr;
		const bool reused = id && strcmp(id, kImageSourceId) == 0;
		if (reused) {
			obs_source_update(badgeSource, settings);
		} else {
			// Nothing by that name, or something that isn't an image: a new
			// source (OBS may suffix its name; the marker finds it anyway).
			badgeSource = obs_source_create(kImageSourceId, autonames::kOverlayBadge, settings, nullptr);
		}
		if (badgeSource)
			badgeItem = obs_scene_add(wrapperScene, badgeSource); // The item holds its own reference.
		if (!badgeItem) {
			TRIGGLOW_LOG_WARN(kComponent, "delay overlay: could not create the image source");
			return false;
		}
		TRIGGLOW_LOG_INFO(kComponent, "delay overlay: %s \"%s\"", reused ? "reused" : "created",
				  obs_source_get_name(badgeSource));
	} else {
		obs_source_update(obs_sceneitem_get_source(badgeItem), settings);
	}

	OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(badgeItem);
	if (priv)
		obs_data_set_bool(priv, kBadgeMarkerKey, true); // Also tags items of v0.5.x and earlier 0.6 builds.

	// Placed in the chosen corner the first time and whenever the dock's
	// corner changes; otherwise the position and size the user gave it in OBS
	// stay (overlaylayout::ShouldPlace). The safety margin keeps it clear of
	// players' controls and clip crops.
	obs_video_info ovi = {};
	float canvasW = 0.0f;
	float canvasH = 0.0f;
	if (obs_get_video_info(&ovi)) {
		canvasW = static_cast<float>(ovi.base_width);
		canvasH = static_cast<float>(ovi.base_height);
	}
	vec2 pos;
	vec2 scale;
	obs_sceneitem_get_pos(badgeItem, &pos);
	obs_sceneitem_get_scale(badgeItem, &scale);
	const overlaylayout::ItemTransform current{pos.x, pos.y, scale.x, scale.y};
	if (overlaylayout::ShouldPlace(ReadAppliedLayout(priv), current, canvasW, canvasH, corner)) {
		const overlaylayout::Placement placement = overlaylayout::Place(canvasW, canvasH, corner);
		vec2_set(&pos, placement.x, placement.y);
		vec2_set(&scale, placement.scale, placement.scale);
		obs_sceneitem_set_alignment(badgeItem, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		obs_sceneitem_set_bounds_type(badgeItem, OBS_BOUNDS_NONE);
		obs_sceneitem_set_pos(badgeItem, &pos);
		obs_sceneitem_set_scale(badgeItem, &scale);
		WriteAppliedLayout(priv, corner, canvasW, canvasH, placement);
	}
	// The live scene can be (re)added to the wrapper AFTER the overlay was
	// created (e.g. after switching live scene), which would put it on top
	// and hide the overlay. Always bring the overlay back above everything.
	obs_sceneitem_set_order(badgeItem, OBS_ORDER_MOVE_TOP);
	obs_sceneitem_set_visible(badgeItem, true);
	return true;
}

void ObsFrontendBridge::MigrateLegacyAutoNames() const
{
	int renamed = 0;
	auto nameTaken = [](const std::string &name) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(name.c_str());
		return existing != nullptr;
	};
	// Renames `source` from a v0.5.1 name to the current one when that name is
	// free; obs_source_set_name() keeps the object (scene items, filters,
	// settings, position) and libobs/the frontend follow the rename.
	auto migrate = [&](obs_source_t *source) {
		const char *name = obs_source_get_name(source);
		if (!name)
			return;
		const std::string target = autonames::MigratedName(name);
		if (target.empty())
			return;
		switch (autonames::PlanRename(true, nameTaken(target))) {
		case autonames::RenamePlan::Rename: {
			const std::string previous = name; // `name` is freed by the rename.
			obs_source_set_name(source, target.c_str());
			++renamed;
			TRIGGLOW_LOG_INFO(kComponent, "renamed \"%s\" to \"%s\"", previous.c_str(), target.c_str());
			break;
		}
		case autonames::RenamePlan::KeepLegacy:
			TRIGGLOW_LOG_WARN(kComponent, "kept \"%s\": \"%s\" is already used by something else", name,
					  target.c_str());
			break;
		case autonames::RenamePlan::NothingToDo:
			break;
		}
	};

	// 1. The wrapper scene.
	const std::vector<std::string> wrapperNames = autonames::LookupNames(Kind::WrapperScene);
	for (size_t i = 1; i < wrapperNames.size(); ++i) {
		OBSSourceAutoRelease legacy = obs_get_source_by_name(wrapperNames[i].c_str());
		if (legacy && obs_scene_from_source(legacy))
			migrate(legacy);
	}

	// 2. The video and audio delay filters, wherever they are attached. Same
	// three passes as DisableAllDelayFilters(): nothing is renamed under a
	// libobs lock ("rename" signal handlers may take them).
	std::vector<obs_source_t *> parents; // Strong refs.
	auto collectParent = [](void *param, obs_source_t *source) -> bool {
		if (obs_source_t *ref = obs_source_get_ref(source))
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
		return true;
	};
	obs_enum_sources(collectParent, &parents);
	obs_enum_scenes(collectParent, &parents);
	for (uint32_t channel = 0; channel < MAX_CHANNELS; ++channel) {
		if (obs_source_t *source = obs_get_output_source(channel)) // Already +1.
			parents.push_back(source);
	}
	std::vector<obs_source_t *> filters; // Strong refs.
	for (obs_source_t *parent : parents) {
		obs_source_enum_filters(
			parent,
			[](obs_source_t *, obs_source_t *filter, void *param) {
				const char *id = obs_source_get_id(filter);
				if (!id || (strcmp(id, VideoDelayFilter::Id()) != 0 &&
					    strcmp(id, AudioDelayFilter::Id()) != 0))
					return;
				if (obs_source_t *ref = obs_source_get_ref(filter))
					static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
			},
			&filters);
		obs_source_release(parent);
	}
	for (obs_source_t *filter : filters) {
		migrate(filter);
		obs_source_release(filter);
	}

	// 3. The overlay: v0.5.x's badge image source is renamed (and repointed to
	// the generated PNG the next time it shows), its text source removed.
	OBSSourceAutoRelease wrapperSource = GetWrapperSceneSourceRef();
	if (obs_scene_t *wrapperScene = wrapperSource ? obs_scene_from_source(wrapperSource) : nullptr) {
		RemoveRetiredOverlayItems(wrapperScene);
		if (obs_sceneitem_t *badgeItem = FindOverlayBadgeItem(wrapperScene))
			migrate(obs_sceneitem_get_source(badgeItem));
	}

	if (renamed > 0)
		TRIGGLOW_LOG_INFO(kComponent, "migrated %d auto-managed object(s) to the \"(auto)\" names", renamed);
}

bool ObsFrontendBridge::ShowBufferWrapperScene() const
{
	OBSSourceAutoRelease wrapperSource = GetWrapperSceneSourceRef();
	const char *name = wrapperSource ? obs_source_get_name(wrapperSource) : nullptr;
	return name && SetCurrentSceneByName(name);
}

std::vector<OBSSourceAutoRelease> ObsFrontendBridge::GetAudioCapableChildren(const std::string &liveSceneName) const
{
	std::vector<OBSSourceAutoRelease> result;

	OBSSourceAutoRelease liveSource = obs_get_source_by_name(liveSceneName.c_str());
	if (!liveSource)
		return result;

	obs_scene_t *liveScene = obs_scene_from_source(liveSource);
	if (liveScene) {
		obs_scene_enum_items(
			liveScene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
				obs_source_t *itemSource = obs_sceneitem_get_source(item); // Borrowed from the item.
				if (itemSource && (obs_source_get_output_flags(itemSource) & OBS_SOURCE_AUDIO) != 0) {
					// Own reference: the caller uses it after the enumeration.
					if (obs_source_t *ref = obs_source_get_ref(itemSource))
						static_cast<std::vector<OBSSourceAutoRelease> *>(param)->emplace_back(
							ref);
				}
				return true; // Keep enumerating -- unlike FindBufferFilter, we want ALL matches.
			},
			&result);
	}
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

	for (const OBSSourceAutoRelease &childRef : children) {
		obs_source_t *child = childRef.Get();
		const char *childName = obs_source_get_name(child);
		std::string filterName = autonames::AudioFilterName(childName ? childName : "");

		OBSSourceAutoRelease existing = FindAudioFilterOn(child);
		if (existing)
			continue; // Already attached (under the current or a v0.5.1 name).

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
	// Switching on: the filters under the live scene (a previous cycle's list
	// is normally released already). Switching off: the ones switched on,
	// wherever they are now, unguarded before they're switched off (or
	// DelayFilterEnableSignal() would switch them straight back on).
	if (enabled)
		ForgetActiveAudioFilters();
	const std::vector<OBSSourceAutoRelease> filters = AudioFiltersFor(liveSceneName);
	if (!enabled)
		ForgetActiveAudioFilters();
	const bool foundAny = !filters.empty();

	for (const OBSSourceAutoRelease &filterRef : filters) {
		obs_source_t *filter = filterRef.Get();
		if (!enabled) {
			DisconnectGuard(filter);
			obs_source_set_enabled(filter, false);
			continue;
		}
		// The ring still holds whatever this source sent before the last
		// Disable; flush it so the new Filling window starts clean instead of
		// replaying old audio.
		obs_data_t *settings = obs_source_get_settings(filter);
		obs_data_set_int(settings, "reset_token", obs_data_get_int(settings, "reset_token") + 1);
		obs_source_update(filter, settings);
		obs_data_release(settings);
		obs_source_set_enabled(filter, true);
		{
			std::lock_guard<std::mutex> lock(guardMutex_);
			activeAudioFilters_.push_back(obs_source_get_weak_source(filter));
		}
		ConnectGuard(filter);
	}
	if (enabled)
		warnedFilterSwitchedOff_ = false;

	TRIGGLOW_LOG_INFO(kComponent, "audio delay: SetAudioDelayFiltersEnabled(%s) applied to %s",
			  enabled ? "true" : "false", foundAny ? "at least one filter" : "no filters (none found)");
	return foundAny;
}

bool ObsFrontendBridge::SetAudioDelayFiltersDelaySeconds(const std::string &liveSceneName, uint32_t seconds) const
{
	const std::vector<OBSSourceAutoRelease> filters = AudioFiltersFor(liveSceneName);
	for (const OBSSourceAutoRelease &filter : filters) {
		// Managed first, like the video filter's delay (see the header).
		if (!CallIntProc(filter, "set_managed_delay_seconds", "seconds", seconds))
			TRIGGLOW_LOG_WARN(kComponent, "audio delay: set_managed_delay_seconds proc failed on \"%s\"",
					  obs_source_get_name(filter));
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "delay_seconds", seconds);
		obs_source_update(filter, settings);
		obs_data_release(settings);
	}
	return !filters.empty();
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
									 uint32_t minResolutionHeight, bool capToOutput,
									 bufferfit::RingClass ringClass) const
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

	// Canvas (base_*) and output (output_*) geometry plus the exact fps_num/
	// fps_den: the same inputs EnsureRingSized() reads in Tick(), so the
	// estimate includes the cap at OBS's output resolution. Zeros (query
	// failed) just mean "no cap", same as the filter.
	obs_video_info ovi = {};
	uint32_t baseWidth = 0, baseHeight = 0, outputWidth = 0, outputHeight = 0;
	uint32_t fpsNum = 30, fpsDen = 1;
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0) {
		baseWidth = ovi.base_width;
		baseHeight = ovi.base_height;
		outputWidth = ovi.output_width;
		outputHeight = ovi.output_height;
		fpsNum = ovi.fps_num;
		fpsDen = ovi.fps_den;
	}

	return VideoDelayFilter::EstimateBufferFit(requestedDelaySeconds, minResolutionHeight, width, height, baseWidth,
						   baseHeight, outputWidth, outputHeight, fpsNum, fpsDen, capToOutput,
						   ringClass);
}

std::vector<ActiveVideoEncoder> ObsFrontendBridge::ListActiveVideoEncoders() const
{
	video_t *mainVideo = obs_get_video();
	obs_video_info ovi = {};
	if (!mainVideo || !obs_get_video_info(&ovi))
		return {};

	// The mixes of the other named canvases, compared by pointer only (never
	// dereferenced later, so a canvas removed meanwhile is harmless). Under
	// canvases_mutex: plain reads only. Private canvases aren't listed; their
	// encoders fall into the aspect-ratio heuristic like any other mix.
	struct CanvasCtx {
		const video_t *mainVideo;
		std::vector<const video_t *> others;
	} canvases{mainVideo, {}};
	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) -> bool {
			auto *c = static_cast<CanvasCtx *>(param);
			const video_t *video = obs_canvas_get_video(canvas);
			if (video && video != c->mainVideo)
				c->others.push_back(video);
			return true;
		},
		&canvases);

	struct RawEncoder {
		std::string outputName;
		std::string encoderName;
		std::string encoderId;
		uint32_t width;
		uint32_t height;
		const video_t *parentVideo;
	};
	std::vector<RawEncoder> raw;
	// Runs under outputs_mutex: only lock-free reads in here (see the header).
	// An output in the list isn't freed meanwhile (obs_output_destroy() first
	// unlinks it under this same mutex), and it holds a ref on each of its
	// encoders, which libobs refuses to swap while the output is active.
	obs_enum_outputs(
		[](void *param, obs_output_t *output) -> bool {
			auto *list = static_cast<std::vector<RawEncoder> *>(param);
			if (!obs_output_active(output))
				return true;
			for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; ++idx) {
				obs_encoder_t *encoder = obs_output_get_video_encoder2(output, idx);
				if (!encoder)
					continue;
				const char *outputName = obs_output_get_name(output);
				const char *encoderName = obs_encoder_get_name(encoder);
				const char *encoderId = obs_encoder_get_id(encoder);
				// parent_video, not obs_encoder_video(): with an fps divisor the
				// latter returns a derived video_t, not the mix it renders.
				list->push_back({outputName ? outputName : "", encoderName ? encoderName : "",
						 encoderId ? encoderId : "", obs_encoder_get_width(encoder),
						 obs_encoder_get_height(encoder), obs_encoder_parent_video(encoder)});
			}
			return true;
		},
		&raw);

	const encodercheck::Size mainBase{ovi.base_width, ovi.base_height};
	const encodercheck::Size mainOutput{ovi.output_width, ovi.output_height};
	std::vector<ActiveVideoEncoder> encoders;
	encoders.reserve(raw.size());
	for (RawEncoder &r : raw) {
		encodercheck::VideoSource source = encodercheck::VideoSource::UnlistedMix;
		if (r.parentVideo == mainVideo)
			source = encodercheck::VideoSource::MainMix;
		else if (std::find(canvases.others.begin(), canvases.others.end(), r.parentVideo) !=
			 canvases.others.end())
			source = encodercheck::VideoSource::OtherCanvasMix;
		const encodercheck::Size size{r.width, r.height};
		encoders.push_back({std::move(r.outputName), std::move(r.encoderName), std::move(r.encoderId), r.width,
				    r.height, encodercheck::Classify(source, size, mainBase, mainOutput), source});
	}
	return encoders;
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
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
		mapped = FrontendEvent::RecordingStarting;
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		mapped = FrontendEvent::RecordingStarted;
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		mapped = FrontendEvent::RecordingStopped;
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING:
		mapped = FrontendEvent::ReplayBufferStarting;
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		mapped = FrontendEvent::ReplayBufferStarted;
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		mapped = FrontendEvent::ReplayBufferStopped;
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
		mapped = FrontendEvent::VirtualcamStarted;
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		mapped = FrontendEvent::VirtualcamStopped;
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		mapped = FrontendEvent::FinishedLoading;
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		mapped = FrontendEvent::SceneCollectionChanging;
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		mapped = FrontendEvent::SceneCollectionChanged;
		break;
	case OBS_FRONTEND_EVENT_THEME_CHANGED:
		mapped = FrontendEvent::ThemeChanged;
		break;
	default:
		return; // Don't spam the handler with events we don't act on.
	}

	// v0.7.0: an output that just started is watched for an encoder error.
	if (mapped == FrontendEvent::StreamingStarted || mapped == FrontendEvent::RecordingStarted ||
	    mapped == FrontendEvent::ReplayBufferStarted)
		self->WatchOutputStops();
	self->handler_(mapped);
}

} // namespace trigglow
