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
#include "auto-names.hpp"
#include "i18n.hpp"
#include "logging.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <initializer_list>

namespace trigglow {

namespace {
constexpr const char *kComponent = "buffer-mode-controller";

// Replaces every "%1".."%9" in a Str() template with the matching value --
// this file has no Qt (no QString::arg) and isn't worth pulling in a format
// library for the few status messages that need numbers (Filling's seconds,
// and the RAM-trimmed variant's two). A "%N" without a value stays as is.
uint64_t SteadyNowNs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					     std::chrono::steady_clock::now().time_since_epoch())
					     .count());
}

// The buffer's hardware encoder gets its engine back this long after an
// output that made it yield started (design 4.5), or this long after the
// yield whatever happened.
constexpr uint64_t kResumeAfterStartNs = 3000000000ULL;
constexpr uint64_t kResumeAtMostNs = 10000000000ULL;

std::string FormatArgs(const char *tmpl, std::initializer_list<std::string> values)
{
	std::string result;
	for (const char *p = tmpl; *p; ++p) {
		const size_t index = p[0] == '%' && p[1] >= '1' && p[1] <= '9' ? static_cast<size_t>(p[1] - '1')
									       : values.size();
		if (index < values.size()) {
			result += *(values.begin() + index);
			++p;
		} else {
			result += *p;
		}
	}
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

std::vector<std::string> BufferModeController::ListAvailableScenes() const
{
	std::vector<std::string> names = bridge_.ListSceneNames();
	names.erase(std::remove_if(names.begin(), names.end(),
				   [](const std::string &name) { return autonames::IsWrapperSceneName(name); }),
		    names.end());
	return names;
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
	// After the sync: the dock shows the effective seconds it computed.
	NotifyStatusChanged();
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
	bridge_.SetDelayOverlay(visible, AppliedDelaySeconds(), status_.overlayCorner);
}

void BufferModeController::SetMinResolutionHeight(uint32_t heightPixels)
{
	const uint32_t previousHeight = status_.minResolutionHeight;
	status_.minResolutionHeight = heightPixels;
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
	NotifyStatusChanged();
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
	bridge_.SetBufferFilterCodec(status_.liveSceneName, bufferCodecAuto_);
	// v0.7.0: the storage class is fixed here for the whole cycle (I9).
	cycleStorageInter_ = NextCycleStorageInter();
	bridge_.SetBufferFilterStorageClass(status_.liveSceneName, cycleStorageInter_);

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

	// v0.7.0 storage codec: OBS's encoders as they are now and the ceiling
	// they put on the buffer, BEFORE the filter is switched on -- its first
	// ring and its first codec setup (ranking, NVENC sessions, engine
	// sharing) then work with them, not with the previous cycle's -- and a
	// fresh baseline for their mixes' skipped frames.
	yieldPending_ = false;
	lastCeilingWidth_ = UINT32_MAX;
	lastCeilingHeight_ = UINT32_MAX;
	RefreshObsEncoders(true);
	bridge_.TakeMixSkippedFrames();
	bridge_.WatchOutputStops();

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

	++status_.fillGeneration; // A new fill window: the dock starts its countdown.
	SetState(BufferModeState::Filling, FillingMessage());
	TRIGGLOW_LOG_INFO(
		kComponent,
		"enabled: live=\"%s\" loading=\"%s\" delay=%us output_cap=%s codec_workers=%s buffer_codec=%s%s "
		"storage_class=%s",
		status_.liveSceneName.c_str(), status_.loadingSceneName.c_str(), status_.delaySeconds,
		bufferOutputCap_ ? "on" : "off", codecWorkers_ ? "on" : "off", bufferCodecAuto_ ? "auto" : "mjpeg",
		bufferCodecAuto_ && !codecWorkers_ ? " (ignored: codec_workers=false keeps MJPEG)" : "",
		cycleStorageInter_ ? "inter" : "intra");

	// New cycle: warn again about any encoder above the buffer, starting with
	// the outputs already running right now.
	warnedEncoders_.clear();
	WarnAboutEncodersAboveBuffer();
}

void BufferModeController::RefreshObsEncoders(bool force)
{
	const std::vector<ActiveVideoEncoder> active = bridge_.ListActiveVideoEncoders();
	// What tells one set of encoders from another: every output's encoder,
	// its id and its size.
	std::vector<std::string> signature;
	signature.reserve(active.size());
	for (const ActiveVideoEncoder &encoder : active)
		signature.push_back(encoder.outputName + '\x1f' + encoder.encoderName + '\x1f' + encoder.encoderId +
				    '\x1f' + std::to_string(encoder.width) + 'x' + std::to_string(encoder.height));
	std::sort(signature.begin(), signature.end());
	const bool changed = signature != encoderSignature_;
	if (!force && !changed)
		return;
	encoderSignature_ = std::move(signature);
	PushObsEncoders(active);
	PushEncoderCeiling(active);
	if (!force) {
		// Found by the once-a-second check, not by a frontend event: an
		// output another plugin started or stopped.
		TRIGGLOW_LOG_INFO(kComponent,
				  "OBS's active video encoders changed without a frontend event (an output of "
				  "another plugin?): %zu now, pushed to the delay buffer",
				  active.size());
		WarnAboutEncodersAboveBuffer();
	}
}

void BufferModeController::PushEncoderCeiling(const std::vector<ActiveVideoEncoder> &active)
{
	encodercheck::Size base, output;
	if (!bridge_.MainCanvasGeometry(base, output))
		return;
	std::vector<encodercheck::CeilingEncoder> encoders;
	for (const ActiveVideoEncoder &encoder : active)
		encoders.push_back({encoder.source, encoder.scope, {encoder.width, encoder.height}});
	const encodercheck::Ceiling ceiling = encodercheck::EncoderCeiling(encoders, base, output, bufferOutputCap_,
									   bridge_.RawVideoOutputOnMainMix());
	const uint32_t width = ceiling.applies ? ceiling.size.width : 0;
	const uint32_t height = ceiling.applies ? ceiling.size.height : 0;
	if (width == lastCeilingWidth_ && height == lastCeilingHeight_)
		return;
	lastCeilingWidth_ = width;
	lastCeilingHeight_ = height;
	if (ceiling.applies)
		TRIGGLOW_LOG_INFO(kComponent, "encoder ceiling %ux%u: %s (output %ux%u, canvas %ux%u)", width, height,
				  encodercheck::CeilingWhyText(ceiling.why), output.width, output.height, base.width,
				  base.height);
	else
		TRIGGLOW_LOG_INFO(kComponent, "no encoder ceiling: %s", encodercheck::CeilingWhyText(ceiling.why));
	bridge_.SetBufferFilterEncoderCeiling(status_.liveSceneName, width, height);
}

bool BufferModeController::NextCycleStorageInter() const
{
	return bufferCodecAuto_ && codecWorkers_ && bridge_.HardwareStorageAvailable();
}

bool BufferModeController::PlannedStorageInter() const
{
	const bool busy = status_.state == BufferModeState::Filling || status_.state == BufferModeState::Active;
	return busy ? cycleStorageInter_ : NextCycleStorageInter();
}

void BufferModeController::PushObsEncoders(const std::vector<ActiveVideoEncoder> &active)
{
	VideoDelayFilter::ObsEncoderSet set;
	set.known = true;
	std::vector<std::string> seen;
	for (const ActiveVideoEncoder &encoder : active) {
		// One encoder can feed several outputs (stream and replay buffer): one session.
		if (std::find(seen.begin(), seen.end(), encoder.encoderName) != seen.end())
			continue;
		seen.push_back(encoder.encoderName);
		if (set.count < VideoDelayFilter::ObsEncoderSet::kMax)
			set.families[set.count++] = storage::FamilyFromObsEncoderId(encoder.encoderId);
	}
	bridge_.SetBufferFilterObsEncoders(status_.liveSceneName, set);
}

void BufferModeController::YieldForOutputStart()
{
	if (status_.state != BufferModeState::Filling && status_.state != BufferModeState::Active)
		return;
	uint32_t obsNvenc = 0;
	std::vector<std::string> seen;
	for (const ActiveVideoEncoder &encoder : bridge_.ListActiveVideoEncoders()) {
		if (std::find(seen.begin(), seen.end(), encoder.encoderName) != seen.end())
			continue;
		seen.push_back(encoder.encoderName);
		if (storage::FamilyFromObsEncoderId(encoder.encoderId) == storage::EncoderFamily::Nvenc)
			++obsNvenc;
	}
	// OBS's NVENC encoders running, the one starting and the buffer's.
	const bool wait = obsNvenc + 2 >= 4;
	bridge_.YieldBufferHwEncoder(status_.liveSceneName, wait);
	yieldPending_ = true;
	yieldSinceNs_ = SteadyNowNs();
	outputStartedNs_ = 0;
}

void BufferModeController::Tick()
{
	if (status_.state != BufferModeState::Filling && status_.state != BufferModeState::Active)
		return;
	// Outputs other plugins start or stop (Aitum Vertical, multi-rtmp, Source
	// Record...) raise no frontend event: without this their encoders were
	// never seen -- the engine counted as not shared with OBS, so nothing
	// evacuated, and the NVENC sessions were counted short. One listing of
	// the active outputs (lock-free reads under libobs' outputs mutex, never
	// the graphics thread), pushed only when it changed; their stop signals
	// are watched for encoder errors too.
	RefreshObsEncoders(false);
	bridge_.WatchOutputStops();
	const uint64_t skipped = bridge_.TakeMixSkippedFrames();
	if (skipped > 0)
		bridge_.ReportBufferEncoderLag(status_.liveSceneName, skipped);
	const uint64_t nowNs = SteadyNowNs();
	if (yieldPending_ && ((outputStartedNs_ != 0 && nowNs >= outputStartedNs_ + kResumeAfterStartNs) ||
			      nowNs >= yieldSinceNs_ + kResumeAtMostNs)) {
		yieldPending_ = false;
		RefreshObsEncoders(true); // The sessions are recounted before the next setup.
		bridge_.ResumeBufferHwEncoder(status_.liveSceneName);
	}
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
						      previousMinResolutionHeight, bufferOutputCap_,
						      cycleStorageInter_);
	const auto after = bridge_.EstimateBufferFit(status_.liveSceneName, status_.delaySeconds,
						     status_.minResolutionHeight, bufferOutputCap_, cycleStorageInter_);
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
	// A new fill window, so the dock re-arms its fill timer (it only does when
	// fillGeneration changes): OnFillTimerElapsed() switches back to the
	// wrapper scene once the new ring is full.
	++status_.fillGeneration;
	SetState(BufferModeState::Filling, FillingMessage());
	TRIGGLOW_LOG_WARN(kComponent, "%s: the video buffer restarted empty -- %s for the next %us while it refills",
			  reason,
			  wasFilling                         ? "the fill window starts over"
			  : status_.loadingSceneName.empty() ? "no loading scene set, the delayed view stays blank"
							     : "back to the loading scene",
			  AppliedDelaySeconds());
}

uint32_t BufferModeController::AppliedDelaySeconds() const
{
	return status_.effectiveDelaySeconds > 0 ? std::min(status_.effectiveDelaySeconds, status_.delaySeconds)
						 : status_.delaySeconds;
}

std::string BufferModeController::FillingMessage() const
{
	const uint32_t applied = AppliedDelaySeconds();
	if (applied < status_.delaySeconds)
		return FormatArgs(Str("Status.FillingTrimmed"),
				  {std::to_string(applied), std::to_string(status_.delaySeconds)});
	return FormatArgs(Str("Status.Filling"), {std::to_string(applied)});
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
	StopDelay({}, /*restoreProgram=*/true);
}

void BufferModeController::StopDelay(std::string message, bool restoreProgram)
{
	bool wasActive = status_.state != BufferModeState::Inactive;

	// Covers Disable() interrupting mid-Filling, before OnFillTimerElapsed()
	// had a chance to release it.
	if (liveSceneRenderingHeld_) {
		bridge_.ReleaseLiveSceneRendering(status_.liveSceneName);
		liveSceneRenderingHeld_ = false;
	}

	bridge_.SetDelayOverlay(false, 0);
	status_.effectiveDelaySeconds = 0;
	yieldPending_ = false;
	bridge_.SetBufferFilterEnabled(status_.liveSceneName, false);
	bridge_.SetAudioDelayFiltersEnabled(status_.liveSceneName, false);
	if (restoreProgram) {
		if (!sceneBeforeEnable_.empty())
			bridge_.SetCurrentSceneByName(sceneBeforeEnable_);
		else if (!status_.liveSceneName.empty())
			bridge_.SetCurrentSceneByName(status_.liveSceneName);
	}
	sceneBeforeEnable_.clear();

	SetState(BufferModeState::Inactive, std::move(message));
	if (wasActive)
		TRIGGLOW_LOG_INFO(kComponent, "disabled");
}

void BufferModeController::StopForSceneCollectionChange(bool oldCollectionLoaded)
{
	if (status_.state != BufferModeState::Filling && status_.state != BufferModeState::Active)
		return;
	TRIGGLOW_LOG_WARN(kComponent,
			  "scene collection %s while the delay was %s: turning the delay off (it belongs to the "
			  "previous collection)",
			  oldCollectionLoaded ? "changing" : "changed (duplicate or rename)",
			  status_.state == BufferModeState::Active ? "on" : "filling");
	// See the header: after a duplicate or rename the copy may have opened on
	// the helper scene it was saved with, which would show the live scene
	// undelayed now that its filter is off.
	const bool restoreProgram = oldCollectionLoaded || autonames::IsWrapperSceneName(bridge_.GetCurrentSceneName());
	StopDelay(Str("Status.CollectionChanged"), restoreProgram);
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
					bool bufferOutputCap, bool codecWorkers, bool bufferCodecAuto)
{
	bufferOutputCap_ = bufferOutputCap;
	codecWorkers_ = codecWorkers;
	bufferCodecAuto_ = bufferCodecAuto;
	status_.delaySeconds = delaySeconds;
	status_.minResolutionHeight = minResolutionHeight;
	status_.liveSceneName = std::move(liveSceneName);
	status_.loadingSceneName = std::move(loadingSceneName);
	status_.showOverlay = showOverlay;
	status_.overlayCorner = overlayCorner > 3 ? 0 : overlayCorner;
	status_.effectiveDelaySeconds = 0;
	// Deliberately NOT calling Enable() here even if the user left it on
	// last session — see header comment. Always starts Inactive.
	SetState(BufferModeState::Inactive);
}

BufferModeController::SettingsSnapshot BufferModeController::SaveSettings() const
{
	return {status_.delaySeconds, status_.minResolutionHeight, status_.liveSceneName, status_.loadingSceneName,
		status_.showOverlay,  status_.overlayCorner,       bufferOutputCap_,      codecWorkers_,
		bufferCodecAuto_};
}

PipelineHealth BufferModeController::GetPipelineHealth() const
{
	PipelineHealth health;
	health.codecWorkers = codecWorkers_;
	bridge_.GetBufferPipelineHealth(health);
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
	// First, so the collection that just loaded finds the delay Inactive below.
	if (event == FrontendEvent::SceneCollectionChanging)
		StopForSceneCollectionChange(/*oldCollectionLoaded=*/true);
	else if (event == FrontendEvent::SceneCollectionChanged)
		StopForSceneCollectionChange(/*oldCollectionLoaded=*/false);

	const bool collectionLoaded = event == FrontendEvent::FinishedLoading ||
				      event == FrontendEvent::SceneCollectionChanged;
	// On startup AND on every switch to another collection, while Inactive (so
	// nothing changes under a running delay): rename collections saved by
	// v0.5.1 and earlier ("(no tocar)" -> "(auto)"), and switch off delay
	// filters a session closed while Active saved enabled -- the collection
	// switched to can carry them as much as the one loaded at startup.
	if (collectionLoaded && status_.state == BufferModeState::Inactive) {
		bridge_.MigrateLegacyAutoNames();
		bridge_.DisableAllDelayFilters();
	}
	if (collectionLoaded && onSceneListRefresh_)
		onSceneListRefresh_();
	if (event == FrontendEvent::ThemeChanged && onThemeChanged_)
		onThemeChanged_();
	// v0.7.0: OBS's own encoder failed: the buffer leaves a shared engine.
	if (event == FrontendEvent::OutputEncodeError &&
	    (status_.state == BufferModeState::Filling || status_.state == BufferModeState::Active)) {
		TRIGGLOW_LOG_WARN(kComponent, "an OBS output stopped on an encoder error: telling the delay buffer");
		bridge_.ReportBufferObsEncodeError(status_.liveSceneName);
	}
	// v0.7.0: an output about to open its encoder gets the NVENC engine first.
	if (event == FrontendEvent::StreamingStarting || event == FrontendEvent::RecordingStarting ||
	    event == FrontendEvent::ReplayBufferStarting)
		YieldForOutputStart();
	const bool started = event == FrontendEvent::StreamingStarted || event == FrontendEvent::RecordingStarted ||
			     event == FrontendEvent::ReplayBufferStarted;
	const bool stopped = event == FrontendEvent::StreamingStopped || event == FrontendEvent::RecordingStopped ||
			     event == FrontendEvent::ReplayBufferStopped;
	const bool busy = status_.state == BufferModeState::Filling || status_.state == BufferModeState::Active;
	if (started && yieldPending_ && outputStartedNs_ == 0)
		outputStartedNs_ = SteadyNowNs();
	if (busy && (started || stopped || event == FrontendEvent::VirtualcamStarted ||
		     event == FrontendEvent::VirtualcamStopped))
		RefreshObsEncoders(true);
	// A new output's encoders may encode above the delay buffer. No-op unless
	// the delay is Filling/Active (see WarnAboutEncodersAboveBuffer).
	if (started)
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
						      status_.minResolutionHeight, true, cycleStorageInter_);
	const auto uncapped = bridge_.EstimateBufferFit(status_.liveSceneName, status_.delaySeconds,
							status_.minResolutionHeight, false, cycleStorageInter_);
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
	status_.effectiveDelaySeconds = targetSeconds;
	bridge_.SetAudioDelayFiltersDelaySeconds(status_.liveSceneName, targetSeconds);
}

} // namespace trigglow
