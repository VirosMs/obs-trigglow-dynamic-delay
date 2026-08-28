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

// RAM budget for the ring buffer. Even stored as NV12 (1.5 bytes/pixel --
// see Slot's comment in video-delay-filter.hpp for why it's NV12 and not
// RGBA) 1920x1080@60fps is still ~178MB PER SECOND (1920*1080*1.5 bytes *
// 60) — a real 30-60s buffer at that resolution is still multiple GB, not
// something to allocate blindly on top of whatever else the game/OBS/
// Windows itself is doing. No proper compressed encode/decode buffer (a
// much bigger rewrite — libobs has no public decoder API for that, only
// obs-encoder.h for producing an OUTPUT stream tied to the main video mix,
// not something a plugin can feed arbitrary frames to on demand; would need
// vendoring FFmpeg for both encode AND decode — investigated and explicitly
// deferred, 2026-08-25, as multi-session work).
//
// GetBufferBudget() below picks the actual budget from the machine's total
// system RAM (src/hardware-info.hpp) where that's available, falling back
// to kFallbackBufferBytes when it isn't (non-Windows for now, or the query
// failed).
//
// The fraction was 10% in the first RAM-migration pass (RGBA storage back
// then), which turned out far too conservative in practice (2026-08-25 live
// feedback: on a 32GB machine it only budgeted ~3.1GB, enough for just
// ~6.7s of the 30s@1080p the user actually asked for). Raised to 50% that
// same day, then NV12 landed the next day (2026-08-26) after 50%-of-RGBA
// turned out to mean a single 30s@1080p60 buffer alone used ~98% of system
// RAM with nothing even streaming yet -- genuinely risky, not just
// "aggressive". NV12's ~2.7x smaller footprint (a 30s@1080p60 buffer is
// ~5.3GB now, not ~14.2GB) does the real work here; 50% is kept since it
// now leaves comfortable headroom for that same request instead of nearly
// exhausting it.
constexpr uint64_t kFallbackBufferBytes = 2048ULL * 1024 * 1024;
constexpr uint64_t kMinBufferBytes = 1024ULL * 1024 * 1024;             // Floor even on a detected low-RAM machine.
constexpr uint64_t kMaxRecommendedBufferBytes = 24576ULL * 1024 * 1024; // Ceiling even on a very high-RAM machine.
constexpr double kRamFractionForBuffer = 0.50;                          // Don't hog more than ~50% of total system RAM.

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

// v0.3.0: on TRIGGLOW_HAVE_FFMPEG platforms, actualFrames is computed against
// an ASSUMED compression ratio (real per-frame size isn't known until after
// encoding) rather than the raw NV12 byte count -- Policy A from the design
// discussion: pick a conservative assumed ratio up front rather than
// adapting live, and re-measure against real content before ever raising it.
// 3x is deliberately conservative: MJPEG on natural video often does much
// better, but OBS capture content is disproportionately full of exactly what
// compresses worst (sharp UI/HUD edges, on-screen text) -- see
// docs/ROADMAP.md. 1.0 (no assumed compression) on platforms without
// FFmpeg keeps this function's behavior byte-for-byte identical to
// pre-v0.3.0. Slot::pixels is still always allocated at the full raw NV12
// size regardless (EnsureRingSized()) -- this ratio only affects how many
// SLOTS the ring gets, never how big each one physically is.
#ifdef TRIGGLOW_HAVE_FFMPEG
constexpr double kAssumedCompressionRatio = 3.0;

// FFmpeg's generic quantizer scale (AV_CODEC_FLAG_QSCALE): 1 = best quality/
// least compression, 31 = worst/most. 5 is a commonly-cited sweet spot for
// MJPEG -- visually close to lossless while still compressing meaningfully.
// Fixed for this first pass, not a dock setting yet (see docs/ROADMAP.md).
constexpr int kMjpegQuality = 5;
#else
constexpr double kAssumedCompressionRatio = 1.0;
#endif

BufferFit ComputeBufferFit(uint32_t origWidth, uint32_t origHeight, uint32_t fps, uint32_t delaySeconds,
			   uint32_t minResolutionHeight, uint64_t budget, double compressionRatio)
{
	// +1 so a full N-second delay has a valid slot to read from, not just
	// N seconds of frames with none old enough yet.
	uint64_t desiredFrames = static_cast<uint64_t>(delaySeconds) * fps + 1;

	double minScale = std::min(1.0, static_cast<double>(minResolutionHeight) / origHeight);

	// NV12 (Y + subsampled UV): 1.5 bytes/pixel, not 4 -- see Slot's comment
	// in video-delay-filter.hpp for why the ring stores NV12 rather than
	// RGBA. Divided by compressionRatio for the EFFECTIVE per-frame cost
	// used in ring-length sizing below -- the actual per-slot allocation
	// (EnsureRingSized()) stays at the full raw size regardless.
	uint64_t fullResBytesPerFrame = (static_cast<uint64_t>(origWidth) * origHeight * 3) / 2;
	double fullResEffectiveBytesPerFrame = static_cast<double>(fullResBytesPerFrame) / compressionRatio;
	double fullResTotalBytes = static_cast<double>(desiredFrames) * fullResEffectiveBytesPerFrame;

	double scale = 1.0;
	if (fullResTotalBytes > static_cast<double>(budget) && fullResEffectiveBytesPerFrame > 0) {
		double idealScale = std::sqrt(static_cast<double>(budget) /
					      (static_cast<double>(desiredFrames) * fullResEffectiveBytesPerFrame));
		scale = std::max(minScale, std::min(1.0, idealScale));
	}

	uint32_t bufferWidth = std::max<uint32_t>(2, static_cast<uint32_t>(origWidth * scale));
	uint32_t bufferHeight = std::max<uint32_t>(2, static_cast<uint32_t>(origHeight * scale));
	// NV12 chroma subsampling needs even dimensions -- both inputs to this
	// mask are already >=2, so the result stays >=2 too.
	bufferWidth &= ~1u;
	bufferHeight &= ~1u;

	uint64_t bufferBytesPerFrame = (static_cast<uint64_t>(bufferWidth) * bufferHeight * 3) / 2;
	double bufferEffectiveBytesPerFrame = static_cast<double>(bufferBytesPerFrame) / compressionRatio;
	uint64_t maxFramesAtBufferRes = std::max<uint64_t>(
		1, static_cast<uint64_t>(static_cast<double>(budget) / bufferEffectiveBytesPerFrame));
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

// GPU-side RGBA<->NV12 conversion, compiled once via gs_effect_create() (no
// file on disk needed -- the whole plugin is one DLL). Ordinary full-screen-
// quad pixel shaders, the same mechanism any custom OBS filter (color
// correction, chroma key, etc.) uses for per-pixel work; libobs has no
// compute/dispatch API at all (confirmed 2026-08-25 grepping graphics.h) so
// this is the only public way to do the conversion on the GPU.
//   DrawY   -- samples the RGBA capture (`image`) at full resolution, writes
//              luma to a GS_R8 target.
//   DrawUV  -- samples the SAME RGBA capture but into a HALF-resolution
//              target; `texel` (1/full-res width, 1/full-res height) lets it
//              box-average each 2x2 source block instead of point-sampling
//              one corner, avoiding the checkerboard-y chroma artifacts a
//              naive single-tap downsample would cause. Writes to GS_R8G8.
//   DrawNV12 -- playback: samples `image` (Y, GS_R8) and `image_uv` (UV,
//               GS_R8G8, still at half resolution -- bilinear sampling on
//               `def_sampler` upsamples it smoothly) and reconstructs RGBA.
// Coefficients are standard BT.601 studio-range (16-235 luma, 16-240
// chroma), matching what NV12 conventionally means -- good enough for a
// streaming delay buffer, not broadcast-grade color science.
constexpr const char *kNv12EffectSource = R"efx(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d image_uv;
uniform float2 texel;

sampler_state def_sampler {
	Filter    = Linear;
	AddressU  = Clamp;
	AddressV  = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 PSDrawY(VertData v_in) : TARGET
{
	float3 rgb = image.Sample(def_sampler, v_in.uv).rgb;
	float y = dot(rgb, float3(0.257, 0.504, 0.098)) + (16.0 / 255.0);
	return float4(y, y, y, 1.0);
}

float4 PSDrawUV(VertData v_in) : TARGET
{
	float2 base = v_in.uv - texel * 0.5;
	float3 c0 = image.Sample(def_sampler, base).rgb;
	float3 c1 = image.Sample(def_sampler, base + float2(texel.x, 0.0)).rgb;
	float3 c2 = image.Sample(def_sampler, base + float2(0.0, texel.y)).rgb;
	float3 c3 = image.Sample(def_sampler, base + texel).rgb;
	float3 rgb = (c0 + c1 + c2 + c3) * 0.25;
	float u = dot(rgb, float3(-0.148, -0.291, 0.439)) + (128.0 / 255.0);
	float v = dot(rgb, float3(0.439, -0.368, -0.071)) + (128.0 / 255.0);
	return float4(u, v, 0.0, 1.0);
}

float4 PSDrawNV12(VertData v_in) : TARGET
{
	float y = image.Sample(def_sampler, v_in.uv).r - (16.0 / 255.0);
	float2 uv = image_uv.Sample(def_sampler, v_in.uv).rg - (128.0 / 255.0);
	float3 rgb;
	rgb.r = 1.164 * y + 1.596 * uv.y;
	rgb.g = 1.164 * y - 0.392 * uv.x - 0.813 * uv.y;
	rgb.b = 1.164 * y + 2.017 * uv.x;
	return float4(saturate(rgb), 1.0);
}

technique DrawY
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSDrawY(v_in);
	}
}

technique DrawUV
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSDrawUV(v_in);
	}
}

technique DrawNV12
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSDrawNV12(v_in);
	}
}
)efx";
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
					 GetBufferBudget(), kAssumedCompressionRatio);

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
	ReleaseSizedGpuObjects();
	// nv12Effect_ is just compiled shader code -- unlike everything in
	// ReleaseSizedGpuObjects() it isn't sized to bufferWidth_/bufferHeight_,
	// so a resolution change (EnsureRingSized's resolutionChanged path)
	// doesn't need to recompile it. Only the real destructor tears this down.
	if (nv12Effect_) {
		gs_effect_destroy(nv12Effect_);
		nv12Effect_ = nullptr;
	}
}

void VideoDelayFilter::ReleaseSizedGpuObjects()
{
#ifdef TRIGGLOW_HAVE_FFMPEG
	// Codec contexts are sized to bufferWidth_/bufferHeight_ exactly like
	// the GPU pool below -- same lifecycle, same two call sites
	// (resolutionChanged in EnsureRingSized(), and the destructor).
	ReleaseCodecContexts();
#endif

	if (captureTexrender_) {
		gs_texrender_destroy(captureTexrender_);
		captureTexrender_ = nullptr;
	}
	if (yTexrender_) {
		gs_texrender_destroy(yTexrender_);
		yTexrender_ = nullptr;
	}
	if (uvTexrender_) {
		gs_texrender_destroy(uvTexrender_);
		uvTexrender_ = nullptr;
	}
	if (yPlaybackTexture_) {
		gs_texture_destroy(yPlaybackTexture_);
		yPlaybackTexture_ = nullptr;
	}
	if (uvPlaybackTexture_) {
		gs_texture_destroy(uvPlaybackTexture_);
		uvPlaybackTexture_ = nullptr;
	}
	for (auto &slot : stagingSlots_) {
		if (slot.ySurface) {
			gs_stagesurface_destroy(slot.ySurface);
			slot.ySurface = nullptr;
		}
		if (slot.uvSurface) {
			gs_stagesurface_destroy(slot.uvSurface);
			slot.uvSurface = nullptr;
		}
		slot.pending = false;
	}
}

#ifdef TRIGGLOW_HAVE_FFMPEG
void VideoDelayFilter::EnsureCodecContextsOpen()
{
	if (encoderCtx_ && decoderCtx_)
		return; // Already open at the current bufferWidth_/bufferHeight_.

	loggedFirstEncode_ = false; // New contexts at a possibly new resolution -- log a fresh ratio for them.

	const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
	if (!encoder || !decoder) {
		TRIGGLOW_LOG_WARN(kComponent, "MJPEG encoder/decoder not found in linked FFmpeg -- buffer will store "
					      "uncompressed NV12 this session (same as before v0.3.0)");
		return;
	}

	encoderCtx_ = avcodec_alloc_context3(encoder);
	encoderCtx_->width = static_cast<int>(bufferWidth_);
	encoderCtx_->height = static_cast<int>(bufferHeight_);
	// Modern replacement for the deprecated AV_PIX_FMT_YUVJ420P: plain 4:2:0
	// planar plus an explicit full-range color_range, which is what "J"
	// (JPEG-range) formats meant anyway.
	encoderCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
	encoderCtx_->color_range = AVCOL_RANGE_JPEG;
	encoderCtx_->time_base = AVRational{1, static_cast<int>(std::max<uint32_t>(1, currentFps_))};
	// Fixed quality via AV_CODEC_FLAG_QSCALE rather than a bitrate target --
	// simpler for a first pass, no rate-control ramp-up/lookahead to reason
	// about (irrelevant anyway for an all-intra codec with no lookahead).
	// Not a user-facing setting yet -- see docs/ROADMAP.md's "Fases futuras"
	// for the dock quality control this could grow into later.
	encoderCtx_->flags |= AV_CODEC_FLAG_QSCALE;
	encoderCtx_->qmin = kMjpegQuality;
	encoderCtx_->qmax = kMjpegQuality;

	if (avcodec_open2(encoderCtx_, encoder, nullptr) < 0) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "failed to open MJPEG encoder at %ux%u -- storing uncompressed NV12 "
				  "this session",
				  bufferWidth_, bufferHeight_);
		avcodec_free_context(&encoderCtx_);
		return;
	}

	decoderCtx_ = avcodec_alloc_context3(decoder);
	if (avcodec_open2(decoderCtx_, decoder, nullptr) < 0) {
		TRIGGLOW_LOG_WARN(kComponent, "failed to open MJPEG decoder -- storing uncompressed NV12 this "
					      "session");
		avcodec_free_context(&encoderCtx_);
		avcodec_free_context(&decoderCtx_);
		return;
	}

	if (!encodeFrame_)
		encodeFrame_ = av_frame_alloc();
	if (!encodePacket_)
		encodePacket_ = av_packet_alloc();
	if (!decodeFrame_)
		decodeFrame_ = av_frame_alloc();
	if (!decodePacket_)
		decodePacket_ = av_packet_alloc();

	TRIGGLOW_LOG_INFO(kComponent, "MJPEG encoder/decoder open at %ux%u (quality qscale=%d)", bufferWidth_,
			  bufferHeight_, kMjpegQuality);
}

void VideoDelayFilter::ReleaseCodecContexts()
{
	if (encoderCtx_)
		avcodec_free_context(&encoderCtx_);
	if (decoderCtx_)
		avcodec_free_context(&decoderCtx_);
	if (encodeFrame_)
		av_frame_free(&encodeFrame_);
	if (encodePacket_)
		av_packet_free(&encodePacket_);
	if (decodeFrame_)
		av_frame_free(&decodeFrame_);
	if (decodePacket_)
		av_packet_free(&decodePacket_);
}

bool VideoDelayFilter::EncodeScratchNv12Into(Slot &dst)
{
	if (!encoderCtx_ || !encodeFrame_ || !encodePacket_)
		return false;

	uint32_t uvWidth = bufferWidth_ / 2;
	uint32_t uvHeight = bufferHeight_ / 2;
	size_t yBytes = static_cast<size_t>(bufferWidth_) * bufferHeight_;
	size_t chromaBytes = static_cast<size_t>(uvWidth) * uvHeight;

	// MJPEG's encoder wants PLANAR 4:2:0 (separate U and V planes) --
	// de-interleave scratchNv12_'s NV12 UV plane into two scratch planes we
	// own and control the (tightly-packed) layout of.
	encodeScratchU_.resize(chromaBytes);
	encodeScratchV_.resize(chromaBytes);
	const uint8_t *uv = scratchNv12_.data() + yBytes;
	for (uint32_t row = 0; row < uvHeight; ++row) {
		const uint8_t *uvRow = uv + static_cast<size_t>(row) * uvWidth * 2;
		uint8_t *uRow = encodeScratchU_.data() + static_cast<size_t>(row) * uvWidth;
		uint8_t *vRow = encodeScratchV_.data() + static_cast<size_t>(row) * uvWidth;
		for (uint32_t col = 0; col < uvWidth; ++col) {
			uRow[col] = uvRow[col * 2 + 0];
			vRow[col] = uvRow[col * 2 + 1];
		}
	}

	av_frame_unref(encodeFrame_);
	encodeFrame_->format = encoderCtx_->pix_fmt;
	encodeFrame_->width = static_cast<int>(bufferWidth_);
	encodeFrame_->height = static_cast<int>(bufferHeight_);
	if (av_frame_get_buffer(encodeFrame_, 0) < 0 || av_frame_make_writable(encodeFrame_) < 0)
		return false;

	// av_frame_get_buffer() picks FFmpeg's own (possibly padded) alignment --
	// respect ITS linesize when copying our tightly-packed scratch planes in,
	// same care already taken when reading GPU-staged memory elsewhere in
	// this file.
	for (int row = 0; row < encodeFrame_->height; ++row) {
		std::memcpy(encodeFrame_->data[0] + static_cast<size_t>(row) * encodeFrame_->linesize[0],
			    scratchNv12_.data() + static_cast<size_t>(row) * bufferWidth_, bufferWidth_);
	}
	for (uint32_t row = 0; row < uvHeight; ++row) {
		std::memcpy(encodeFrame_->data[1] + static_cast<size_t>(row) * encodeFrame_->linesize[1],
			    encodeScratchU_.data() + static_cast<size_t>(row) * uvWidth, uvWidth);
		std::memcpy(encodeFrame_->data[2] + static_cast<size_t>(row) * encodeFrame_->linesize[2],
			    encodeScratchV_.data() + static_cast<size_t>(row) * uvWidth, uvWidth);
	}
	encodeFrame_->pts = static_cast<int64_t>(captureFrameCounter_);

	if (avcodec_send_frame(encoderCtx_, encodeFrame_) < 0)
		return false;

	av_packet_unref(encodePacket_);
	// MJPEG is all-intra with zero lookahead, so this should always succeed
	// immediately after a send -- if it doesn't (EAGAIN or a real error),
	// this frame just falls back to raw storage for this one tick rather
	// than blocking/retrying, same spirit as the rest of this file's
	// "degrade, don't stall" error handling.
	if (avcodec_receive_packet(encoderCtx_, encodePacket_) < 0)
		return false;

	size_t packetSize = static_cast<size_t>(encodePacket_->size);
	// The true ceiling is the raw NV12 size, not dst.pixels.size() -- v0.3.0
	// Phase 2 starts slots at a smaller BUDGETED allocation (see
	// EnsureRingSized()) and grows a given slot only as far as an individual
	// frame actually needs, so dst.pixels.size() no longer reflects a fixed
	// worst-case ceiling the way it did in Phase 1.
	size_t rawFrameBytes = (static_cast<size_t>(bufferWidth_) * bufferHeight_ * 3) / 2;
	if (packetSize > rawFrameBytes) {
		// Should never happen in practice (compressed output exceeding the
		// raw NV12 worst-case), but never write out of bounds.
		TRIGGLOW_LOG_WARN(kComponent,
				  "MJPEG packet (%zu bytes) exceeded the raw NV12 ceiling (%zu bytes) -- dropping "
				  "this frame",
				  packetSize, rawFrameBytes);
		return false;
	}

	// Grow-only: a slot that has previously needed more room than the
	// budgeted default keeps that larger capacity permanently (never shrunk
	// back down) rather than paying a realloc on every single frame -- see
	// EnsureRingSized()'s comment on the tradeoff this accepts.
	if (dst.pixels.size() < packetSize)
		dst.pixels.resize(packetSize);

	std::memcpy(dst.pixels.data(), encodePacket_->data, packetSize);
	dst.usedBytes = packetSize;
	dst.compressed = true;

	if (!loggedFirstEncode_) {
		TRIGGLOW_LOG_INFO(kComponent,
				  "first MJPEG encode this session: %zu bytes (raw NV12 would be %zu -- %.1fx)",
				  packetSize, rawFrameBytes,
				  static_cast<double>(rawFrameBytes) / std::max<size_t>(1, packetSize));
		loggedFirstEncode_ = true;
	}

	return true;
}

bool VideoDelayFilter::DecodeSlotIntoScratchNv12(const Slot &src)
{
	if (!decoderCtx_ || !decodeFrame_ || !decodePacket_)
		return false;

	av_packet_unref(decodePacket_);
	if (av_new_packet(decodePacket_, static_cast<int>(src.usedBytes)) < 0)
		return false;
	std::memcpy(decodePacket_->data, src.pixels.data(), src.usedBytes);

	if (avcodec_send_packet(decoderCtx_, decodePacket_) < 0)
		return false;

	av_frame_unref(decodeFrame_);
	if (avcodec_receive_frame(decoderCtx_, decodeFrame_) < 0)
		return false;

	uint32_t uvWidth = bufferWidth_ / 2;
	uint32_t uvHeight = bufferHeight_ / 2;
	size_t yBytes = static_cast<size_t>(bufferWidth_) * bufferHeight_;

	for (uint32_t row = 0; row < bufferHeight_; ++row) {
		std::memcpy(scratchNv12_.data() + static_cast<size_t>(row) * bufferWidth_,
			    decodeFrame_->data[0] + static_cast<size_t>(row) * decodeFrame_->linesize[0], bufferWidth_);
	}

	uint8_t *uv = scratchNv12_.data() + yBytes;
	for (uint32_t row = 0; row < uvHeight; ++row) {
		const uint8_t *uRow = decodeFrame_->data[1] + static_cast<size_t>(row) * decodeFrame_->linesize[1];
		const uint8_t *vRow = decodeFrame_->data[2] + static_cast<size_t>(row) * decodeFrame_->linesize[2];
		uint8_t *uvRow = uv + static_cast<size_t>(row) * uvWidth * 2;
		for (uint32_t col = 0; col < uvWidth; ++col) {
			uvRow[col * 2 + 0] = uRow[col];
			uvRow[col * 2 + 1] = vRow[col];
		}
	}
	return true;
}
#endif // TRIGGLOW_HAVE_FFMPEG

void VideoDelayFilter::EnsureRingSized(uint32_t origWidth, uint32_t origHeight)
{
	if (origWidth == 0 || origHeight == 0)
		return;

	uint32_t fps = std::max<uint32_t>(1, currentFps_);
	BufferFit fit = ComputeBufferFit(origWidth, origHeight, fps, configuredDelaySeconds_,
					 configuredMinResolutionHeight_, GetBufferBudget(), kAssumedCompressionRatio);

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
	// The playback/staging objects are fixed-resolution GPU objects (unlike
	// the texrenders, which auto-resize on gs_texrender_begin) -- they must
	// be torn down and lazily recreated at the new size in Render().
	if (resolutionChanged) {
		ReleaseSizedGpuObjects();
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
	// NV12: Y plane (bufferWidth_*bufferHeight_ bytes) + interleaved UV
	// plane at half resolution ((bufferWidth_/2)*(bufferHeight_/2)*2 bytes,
	// i.e. bufferWidth_*bufferHeight_/2) -- 1.5 bytes/pixel total. Always an
	// exact integer since ComputeBufferFit() forces both dimensions even.
	size_t frameBytes = (static_cast<size_t>(bufferWidth_) * bufferHeight_ * 3) / 2;

	// v0.3.0 Phase 2: slots start at a BUDGETED size (frameBytes divided by
	// the same kAssumedCompressionRatio ComputeBufferFit() already assumed
	// for ring LENGTH above) instead of the full raw ceiling -- this is what
	// actually realizes the RAM win Phase 1 only allocated for on paper
	// (Slot::pixels was always the full raw size there, so a compressed
	// packet's smaller usedBytes never translated into less memory
	// reserved). EncodeScratchNv12Into()/the raw fallback below both grow a
	// given slot past this budget (permanently -- capacity is never shrunk
	// back down) whenever one particular frame genuinely needs more room
	// than assumed; see Slot's comment in the header for the full tradeoff.
	// 1.0 on non-FFmpeg platforms makes this identical to the pre-Phase-2
	// full raw allocation, unchanged.
	size_t budgetedBytes =
		static_cast<size_t>(std::ceil(static_cast<double>(frameBytes) / kAssumedCompressionRatio));
	ring_.resize(static_cast<size_t>(fit.actualFrames));
	for (auto &slot : ring_)
		slot.pixels.assign(budgetedBytes, 0);

	// scratchNv12_ is this same size -- one frame's worth of raw NV12,
	// reused every tick as the encode input / decode output staging area
	// (see EncodeScratchNv12Into()/DecodeSlotIntoScratchNv12(), or the
	// fallback path in Render() on platforms without TRIGGLOW_HAVE_FFMPEG).
	scratchNv12_.assign(frameBytes, 0);

#ifdef TRIGGLOW_HAVE_FFMPEG
	// If resolutionChanged, ReleaseSizedGpuObjects() above already tore down
	// any codec contexts sized for the OLD bufferWidth_/bufferHeight_ (see
	// its own ReleaseCodecContexts() call) -- this opens fresh ones at the
	// new size. If NOT resolutionChanged, the existing contexts are already
	// correct and EnsureCodecContextsOpen()'s own early-return handles that.
	EnsureCodecContextsOpen();
#endif
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

		// --- Capture step 2: convert the just-captured RGBA into Y (full
		// res) and UV (half res) planes via nv12Effect_'s two small
		// full-screen-quad shader passes -- see this file's header comment
		// and kNv12EffectSource for why (no compute-shader/RGBA->NV12 API
		// exists in public libobs). ---
		gs_texture_t *captured = gs_texrender_get_texture(captureTexrender_);
		if (!nv12Effect_) {
			char *errorString = nullptr;
			nv12Effect_ = gs_effect_create(kNv12EffectSource, "trigglow-nv12-convert.effect", &errorString);
			if (!nv12Effect_) {
				TRIGGLOW_LOG_WARN(kComponent, "failed to compile the NV12 conversion shader: %s",
						  errorString ? errorString : "(no error string)");
			}
			bfree(errorString);
		}

		if (captured && nv12Effect_) {
			uint32_t uvWidth = bufferWidth_ / 2;
			uint32_t uvHeight = bufferHeight_ / 2;

			if (!yTexrender_)
				yTexrender_ = gs_texrender_create(GS_R8, GS_ZS_NONE);
			if (!uvTexrender_)
				uvTexrender_ = gs_texrender_create(GS_R8G8, GS_ZS_NONE);

			gs_eparam_t *imageParam = gs_effect_get_param_by_name(nv12Effect_, "image");
			gs_eparam_t *texelParam = gs_effect_get_param_by_name(nv12Effect_, "texel");

			gs_texrender_reset(yTexrender_);
			if (gs_texrender_begin(yTexrender_, bufferWidth_, bufferHeight_)) {
				gs_matrix_push();
				gs_ortho(0.0f, static_cast<float>(bufferWidth_), 0.0f,
					 static_cast<float>(bufferHeight_), -100.0f, 100.0f);
				gs_effect_set_texture(imageParam, captured);
				while (gs_effect_loop(nv12Effect_, "DrawY"))
					gs_draw_sprite(captured, 0, bufferWidth_, bufferHeight_);
				gs_matrix_pop();
				gs_texrender_end(yTexrender_);
			}

			gs_texrender_reset(uvTexrender_);
			if (gs_texrender_begin(uvTexrender_, uvWidth, uvHeight)) {
				struct vec2 texel;
				vec2_set(&texel, 1.0f / static_cast<float>(bufferWidth_),
					 1.0f / static_cast<float>(bufferHeight_));
				gs_matrix_push();
				gs_ortho(0.0f, static_cast<float>(uvWidth), 0.0f, static_cast<float>(uvHeight), -100.0f,
					 100.0f);
				gs_effect_set_texture(imageParam, captured);
				gs_effect_set_vec2(texelParam, &texel);
				while (gs_effect_loop(nv12Effect_, "DrawUV"))
					gs_draw_sprite(captured, 0, uvWidth, uvHeight);
				gs_matrix_pop();
				gs_texrender_end(uvTexrender_);
			}

			// --- Capture step 3: harvest whichever staging slot was kicked
			// off LAST frame (its GPU->CPU copy has now had a full frame to
			// finish, so mapping it here never stalls the CPU the way
			// mapping the SAME frame's staging surface immediately would),
			// then kick off a fresh async readback of the Y/UV planes just
			// rendered above into the OTHER staging slot.
			// kStagingSlotCount==2 alternates cleanly via parity. ---
			size_t kickSlotIdx = captureFrameCounter_ % kStagingSlotCount;
			size_t harvestSlotIdx = (captureFrameCounter_ + 1) % kStagingSlotCount;
			++captureFrameCounter_;

			StagingSlot &harvest = stagingSlots_[harvestSlotIdx];
			if (harvest.pending && harvest.ySurface && harvest.uvSurface) {
				Slot &dst = ring_[harvest.targetRingIndex];
				size_t yBytes = static_cast<size_t>(bufferWidth_) * bufferHeight_;

				// Always land the raw readback in scratchNv12_ first (never
				// straight into dst.pixels anymore) -- on FFmpeg-capable
				// platforms this is the ENCODER's input, not the final
				// stored form; see EncodeScratchNv12Into()'s fallback below
				// for why dst still ends up with the same raw bytes when
				// compression isn't available/fails.
				uint8_t *mappedY = nullptr;
				uint32_t linesizeY = 0;
				if (gs_stagesurface_map(harvest.ySurface, &mappedY, &linesizeY)) {
					for (uint32_t row = 0; row < bufferHeight_; ++row) {
						std::memcpy(
							scratchNv12_.data() + static_cast<size_t>(row) * bufferWidth_,
							mappedY + static_cast<size_t>(row) * linesizeY, bufferWidth_);
					}
					gs_stagesurface_unmap(harvest.ySurface);
				}

				uint8_t *mappedUv = nullptr;
				uint32_t linesizeUv = 0;
				if (gs_stagesurface_map(harvest.uvSurface, &mappedUv, &linesizeUv)) {
					uint32_t uvRowBytes = uvWidth * 2; // GS_R8G8: 2 bytes/pixel.
					for (uint32_t row = 0; row < uvHeight; ++row) {
						std::memcpy(scratchNv12_.data() + yBytes +
								    static_cast<size_t>(row) * uvRowBytes,
							    mappedUv + static_cast<size_t>(row) * linesizeUv,
							    uvRowBytes);
					}
					gs_stagesurface_unmap(harvest.uvSurface);
				}

				bool encoded = false;
#ifdef TRIGGLOW_HAVE_FFMPEG
				encoded = EncodeScratchNv12Into(dst);
#endif
				if (!encoded) {
					// No FFmpeg on this platform, or the encoder isn't open,
					// or this particular frame failed to encode -- store the
					// raw NV12 bytes exactly like pre-v0.3.0 did. dst.pixels
					// starts at a smaller BUDGETED size on FFmpeg platforms
					// (see EnsureRingSized()), so grow it (permanently, same
					// tradeoff as EncodeScratchNv12Into()) if this is the
					// first time this slot has needed the full raw size.
					if (dst.pixels.size() < scratchNv12_.size())
						dst.pixels.resize(scratchNv12_.size());
					std::memcpy(dst.pixels.data(), scratchNv12_.data(), scratchNv12_.size());
					dst.usedBytes = scratchNv12_.size();
					dst.compressed = false;
				}

				dst.valid = true;
				harvest.pending = false;
			}

			StagingSlot &kick = stagingSlots_[kickSlotIdx];
			if (!kick.ySurface)
				kick.ySurface = gs_stagesurface_create(bufferWidth_, bufferHeight_, GS_R8);
			if (!kick.uvSurface)
				kick.uvSurface = gs_stagesurface_create(uvWidth, uvHeight, GS_R8G8);

			gs_texture_t *yTex = gs_texrender_get_texture(yTexrender_);
			gs_texture_t *uvTex = gs_texrender_get_texture(uvTexrender_);
			if (yTex && uvTex && kick.ySurface && kick.uvSurface) {
				gs_stage_texture(kick.ySurface, yTex);
				gs_stage_texture(kick.uvSurface, uvTex);
				kick.pending = true;
				kick.targetRingIndex = writeIndex_;
			}
		}
	}

	// --- Playback: draw whichever slot is configuredDelaySeconds_ old. Both
	// Y and UV planes get uploaded fresh from RAM into the two reusable
	// playback textures, then nv12Effect_'s "DrawNV12" technique samples
	// both and writes RGBA straight out -- no separate RGBA reconstruction
	// texture needed. ---
	uint64_t delayFrames = static_cast<uint64_t>(configuredDelaySeconds_) * std::max<uint32_t>(1, currentFps_);
	delayFrames = std::min(delayFrames, static_cast<uint64_t>(ring_.size() - 1));
	size_t readIndex = (writeIndex_ + ring_.size() - static_cast<size_t>(delayFrames)) % ring_.size();
	bool haveEnoughHistory = bufferedCount_ > delayFrames;

	if (haveEnoughHistory && ring_[readIndex].valid && nv12Effect_) {
		uint32_t uvWidth = bufferWidth_ / 2;
		uint32_t uvHeight = bufferHeight_ / 2;
		size_t yBytes = static_cast<size_t>(bufferWidth_) * bufferHeight_;

		const Slot &src = ring_[readIndex];
		const uint8_t *pixels = nullptr;
		bool ready = true;
		if (src.compressed) {
#ifdef TRIGGLOW_HAVE_FFMPEG
			ready = DecodeSlotIntoScratchNv12(src);
			pixels = scratchNv12_.data();
#else
			// A slot marked compressed can't exist on a platform that never
			// defines TRIGGLOW_HAVE_FFMPEG in the first place (nothing here
			// ever sets Slot::compressed=true without it) -- guarded purely
			// so this translation unit still compiles without the decoder.
			ready = false;
#endif
		} else {
			pixels = src.pixels.data();
		}

		if (!yPlaybackTexture_)
			yPlaybackTexture_ =
				gs_texture_create(bufferWidth_, bufferHeight_, GS_R8, 1, nullptr, GS_DYNAMIC);
		if (!uvPlaybackTexture_)
			uvPlaybackTexture_ = gs_texture_create(uvWidth, uvHeight, GS_R8G8, 1, nullptr, GS_DYNAMIC);

		if (ready && pixels && yPlaybackTexture_ && uvPlaybackTexture_) {
			gs_texture_set_image(yPlaybackTexture_, pixels, bufferWidth_, false);
			gs_texture_set_image(uvPlaybackTexture_, pixels + yBytes, uvWidth * 2, false);

			gs_eparam_t *imageParam = gs_effect_get_param_by_name(nv12Effect_, "image");
			gs_eparam_t *imageUvParam = gs_effect_get_param_by_name(nv12Effect_, "image_uv");
			gs_effect_set_texture(imageParam, yPlaybackTexture_);
			gs_effect_set_texture(imageUvParam, uvPlaybackTexture_);
			while (gs_effect_loop(nv12Effect_, "DrawNV12"))
				gs_draw_sprite(yPlaybackTexture_, 0, width, height);
		}
		// Else: decode failed for this one frame -- draw nothing this tick
		// rather than showing stale/garbage pixels; the next tick's read
		// index will move on to a different slot regardless.
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

	proc_handler_t *procHandler = obs_source_get_proc_handler(source);
	if (procHandler) {
		proc_handler_add(procHandler, "void release_buffers()", &VideoDelayFilter::ReleaseBuffersProc, filter);
		proc_handler_add(procHandler, "void get_effective_delay_seconds(out int seconds)",
				 &VideoDelayFilter::GetEffectiveDelaySecondsProc, filter);
	}

	return filter;
}

void VideoDelayFilter::Destroy(void *data)
{
	delete static_cast<VideoDelayFilter *>(data);
}

void VideoDelayFilter::ReleaseBuffersProc(void *data, calldata_t * /*params*/)
{
	// Called from ObsFrontendBridge::SetBufferFilterEnabled(false), which in
	// practice runs on whatever thread pressed Disable (the Qt dock's
	// button-click thread) -- but ring_ is only ever touched from the
	// graphics/video thread (inside Render()/EnsureRingSized()), so clearing
	// it right here would race a Render() call still in flight. Queueing
	// onto OBS_TASK_GRAPHICS instead runs it strictly after any such call
	// (the filter's already disabled by the time this fires, so nothing
	// else gets queued behind it). wait=true: Disable() should block until
	// the RAM is actually freed, not just "eventually".
	obs_queue_task(OBS_TASK_GRAPHICS, &VideoDelayFilter::ReleaseBuffersTask, data, true);
}

void VideoDelayFilter::ReleaseBuffersTask(void *param)
{
	auto *filter = static_cast<VideoDelayFilter *>(param);
	filter->ReleaseRing();
	TRIGGLOW_LOG_INFO(kComponent, "released the RAM ring buffer (filter disabled)");
}

namespace {
// Bundles what GetEffectiveDelaySecondsTask needs -- params outlives the
// graphics-thread hop since GetEffectiveDelaySecondsProc blocks on it
// (obs_queue_task's wait=true).
struct EffectiveDelayQuery {
	VideoDelayFilter *filter;
	calldata_t *params;
};
} // namespace

void VideoDelayFilter::GetEffectiveDelaySecondsProc(void *data, calldata_t *params)
{
	EffectiveDelayQuery query{static_cast<VideoDelayFilter *>(data), params};
	obs_queue_task(OBS_TASK_GRAPHICS, &VideoDelayFilter::GetEffectiveDelaySecondsTask, &query, true);
}

void VideoDelayFilter::GetEffectiveDelaySecondsTask(void *param)
{
	auto *query = static_cast<EffectiveDelayQuery *>(param);
	VideoDelayFilter *filter = query->filter;
	uint32_t fps = std::max<uint32_t>(1, filter->currentFps_);
	uint32_t seconds = filter->ring_.empty() ? 0 : static_cast<uint32_t>((filter->ring_.size() - 1) / fps);
	calldata_set_int(query->params, "seconds", seconds);
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
