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
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "obs-frontend-bridge.hpp"
#include "pipeline-health.hpp"

// buffer-mode-controller owns the state machine for the "no reconnect"
// delay (issue #173 phase 2, video-only): unlike delay-controller (which
// touches the streaming output's own native delay), this mode NEVER
// touches streaming at all — it only orchestrates a wrapper scene + the
// trigglow_video_delay_filter (video-delay-filter.hpp) and switches scenes
// via ObsFrontendBridge, exactly like delay-controller's optional
// reconnect-scene picker already does.
//
// Deliberately a separate class from DelayController rather than folded
// into it: DelayController's DelayState (Inactive/Applying/Active/Error)
// and its whole Enable/Disable/Toggle contract are specifically about
// reconnect semantics. This mode has a different lifecycle (Filling has a
// real duration the user waits out, not a brief "Applying...") and talks
// to a completely different part of OBS (scenes/filters, not the streaming
// output) — keeping them separate avoids overloading one state machine
// with two unrelated meanings.
//
// Qt-free like delay-controller, for the same reason: unit-testable
// without a running OBS process (only ObsFrontendBridge would need
// mocking), and settings-ui.cpp stays the only Qt-facing file.
namespace trigglow {

enum class BufferModeState {
	Inactive, // Not running; live scene shown directly and unmodified.
	Filling,  // Enable() just ran; loading scene showing, filter still disabled, waiting delaySeconds.
	Active,   // Buffer full; wrapper scene showing delayed content, filter enabled.
	Error,
};

struct BufferModeStatus {
	BufferModeState state = BufferModeState::Inactive;
	uint32_t delaySeconds = 5;
	// Seconds the delay really applies from Filling on: delaySeconds, or fewer
	// when the video ring couldn't fit them in the RAM budget at the quality
	// floor. Whole seconds, the same value the video filter delays by and
	// SyncAudioDelayToVideoEffective() pushes into the audio filters (v0.6.0
	// design A.4: VideoDelayFilter::PlannedEffectiveDelaySeconds, known from
	// Enable() on). What viewers get, so the dock and the overlay both show
	// this one -- the only copy of it. 0 while Inactive or in Error.
	uint32_t effectiveDelaySeconds = 0;
	// Fill windows started so far: +1 at Enable() and at every refill
	// (EnterRefill(): a buffer restart, or a delay/quality change that resizes
	// the ring). The dock starts its fill countdown over only when this
	// changes, never on the other status updates that arrive while Filling
	// (an overlay toggle or corner change notifies too).
	uint32_t fillGeneration = 0;
	// Floor for the RAM reduction of the delayed video's resolution --
	// VideoDelayFilter never reduces below this for RAM, shortening the ACTUAL
	// buffered duration instead if delaySeconds doesn't fit the RAM budget at
	// this height. It never raises the buffer above OBS's output resolution,
	// though (v0.5.1): a 720p output buffers at 720p even with 1080p here.
	// See VideoDelayFilter::EnsureRingSized. 720 matches
	// kDefaultMinResolutionHeight.
	uint32_t minResolutionHeight = 720;
	std::string liveSceneName;    // Required to Enable(); the streamer's real content.
	std::string loadingSceneName; // Optional; shown during Filling. Empty = no scene switch while filling.
	// Show a "Delay Ns" text overlay on the delayed output while Active.
	bool showOverlay = true;
	// Canvas corner for the overlay: 0 top-left (default), 1 top-right, 2 bottom-left, 3 bottom-right.
	uint32_t overlayCorner = 0;
	std::string message; // Human-readable (Spanish), empty outside Error/info states.
};

using BufferStatusChangedCallback = std::function<void(const BufferModeStatus &)>;

class BufferModeController {
public:
	explicit BufferModeController(ObsFrontendBridge &bridge);

	void SetStatusChangedCallback(BufferStatusChangedCallback callback) { onStatusChanged_ = std::move(callback); }

	// Free account gate (see AuthManager): if set, Enable() calls this before
	// doing anything else and refuses (Error state) if it returns false.
	// Unset by default (always allowed) so existing tests/mocking that never
	// touch AuthManager keep working unchanged. Deliberately just a predicate,
	// not a dependency on AuthManager itself, to keep this class Qt-free and
	// unit-testable without a running OBS process -- same reasoning as the
	// rest of this header. Single choke point: both the dock's Enable button
	// and the hotkeys/Stream Deck path (hotkeys.cpp) call straight into
	// Enable()/Toggle(), so gating here (rather than in each caller) covers
	// both for free and can never drift out of sync between them.
	void SetAuthorizationCheck(std::function<bool()> check) { isAuthorized_ = std::move(check); }

	// Ensures the wrapper scene/filter exist for the configured live scene,
	// switches to the loading scene (if set), and moves to Filling. The
	// dock is responsible for arming a single-shot timer for
	// status_.delaySeconds and calling OnFillTimerElapsed() once it fires
	// (same pattern as DelayController's apply watchdog — see
	// settings-ui.cpp's ArmApplyWatchdog).
	void Enable();

	// Disables the filter and switches back to whatever scene was active
	// right before Enable() (falls back to the live scene itself if that's
	// unavailable). Safe to call from any state, including mid-Filling. Also
	// what a scene collection switch does to a running delay (see
	// StopForSceneCollectionChange()).
	void Disable();

	void Toggle();

	void SetLiveScene(std::string sceneName);
	void SetLoadingScene(std::string sceneName);
	void SetDelaySeconds(uint32_t seconds);
	void SetMinResolutionHeight(uint32_t heightPixels);
	// Toggles the "Delay Ns" overlay; takes effect immediately if Active.
	void SetShowOverlay(bool show);
	void SetOverlayCorner(uint32_t corner);

	BufferModeStatus GetStatus() const { return status_; }

	// The scenes the dock offers as live/loading scene, so settings-ui never
	// needs to touch ObsFrontendBridge directly: every scene of the collection
	// in OBS's order except the wrapper scene (current or v0.5.1 name), which
	// the delay itself shows and can never be one of them.
	std::vector<std::string> ListAvailableScenes() const;

	// Informational only (bytes) -- what the video buffer's VRAM budget
	// actually is on this machine right now, detected from real hardware
	// where possible. Never restricts delaySeconds/minResolutionHeight,
	// just lets the dock show the user what their choices are working with.
	uint64_t GetBufferBudgetBytes() const { return bridge_.GetBufferBudgetBytes(); }

	// Predicts what the current live scene at (requestedDelaySeconds,
	// minResolutionHeight) would actually achieve -- see
	// ObsFrontendBridge::EstimateBufferFit. For live-updating the dock as
	// the user adjusts either control, before Enable() has even run once.
	// Informational only (no warning of its own): with the delay on, the
	// cycle's storage class; before Enable, the one the next cycle would get.
	VideoDelayFilter::BufferFitEstimate EstimateBufferFit(uint32_t requestedDelaySeconds,
							      uint32_t minResolutionHeight) const
	{
		return bridge_.EstimateBufferFit(status_.liveSceneName, requestedDelaySeconds, minResolutionHeight,
						 bufferOutputCap_, PlannedStorageInter());
	}

	// Health of the video buffer's pipeline (v0.6.0, design B.8): the
	// degradation rung it's on, why, and what it buffers right now -- for a
	// non-blocking dock notice such as "Buffer reduced to 1280x720 because of
	// CPU load" (PipelineHealth::Degraded()). Cheap and never blocks on the
	// graphics thread, so the dock can poll it (e.g. once a second); UI
	// thread. A default PipelineHealth (running == false) when the filter
	// doesn't exist yet.
	PipelineHealth GetPipelineHealth() const;

	// Called once by the dock's fill timer after status_.delaySeconds has
	// elapsed since Enable(). No-op if we're no longer Filling (e.g. the
	// user pressed Disable before the timer fired).
	void OnFillTimerElapsed();

	// v0.7.0: once a second while Filling/Active (the dock's health timer),
	// UI thread: pushes OBS's active video encoders to the video filter again
	// when they changed without a frontend event (outputs other plugins
	// start) and watches those outputs for encoder errors, reports the
	// frames OBS's encoders' own mixes skipped for encoding lag to the video
	// filter's storage codec policy, and lets the buffer's hardware encoder
	// come back once an output that made it yield has started (3 s after, or
	// 10 s after the yield at the latest). Never blocks on the graphics thread.
	void Tick();

	// Persistence, mirroring DelayController::LoadSettings/SaveSettings.
	// Deliberately never auto-Enable()s on load, same reasoning as
	// DelayController: this creates/modifies OBS scenes, which should never
	// happen silently at obs_module_load() time.
	//
	// bufferOutputCap is the hidden "buffer_output_cap" key of
	// buffer-mode-settings.json (no dock control, by design): false turns off
	// the v0.5.1 cap at OBS's output resolution (VideoDelayFilter then sizes
	// like v0.5.0). Only read at load, so changing it needs an OBS restart.
	// codecWorkers is the hidden "codec_workers" key (v0.6.0 kill switch, same
	// rules): false runs the MJPEG encode/decode on OBS's graphics thread, as
	// before v0.6.0, instead of on worker threads. bufferCodecAuto is the
	// hidden "buffer_codec" key (v0.7.0): false ("mjpeg") keeps the buffer on
	// MJPEG instead of a hardware inter-frame codec. codec_workers=false wins
	// over it (the hardware codec needs the workers).
	void LoadSettings(uint32_t delaySeconds, uint32_t minResolutionHeight, std::string liveSceneName,
			  std::string loadingSceneName, bool showOverlay = true, uint32_t overlayCorner = 0,
			  bool bufferOutputCap = true, bool codecWorkers = true, bool bufferCodecAuto = true);
	struct SettingsSnapshot {
		uint32_t delaySeconds;
		uint32_t minResolutionHeight;
		std::string liveSceneName;
		std::string loadingSceneName;
		bool showOverlay;
		uint32_t overlayCorner;
		bool bufferOutputCap;
		bool codecWorkers;
		bool bufferCodecAuto;
	};
	SettingsSnapshot SaveSettings() const;

	using SceneListRefreshCallback = std::function<void()>;

	// Fires once OBS has actually finished loading the scene collection
	// (FrontendEvent::FinishedLoading), and again when the user switches scene
	// collection (FrontendEvent::SceneCollectionChanged). Exists because of a real bug found
	// live, 2026-08-27: LoadSettings() correctly restores
	// liveSceneName/loadingSceneName from disk well before this fires, but
	// obs_module_load() (and therefore TrigglowDelayDock's construction,
	// and its first scene-combo population from ListAvailableScenes()) runs
	// BEFORE OBS has loaded the scene collection -- so the combo boxes came
	// up empty on every fresh OBS start (and every reinstall) even though
	// the saved scene names were fine internally, making it look like the
	// plugin "forgot" the user's configuration. The dock wires this to
	// repopulate its combos once scenes are actually queryable.
	void SetSceneListRefreshCallback(SceneListRefreshCallback callback)
	{
		onSceneListRefresh_ = std::move(callback);
	}

	// Fires when OBS switches theme or font size
	// (OBS_FRONTEND_EVENT_THEME_CHANGED): the dock recomputes its colors and
	// fonts from the new palette. Here only because the frontend bridge's
	// single event handler is this controller.
	void SetThemeChangedCallback(std::function<void()> callback) { onThemeChanged_ = std::move(callback); }

private:
	void SetState(BufferModeState state, std::string message = {});
	void NotifyStatusChanged();
	void OnFrontendEvent(FrontendEvent event);

	// Disable()'s body: the filters, keep-alive and overlay switched off,
	// Inactive with `message` (empty: the default detail). Program goes back
	// to the scene shown before Enable() only if `restoreProgram`.
	void StopDelay(std::string message, bool restoreProgram);

	// A scene collection switch with the delay Filling or Active: the delay
	// can't follow (its helper scene and filters belong to the old
	// collection), so it stops in an orderly way and the dock says why
	// (Status.CollectionChanged) instead of staying "Active" over a collection
	// that has no delay. On SceneCollectionChanging (oldCollectionLoaded): like
	// Disable(), before OBS saves the old collection -- so it's saved with
	// its filters off and its scene back on Program. OBS 31 sends no
	// CHANGING for a duplicate or a rename, only SceneCollectionChanged, with
	// the copy already loaded as it was saved: then the copy's filters are
	// switched off (by name; OnFrontendEvent's DisableAllDelayFilters() covers
	// the rest), and Program leaves the helper scene for the scene shown
	// before Enable() only if the copy opened on the helper scene. No-op in
	// any other state.
	void StopForSceneCollectionChange(bool oldCollectionLoaded);

	// Pushes whichever is SMALLER of status_.delaySeconds and what
	// ObsFrontendBridge::GetVideoEffectiveDelaySeconds() reports video is
	// actually able to buffer right now, into AudioDelayFilter. Found live,
	// 2026-08-27: on long/high-quality delays where the RAM budget doesn't
	// fit the full requested duration, VideoDelayFilter silently shortens
	// its OWN actual delay (EnsureRingSized), but AudioDelayFilter always
	// buffers the full requested seconds (cheap PCM, never RAM-shortened) --
	// so audio kept drifting noticeably behind the shorter video delay
	// whenever this happened. The filter answers from its settings as they
	// are at the call (VideoDelayFilter::PlannedEffectiveDelaySeconds), so
	// this is right from Enable() on; called again when the fill window
	// elapses (OnFillTimerElapsed) and on a buffer restart, plus
	// on any later delay/quality change while Active, since either can
	// change whether shortening is needed. Stores the result in
	// status_.effectiveDelaySeconds; the caller notifies.
	void SyncAudioDelayToVideoEffective();

	// Shows (Active + enabled in settings) or hides the "Delay Ns" overlay,
	// with the delay really in effect (video may have been shortened).
	void UpdateOverlay();

	// The seconds viewers really get: status_.effectiveDelaySeconds once
	// known, never above status_.delaySeconds -- what the overlay shows, the
	// Filling message counts and the dock's fill countdown waits.
	uint32_t AppliedDelaySeconds() const;
	// Status.Filling for AppliedDelaySeconds(), or Status.FillingTrimmed
	// ("41 of your 60 s") when the RAM budget trimmed the delay.
	std::string FillingMessage() const;

	// A hard resize empties the video ring, so the delayed view is blank for
	// `delay` seconds: Active goes back to Filling with the loading scene on
	// Program (and the fill timer armed again by the dock) instead of showing
	// black. BufferRestartsWith() predicts it for a setting the controller
	// changes itself (so Program switches BEFORE the filter empties the ring);
	// OnBufferRestarted() handles the filter's own report
	// (FrontendEvent::BufferRestarted: any hard resize, OBS video resets
	// included -- in Filling it starts the fill window over).
	bool BufferRestartsWith(uint32_t previousDelaySeconds, uint32_t previousMinResolutionHeight) const;
	void ShowLoadingSceneForRefill();
	void EnterRefill(const char *reason);
	void OnBufferRestarted();

	// While Filling/Active with the output cap on: logs a WARN, once per
	// encoder and size per delay cycle, for each video encoder of an active
	// output that renders OBS's main canvas and encodes above the (capped)
	// delay buffer -- e.g. a recording rescaled to the canvas, or an Enhanced
	// Broadcasting track above the output -- since it gets upscaled delayed
	// video, and the hidden buffer_output_cap setting can lift the cap. Only
	// when lifting it would actually buffer more (not when RAM is what limits
	// the buffer); the text says lifting it can shorten the delay if RAM runs
	// short. The decision is encodercheck::ShouldWarn(); GPU-rescaled encoders
	// are attributed to the main canvas by aspect ratio (see
	// encodercheck::Classify() for the limits). Called from Enable(),
	// OnFillTimerElapsed() and the streaming/recording/replay buffer "started"
	// frontend events: all on the UI thread, which is where
	// ObsFrontendBridge::ListActiveVideoEncoders() must run.
	void WarnAboutEncodersAboveBuffer();

	// v0.7.0 storage codec: OBS's active video encoders (one listing), to the
	// video filter: their families (PushObsEncoders()) and the ceiling they
	// put on the buffer (PushEncoderCeiling()). At Enable (before the filter
	// is switched on), at every output start or stop, and from Tick() when
	// they changed without a frontend event (force false: only then).
	void RefreshObsEncoders(bool force);
	std::vector<std::string> encoderSignature_; // What RefreshObsEncoders() pushed last.
	void PushObsEncoders(const std::vector<ActiveVideoEncoder> &active);
	// v0.7.0 (design 6.3): the ceiling OBS's main-canvas encoders put on the
	// buffer (Enhanced Broadcasting's tracks only exist after Go Live).
	void PushEncoderCeiling(const std::vector<ActiveVideoEncoder> &active);
	uint32_t lastCeilingWidth_ = UINT32_MAX; // What was pushed (UINT32_MAX: nothing yet this cycle).
	uint32_t lastCeilingHeight_ = UINT32_MAX;
	// The storage class of the cycle running, or (Inactive) of the next one:
	// inter only with buffer_codec auto, the codec workers, and a hardware
	// codec that passed its probe this session (the first Enable of a session
	// is always intra, the conservative one). Design 6.4, invariant I9.
	bool PlannedStorageInter() const;
	bool NextCycleStorageInter() const;
	bool cycleStorageInter_ = false; // Fixed at Enable(), pushed to the filter.
	bool bufferCodecAuto_ = true;    // Hidden buffer_codec setting, see LoadSettings().
	// An output is starting: the filter yields an NVENC session it may hold,
	// waiting up to 100 ms only near the old session limits (OBS's NVENC
	// encoders + the starting one + the buffer's >= 4).
	void YieldForOutputStart();
	bool yieldPending_ = false;
	uint64_t yieldSinceNs_ = 0;
	uint64_t outputStartedNs_ = 0; // An output started since the yield (0 = not yet).

	// The effective delay lives in status_.effectiveDelaySeconds (the one value
	// audio, video, the dock and the overlay share), not in a member here.
	bool bufferOutputCap_ = true; // Hidden buffer_output_cap setting, see LoadSettings().
	bool codecWorkers_ = true;    // Hidden codec_workers setting (kill switch), see LoadSettings().
	// (encoder name, width, height) already warned about this cycle
	// (WarnAboutEncodersAboveBuffer); cleared by Enable().
	std::set<std::tuple<std::string, uint32_t, uint32_t>> warnedEncoders_;

	ObsFrontendBridge &bridge_;
	BufferModeStatus status_;
	// Scene that was on Program right before Enable() switched away from
	// it, restored by Disable(). Empty when not currently enabled.
	std::string sceneBeforeEnable_;
	// True between a successful AcquireLiveSceneRendering() (in Enable())
	// and the matching ReleaseLiveSceneRendering() (in OnFillTimerElapsed()
	// once the wrapper scene takes over showing it naturally, or in
	// Disable() if we're interrupted mid-Filling). Tracked so we never
	// double-release or leak the keep-alive.
	bool liveSceneRenderingHeld_ = false;
	BufferStatusChangedCallback onStatusChanged_;
	SceneListRefreshCallback onSceneListRefresh_;
	std::function<void()> onThemeChanged_;
	std::function<bool()> isAuthorized_;
};

} // namespace trigglow
