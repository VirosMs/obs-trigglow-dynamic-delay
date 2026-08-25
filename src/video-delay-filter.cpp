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

#include "video-delay-filter.hpp"
#include "hardware-info.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace trigglow {

namespace {
constexpr const char *kComponent = "video-delay-filter";
constexpr const char *kFilterId = "trigglow_video_delay_filter";
constexpr const char *kSettingDelaySeconds = "delay_seconds";
constexpr const char *kSettingMinResolutionHeight = "min_resolution_height";

// RAM budget for the ring buffer. Uncompressed RGBA at 1920x1080@60fps is
// ~475MB PER SECOND (1920*1080*4 bytes * 60) — a real 30-60s buffer at that
// resolution would need ~14-28GB, not realistic on top of whatever else the
// game/OBS/Windows itself is doing. No proper compressed encode/decode
// buffer yet (a much bigger rewrite — libobs has no public decoder API for
// that, only obs-encoder.h for producing an OUTPUT stream tied to the main
// video mix, not something a plugin can feed arbitrary frames to on demand;
// would need vendoring FFmpeg for both encode AND decode — investigated and
// explicitly deferred, 2026-08-25, as multi-session work).
//
// GetBufferBudget() below picks the actual budget from the machine's total
// system RAM (src/hardware-info.hpp) where that's available, falling back
// to kFallbackBufferBytes when it isn't (non-Windows for now, or the query
// failed) -- these bounds keep that recommendation sane on both a modest
// 8GB machine and a 64GB+ workstation. The fraction is deliberately lower
// than VRAM used to get (15%): unlike a dedicated GPU's VRAM, system RAM is
// also what the game itself, Windows, and everything else running is
// fighting over, so this leaves much more headroom.
constexpr uint64_t kFallbackBufferBytes = 2048ULL * 1024 * 1024;
constexpr uint64_t kMinBufferBytes = 1024ULL * 1024 * 1024;            // Floor even on a detected low-RAM machine.
constexpr uint64_t kMaxRecommendedBufferBytes = 8192ULL * 1024 * 1024; // Ceiling even on a very high-RAM machine.
constexpr double kRamFractionForBuffer = 0.10;                         // Don't hog more than ~10% of total system RAM.

// Pure math shared by EnsureRingSized() (actually resizes the ring) and
// VideoDelayFilter::EstimateBufferFit() (predicts the outcome without one --
// see that method's header comment). Quality floor first, duration second
// (flipped 2026-08-25 -- see kDefaultMinResolutionHeight's comment): never
// go narrower than minResolutionHeight pixels tall; shrink the FRAME COUNT
// instead if the full requested duration doesn't fit the budget at that
// floor.
struct BufferFit {
	uint32_t bufferWidth;
	uint32_t bufferHeight;
	uint64_t actualFrames;
	uint64_t desiredFrames;
	double scale;
};

BufferFit ComputeBufferFit(uint32_t origWidth, uint32_t origHeight, uint32_t fps, uint32_t delaySeconds,
			   uint32_t minResolutionHeight, uint64_t budget)
{
	// +1 so a full N-second delay has a valid slot to read from, not just
	// N seconds of frames with none old enough yet.
	uint64_t desiredFrames = static_cast<uint64_t>(delaySeconds) * fps + 1;

	double minScale = std::min(1.0, static_cast<double>(minResolutionHeight) / origHeight);

	uint64_t fullResBytesPerFrame = static_cast<uint64_t>(origWidth) * origHeight * 4;
	uint64_t fullResTotalBytes = desiredFrames * fullResBytesPerFrame;

	double scale = 1.0;
	if (fullResTotalBytes > budget && fullResBytesPerFrame > 0) {
		double idealScale = std::sqrt(static_cast<double>(budget) /
					      static_cast<double>(desiredFrames * fullResBytesPerFrame));
		scale = std::max(minScale, std::min(1.0, idealScale));
	}

	uint32_t bufferWidth = std::max<uint32_t>(2, static_cast<uint32_t>(origWidth * scale));
	uint32_t bufferHeight = std::max<uint32_t>(2, static_cast<uint32_t>(origHeight * scale));

	uint64_t bufferBytesPerFrame = static_cast<uint64_t>(bufferWidth) * bufferHeight * 4;
	uint64_t maxFramesAtBufferRes = std::max<uint64_t>(1, budget / bufferBytesPerFrame);
	uint64_t actualFrames = std::min(desiredFrames, maxFramesAtBufferRes);

	return {bufferWidth, bufferHeight, actualFrames, desiredFrames, scale};
}

uint64_t GetBufferBudget()
{
	// Computed once, not per-frame: GlobalMemoryStatusEx is cheap but
	// there's no reason to repeat it every EnsureRingSized() call, and total
	// system RAM doesn't change mid-session. Thread-safe init (C++11 magic
	// statics); EnsureRingSized only ever runs on the video render thread
	// anyway.
	static const uint64_t budget = [] {
		uint64_t ram = QueryTotalSystemRamBytes();
		if (ram == 0) {
			TRIGGLOW_LOG_INFO(kComponent, "RAM detection unavailable, using the %lluMB fallback budget",
					  static_cast<unsigned long long>(kFallbackBufferBytes / (1024 * 1024)));
			return kFallbackBufferBytes;
		}
		uint64_t recommended = std::clamp<uint64_t>(static_cast<uint64_t>(ram * kRamFractionForBuffer),
							    kMinBufferBytes, kMaxRecommendedBufferBytes);
		TRIGGLOW_LOG_INFO(kComponent, "detected %lluMB system RAM, using a %lluMB buffer budget (~%.0f%%)",
				  static_cast<unsigned long long>(ram / (1024 * 1024)),
				  static_cast<unsigned long long>(recommended / (1024 * 1024)),
				  kRamFractionForBuffer * 100.0);
		return recommended;
	}();
	return budget;
}

// Quality now wins over duration, not the other way around (flipped
// 2026-08-25 after live feedback: a 30s buffer downscaled to 727x409 "looks
// terrible" — correct, that's what honoring the full 30s at this budget
// actually costs). EnsureRingSized() never captures below
// configuredMinResolutionHeight_ pixels tall; if the full requested delay
// doesn't fit the budget at that floor, it shortens the ACTUAL buffered
// duration instead (same "last resort" trimming the old design used, just
// now the normal case for long delays instead of a rare edge case).
constexpr uint32_t kDefaultMinResolutionHeight = 720;

// UI slider cap. The real enforcement is EnsureRingSized()'s budget math —
// this just keeps the properties panel from offering a wildly unrealistic
// number before that math clamps it anyway.
constexpr int kUiMaxDelaySeconds = 60;

// Extra headroom on top of configuredDelaySeconds_ worth of audio frames, so
// a write can never catch up to a read mid-copy. Audio is cheap (raw PCM,
// no VRAM concern like video), so this is generously 1 full second per
// channel rather than trying to shave it down.
constexpr uint32_t kAudioRingMarginSeconds = 1;
} // namespace

const char *VideoDelayFilter::Id()
{
	return kFilterId;
}

uint64_t VideoDelayFilter::GetBufferBudgetBytes()
{
	return GetBufferBudget();
}

VideoDelayFilter::BufferFitEstimate VideoDelayFilter::EstimateBufferFit(uint32_t requestedDelaySeconds,
									uint32_t minResolutionHeight,
									uint32_t sourceWidth, uint32_t sourceHeight,
									uint32_t fps)
{
	if (sourceWidth == 0 || sourceHeight == 0 || fps == 0)
		return {};

	BufferFit fit = ComputeBufferFit(sourceWidth, sourceHeight, fps, requestedDelaySeconds, minResolutionHeight,
					 GetBufferBudget());

	BufferFitEstimate estimate;
	estimate.width = fit.bufferWidth;
	estimate.height = fit.bufferHeight;
	estimate.actualSeconds = static_cast<double>(fit.actualFrames) / fps;
	estimate.fitsFullDuration = fit.actualFrames >= fit.desiredFrames;
	return estimate;
}

void VideoDelayFilter::Register()
{
	obs_source_info info = {};
	info.id = kFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// VIDEO ONLY -- NOT `| OBS_SOURCE_AUDIO`, even though FilterAudio() below
	// is fully implemented. Found live, 2026-08-25, by reading libobs's own
	// obs_source_filter_add() (obs-source.c): it silently REJECTS the whole
	// filter (no error, no log, `da_insert` just never runs) whenever
	// filter_compatible(source, filter) is false, and that check is
	//   (source->info.output_flags & OBS_SOURCE_AV & filter's own flags) == filter's own flags
	// obs-scene.c's own registration proves scenes only ever set
	// OBS_SOURCE_VIDEO in THEIR output_flags -- a scene's audio goes through
	// a completely separate audio_render callback, never advertised via the
	// flag this check reads. So a filter requesting OBS_SOURCE_AUDIO can
	// NEVER attach to a scene (our live scene, wrapped for buffer mode),
	// audio-only or combined -- this is a hard libobs constraint, not
	// something fixable by changing IDs, instance names, or keep-alive
	// primitives (all three of which were tried and ruled out first). This
	// is exactly why the buffer stopped filling the moment OBS_SOURCE_AUDIO
	// was added: obs_source_filter_add() was silently doing nothing at all,
	// for BOTH video and audio, every single Enable() press since.
	//
	// Real fix for audio needs a different mechanism entirely --
	// obs_source_add_audio_capture_callback() (a passive tap, works on any
	// source including scenes, but can't replace/delay what's actually
	// heard) paired with a small custom source that injects the delayed
	// audio into the wrapper scene via obs_source_output_audio() -- not a
	// simple flag flip. Tracked as follow-up work; FilterAudio() stays
	// implemented and unit-shaped below for when that lands, just not
	// wired into `output_flags` until it can actually run.
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = &VideoDelayFilter::GetName;
	info.create = &VideoDelayFilter::Create;
	info.destroy = &VideoDelayFilter::Destroy;
	info.update = &VideoDelayFilter::UpdateCb;
	info.video_tick = &VideoDelayFilter::TickCb;
	info.video_render = &VideoDelayFilter::RenderCb;
	info.get_width = &VideoDelayFilter::GetWidthCb;
	info.get_height = &VideoDelayFilter::GetHeightCb;
	info.get_properties = &VideoDelayFilter::GetProperties;
	info.get_defaults = &VideoDelayFilter::GetDefaults;
	obs_register_source(&info);
}

VideoDelayFilter::VideoDelayFilter(obs_source_t *filterSource) : filterSource_(filterSource) {}

VideoDelayFilter::~VideoDelayFilter()
{
	ReleaseRing();
	ReleaseGpuObjects();
}

void VideoDelayFilter::ReleaseRing()
{
	ring_.clear();
	writeIndex_ = 0;
	bufferedCount_ = 0;
}

void VideoDelayFilter::ReleaseGpuObjects()
{
	if (captureTexrender_) {
		gs_texrender_destroy(captureTexrender_);
		captureTexrender_ = nullptr;
	}
	if (playbackTexture_) {
		gs_texture_destroy(playbackTexture_);
		playbackTexture_ = nullptr;
	}
	for (auto &slot : stagingSlots_) {
		if (slot.surface) {
			gs_stagesurface_destroy(slot.surface);
			slot.surface = nullptr;
		}
		slot.pending = false;
	}
}

void VideoDelayFilter::EnsureRingSized(uint32_t origWidth, uint32_t origHeight)
{
	if (origWidth == 0 || origHeight == 0)
		return;

	uint32_t fps = std::max<uint32_t>(1, currentFps_);
	BufferFit fit = ComputeBufferFit(origWidth, origHeight, fps, configuredDelaySeconds_,
					 configuredMinResolutionHeight_, GetBufferBudget());

	bool resolutionChanged = bufferWidth_ != fit.bufferWidth || bufferHeight_ != fit.bufferHeight;
	if (!resolutionChanged && ring_.size() == fit.actualFrames)
		return; // Already correctly sized.

	if (fit.scale < 1.0) {
		TRIGGLOW_LOG_INFO(kComponent,
				  "buffering at %ux%u (%.0f%% of the source's %ux%u) to fit %us at %ufps within "
				  "the %lluMB budget",
				  fit.bufferWidth, fit.bufferHeight, fit.scale * 100.0, origWidth, origHeight,
				  configuredDelaySeconds_, fps,
				  static_cast<unsigned long long>(GetBufferBudget() / (1024 * 1024)));
	}
	if (fit.actualFrames < fit.desiredFrames) {
		TRIGGLOW_LOG_WARN(kComponent, "even at %ux%u this still doesn't fit the full %us; clamped to ~%.1fs",
				  fit.bufferWidth, fit.bufferHeight, configuredDelaySeconds_,
				  static_cast<double>(fit.actualFrames) / fps);
	}

	ReleaseRing();
	// playbackTexture_/stagingSlots_ are fixed-resolution GPU objects (unlike
	// captureTexrender_, which auto-resizes on gs_texrender_begin) -- they
	// must be torn down and lazily recreated at the new size in Render().
	if (resolutionChanged) {
		ReleaseGpuObjects();
	} else {
		// Resolution is unchanged but the FRAME COUNT just did (ring_ above
		// was just cleared/resized) -- any staging slot still mid-readback
		// holds a targetRingIndex into the OLD ring_, which may now be
		// out-of-bounds for the new (possibly smaller) one. The GPU objects
		// themselves are still valid at this resolution, so just drop the
		// pending readback rather than destroying/recreating them.
		for (auto &slot : stagingSlots_)
			slot.pending = false;
	}

	bufferWidth_ = fit.bufferWidth;
	bufferHeight_ = fit.bufferHeight;
	size_t frameBytes = static_cast<size_t>(bufferWidth_) * bufferHeight_ * 4;
	ring_.resize(static_cast<size_t>(fit.actualFrames));
	for (auto &slot : ring_)
		slot.pixels.assign(frameBytes, 0);
}

void VideoDelayFilter::EnsureAudioRingSized(uint32_t channels, uint32_t samplesPerSec)
{
	if (channels == 0 || samplesPerSec == 0)
		return;

	uint64_t desiredFrames = static_cast<uint64_t>(configuredDelaySeconds_) * samplesPerSec +
				 static_cast<uint64_t>(kAudioRingMarginSeconds) * samplesPerSec;
	desiredFrames = std::max<uint64_t>(1, desiredFrames);

	bool needsResize = channels != audioChannels_ || samplesPerSec != samplesPerSec_ || audioRing_.empty() ||
			   audioRing_[0].size() != desiredFrames;
	if (!needsResize)
		return;

	audioChannels_ = channels;
	samplesPerSec_ = samplesPerSec;
	audioRing_.assign(channels, std::vector<float>(static_cast<size_t>(desiredFrames), 0.0f));
	audioWriteIndex_ = 0;
	audioBufferedFrames_ = 0;
}

obs_audio_data *VideoDelayFilter::FilterAudio(obs_audio_data *audio)
{
	if (!audio || audio->frames == 0)
		return audio;

	audio_t *audioContext = obs_get_audio();
	auto channels = static_cast<uint32_t>(audio_output_get_channels(audioContext));
	uint32_t sampleRate = audio_output_get_sample_rate(audioContext);
	if (channels == 0 || sampleRate == 0)
		return audio;

	EnsureAudioRingSized(channels, sampleRate);
	size_t frames = audio->frames;
	size_t ringLen = audioRing_.empty() ? 0 : audioRing_[0].size();
	// A single call delivering more frames than the whole ring would break
	// the index math below; never happens in practice (chunks are a small
	// fraction of a second), but pass through unmodified rather than risk
	// corrupting the ring if it ever did.
	if (ringLen == 0 || frames >= ringLen)
		return audio;

	auto **inData = reinterpret_cast<float **>(audio->data);

	// --- Write: copy this call's samples into the ring, per channel ---
	for (uint32_t c = 0; c < audioChannels_; ++c) {
		if (!inData[c])
			continue;
		std::vector<float> &channelRing = audioRing_[c];
		size_t firstPart = std::min(frames, ringLen - audioWriteIndex_);
		std::memcpy(channelRing.data() + audioWriteIndex_, inData[c], firstPart * sizeof(float));
		if (firstPart < frames)
			std::memcpy(channelRing.data(), inData[c] + firstPart, (frames - firstPart) * sizeof(float));
	}
	audioWriteIndex_ = (audioWriteIndex_ + frames) % ringLen;
	audioBufferedFrames_ = std::min(audioBufferedFrames_ + frames, ringLen);

	// --- Read: whichever `frames`-sized window is configuredDelaySeconds_ old ---
	uint64_t delayFrames = static_cast<uint64_t>(configuredDelaySeconds_) * sampleRate;
	delayFrames = std::min(delayFrames, static_cast<uint64_t>(ringLen - frames));
	bool haveEnoughHistory = audioBufferedFrames_ >= delayFrames + frames;

	audioOutputChunks_.resize(audioChannels_);
	for (auto &chunk : audioOutputChunks_)
		chunk.resize(frames);

	if (haveEnoughHistory) {
		size_t readIndex =
			(audioWriteIndex_ + ringLen * 2 - frames - static_cast<size_t>(delayFrames)) % ringLen;
		for (uint32_t c = 0; c < audioChannels_; ++c) {
			std::vector<float> &channelRing = audioRing_[c];
			size_t firstPart = std::min(frames, ringLen - readIndex);
			std::memcpy(audioOutputChunks_[c].data(), channelRing.data() + readIndex,
				    firstPart * sizeof(float));
			if (firstPart < frames)
				std::memcpy(audioOutputChunks_[c].data() + firstPart, channelRing.data(),
					    (frames - firstPart) * sizeof(float));
		}
	} else {
		// Still filling — same spirit as video's "draw nothing" while the
		// buffer warms up. Program is on the loading scene during this
		// window anyway (BufferModeController), so silence here is never
		// actually heard.
		for (auto &chunk : audioOutputChunks_)
			std::fill(chunk.begin(), chunk.end(), 0.0f);
	}

	for (uint32_t c = 0; c < audioChannels_; ++c)
		audioOutput_.data[c] = reinterpret_cast<uint8_t *>(audioOutputChunks_[c].data());
	for (uint32_t c = audioChannels_; c < MAX_AV_PLANES; ++c)
		audioOutput_.data[c] = nullptr;
	audioOutput_.frames = static_cast<uint32_t>(frames);
	// Passthrough, not recomputed: mirrors the video side, which draws old
	// pixels at the CURRENT instant rather than rewinding a timestamp — see
	// this file's header comment for why.
	audioOutput_.timestamp = audio->timestamp;

	return &audioOutput_;
}

void VideoDelayFilter::Update(obs_data_t *settings)
{
	auto seconds = static_cast<uint32_t>(obs_data_get_int(settings, kSettingDelaySeconds));
	if (seconds != configuredDelaySeconds_) {
		configuredDelaySeconds_ = seconds;
		TRIGGLOW_LOG_INFO(kComponent, "delay set to %us", configuredDelaySeconds_);
	}

	auto minHeight = static_cast<uint32_t>(obs_data_get_int(settings, kSettingMinResolutionHeight));
	if (minHeight == 0)
		minHeight = kDefaultMinResolutionHeight;
	if (minHeight != configuredMinResolutionHeight_) {
		configuredMinResolutionHeight_ = minHeight;
		TRIGGLOW_LOG_INFO(kComponent, "minimum resolution height set to %up", configuredMinResolutionHeight_);
	}

	// Always reset, even if seconds didn't change: Enable() calls this via
	// SetBufferFilterDelaySeconds at the start of every cycle, so this gives
	// fresh Render()/FilterAudio() diagnostics each time instead of only
	// the very first time this filter instance was ever created.
	loggedNoTarget_ = false;
	loggedZeroSize_ = false;
	loggedFirstRender_ = false;
}

void VideoDelayFilter::Tick(float /*secondsSinceLastTick*/)
{
	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi) && ovi.fps_den > 0)
		currentFps_ = ovi.fps_num / ovi.fps_den;
}

void VideoDelayFilter::Render()
{
	obs_source_t *target = obs_filter_get_target(filterSource_);
	if (!target) {
		if (!loggedNoTarget_) {
			TRIGGLOW_LOG_WARN(kComponent, "Render(): obs_filter_get_target() returned null -- "
						      "filter not properly attached to anything right now");
			loggedNoTarget_ = true;
		}
		obs_source_skip_video_filter(filterSource_);
		return;
	}

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);
	if (width == 0 || height == 0) {
		if (!loggedZeroSize_) {
			TRIGGLOW_LOG_WARN(kComponent, "Render(): target's base size is %ux%u, skipping until it's real",
					  width, height);
			loggedZeroSize_ = true;
		}
		obs_source_skip_video_filter(filterSource_);
		return;
	}

	if (!loggedFirstRender_) {
		TRIGGLOW_LOG_INFO(kComponent, "Render(): first successful render this cycle, target %ux%u", width,
				  height);
		loggedFirstRender_ = true;
	}

	EnsureRingSized(width, height);
	if (ring_.empty()) {
		obs_source_skip_video_filter(filterSource_);
		return;
	}

	// --- Capture step 1: render the target into the ONE reusable capture
	// texrender (not one per slot -- see this file's header comment). Its
	// size is the ring's (possibly downscaled) buffer resolution, but the
	// ortho projection below still spans the source's real width/height —
	// that mismatch is exactly what makes the GPU rasterize the capture
	// down to fit the smaller viewport. ---
	if (!captureTexrender_)
		captureTexrender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	gs_texrender_reset(captureTexrender_);
	if (gs_texrender_begin(captureTexrender_, bufferWidth_, bufferHeight_)) {
		struct vec4 clearColor = {};
		gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
		gs_matrix_push();
		gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
		obs_source_video_render(target);
		gs_matrix_pop();
		gs_texrender_end(captureTexrender_);

		// --- Capture step 2: harvest whichever staging slot was kicked off
		// LAST frame (its GPU->CPU copy has now had a full frame to finish,
		// so mapping it here never stalls the CPU the way mapping the SAME
		// frame's staging surface immediately would), then kick off a fresh
		// async readback of what was just rendered above into the OTHER
		// staging slot. kStagingSlotCount==2 alternates cleanly via parity. ---
		size_t kickSlotIdx = captureFrameCounter_ % kStagingSlotCount;
		size_t harvestSlotIdx = (captureFrameCounter_ + 1) % kStagingSlotCount;
		++captureFrameCounter_;

		StagingSlot &harvest = stagingSlots_[harvestSlotIdx];
		if (harvest.pending && harvest.surface) {
			uint8_t *mapped = nullptr;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(harvest.surface, &mapped, &linesize)) {
				Slot &dst = ring_[harvest.targetRingIndex];
				uint32_t rowBytes = bufferWidth_ * 4;
				for (uint32_t row = 0; row < bufferHeight_; ++row) {
					std::memcpy(dst.pixels.data() + static_cast<size_t>(row) * rowBytes,
						    mapped + static_cast<size_t>(row) * linesize, rowBytes);
				}
				dst.valid = true;
				gs_stagesurface_unmap(harvest.surface);
			}
			harvest.pending = false;
		}

		StagingSlot &kick = stagingSlots_[kickSlotIdx];
		if (!kick.surface)
			kick.surface = gs_stagesurface_create(bufferWidth_, bufferHeight_, GS_RGBA);
		gs_texture_t *captured = gs_texrender_get_texture(captureTexrender_);
		if (captured && kick.surface) {
			gs_stage_texture(kick.surface, captured);
			kick.pending = true;
			kick.targetRingIndex = writeIndex_;
		}
	}

	// --- Playback: draw whichever slot is configuredDelaySeconds_ old, via
	// the ONE reusable playback texture (uploaded fresh from RAM each frame,
	// not one persistent GPU texture per slot) ---
	uint64_t delayFrames = static_cast<uint64_t>(configuredDelaySeconds_) * std::max<uint32_t>(1, currentFps_);
	delayFrames = std::min(delayFrames, static_cast<uint64_t>(ring_.size() - 1));
	size_t readIndex = (writeIndex_ + ring_.size() - static_cast<size_t>(delayFrames)) % ring_.size();
	bool haveEnoughHistory = bufferedCount_ > delayFrames;

	if (haveEnoughHistory && ring_[readIndex].valid) {
		if (!playbackTexture_)
			playbackTexture_ =
				gs_texture_create(bufferWidth_, bufferHeight_, GS_RGBA, 1, nullptr, GS_DYNAMIC);
		if (playbackTexture_) {
			gs_texture_set_image(playbackTexture_, ring_[readIndex].pixels.data(), bufferWidth_ * 4, false);

			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
			gs_effect_set_texture(image, playbackTexture_);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(playbackTexture_, 0, width, height);
		}
	}
	// Else: buffer still filling (first configuredDelaySeconds_ after
	// enabling/raising the delay) — draw nothing, same as
	// obs_source_skip_video_filter would leave. BufferModeController pairs
	// this with a loading scene on Program for exactly this window, and
	// forces this filter to keep receiving frames during it via
	// ObsFrontendBridge::AcquireLiveSceneRendering (otherwise bufferedCount_
	// would never advance while something else is on Program).

	writeIndex_ = (writeIndex_ + 1) % ring_.size();
	bufferedCount_ = std::min(bufferedCount_ + 1, ring_.size());
}

uint32_t VideoDelayFilter::GetWidth() const
{
	obs_source_t *target = obs_filter_get_target(filterSource_);
	return target ? obs_source_get_base_width(target) : 0;
}

uint32_t VideoDelayFilter::GetHeight() const
{
	obs_source_t *target = obs_filter_get_target(filterSource_);
	return target ? obs_source_get_base_height(target) : 0;
}

const char *VideoDelayFilter::GetName(void * /*typeData*/)
{
	return "Trigglow Video Delay Buffer";
}

void *VideoDelayFilter::Create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new VideoDelayFilter(source);
	filter->Update(settings);
	return filter;
}

void VideoDelayFilter::Destroy(void *data)
{
	delete static_cast<VideoDelayFilter *>(data);
}

void VideoDelayFilter::UpdateCb(void *data, obs_data_t *settings)
{
	static_cast<VideoDelayFilter *>(data)->Update(settings);
}

void VideoDelayFilter::TickCb(void *data, float seconds)
{
	static_cast<VideoDelayFilter *>(data)->Tick(seconds);
}

void VideoDelayFilter::RenderCb(void *data, gs_effect_t * /*effect*/)
{
	static_cast<VideoDelayFilter *>(data)->Render();
}

obs_audio_data *VideoDelayFilter::FilterAudioCb(void *data, obs_audio_data *audio)
{
	return static_cast<VideoDelayFilter *>(data)->FilterAudio(audio);
}

uint32_t VideoDelayFilter::GetWidthCb(void *data)
{
	return static_cast<VideoDelayFilter *>(data)->GetWidth();
}

uint32_t VideoDelayFilter::GetHeightCb(void *data)
{
	return static_cast<VideoDelayFilter *>(data)->GetHeight();
}

obs_properties_t *VideoDelayFilter::GetProperties(void * /*data*/)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop =
		obs_properties_add_int(props, kSettingDelaySeconds, "Delay (segundos)", 0, kUiMaxDelaySeconds, 1);
	obs_property_set_long_description(
		prop, "Segundos de retraso pedidos. Se respeta la calidad minima (ver mas abajo) por encima del "
		      "tiempo -- si no caben enteros en el presupuesto de RAM (calculado segun la memoria de tu "
		      "PC) a esa calidad, el tiempo real de buffer se acorta en su lugar (ver el log de OBS).");

	obs_property_t *qualityProp = obs_properties_add_list(props, kSettingMinResolutionHeight, "Calidad minima",
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(qualityProp, "480p", 480);
	obs_property_list_add_int(qualityProp, "720p", 720);
	obs_property_list_add_int(qualityProp, "1080p", 1080);
	obs_property_set_long_description(
		qualityProp, "Nunca se baja de esta resolucion para el tramo delayed, aunque eso signifique "
			     "guardar menos segundos de los pedidos. Un valor mas alto guarda menos tiempo real "
			     "con el mismo presupuesto de RAM.");

	return props;
}

void VideoDelayFilter::GetDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kSettingDelaySeconds, 0);
	obs_data_set_default_int(settings, kSettingMinResolutionHeight, kDefaultMinResolutionHeight);
}

} // namespace trigglow
