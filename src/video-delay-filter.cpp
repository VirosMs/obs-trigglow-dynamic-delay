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
#include "logging.hpp"

#include <algorithm>

namespace trigglow {

namespace {
constexpr const char *kComponent = "video-delay-filter";
constexpr const char *kFilterId = "trigglow_video_delay_filter";
constexpr const char *kSettingDelaySeconds = "delay_seconds";

// VRAM budget for the ring buffer. Uncompressed RGBA at 1920x1080@60fps is
// ~475MB PER SECOND (1920*1080*4 bytes * 60) — this is the real constraint,
// not an arbitrary "max seconds" slider. Requesting more delay than this
// budget covers at the source's actual resolution/fps silently clamps
// (logged once per change), it never grows unbounded. 1.5GB is a
// conservative default: at 1080p60 that's ~3.2s of true headroom; lower
// resolutions/framerates (e.g. a webcam at 720p30) get proportionally more.
constexpr uint64_t kMaxBufferBytes = 1536ULL * 1024 * 1024;

// UI slider cap. The real enforcement is EnsureRingSized()'s budget math —
// this just keeps the properties panel from offering a wildly unrealistic
// number before that math clamps it anyway.
constexpr int kUiMaxDelaySeconds = 60;
} // namespace

const char *VideoDelayFilter::Id()
{
	return kFilterId;
}

void VideoDelayFilter::Register()
{
	obs_source_info info = {};
	info.id = kFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
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
}

void VideoDelayFilter::ReleaseRing()
{
	for (auto &slot : ring_) {
		if (slot.texrender)
			gs_texrender_destroy(slot.texrender);
	}
	ring_.clear();
	writeIndex_ = 0;
	bufferedCount_ = 0;
}

void VideoDelayFilter::EnsureRingSized(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return;

	// Slot count comes from the VRAM budget at this resolution, not
	// directly from configuredDelaySeconds_ — see kMaxBufferBytes above.
	uint64_t bytesPerFrame = static_cast<uint64_t>(width) * height * 4;
	uint64_t maxFrames = std::max<uint64_t>(1, kMaxBufferBytes / bytesPerFrame);

	uint64_t requestedFrames = static_cast<uint64_t>(configuredDelaySeconds_) * std::max<uint32_t>(1, currentFps_);
	// +1 so a full N-second delay has a valid slot to read from, not just
	// N seconds of frames with none old enough yet.
	uint64_t desiredFrames = std::min(requestedFrames + 1, maxFrames);
	desiredFrames = std::max<uint64_t>(1, desiredFrames);

	bool resolutionChanged = !ring_.empty() && (ring_[0].width != width || ring_[0].height != height);
	if (!resolutionChanged && ring_.size() == desiredFrames)
		return; // Already correctly sized.

	if (requestedFrames + 1 > maxFrames) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "requested %us delay at %ux%u@%ufps needs more than the %lluMB VRAM budget; "
				  "clamped to ~%llu frames (~%.1fs)",
				  configuredDelaySeconds_, width, height, currentFps_,
				  static_cast<unsigned long long>(kMaxBufferBytes / (1024 * 1024)),
				  static_cast<unsigned long long>(maxFrames),
				  static_cast<double>(maxFrames) / std::max<uint32_t>(1, currentFps_));
	}

	ReleaseRing();
	ring_.resize(static_cast<size_t>(desiredFrames));
	for (auto &slot : ring_) {
		slot.width = width;
		slot.height = height;
	}
}

void VideoDelayFilter::Update(obs_data_t *settings)
{
	auto seconds = static_cast<uint32_t>(obs_data_get_int(settings, kSettingDelaySeconds));
	if (seconds != configuredDelaySeconds_) {
		configuredDelaySeconds_ = seconds;
		TRIGGLOW_LOG_INFO(kComponent, "delay set to %us", configuredDelaySeconds_);
	}
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
		obs_source_skip_video_filter(filterSource_);
		return;
	}

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);
	if (width == 0 || height == 0) {
		obs_source_skip_video_filter(filterSource_);
		return;
	}

	EnsureRingSized(width, height);
	if (ring_.empty()) {
		obs_source_skip_video_filter(filterSource_);
		return;
	}

	// --- Capture: render the target into this frame's ring slot ---
	Slot &writeSlot = ring_[writeIndex_];
	if (!writeSlot.texrender)
		writeSlot.texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	gs_texrender_reset(writeSlot.texrender);
	if (gs_texrender_begin(writeSlot.texrender, width, height)) {
		struct vec4 clearColor = {};
		gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
		gs_matrix_push();
		gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
		obs_source_video_render(target);
		gs_matrix_pop();
		gs_texrender_end(writeSlot.texrender);
		writeSlot.valid = true;
	}

	// --- Playback: draw whichever slot is configuredDelaySeconds_ old ---
	uint64_t delayFrames = static_cast<uint64_t>(configuredDelaySeconds_) * std::max<uint32_t>(1, currentFps_);
	delayFrames = std::min(delayFrames, static_cast<uint64_t>(ring_.size() - 1));
	size_t readIndex = (writeIndex_ + ring_.size() - static_cast<size_t>(delayFrames)) % ring_.size();
	bool haveEnoughHistory = bufferedCount_ > delayFrames;

	if (haveEnoughHistory && ring_[readIndex].valid) {
		gs_texture_t *tex = gs_texrender_get_texture(ring_[readIndex].texrender);
		if (tex) {
			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
			gs_effect_set_texture(image, tex);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(tex, 0, width, height);
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
		prop, "Segundos de retraso. El maximo real depende de la resolucion/FPS de la fuente y de un "
		      "presupuesto de VRAM fijo (~1.5GB) - pedir mas de lo que cabe se recorta automaticamente "
		      "(ver el log de OBS).");
	return props;
}

void VideoDelayFilter::GetDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kSettingDelaySeconds, 0);
}

} // namespace trigglow
