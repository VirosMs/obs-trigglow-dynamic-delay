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
#include <media-io/audio-io.h> // audio_output_get_channels/sample_rate()
}

// PHASE 2 (audio sync) -- companion to VideoDelayFilter, but attached
// differently. VideoDelayFilter attaches to the live SCENE and can only
// carry video, because libobs's obs_source_filter_add() silently refuses
// any filter requesting OBS_SOURCE_AUDIO on a source whose own
// output_flags don't include it -- and scenes never advertise audio in
// output_flags (their audio goes through a separate .audio_render
// callback, see obs-scene.c). Found live, 2026-08-25, after several failed
// attempts to make a combined video+audio filter attach to a scene at all.
//
// This filter instead attaches to each individual LEAF audio source INSIDE
// the live scene (Mic, Desktop, etc. -- real wasapi_input_capture /
// wasapi_output_capture sources, which DO advertise OBS_SOURCE_AUDIO on
// themselves, so obs_source_filter_add() accepts it normally). Verified
// live, 2026-08-25, that a leaf source's filter_audio callback keeps firing
// on schedule even while that source isn't part of the current Program
// scene's tree -- unlike the scene-level video/audio mixing tree, a leaf
// source's own obs_source_output_audio() -> filter chain runs continuously
// regardless of visibility, so this filter (unlike VideoDelayFilter) needs
// NO keep-alive trick at all. One instance is attached per audio-capable
// child of the live scene; ObsFrontendBridge manages enabling/disabling/
// configuring all of them together so they always agree with the video
// delay's seconds and Filling/Active state.
//
// Same ring-buffer delay-window logic as VideoDelayFilter::FilterAudio
// (this is where that logic actually gets used -- VideoDelayFilter's own
// copy stays unwired, see its Register() comment). Audio is cheap CPU-side
// PCM (no VRAM budget tradeoff needed), so the full requested delay is
// always buffered exactly, once per attached leaf source.
namespace trigglow {

class AudioDelayFilter {
public:
	static const char *Id();

	// Registers this filter type with OBS. Call once from obs_module_load().
	static void Register();

private:
	explicit AudioDelayFilter(obs_source_t *filterSource);
	~AudioDelayFilter() = default;

	void Update(obs_data_t *settings);
	obs_audio_data *FilterAudio(obs_audio_data *audio);

	// Resizes ring_ for the given channel count/sample rate and the current
	// configuredDelaySeconds_. No-op if already correctly sized.
	void EnsureRingSized(uint32_t channels, uint32_t samplesPerSec);

	// --- obs_source_info callback trampolines ---
	static const char *GetName(void *typeData);
	static void *Create(obs_data_t *settings, obs_source_t *source);
	static void Destroy(void *data);
	static void UpdateCb(void *data, obs_data_t *settings);
	static obs_audio_data *FilterAudioCb(void *data, obs_audio_data *audio);
	static obs_properties_t *GetProperties(void *data);
	static void GetDefaults(obs_data_t *settings);

	obs_source_t *filterSource_; // Not owned; valid for this object's lifetime.
	uint32_t configuredDelaySeconds_ = 0;

	// ring_[channel][frame] -- one flat sample buffer per channel, big
	// enough for configuredDelaySeconds_ at samplesPerSec_. Resized by
	// EnsureRingSized whenever the delay, channel count, or sample rate
	// changes.
	std::vector<std::vector<float>> ring_;
	size_t writeIndex_ = 0;
	size_t bufferedFrames_ = 0; // How many frames of real history ring_ holds so far.
	uint32_t channels_ = 0;
	uint32_t samplesPerSec_ = 0;

	// Owned output storage for FilterAudio(): filter_audio's contract
	// requires returned data to stay valid until the next call, so this
	// can't be a stack buffer -- resized to match each call's audio->frames.
	std::vector<std::vector<float>> outputChunks_;
	obs_audio_data output_ = {};
};

} // namespace trigglow
