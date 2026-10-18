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

#include "buffer-mode-controller.hpp"
#include "i18n.hpp"
#include "logging.hpp"

#include <cstdio>

namespace trigglow {

namespace {
constexpr const char *kComponent = "buffer-mode-controller";

// Replaces the FIRST "%1" in a Str() template with `value` -- this file has
// no Qt (no QString::arg) and isn't worth pulling in a format library for
// the one status message that needs a numeric substitution (Filling's
// seconds count). Not a general-purpose formatter: only ever called with
// templates that contain exactly one "%1", by construction (see
// data/locale/en-US.ini).
std::string FormatOne(const char *tmpl, const std::string &value)
{
	std::string result = tmpl;
	size_t pos = result.find("%1");
	if (pos != std::string::npos)
		result.replace(pos, 2, value);
	return result;
}
} // namespace

BufferModeController::BufferModeController(ObsFrontendBridge &bridge) : bridge_(bridge)
{
	// The only subscriber to ObsFrontendBridge's frontend-event callback in
	// the currently-active (buffer-mode) code path -- DelayController owned
	// this before buffer mode replaced it as the only instantiated
	// controller (see plugin-main.cpp), leaving it wired to nothing. See
	// SetSceneListRefreshCallback's comment for why this is needed.
	bridge_.Init([this](FrontendEvent event) { OnFrontendEvent(event); });
}

void BufferModeController::SetLiveScene(std::string sceneName)
{
	status_.liveSceneName = std::move(sceneName);
	NotifyStatusChanged();
}

void BufferModeController::SetLoadingScene(std::string sceneName)
{
	status_.loadingSceneName = std::move(sceneName);
	NotifyStatusChanged();
}

void BufferModeController::SetDelaySeconds(uint32_t seconds)
{
	const uint32_t previousSeconds = status_.delaySeconds;
	status_.delaySeconds = seconds;
	NotifyStatusChanged();
	if (status_.state == BufferModeState::Active) {
		const bool refill = BufferRestartsWith(previousSeconds, status_.minResolutionHeight);
		if (refill)
			ShowLoadingSceneForRefill(); // Before the filter empties its ring.
		bridge_.SetBufferFilterDelaySeconds(status_.liveSceneName, seconds);
		// The filter computes its effective delay from the settings just
		// pushed (its ring follows on the next Render()), so audio gets the
		// new value right now. See SyncAudioDelayToVideoEffective's comment.
		SyncAudioDelayToVideoEffective();
		if (refill)
			EnterRefill("delay changed");
		UpdateOverlay();
	}
}

void BufferModeController::SetShowOverlay(bool show)
{
	status_.showOverlay = show;
	NotifyStatusChanged();
	UpdateOverlay();
}

void BufferModeController::SetOverlayCorner(uint32_t corner)
{
	status_.overlayCorner = corner > 3 ? 0 : corner;
	NotifyStatusChanged();
	UpdateOverlay();
}

void BufferModeController::UpdateOverlay()
{
	bool visible = status_.showOverlay && status_.state == BufferModeState::Active;
	std::string text;
	if (visible) {
		uint32_t seconds = effectiveDelaySeconds_ > 0 ? effectiveDelaySeconds_ : status_.delaySeconds;
		text = FormatOne(Str("Overlay.Text"), std::to_string(seconds));
	}
	bridge_.SetDelayOverlay(visible, text, status_.overlayCorner);
}

void BufferModeController::SetMinResolutionHeight(uint32_t heightPixels)
{
	const uint32_t previousHeight = status_.minResolutionHeight;
	status_.minResolutionHeight = heightPixels;
	NotifyStatusChanged();
	if (status_.state == BufferModeState::Active) {
		const bool refill = BufferRestartsWith(status_.delaySeconds, previousHeight);
		if (refill)
			ShowLoadingSceneForRefill(); // Before the filter empties its ring.
		bridge_.SetBufferFilterMinResolutionHeight(status_.liveSceneName, heightPixels);
		// A quality-floor change can change the ring's RAM fit just as much
		// as a delay change can -- re-check whether audio needs shortening
		// to match. See SyncAudioDelayToVideoEffective's comment.
		SyncAudioDelayToVideoEffective();
		if (refill)
			EnterRefill("minimum quality changed");
		UpdateOverlay();
	}
}

void BufferModeController::Enable()
{
	// Guard against re-entry: pressing Enable again while already
	// Filling/Active must be a no-op, not re-run the whole sequence. Found
	// live (2026-08-24): re-running it while Active overwrote
	// sceneBeforeEnable_ with the WRAPPER scene itself (since that's what's
	// on Program at that point), not the true original live scene — Disable()
	// would then "restore" to the wrapper instead of the real live content.
	// The observed symptom was Program getting stuck on the loading scene /
	// black after a second Enable press.
	if (status_.state == BufferModeState::Filling || status_.state == BufferModeState::Active)
		return;

	// Free account gate (see AuthManager / SetAuthorizationCheck's comment):
	// checked before anything else so a logged-out Enable() (from the dock
	// button OR a hotkey/Stream Deck press) never touches OBS state at all.
	if (isAuthorized_ && !isAuthorized_()) {
		SetState(BufferModeState::Error, Str("Status.LoginRequired"));
		return;
	}

	if (status_.liveSceneName.empty()) {
		SetState(BufferModeState::Error, Str("Status.ChooseLiveScene"));
		return;
	}

	if (!bridge_.EnsureBufferWrapperScene(status_.liveSceneName)) {
		SetState(BufferModeState::Error, Str("Status.WrapperSceneFailed"));
		return;
	}
	bridge_.SetBufferFilterDelaySeconds(status_.liveSceneName, status_.delaySeconds);
	bridge_.SetBufferFilterMinResolutionHeight(status_.liveSceneName, status_.minResolutionHeight);
	// Every cycle, so the plugin settings file always wins over whatever the
	// filter saved with the scene collection.
	bridge_.SetBufferFilterOutputCap(status_.liveSceneName, bufferOutputCap_);
	bridge_.SetBufferFilterCodecWorkers(status_.liveSceneName, codecWorkers_);

	// Audio: unlike the video filter, these attach to individual leaf audio
	// sources inside the live scene (Mic, Desktop, etc.), not the scene
	// itself -- see ObsFrontendBridge::EnsureAudioDelayFilters's comment for
	// why. No keep-alive needed for these (verified live, 2026-08-25): a
	// leaf source's own audio pipeline runs continuously regardless of
	// Program visibility, unlike video rendering. Not treated as fatal if
	// the live scene simply has no audio sources.
	bridge_.EnsureAudioDelayFilters(status_.liveSceneName);
	// The video filter reports the delay its ring will have for the settings
	// just pushed (Render() hasn't sized it yet), so audio starts out with
	// the shortened delay when RAM shortens it. See
	// SyncAudioDelayToVideoEffective's comment.
	SyncAudioDelayToVideoEffective();
	bridge_.SetAudioDelayFiltersEnabled(status_.liveSceneName, true);

	// Enable the filter AND force the live scene to keep rendering in the
	// background right now, not once the fill timer elapses: Program is
	// about to switch to the loading scene, and without both of these the
	// live scene stops rendering entirely (nothing shows it), so the
	// filter's ring buffer would sit empty for the whole Filling window
	// instead of actually accumulating delayFrames worth of history. See
	// ObsFrontendBridge::AcquireLiveSceneRendering's comment for the full
	// story (found live, 2026-08-24: the delay collapsed to ~0 within a
	// second of switching to the delayed view).
	bridge_.SetBufferFilterEnabled(status_.liveSceneName, true);
	liveSceneRenderingHeld_ = bridge_.AcquireLiveSceneRendering(status_.liveSceneName);

	sceneBeforeEnable_ = bridge_.GetCurrentSceneName();
	if (!status_.loadingSceneName.empty())
		bridge_.SetCurrentSceneByName(status_.loadingSceneName);

	SetState(BufferModeState::Filling, FormatOne(Str("Status.Filling"), std::to_string(status_.delaySeconds)));
	TRIGGLOW_LOG_INFO(kComponent, "enabled: live=\"%s\" loading=\"%s\" delay=%us output_cap=%s codec_workers=%s",
			  status_.liveSceneName.c_str(), status_.loadingSceneName.c_str(), status_.delaySeconds,
			  bufferOutputCap_ ? "on" : "off", codecWorkers_ ? "on" : "off");

	// New cycle: warn again about any encoder above the buffer, starting with
	// the outputs already running right now.
	warnedEncoders_.clear();
	WarnAboutEncodersAboveBuffer();
}

void BufferModeController::OnFillTimerElapsed()
{
	if (status_.state != BufferModeState::Filling)
		return;

	// The filter was already enabled back in Enable(); the wrapper scene
	// nesting the live scene now naturally keeps it rendering once it's on
	// Program, so the manual keep-alive from Enable() can be released.
	if (liveSceneRenderingHeld_) {
		bridge_.ReleaseLiveSceneRendering(status_.liveSceneName);
		liveSceneRenderingHeld_ = false;
	}
	bridge_.ShowBufferWrapperScene();
	// Normally the value Enable() already pushed; re-read in case OBS's
	// video settings changed the fit meanwhile. See
	// SyncAudioDelayToVideoEffective's comment.
	SyncAudioDelayToVideoEffective();
	SetState(BufferModeState::Active);
	UpdateOverlay();
	TRIGGLOW_LOG_INFO(kComponent, "buffer full, now showing delayed content");
	// Catches outputs started during Filling without a frontend event (e.g.
	// by another plugin); already-warned encoders stay silent.
	WarnAboutEncodersAboveBuffer();
}

bool BufferModeController::BufferRestartsWith(uint32_t previousDelaySeconds, uint32_t previousMinResolutionHeight) const
{
	// The filter keeps its ring only if bufferfit sizes it exactly the same
	// (the same Compute() it runs, through EstimateBufferFit()).
	const auto before = bridge_.EstimateBufferFit(status_.liveSceneName, previousDelaySeconds,
						      previousMinResolutionHeight, bufferOutputCap_);
	const auto after = bridge_.EstimateBufferFit(status_.liveSceneName, status_.delaySeconds,
						     status_.minResolutionHeight, bufferOutputCap_);
	return before.width != after.width || before.height != after.height ||
	       before.actualSeconds != after.actualSeconds || before.effectiveSeconds != after.effectiveSeconds;
}

void BufferModeController::ShowLoadingSceneForRefill()
{
	// No loading scene: Program stays on the wrapper scene, blank until the
	// ring refills -- never the undelayed live scene.
	if (status_.loadingSceneName.empty())
		return;
	// Keep the live scene rendering once Program leaves the wrapper, or the
	// new ring never fills (see Enable()).
	if (!liveSceneRenderingHeld_)
		liveSceneRenderingHeld_ = bridge_.AcquireLiveSceneRendering(status_.liveSceneName);
	bridge_.SetCurrentSceneByName(status_.loadingSceneName);
}

void BufferModeController::EnterRefill(const char *reason)
{
	const bool wasFilling = status_.state == BufferModeState::Filling;
	// The dock (re)arms its fill timer on every Filling status: OnFillTimerElapsed()
	// switches back to the wrapper scene once the new ring is full.
	SetState(BufferModeState::Filling, FormatOne(Str("Status.Filling"), std::to_string(status_.delaySeconds)));
	TRIGGLOW_LOG_WARN(kComponent, "%s: the video buffer restarted empty -- %s for the next %us while it refills",
			  reason,
			  wasFilling                         ? "the fill window starts over"
			  : status_.loadingSceneName.empty() ? "no loading scene set, the delayed view stays blank"
							     : "back to the loading scene",
			  status_.delaySeconds);
}

void BufferModeController::OnBufferRestarted()
{
	if (status_.state != BufferModeState::Active && status_.state != BufferModeState::Filling)
		return;
	// Active: Program is on the wrapper, blank since the ring emptied (a few
	// frames ago). Filling: the loading scene is already up.
	if (status_.state == BufferModeState::Active)
		ShowLoadingSceneForRefill();
	// The restart may come with another effective delay (an OBS video reset
	// can change the RAM fit): audio follows.
	SyncAudioDelayToVideoEffective();
	EnterRefill("video buffer restarted");
	UpdateOverlay();
}

void BufferModeController::Disable()
{
	bool wasActive = status_.state != BufferModeState::Inactive;

	// Covers Disable() interrupting mid-Filling, before OnFillTimerElapsed()
	// had a chance to release it.
	if (liveSceneRenderingHeld_) {
		bridge_.ReleaseLiveSceneRendering(status_.liveSceneName);
		liveSceneRenderingHeld_ = false;
	}

	bridge_.SetDelayOverlay(false, {});
	bridge_.SetBufferFilterEnabled(status_.liveSceneName, false);
	bridge_.SetAudioDelayFiltersEnabled(status_.liveSceneName, false);
	if (!sceneBeforeEnable_.empty()) {
		bridge_.SetCurrentSceneByName(sceneBeforeEnable_);
		sceneBeforeEnable_.clear();
	} else if (!status_.liveSceneName.empty()) {
		bridge_.SetCurrentSceneByName(status_.liveSceneName);
	}

	SetState(BufferModeState::Inactive);
	if (wasActive)
		TRIGGLOW_LOG_INFO(kComponent, "disabled");
}

void BufferModeController::Toggle()
{
	if (status_.state == BufferModeState::Inactive)
		Enable();
	else
		Disable();
}

void BufferModeController::LoadSettings(uint32_t delaySeconds, uint32_t minResolutionHeight, std::string liveSceneName,
					std::string loadingSceneName, bool showOverlay, uint32_t overlayCorner,
					bool bufferOutputCap, bool codecWorkers)
{
	bufferOutputCap_ = bufferOutputCap;
	codecWorkers_ = codecWorkers;
	status_.delaySeconds = delaySeconds;
	status_.minResolutionHeight = minResolutionHeight;
	status_.liveSceneName = std::move(liveSceneName);
	status_.loadingSceneName = std::move(loadingSceneName);
	status_.showOverlay = showOverlay;
	status_.overlayCorner = overlayCorner > 3 ? 0 : overlayCorner;
	// Deliberately NOT calling Enable() here even if the user left it on
	// last session — see header comment. Always starts Inactive.
	SetState(BufferModeState::Inactive);
}

BufferModeController::SettingsSnapshot BufferModeController::SaveSettings() const
{
	return {status_.delaySeconds, status_.minResolutionHeight, status_.liveSceneName, status_.loadingSceneName,
		status_.showOverlay,  status_.overlayCorner,       bufferOutputCap_,      codecWorkers_};
}

PipelineHealth BufferModeController::GetPipelineHealth() const
{
	PipelineHealth health;
	health.codecWorkers = codecWorkers_;
	if (!status_.liveSceneName.empty())
		bridge_.GetBufferPipelineHealth(status_.liveSceneName, health);
	return health;
}

void BufferModeController::SetState(BufferModeState state, std::string message)
{
	status_.state = state;
	status_.message = std::move(message);
	NotifyStatusChanged();
}

void BufferModeController::NotifyStatusChanged()
{
	if (onStatusChanged_)
		onStatusChanged_(status_);
}

void BufferModeController::OnFrontendEvent(FrontendEvent event)
{
	if (event == FrontendEvent::FinishedLoading && status_.state == BufferModeState::Inactive)
		bridge_.DisableAllDelayFilters();
	if (event == FrontendEvent::FinishedLoading && onSceneListRefresh_)
		onSceneListRefresh_();
	// A new output's encoders may encode above the delay buffer. No-op unless
	// the delay is Filling/Active (see WarnAboutEncodersAboveBuffer).
	if (event == FrontendEvent::StreamingStarted || event == FrontendEvent::RecordingStarted ||
	    event == FrontendEvent::ReplayBufferStarted)
		WarnAboutEncodersAboveBuffer();
	if (event == FrontendEvent::BufferRestarted)
		OnBufferRestarted();
}

void BufferModeController::WarnAboutEncodersAboveBuffer()
{
	if (!bufferOutputCap_ || status_.liveSceneName.empty() ||
	    (status_.state != BufferModeState::Filling && status_.state != BufferModeState::Active))
		return;

	// Same bufferfit::Compute() the filter sizes its ring with, so this is the
	// buffer it uses (or will use, before its first Render()), cap on and off.
	const auto capped = bridge_.EstimateBufferFit(status_.liveSceneName, status_.delaySeconds,
						      status_.minResolutionHeight, true);
	const auto uncapped = bridge_.EstimateBufferFit(status_.liveSceneName, status_.delaySeconds,
							status_.minResolutionHeight, false);
	const encodercheck::Size cappedSize{capped.width, capped.height};
	const encodercheck::Size uncappedSize{uncapped.width, uncapped.height};
	// The cap isn't what limits the buffer (no cap applies, or RAM sets the
	// size anyway): lifting it would change nothing, so nothing to say and no
	// need to walk the outputs. ShouldWarn() checks this again per encoder.
	if (!encodercheck::CapLimitsBuffer(cappedSize, uncappedSize))
		return;

	// What lifting the cap would cost in duration at today's RAM budget: the
	// uncapped ring may not fit the whole delay where the capped one does.
	char shorterNote[96] = "";
	if (!uncapped.fitsFullDuration)
		std::snprintf(shorterNote, sizeof(shorterNote), " (here: only %.1fs of the %us requested)",
			      uncapped.actualSeconds, status_.delaySeconds);

	for (const ActiveVideoEncoder &encoder : bridge_.ListActiveVideoEncoders()) {
		if (!encodercheck::ShouldWarn(encoder.scope, {encoder.width, encoder.height}, cappedSize, uncappedSize,
					      bufferOutputCap_))
			continue;
		// Once per encoder AND size per cycle: the same encoder restarted at
		// another resolution (output stopped, settings changed, started
		// again) is warned about again.
		if (!warnedEncoders_.emplace(encoder.encoderName, encoder.width, encoder.height).second)
			continue;
		const char *scopeNote =
			encoder.scope == encodercheck::Scope::MainCanvasByAspect
				? " (GPU-rescaled encoder, taken for the main canvas by its aspect ratio)"
				: "";
		TRIGGLOW_LOG_WARN(kComponent,
				  "video encoder \"%s\" (output \"%s\") is %ux%u, above the delay buffer's %ux%u "
				  "(capped to OBS's output resolution): its delayed video is upscaled%s. Without the "
				  "cap the buffer would be %ux%u -- to lift it, set \"buffer_output_cap\": false in "
				  "the plugin's buffer-mode-settings.json with OBS closed, then restart OBS. Lifting "
				  "it costs more CPU and RAM, and if RAM runs short the delay can end up shorter than "
				  "requested%s",
				  encoder.encoderName.c_str(), encoder.outputName.c_str(), encoder.width,
				  encoder.height, capped.width, capped.height, scopeNote, uncapped.width,
				  uncapped.height, shorterNote);
	}
}

void BufferModeController::SyncAudioDelayToVideoEffective()
{
	uint32_t effectiveSeconds = bridge_.GetVideoEffectiveDelaySeconds(status_.liveSceneName);
	uint32_t targetSeconds = status_.delaySeconds;
	if (effectiveSeconds > 0 && effectiveSeconds < status_.delaySeconds) {
		targetSeconds = effectiveSeconds;
		// AudioDelayFilter::EnsureRingSized resets that ring's history
		// whenever its configured seconds actually changes. Since the video
		// filter reports its planned delay (not its ring's), the shortened
		// value reaches audio at Enable() and on each change, while Program
		// shows the loading scene -- no longer as a brief silence right when
		// it switches to the delayed wrapper scene.
		TRIGGLOW_LOG_WARN(kComponent,
				  "video could only actually buffer %us of the requested %us (RAM budget) -- "
				  "shortening audio's delay to match so it doesn't lag behind video",
				  effectiveSeconds, status_.delaySeconds);
	}
	effectiveDelaySeconds_ = targetSeconds;
	bridge_.SetAudioDelayFiltersDelaySeconds(status_.liveSceneName, targetSeconds);
}

} // namespace trigglow
