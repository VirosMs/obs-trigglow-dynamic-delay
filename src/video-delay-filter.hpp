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
#include <vector>

extern "C" {
#include <obs.h>
#include <graphics/graphics.h>
#include <media-io/audio-io.h> // audio_output_get_channels/sample_rate() — used to size the audio ring
}

// Delays video by a configured number of seconds, with the streaming OUTPUT
// never touched — no reconnection, ever, at any point. A standard OBS
// VIDEO-ONLY filter, attached automatically by BufferModeController to the
// live scene via ObsFrontendBridge — the user never opens OBS's own Filters
// dialog for it.
//
// Video mechanism: every video_render call renders the filter's target
// source into the NEXT slot of a ring buffer of GPU textures
// (gs_texrender_t), then draws whichever slot is `delaySeconds` old. Memory
// is the hard constraint, not CPU: uncompressed RGBA at 1920x1080@60fps is
// ~475MB PER SECOND. EnsureRingSized() picks a buffer budget from the real
// GPU's VRAM (src/gpu-info.hpp) and never captures below a configurable
// minimum resolution (quality floor, user-chosen) -- if the full requested
// delay doesn't fit the budget at that floor, the ACTUAL buffered duration
// shortens instead (flipped 2026-08-25 after live feedback that always
// honoring the full duration made long delays look terrible).
//
// FilterAudio()/EnsureAudioRingSized() below are a COMPLETE, implemented
// audio ring buffer (same delay-window logic as video, in sample-space) --
// but NOT currently wired into Register()'s output_flags. Found live,
// 2026-08-25: libobs's obs_source_filter_add() silently refuses to attach
// ANY filter requesting OBS_SOURCE_AUDIO to a SCENE (obs-scene.c's own
// registration proves scenes never set that flag on themselves -- their
// audio goes through a separate audio_render callback), and it does so by
// just not inserting the filter at all, no error anywhere -- which is
// exactly what broke VIDEO too the moment this filter started requesting
// audio, since obs_source_filter_add() rejects the filter as a whole, not
// per-capability. See Register()'s comment for the real fix audio will
// need (an audio capture callback + a small source that injects delayed
// audio into the wrapper scene) once that's built as its own follow-up.
//
// Both rings only fill while ObsFrontendBridge::AcquireLiveSceneRendering
// forces the live scene to stay active in the background (Filling window)
// or while it's naturally showing via the wrapper scene (Active) — see that
// function's comment for why a source that isn't on Program/Preview
// normally renders/mixes nothing at all.
namespace trigglow {

class VideoDelayFilter {
public:
	static const char *Id();

	// Registers this filter type with OBS. Call once from obs_module_load().
	static void Register();

	// The VRAM budget EnsureRingSized() is actually using right now (bytes)
	// -- computed once from the real GPU's detected VRAM (src/gpu-info.hpp)
	// or a safe fallback if that's unavailable. Exposed so the dock can show
	// the user what their hardware allows, per BufferModeController's
	// "aconsejar segun el hardware, pero a su eleccion" requirement -- this
	// never restricts delaySeconds/minResolutionHeight, it's informational.
	static uint64_t GetBufferBudgetBytes();

	// What EnsureRingSized() would actually do for a given
	// (requestedDelaySeconds, minResolutionHeight) at the live scene's real
	// (sourceWidth, sourceHeight, fps) -- same math, exposed as a pure
	// query so the dock can warn the user BEFORE they press Enable, not
	// just after. Never restricts their choice: this is purely informational,
	// "aconsejar segun el hardware, pero a su eleccion" per the 2026-08-25
	// live feedback that asked for exactly this.
	struct BufferFitEstimate {
		uint32_t width = 0;
		uint32_t height = 0;
		double actualSeconds = 0.0;
		// False if actualSeconds ends up shorter than requestedDelaySeconds
		// -- the quality floor didn't leave room for the full duration at
		// the current VRAM budget.
		bool fitsFullDuration = true;
	};
	static BufferFitEstimate EstimateBufferFit(uint32_t requestedDelaySeconds, uint32_t minResolutionHeight,
						   uint32_t sourceWidth, uint32_t sourceHeight, uint32_t fps);

private:
	// One buffered historical frame.
	struct Slot {
		gs_texrender_t *texrender = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		bool valid = false; // false until first successfully rendered.
	};

	explicit VideoDelayFilter(obs_source_t *filterSource);
	~VideoDelayFilter();

	void Update(obs_data_t *settings);
	void Tick(float secondsSinceLastTick);
	void Render();
	obs_audio_data *FilterAudio(obs_audio_data *audio);
	uint32_t GetWidth() const;
	uint32_t GetHeight() const;

	// Resizes ring_ (VRAM-budget-capped) for the given target resolution and
	// current output frame rate. No-op if slotCapacity_ is already correct
	// for this width/height. Must be called from within a video_render (or
	// otherwise graphics-context-active) call, since it creates/destroys
	// gs_texrender_t objects.
	void EnsureRingSized(uint32_t width, uint32_t height);
	void ReleaseRing();

	// Resizes audioRing_ for the given channel count/sample rate and the
	// current configuredDelaySeconds_. Plain CPU memory (no graphics
	// context needed, unlike EnsureRingSized). No-op if already correctly
	// sized.
	void EnsureAudioRingSized(uint32_t channels, uint32_t samplesPerSec);

	// --- obs_source_info callback trampolines (static: OBS calls C
	// function pointers, not member functions) ---
	static const char *GetName(void *typeData);
	static void *Create(obs_data_t *settings, obs_source_t *source);
	static void Destroy(void *data);
	static void UpdateCb(void *data, obs_data_t *settings);
	static void TickCb(void *data, float seconds);
	static void RenderCb(void *data, gs_effect_t *effect);
	static obs_audio_data *FilterAudioCb(void *data, obs_audio_data *audio);
	static uint32_t GetWidthCb(void *data);
	static uint32_t GetHeightCb(void *data);
	static obs_properties_t *GetProperties(void *data);
	static void GetDefaults(obs_data_t *settings);

	obs_source_t *filterSource_; // Not owned; valid for this object's lifetime.
	uint32_t configuredDelaySeconds_ = 0;
	// Floor on the ring's capture height, in pixels -- quality wins over
	// duration: EnsureRingSized() never captures shorter than this, and
	// shortens the ACTUAL buffered seconds instead if the full requested
	// delay doesn't fit the VRAM budget at this floor. Defaults to 720
	// (kDefaultMinResolutionHeight); user-configurable via the dock.
	uint32_t configuredMinResolutionHeight_ = 720;

	std::vector<Slot> ring_;
	size_t writeIndex_ = 0;
	size_t bufferedCount_ = 0; // How many ring_ slots hold a real rendered frame so far.
	uint32_t currentFps_ = 30; // Updated from obs_get_video_info() each Tick().

	// audioRing_[channel][frame] — one flat sample buffer per channel, big
	// enough for configuredDelaySeconds_ at samplesPerSec_. Resized by
	// EnsureAudioRingSized whenever the delay, channel count, or sample
	// rate changes.
	std::vector<std::vector<float>> audioRing_;
	size_t audioWriteIndex_ = 0;
	size_t audioBufferedFrames_ = 0; // How many frames of real history audioRing_ holds so far.
	uint32_t audioChannels_ = 0;
	uint32_t samplesPerSec_ = 0;

	// Owned output storage for FilterAudio(): filter_audio's contract
	// requires returned data to stay valid until the next call, so this
	// can't be a stack buffer — resized to match each call's audio->frames.
	std::vector<std::vector<float>> audioOutputChunks_;
	obs_audio_data audioOutput_ = {};

	// One-shot diagnostic flags: Render() used to fail completely silently
	// on its early-return paths (obs_filter_get_target() null, or the
	// target's base size being 0x0), which made a real live bug (2026-08-25:
	// the buffer never filled, no error anywhere) take multiple round-trips
	// to even locate. Logged once per Update() (i.e. once per Enable()
	// cycle, since Enable() always pushes a fresh delay via
	// SetBufferFilterDelaySeconds) rather than every frame.
	bool loggedNoTarget_ = false;
	bool loggedZeroSize_ = false;
	bool loggedFirstRender_ = false;
};

} // namespace trigglow
