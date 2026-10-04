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

#include "audio-delay-filter.hpp"
#include "i18n.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cstring>

namespace trigglow {

namespace {
constexpr const char *kComponent = "audio-delay-filter";
constexpr const char *kFilterId = "trigglow_audio_delay_filter";
constexpr const char *kSettingDelaySeconds = "delay_seconds";
constexpr int kUiMaxDelaySeconds = 60;

// Extra headroom on top of configuredDelaySeconds_ worth of frames, so a
// write can never catch up to a read mid-copy. Cheap (raw PCM, no VRAM
// concern), so this is generously 1 full second per channel.
constexpr uint32_t kRingMarginSeconds = 1;

// OBS hands a filter exactly contiguous timestamps from a source that is
// delivering steadily, so anything past this is a real gap (or rewind), not
// jitter. Small enough that dropouts don't accumulate into audible A/V drift.
constexpr uint64_t kGapToleranceNs = 10ULL * 1000 * 1000;

// Bumped by the bridge on every Enable to flush stale ring contents.
constexpr const char *kSettingResetToken = "reset_token";
} // namespace

const char *AudioDelayFilter::Id()
{
	return kFilterId;
}

void AudioDelayFilter::Register()
{
	obs_source_info info = {};
	info.id = kFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = &AudioDelayFilter::GetName;
	info.create = &AudioDelayFilter::Create;
	info.destroy = &AudioDelayFilter::Destroy;
	info.update = &AudioDelayFilter::UpdateCb;
	info.filter_audio = &AudioDelayFilter::FilterAudioCb;
	info.get_properties = &AudioDelayFilter::GetProperties;
	info.get_defaults = &AudioDelayFilter::GetDefaults;
	obs_register_source(&info);
}

AudioDelayFilter::AudioDelayFilter(obs_source_t *filterSource) : filterSource_(filterSource) {}

void AudioDelayFilter::EnsureRingSized(uint32_t channels, uint32_t samplesPerSec, uint32_t delaySeconds)
{
	if (channels == 0 || samplesPerSec == 0)
		return;

	uint64_t desiredFrames = static_cast<uint64_t>(delaySeconds) * samplesPerSec +
				 static_cast<uint64_t>(kRingMarginSeconds) * samplesPerSec;
	desiredFrames = std::max<uint64_t>(1, desiredFrames);

	bool needsResize = channels != channels_ || samplesPerSec != samplesPerSec_ || ring_.empty() ||
			   ring_[0].size() != desiredFrames;
	if (!needsResize)
		return;

	channels_ = channels;
	samplesPerSec_ = samplesPerSec;
	ring_.assign(channels, std::vector<float>(static_cast<size_t>(desiredFrames), 0.0f));
	writeIndex_ = 0;
	bufferedFrames_ = 0;
	haveExpectedTimestamp_ = false;
}

void AudioDelayFilter::ResetHistory()
{
	writeIndex_ = 0;
	bufferedFrames_ = 0;
	haveExpectedTimestamp_ = false;
}

void AudioDelayFilter::WriteSilence(size_t frames)
{
	size_t ringLen = ring_.empty() ? 0 : ring_[0].size();
	if (ringLen == 0 || frames == 0 || frames >= ringLen)
		return;
	for (auto &channelRing : ring_) {
		size_t firstPart = std::min(frames, ringLen - writeIndex_);
		std::fill_n(channelRing.begin() + static_cast<std::ptrdiff_t>(writeIndex_), firstPart, 0.0f);
		if (firstPart < frames)
			std::fill_n(channelRing.begin(), frames - firstPart, 0.0f);
	}
	writeIndex_ = (writeIndex_ + frames) % ringLen;
	bufferedFrames_ = std::min(bufferedFrames_ + frames, ringLen);
}

obs_audio_data *AudioDelayFilter::FilterAudio(obs_audio_data *audio)
{
	if (!audio || audio->frames == 0)
		return audio;

	audio_t *audioContext = obs_get_audio();
	auto channels = static_cast<uint32_t>(audio_output_get_channels(audioContext));
	uint32_t sampleRate = audio_output_get_sample_rate(audioContext);
	if (channels == 0 || sampleRate == 0)
		return audio;

	// One consistent snapshot for the whole call: Update() can change the
	// delay from the UI thread at any moment.
	const uint32_t delaySeconds = configuredDelaySeconds_.load(std::memory_order_relaxed);

	EnsureRingSized(channels, sampleRate, delaySeconds);
	size_t frames = audio->frames;
	size_t ringLen = ring_.empty() ? 0 : ring_[0].size();
	if (ringLen == 0 || frames >= ringLen)
		return audio;

	if (resetRequested_.exchange(false, std::memory_order_acq_rel))
		ResetHistory();

	// --- Keep the ring on the wall clock, not on "samples received" ---
	// Sources that stop delivering audio while idle (application/window
	// audio capture, paused media, browser sources) would otherwise make
	// this delay stretch beyond the video's: the ring only advanced when
	// samples arrived. A forward gap is filled with silence; a rewind, or a
	// gap longer than the whole ring (source restarted), invalidates the
	// history instead.
	const auto chunkNs = static_cast<uint64_t>(frames) * 1000000000ULL / sampleRate;
	if (haveExpectedTimestamp_) {
		auto drift = static_cast<int64_t>(audio->timestamp) - static_cast<int64_t>(expectedTimestamp_);
		if (drift > static_cast<int64_t>(kGapToleranceNs)) {
			uint64_t gapFrames = static_cast<uint64_t>(drift) * sampleRate / 1000000000ULL;
			bool tooLong = gapFrames >= ringLen - frames;
			if (tooLong)
				ResetHistory();
			else
				WriteSilence(static_cast<size_t>(gapFrames));
			if (timelineEventCount_++ < 20)
				TRIGGLOW_LOG_INFO(kComponent, "\"%s\": %lldms gap in source audio, %s",
						  obs_source_get_name(filterSource_),
						  static_cast<long long>(drift / 1000000LL),
						  tooLong ? "history reset" : "filled with silence");
		} else if (drift < -static_cast<int64_t>(kGapToleranceNs)) {
			ResetHistory();
			if (timelineEventCount_++ < 20)
				TRIGGLOW_LOG_INFO(kComponent,
						  "\"%s\": source timestamp went back %lldms, history reset",
						  obs_source_get_name(filterSource_),
						  static_cast<long long>(-drift / 1000000LL));
		}
	}
	haveExpectedTimestamp_ = true;
	expectedTimestamp_ = audio->timestamp + chunkNs;

	auto **inData = reinterpret_cast<float **>(audio->data);

	// --- Write: copy this call's samples into the ring, per channel ---
	for (uint32_t c = 0; c < channels_; ++c) {
		std::vector<float> &channelRing = ring_[c];
		size_t firstPart = std::min(frames, ringLen - writeIndex_);
		if (!inData[c]) {
			// Missing plane: write silence rather than leaving samples
			// from a previous lap of the ring to be read back later.
			std::fill_n(channelRing.begin() + static_cast<std::ptrdiff_t>(writeIndex_), firstPart, 0.0f);
			if (firstPart < frames)
				std::fill_n(channelRing.begin(), frames - firstPart, 0.0f);
			continue;
		}
		std::memcpy(channelRing.data() + writeIndex_, inData[c], firstPart * sizeof(float));
		if (firstPart < frames)
			std::memcpy(channelRing.data(), inData[c] + firstPart, (frames - firstPart) * sizeof(float));
	}
	writeIndex_ = (writeIndex_ + frames) % ringLen;
	bufferedFrames_ = std::min(bufferedFrames_ + frames, ringLen);

	// --- Read: whichever `frames`-sized window is delaySeconds old ---
	uint64_t delayFrames = static_cast<uint64_t>(delaySeconds) * sampleRate;
	delayFrames = std::min(delayFrames, static_cast<uint64_t>(ringLen - frames));
	bool haveEnoughHistory = bufferedFrames_ >= delayFrames + frames;

	outputChunks_.resize(channels_);
	for (auto &chunk : outputChunks_)
		chunk.resize(frames);

	if (haveEnoughHistory) {
		size_t readIndex = (writeIndex_ + ringLen * 2 - frames - static_cast<size_t>(delayFrames)) % ringLen;
		for (uint32_t c = 0; c < channels_; ++c) {
			std::vector<float> &channelRing = ring_[c];
			size_t firstPart = std::min(frames, ringLen - readIndex);
			std::memcpy(outputChunks_[c].data(), channelRing.data() + readIndex, firstPart * sizeof(float));
			if (firstPart < frames)
				std::memcpy(outputChunks_[c].data() + firstPart, channelRing.data(),
					    (frames - firstPart) * sizeof(float));
		}

		// Coming out of warm-up/reset silence straight into mid-waveform
		// audio is a hard step, i.e. an audible click. Short fade-in.
		if (!outputWasActive_) {
			size_t fadeFrames = std::min<size_t>(frames, std::max<uint32_t>(1, sampleRate / 100));
			for (auto &chunk : outputChunks_)
				for (size_t i = 0; i < fadeFrames; ++i)
					chunk[i] *= static_cast<float>(i) / static_cast<float>(fadeFrames);
		}
		outputWasActive_ = true;
	} else {
		// Still filling -- silence, same spirit as VideoDelayFilter drawing
		// nothing while its own buffer warms up. Program is on the loading
		// scene during this window anyway, so this is never actually heard.
		for (auto &chunk : outputChunks_)
			std::fill(chunk.begin(), chunk.end(), 0.0f);
		outputWasActive_ = false;
	}

	for (uint32_t c = 0; c < channels_; ++c)
		output_.data[c] = reinterpret_cast<uint8_t *>(outputChunks_[c].data());
	for (uint32_t c = channels_; c < MAX_AV_PLANES; ++c)
		output_.data[c] = nullptr;
	output_.frames = static_cast<uint32_t>(frames);
	// Passthrough, not recomputed: mirrors VideoDelayFilter, which draws old
	// pixels at the CURRENT instant rather than rewinding a timestamp.
	output_.timestamp = audio->timestamp;

	return &output_;
}

void AudioDelayFilter::Update(obs_data_t *settings)
{
	auto seconds = static_cast<uint32_t>(obs_data_get_int(settings, kSettingDelaySeconds));
	if (seconds != configuredDelaySeconds_.load()) {
		configuredDelaySeconds_.store(seconds);
		TRIGGLOW_LOG_INFO(kComponent, "delay set to %us on \"%s\"", seconds,
				  obs_source_get_name(filterSource_));
	}

	int64_t token = obs_data_get_int(settings, kSettingResetToken);
	if (token != lastResetToken_) {
		lastResetToken_ = token;
		resetRequested_.store(true, std::memory_order_release);
	}
}

const char *AudioDelayFilter::GetName(void * /*typeData*/)
{
	return Str("Filter.AudioDelay.Name");
}

void *AudioDelayFilter::Create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new AudioDelayFilter(source);
	filter->Update(settings);
	return filter;
}

void AudioDelayFilter::Destroy(void *data)
{
	delete static_cast<AudioDelayFilter *>(data);
}

void AudioDelayFilter::UpdateCb(void *data, obs_data_t *settings)
{
	static_cast<AudioDelayFilter *>(data)->Update(settings);
}

obs_audio_data *AudioDelayFilter::FilterAudioCb(void *data, obs_audio_data *audio)
{
	return static_cast<AudioDelayFilter *>(data)->FilterAudio(audio);
}

obs_properties_t *AudioDelayFilter::GetProperties(void * /*data*/)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, kSettingDelaySeconds, "Delay (segundos)", 0, kUiMaxDelaySeconds, 1);
	return props;
}

void AudioDelayFilter::GetDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kSettingDelaySeconds, 0);
	obs_data_set_default_int(settings, kSettingResetToken, 0);
}

} // namespace trigglow
