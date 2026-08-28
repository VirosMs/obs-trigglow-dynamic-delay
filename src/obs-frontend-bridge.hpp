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

#include "video-delay-filter.hpp" // VideoDelayFilter::BufferFitEstimate, for EstimateBufferFit()

extern "C" {
#include <obs-frontend-api.h>  // needed here too: obs_frontend_event is the exact callback param type
#include <graphics/graphics.h> // gs_texrender_t, for AcquireLiveSceneRendering's private render target
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

	// Passthrough to VideoDelayFilter::GetBufferBudgetBytes() so settings-ui
	// never needs to include video-delay-filter.hpp itself. See that
	// method's comment.
	uint64_t GetBufferBudgetBytes() const;

	// Predicts what EnsureRingSized() would actually do for `liveSceneName`
	// at its real current width/height/fps -- see
	// VideoDelayFilter::EstimateBufferFit's comment. Works even before
	// EnsureBufferWrapperScene has ever run (reads the live scene directly,
	// not the filter), so the dock can warn the user as they adjust the
	// seconds/quality controls, before they've pressed Enable at all.
	// Returns a zeroed estimate if liveSceneName doesn't resolve.
	VideoDelayFilter::BufferFitEstimate EstimateBufferFit(const std::string &liveSceneName,
							      uint32_t requestedDelaySeconds,
							      uint32_t minResolutionHeight) const;

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

	// --- No-reconnect buffer mode orchestration (issue #173 phase 2) ---
	//
	// IMPORTANT: nesting an existing scene inside another scene (via
	// obs_scene_add) does NOT duplicate it — obs_sceneitem_get_source() on
	// the resulting item returns the SAME obs_source_t as the original
	// scene. A filter attached to that source therefore affects the scene
	// EVERYWHERE it's used, not just inside our wrapper. So the delay
	// filter is attached to the live scene ONCE (idempotent,
	// EnsureBufferWrapperScene) and its effect is controlled entirely via
	// SetBufferFilterEnabled(), never by adding/removing it — enabling it
	// only while our own scene-switch flow is actively showing the wrapper
	// (never while the raw live scene could also be on Program directly).

	// Idempotent: ensures the wrapper scene exists, contains a scene-item
	// wrapping `liveSceneName`, and that scene-item's source has our video
	// delay filter attached (disabled by default — see SetBufferFilterEnabled).
	// Returns false if liveSceneName doesn't resolve to a real scene, or
	// setup otherwise fails.
	bool EnsureBufferWrapperScene(const std::string &liveSceneName) const;

	// Enables/disables the buffer filter attached by EnsureBufferWrapperScene.
	// No-op (returns false) if that hasn't been called successfully yet for
	// the current live scene.
	bool SetBufferFilterEnabled(const std::string &liveSceneName, bool enabled) const;

	// Pushes `seconds` into the already-attached filter's configured delay.
	// No-op (returns false) if EnsureBufferWrapperScene hasn't run yet.
	bool SetBufferFilterDelaySeconds(const std::string &liveSceneName, uint32_t seconds) const;

	// Pushes `heightPixels` into the already-attached filter's minimum
	// capture resolution floor (see VideoDelayFilter::EnsureRingSized).
	// No-op (returns false) if EnsureBufferWrapperScene hasn't run yet.
	bool SetBufferFilterMinResolutionHeight(const std::string &liveSceneName, uint32_t heightPixels) const;

	// What the video filter is ACTUALLY delaying by right now, in seconds --
	// can be less than its configured delay if the RAM budget didn't fit the
	// full requested duration at the chosen quality floor (see
	// VideoDelayFilter::EnsureRingSized). 0 if the filter doesn't exist yet
	// or its ring hasn't been sized yet. Used to keep AudioDelayFilter (which
	// always buffers the full requested delay -- cheap PCM, never
	// RAM-shortened) from silently drifting ahead of a shortened video delay
	// -- see BufferModeController::SyncAudioDelayToVideoEffective.
	uint32_t GetVideoEffectiveDelaySeconds(const std::string &liveSceneName) const;

	// Switches Program to the wrapper scene created by EnsureBufferWrapperScene.
	// Callers don't need to know its literal name. False if it doesn't exist
	// yet (EnsureBufferWrapperScene hasn't been called/succeeded).
	bool ShowBufferWrapperScene() const;

	// --- Audio delay orchestration ---
	//
	// AudioDelayFilter can't be attached to the live SCENE the way
	// VideoDelayFilter is -- verified live, 2026-08-25, and by reading
	// obs_source_filter_add()'s real implementation: it silently refuses
	// any filter requesting OBS_SOURCE_AUDIO on a source whose own
	// output_flags don't include it, and scenes never advertise audio in
	// output_flags (their audio goes through a separate .audio_render
	// callback, not the per-source filter chain). So instead, one instance
	// is attached to each individual audio-capable LEAF source directly
	// inside the live scene (Mic, Desktop, etc.) -- those DO advertise
	// OBS_SOURCE_AUDIO on themselves, so attachment works normally, and
	// (also verified live) their filter_audio keeps firing on schedule with
	// NO keep-alive trick needed, unlike video: a leaf source's own
	// obs_source_output_audio() -> filter-chain pipeline runs continuously
	// regardless of whether that source is part of the current Program
	// scene's tree.
	//
	// Known limitation: only DIRECT children of the live scene are covered
	// -- audio from a source nested inside a SUB-scene/group within the
	// live scene won't get delayed. Acceptable for now; revisit if it turns
	// out to matter.

	// Idempotent: attaches AudioDelayFilter (disabled by default, matching
	// SetBufferFilterEnabled's pattern) to every direct child of
	// `liveSceneName` that advertises OBS_SOURCE_AUDIO and doesn't already
	// have one. Returns false if liveSceneName doesn't resolve to a real
	// scene.
	bool EnsureAudioDelayFilters(const std::string &liveSceneName) const;

	// Enables/disables every AudioDelayFilter attached under liveSceneName
	// by EnsureAudioDelayFilters(). Returns false if liveSceneName doesn't
	// resolve to a real scene (not an error if it simply has no audio
	// children -- that's a normal, silent no-op).
	bool SetAudioDelayFiltersEnabled(const std::string &liveSceneName, bool enabled) const;

	// Pushes `seconds` into every AudioDelayFilter attached under
	// liveSceneName. Same not-an-error semantics as SetAudioDelayFiltersEnabled.
	bool SetAudioDelayFiltersDelaySeconds(const std::string &liveSceneName, uint32_t seconds) const;

	// Nothing calls obs_source_video_render() on a source that isn't part of
	// whatever's currently on Program/Preview (or nested inside it) — OBS's
	// per-frame render pass is just "run the registered draw callbacks, then
	// render the active view's scene tree" (see libobs/obs-video.c
	// render_main_texture()), nothing walks a global "showing" list. During
	// the Filling window Program shows the loading scene, NOT the live scene
	// and not yet the wrapper scene either, so without this the live scene
	// (and its attached buffer filter) never renders a single frame while
	// "filling" — confirmed live (2026-08-24): the delay collapsed to ~0
	// within a second of switching to the wrapper scene, because the ring
	// buffer had been sitting empty the entire time.
	//
	// Fix, mirroring the technique the well-known "Source Record" OBS
	// plugin uses to capture a source that isn't the active scene:
	//   1. obs_add_main_render_callback() to force obs_source_video_render()
	//      on the live scene every frame regardless of what Program shows.
	//   2. Render it into our OWN throwaway gs_texrender_t, never the real
	//      Program output texture — draw callbacks run with the render
	//      target already pointed at that texture (see obs-video.c), so
	//      rendering the live scene directly there would visibly smear it
	//      into whatever's on screen. Same private-texrender pattern
	//      VideoDelayFilter::Render() already uses for its ring buffer.
	//   3. obs_source_inc_showing() — ONLY this, not obs_source_inc_active()
	//      too. Tried adding inc_active on top of inc_showing (hoping it'd
	//      also help audio) and, separately, swapping to inc_active alone —
	//      both regressed video with no visible error, confirmed live twice
	//      on 2026-08-25: our draw callback still fired, but
	//      VideoDelayFilter::Render() never ran at all, not even its own
	//      diagnostic branches. Best working theory: obs-source.c's
	//      render_video() guards filter application with a plain bool
	//      (`rendering_filter`, not per-call-stack), and making the scene
	//      "active" seems to make something else in libobs also attempt to
	//      render it the same frame, tripping that guard and falling back
	//      to obs_source_main_render() — the scene's raw children, no
	//      filters. inc_showing alone was proven working before any of
	//      this; stay there rather than stack unverified primitives.
	// Must be paired 1:1 with ReleaseLiveSceneRendering(); only one live
	// scene can be held at a time (matches the rest of this design, see the
	// header comment above).
	bool AcquireLiveSceneRendering(const std::string &liveSceneName);

	// Releases everything acquired by AcquireLiveSceneRendering(). False if
	// nothing is currently held.
	bool ReleaseLiveSceneRendering(const std::string &liveSceneName);

private:
	// Must match obs_frontend_event_cb exactly: void(*)(enum obs_frontend_event, void*).
	static void FrontendEventCallback(enum obs_frontend_event event, void *privateData);

	// Matches obs_add_main_render_callback's signature. Renders
	// liveSceneRenderSource_ into liveSceneRenderTarget_ (never the real
	// Program output) — see AcquireLiveSceneRendering's comment.
	static void RenderLiveSceneCallback(void *param, uint32_t cx, uint32_t cy);

	// Locates the video-delay filter previously attached to `liveSceneName`'s
	// source by EnsureBufferWrapperScene, if the wrapper scene, its
	// scene-item, and the filter all still exist. Returned pointer is
	// borrowed (caller must NOT release it) — release the object obtained
	// from obs_source_get_filter_by_name would be correct per libobs
	// convention, but every call site here needs it only transiently, so
	// this wraps that release internally and returns a raw non-owning
	// pointer valid only for the duration of the calling function.
	obs_source_t *FindBufferFilter(const std::string &liveSceneName) const;

	// Collects the direct children of `liveSceneName` that advertise
	// OBS_SOURCE_AUDIO (see EnsureAudioDelayFilters's header comment for
	// why only direct children, and why this naturally skips nested
	// scenes/groups without needing to special-case them). Every returned
	// pointer is a borrowed reference (owned by the scene item), valid only
	// for the duration of the calling function -- same convention as
	// FindBufferFilter.
	std::vector<obs_source_t *> GetAudioCapableChildren(const std::string &liveSceneName) const;

	FrontendEventHandler handler_;
	bool initialized_ = false;

	// Owned strong ref to the scene AcquireLiveSceneRendering() is currently
	// keeping alive, or null if nothing is held. Only one at a time (matches
	// the rest of buffer mode's single-live-scene design).
	obs_source_t *liveSceneRenderSource_ = nullptr;
	// Throwaway render target RenderLiveSceneCallback() draws into every
	// frame. Its contents are never read — see AcquireLiveSceneRendering's
	// comment for why we can't just render straight to the real output.
	gs_texrender_t *liveSceneRenderTarget_ = nullptr;

	// One-shot: logs the first time RenderLiveSceneCallback actually fires
	// after an Acquire, so a "the buffer never fills" bug report can tell
	// whether the draw callback itself never ran versus it running but not
	// reaching the filter (see VideoDelayFilter::Render()'s own one-shot
	// logs for that second half of the picture).
	bool loggedFirstRenderCallback_ = false;
};

} // namespace trigglow
