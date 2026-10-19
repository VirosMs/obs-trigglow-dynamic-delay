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
#include "i18n.hpp"
#include "logging.hpp"

#ifdef TRIGGLOW_HAVE_FFMPEG
#include "mjpeg-codec.hpp"
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread> // std::thread::hardware_concurrency(), for the sync path's slice threads

extern "C" {
#include <util/platform.h>  // os_gettime_ns()
#include <util/threading.h> // os_set_thread_name()
}

namespace trigglow {

namespace {
constexpr const char *kComponent = "video-delay-filter";
constexpr const char *kFilterId = "trigglow_video_delay_filter";
constexpr const char *kSettingDelaySeconds = "delay_seconds";
constexpr const char *kSettingMinResolutionHeight = "min_resolution_height";
// Hidden (no property, no dock control): false drops the v0.5.1 cap at OBS's
// output resolution, back to v0.5.0 sizing. Pushed from the plugin settings
// file by ObsFrontendBridge::SetBufferFilterOutputCap() on every Enable(), so
// a value edited by hand on the filter itself would just be overwritten.
constexpr const char *kSettingBufferOutputCap = "buffer_output_cap";
// Hidden v0.6.0 kill switch, same mechanism as buffer_output_cap (pushed by
// ObsFrontendBridge::SetBufferFilterCodecWorkers() on every Enable()): false
// runs the MJPEG encode/decode on the graphics thread, no codec worker threads
// (the pipeline's reaper thread still frees memory off it).
constexpr const char *kSettingCodecWorkers = "codec_workers";
// Hidden v0.7.0 storage codec switch, same mechanism (pushed by
// ObsFrontendBridge::SetBufferFilterCodec() on every Enable()): "auto" lets
// the buffer store with a hardware inter-frame codec where one works,
// "mjpeg" keeps it on MJPEG. Read as strictly as the plugin settings file.
constexpr const char *kSettingBufferCodec = "buffer_codec";
// During development (v0.7.0 F2/F3) the filter's own default: MJPEG.
constexpr const char *kDefaultBufferCodec = "mjpeg";
// v0.7.0, written by ObsFrontendBridge::SetBufferFilterStorageClass() on
// every Enable(): "inter" or "intra" (anything else reads as intra).
constexpr const char *kSettingStorageClass = "storage_class";
constexpr const char *kStorageClassInter = "inter";
// The one entry of the filter's properties: a note that the dock manages it
// (no delay or quality fields, see GetProperties()). Never a stored setting.
constexpr const char *kPropertyManagedInfo = "trigglow_managed_info";

// RAM budget for the ring buffer. Even stored as NV12/I420 (1.5 bytes/pixel)
// 1920x1080@60fps is ~178MB PER SECOND raw (1920*1080*1.5 bytes * 60) -- a
// real 30-60s buffer at that resolution is multiple GB before compression,
// not something to allocate blindly on top of whatever else the game/OBS/
// Windows itself is doing.
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
// same day; NV12 (2026-08-26) and MJPEG (v0.3.0) then made the same request
// a fraction of that, so 50% now leaves comfortable headroom.
constexpr uint64_t kFallbackBufferBytes = 2048ULL * 1024 * 1024;
constexpr uint64_t kMinBufferBytes = 1024ULL * 1024 * 1024;             // Floor even on a detected low-RAM machine.
constexpr uint64_t kMaxRecommendedBufferBytes = 24576ULL * 1024 * 1024; // Ceiling even on a very high-RAM machine.
constexpr double kRamFractionForBuffer = 0.50;                          // Don't hog more than ~50% of total system RAM.

// The ring sizing math itself (output cap, quality floor, RAM budget, exact
// fps, A.4's whole-second effective delay) lives in src/buffer-fit.hpp/.cpp --
// pure and free of OBS so it can be unit-tested (tests/buffer_fit_tests.cpp).
// EnsureRingSized() and VideoDelayFilter::EstimateBufferFit() both call
// bufferfit::Compute().

// v0.3.0: on TRIGGLOW_HAVE_FFMPEG platforms, actualFrames is computed against
// an ASSUMED compression ratio (real per-frame size isn't known until after
// encoding) rather than the raw byte count -- Policy A from the design
// discussion: pick a conservative assumed ratio up front rather than
// adapting live, and re-measure against real content before ever raising it.
//
// 2026-09-07: retuned from 3.0 to 5.0 using the first real measurement this
// plugin has ever had -- a live 30s/1080p/60fps session (competitive FPS
// gameplay, HUD included) held a STEADY ~11-15x instantaneous ratio
// throughout, with individual frames sampled up to ~299000 bytes. Since
// v0.6.0 the payloads live in a FrameArena that only commits what the frames
// really weigh, so this ratio only decides how many SECONDS fit the budget
// (and the arena's limit); it no longer pre-allocates raw/5 per slot. 1.0 (no
// assumed compression) on platforms without FFmpeg.
#ifdef TRIGGLOW_HAVE_FFMPEG
constexpr double kAssumedCompressionRatio = 5.0;

// FFmpeg's generic quantizer scale (AV_CODEC_FLAG_QSCALE): 1 = best quality/
// least compression, 31 = worst/most. 5 is a commonly-cited sweet spot for
// MJPEG -- visually close to lossless while still compressing meaningfully.
// Fixed for this first pass, not a dock setting yet (see docs/ROADMAP.md).
constexpr int kMjpegQuality = 5;
#else
constexpr double kAssumedCompressionRatio = 1.0;
#endif

// Sync path (codec_workers=false) only: slice threads for the MJPEG encoder
// running on the graphics thread, as in v0.5.1 -- hardware_concurrency()/4
// (logical CPUs), clamped to [1, CodecPipeline::kMaxSlicesPerContext]: 2 on
// an 8C/16T CPU, 1 on a 4-thread one. They cut the encode's WALL time there
// (1080p: 5.1 ms p50 with 1 slice, ~3 ms with 2, 1.8 ms with the 4 v0.6.0
// allowed; same total CPU, ~3.5% bigger packets). v0.7.0 caps them at 2 for
// RAM (design 6.2): every slice holds its own encoder buffers, and this path
// is the kill switch's rescue mode. With codec workers the degradation policy
// starts at one context and one slice and adds threads only when the
// measured load asks for them.
uint32_t EncodeSliceThreadCount()
{
	const unsigned logicalCpus = std::thread::hardware_concurrency(); // 0 = unknown -> 1 thread.
	return std::clamp<uint32_t>(logicalCpus / 4, 1, CodecPipeline::kMaxSlicesPerContext);
}

// Decode-ahead depth (design B.5, L): frames decoded ahead of what's on
// screen. MJPEG decode p95 is 5.8ms at 1440p (35% of a 60fps frame), so 2
// would do; 3 absorbs a slow frame or a short CPU spike from the game.
constexpr uint32_t kDecodeLookaheadFrames = 3;
// How long before the fill window ends the decoder is pointed at the oldest
// frame (CodecPipeline::PrimeDecode()).
constexpr uint64_t kDecodePrimeWindowNs = 250000000ULL;

// Adaptive degradation (B.8): evaluated this often, over trailing windows of
// these lengths (perf2: p95 over ~2 s, the immediate p50 rule over less).
constexpr uint64_t kPolicyEvalIntervalNs = 250000000ULL;
constexpr uint64_t kPolicyWindowNs = 2000000000ULL;
constexpr uint64_t kPolicyShortWindowNs = 500000000ULL;
// The storage codec policy's latency window (design 4.2: held for a second).
constexpr uint64_t kCodecLatencyWindowNs = 1000000000ULL;

// v0.5.1 telemetry: one "perf check" line per this much rendering (plus, since
// v0.6.0, a "pipeline check" line for the codec workers, queues and arena).
constexpr uint64_t kPerfLogIntervalNs = 10ULL * 1000000000ULL;
// A pause in Render() calls longer than this (filter hidden, OBS stalled)
// closes the current window, so its per-second rates aren't diluted.
constexpr uint64_t kPerfGapResetNs = 1000000000ULL;
// Windows shorter than this (e.g. right after the last periodic line when the
// filter is disabled) aren't worth a line of their own.
constexpr uint64_t kPerfMinWindowNs = 1000000000ULL;

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
// actually costs). The RAM reduction in EnsureRingSized() never goes below
// configuredMinResolutionHeight_ pixels tall; if the full requested delay
// doesn't fit the budget at that floor, it shortens the ACTUAL buffered
// duration instead (same "last resort" trimming the old design used, just
// now the normal case for long delays instead of a rare edge case). Since
// v0.5.1 the buffer also never exceeds OBS's output resolution, and that
// ceiling wins over this floor: a 720p output buffers at 720p even with a
// 1080p minimum (bufferfit::Compute()).
constexpr uint32_t kDefaultMinResolutionHeight = 720;

// Extra headroom on top of configuredDelaySeconds_ worth of audio frames, so
// a write can never catch up to a read mid-copy. Audio is cheap (raw PCM,
// no VRAM concern like video), so this is generously 1 full second per
// channel rather than trying to shave it down.
constexpr uint32_t kAudioRingMarginSeconds = 1;

// GPU-side RGBA<->I420 conversion, compiled once via gs_effect_create() (no
// file on disk needed -- the whole plugin is one DLL). Ordinary full-screen-
// quad pixel shaders, the same mechanism any custom OBS filter (color
// correction, chroma key, etc.) uses for per-pixel work; libobs has no
// compute/dispatch API at all (confirmed 2026-08-25 grepping graphics.h) so
// this is the only public way to do the conversion on the GPU.
//   DrawY    -- samples the RGBA capture (`image`) at full resolution, writes
//               luma to a GS_R8 target.
//   DrawU/V  -- sample the SAME capture into a HALF-resolution GS_R8 target
//               each; `texel` (1/full width, 1/full height) lets them
//               box-average each 2x2 source block instead of point-sampling
//               one corner (no checkerboard chroma). Since v0.6.0 U and V are
//               separate planes (I420), so the readback goes straight into the
//               encoder's layout and nothing is (de)interleaved on the CPU.
//   DrawI420 -- playback: samples `image` (Y), `image_u` and `image_v` (half
//               resolution, upsampled by the bilinear `def_sampler`) and
//               reconstructs RGBA.
// Coefficients are standard BT.601 studio-range (16-235 luma, 16-240
// chroma) -- good enough for a streaming delay buffer, not broadcast-grade
// color science.
constexpr const char *kEffectSource = R"efx(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d image_u;
uniform texture2d image_v;
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

float3 Box2x2(float2 uv)
{
	float2 base = uv - texel * 0.5;
	float3 c0 = image.Sample(def_sampler, base).rgb;
	float3 c1 = image.Sample(def_sampler, base + float2(texel.x, 0.0)).rgb;
	float3 c2 = image.Sample(def_sampler, base + float2(0.0, texel.y)).rgb;
	float3 c3 = image.Sample(def_sampler, base + texel).rgb;
	return (c0 + c1 + c2 + c3) * 0.25;
}

float4 PSDrawY(VertData v_in) : TARGET
{
	float3 rgb = image.Sample(def_sampler, v_in.uv).rgb;
	float y = dot(rgb, float3(0.257, 0.504, 0.098)) + (16.0 / 255.0);
	return float4(y, y, y, 1.0);
}

float4 PSDrawU(VertData v_in) : TARGET
{
	float u = dot(Box2x2(v_in.uv), float3(-0.148, -0.291, 0.439)) + (128.0 / 255.0);
	return float4(u, u, u, 1.0);
}

float4 PSDrawV(VertData v_in) : TARGET
{
	float v = dot(Box2x2(v_in.uv), float3(0.439, -0.368, -0.071)) + (128.0 / 255.0);
	return float4(v, v, v, 1.0);
}

float4 PSDrawI420(VertData v_in) : TARGET
{
	float y = image.Sample(def_sampler, v_in.uv).r - (16.0 / 255.0);
	float u = image_u.Sample(def_sampler, v_in.uv).r - (128.0 / 255.0);
	float v = image_v.Sample(def_sampler, v_in.uv).r - (128.0 / 255.0);
	float3 rgb;
	rgb.r = 1.164 * y + 1.596 * v;
	rgb.g = 1.164 * y - 0.392 * u - 0.813 * v;
	rgb.b = 1.164 * y + 2.017 * u;
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

technique DrawU
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSDrawU(v_in);
	}
}

technique DrawV
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSDrawV(v_in);
	}
}

technique DrawI420
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSDrawI420(v_in);
	}
}
)efx";

constexpr const char *kPlaneTechniques[3] = {"DrawY", "DrawU", "DrawV"};

// Plane p's size for a width x height I420 frame.
uint32_t PlaneWidth(int plane, uint32_t width)
{
	return plane == 0 ? width : width / 2;
}
uint32_t PlaneHeight(int plane, uint32_t height)
{
	return plane == 0 ? height : height / 2;
}

void PipelineLog(PipelineLogLevel level, const char *line)
{
	if (level == PipelineLogLevel::Warning)
		TRIGGLOW_LOG_WARN(kComponent, "%s", line);
	else
		TRIGGLOW_LOG_INFO(kComponent, "%s", line);
}

// The codec-class bit of a decode tag (CodecPipeline::kInterTagBit), for the
// degradation policy's (rung, codec) labels.
uint32_t ClassBit(bool inter)
{
	return inter ? CodecPipeline::kInterTagBit : 0u;
}

uint32_t ClassBitOf(storage::CodecId codec)
{
	return ClassBit(storage::ClassOf(codec) == storage::StorageClass::Inter);
}

#ifdef TRIGGLOW_HAVE_HW_STORAGE
using ProviderRef = std::shared_ptr<hwstorage::Provider>;
#else
using ProviderRef = std::shared_ptr<void>; // No hardware storage codec in this build.
#endif

CodecPipeline::Options MakePipelineOptions(bool threaded, uint32_t fpsNum, uint32_t fpsDen,
					   std::shared_ptr<storage::InterControl> interControl, ProviderRef provider)
{
	CodecPipeline::Options options;
	options.threaded = threaded;
	options.clockNs = &os_gettime_ns; // The same clock as every captureNs.
	options.lookaheadFrames = kDecodeLookaheadFrames;
	options.log = &PipelineLog;
	options.nameThread = [](const char *name) {
		os_set_thread_name(name);
	};
	options.interControl = std::move(interControl);
	// The storage codec (storage-codec.hpp): MJPEG where FFmpeg has it, raw
	// otherwise (the pipeline's default, and its own raw decoder).
#ifdef TRIGGLOW_HAVE_FFMPEG
	if (MjpegAvailable()) {
		const uint32_t fps = std::max<uint32_t>(1, fpsNum / std::max<uint32_t>(1, fpsDen));
		options.encoderFactory = [fps](const storage::EncoderConfig &requested) {
			storage::EncoderConfig config = requested; // Frame size and slice threads.
			config.quality = kMjpegQuality;
			config.fpsNum = fps;
			config.fpsDen = 1;
			return MakeMjpegEncoder(config, &PipelineLog);
		};
#ifdef TRIGGLOW_HAVE_HW_STORAGE
		// v0.7.0: the hardware inter-frame codec, set up on the pipeline's
		// setup thread (only with the codec workers: it needs them).
		if (threaded && provider && provider->AnyHardware()) {
			options.interSetup = [provider](const storage::EncoderConfig &frame) {
				return provider->Setup(frame);
			};
			options.softwareDecoderFactory = [provider](storage::CodecId codec) {
				return provider->MakeSoftwareDecoder(codec);
			};
		}
		options.decoderFactory = [provider](storage::CodecId codec) -> std::unique_ptr<storage::Decoder> {
			if (codec == storage::CodecId::Mjpeg)
				return MakeMjpegDecoder(&PipelineLog);
			if (provider)
				return provider->MakeHardwareDecoder(codec);
			return nullptr;
		};
#else
		options.decoderFactory = [](storage::CodecId codec) -> std::unique_ptr<storage::Decoder> {
			if (codec == storage::CodecId::Mjpeg)
				return MakeMjpegDecoder(&PipelineLog);
			return nullptr;
		};
#endif
	} else {
		TRIGGLOW_LOG_WARN(kComponent, "MJPEG encoder/decoder not found in linked FFmpeg -- the buffer stores "
					      "uncompressed I420 this session");
		options.encoderFactory = &storage::MakeRawEncoder;
	}
#else
	(void)fpsNum;
	(void)fpsDen;
	options.encoderFactory = &storage::MakeRawEncoder;
#endif
#ifndef TRIGGLOW_HAVE_HW_STORAGE
	(void)provider;
#endif
	return options;
}

// The hardware encoder families, in the storage codec's log lines.
const char *BackendLabel(storage::Backend backend)
{
	switch (backend) {
	case storage::Backend::Nvenc:
		return "NVENC";
	case storage::Backend::Amf:
		return "AMF";
	case storage::Backend::Qsv:
		return "Quick Sync";
	case storage::Backend::Cpu:
		return "CPU";
	case storage::Backend::D3d12va:
		return "D3D12";
	}
	return "?";
}

// The storage class a cycle may have (only with FFmpeg: inter-frame codecs need it).
bool StorageInter(bool requested)
{
#ifdef TRIGGLOW_HAVE_FFMPEG
	return requested;
#else
	(void)requested;
	return false;
#endif
}

FrameArena::Config ArenaConfigFor(uint64_t slots, uint32_t width, uint32_t height, uint64_t budgetBytes,
				  double assumedRatio)
{
	const uint64_t rawBytes = bufferfit::Nv12FrameBytes(width, height); // I420: same 1.5 bytes/pixel.
	const auto expectedBytes =
		static_cast<uint64_t>(static_cast<double>(slots) * static_cast<double>(rawBytes) / assumedRatio);
	// Never past the RAM budget (or what the ring was sized for, if that's
	// more): a sustained run of frames that compress far worse than assumed
	// drops frames instead of growing without bound.
#ifdef TRIGGLOW_HAVE_FFMPEG
	return FrameArena::SizedFor(expectedBytes, budgetBytes);
#else
	// No codec (assumed ratio 1): every payload is a raw frame, and blocks
	// hold a whole number of them.
	return FrameArena::SizedFor(expectedBytes, budgetBytes, static_cast<size_t>(rawBytes));
#endif
}

PipelineLoad LoadFromCause(degradation::Cause cause)
{
	switch (cause) {
	case degradation::Cause::Graphics:
		return PipelineLoad::Graphics;
	case degradation::Cause::Encode:
		return PipelineLoad::Encode;
	case degradation::Cause::Decode:
		return PipelineLoad::Decode;
	case degradation::Cause::None:
	case degradation::Cause::Recovered:
		break;
	}
	return PipelineLoad::None;
}
} // namespace

const char *VideoDelayFilter::Id()
{
	return kFilterId;
}

uint64_t VideoDelayFilter::GetBufferBudgetBytes()
{
	return GetBufferBudget();
}

VideoDelayFilter::BufferFitEstimate
VideoDelayFilter::EstimateBufferFit(uint32_t requestedDelaySeconds, uint32_t minResolutionHeight, uint32_t sourceWidth,
				    uint32_t sourceHeight, uint32_t baseWidth, uint32_t baseHeight,
				    uint32_t outputWidth, uint32_t outputHeight, uint32_t fpsNum, uint32_t fpsDen,
				    bool capToOutput, bool storageInter)
{
	bufferfit::Input in;
	in.sourceWidth = sourceWidth;
	in.sourceHeight = sourceHeight;
	in.baseWidth = baseWidth;
	in.baseHeight = baseHeight;
	in.outputWidth = outputWidth;
	in.outputHeight = outputHeight;
	in.fpsNum = fpsNum;
	in.fpsDen = fpsDen;
	in.delaySeconds = requestedDelaySeconds;
	in.minResolutionHeight = minResolutionHeight;
	in.budgetBytes = GetBufferBudget();
	in.capToOutput = capToOutput;
	bufferfit::ApplyStorageClass(in, StorageInter(storageInter), kAssumedCompressionRatio);
	const bufferfit::Result fit = bufferfit::Compute(in);
	if (fit.bufferWidth == 0 || fit.bufferHeight == 0)
		return {};

	BufferFitEstimate estimate;
	estimate.width = fit.bufferWidth;
	estimate.height = fit.bufferHeight;
	estimate.actualSeconds = static_cast<double>(fit.actualFrames) * fpsDen / fpsNum;
	estimate.fitsFullDuration = fit.fitsFullDuration;
	estimate.effectiveSeconds = fit.effectiveDelaySeconds;
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

VideoDelayFilter::VideoDelayFilter(obs_source_t *filterSource) : filterSource_(filterSource)
{
	// Real fps and mix geometry from the start, not the 30fps placeholders:
	// the first Render() can come before the first Tick().
	RefreshVideoInfo();
#ifdef TRIGGLOW_HAVE_HW_STORAGE
	// Created once, here, so the UI thread's procs can reach it without a race
	// (it only enumerates the GPUs; nothing is opened until a setup asks).
	hwstorage::ProviderSettings settings;
	settings.obsAdapterIndex = obsAdapterIndex_;
	settings.fpsNum = fpsNum_;
	settings.fpsDen = fpsDen_;
	settings.log = &PipelineLog;
	settings.clockNs = &os_gettime_ns;
	settings.control = interControl_;
	hwProvider_ = std::make_shared<hwstorage::Provider>(settings);
#endif
}

VideoDelayFilter::~VideoDelayFilter()
{
	// Last partial telemetry window (OBS closing, or the filter removed while
	// the delay was on). Safe here: OBS runs a source's destroy only after
	// detaching it from its parent, so no Render() is in flight. A window
	// already logged by ReleaseBuffersTask (delay disabled) is empty and
	// writes nothing.
	LogPerfWindow("filter destroyed");
	// Stop and join the codec workers first (each closes its own codec
	// contexts on its way out; a join waits for at most one frame's encode or
	// decode). This is OBS's destruction thread, never the graphics thread.
	if (pipeline_)
		pipeline_->Shutdown();
	for (auto &retired : retiredPipelines_)
		retired->Shutdown();
	retiredPipelines_.clear();
	ring_.reset();
	pipeline_.reset();
	// libobs 31.1.1 calls a source's destroy on its destruction task thread
	// (obs_source_destroy_defer, obs-source.c) with NO graphics context, and
	// every gs_*_destroy() is then a silent no-op (gs_valid() fails) that
	// leaks the object -- so enter it explicitly, like OBS's own filters do.
	// Reentrant, so harmless if a context is already current. The ring above
	// is freed outside it: that's plain RAM and needn't hold the graphics lock.
	obs_enter_graphics();
	ReleaseGpuObjects();
	obs_leave_graphics();
}

bool VideoDelayFilter::RefreshVideoInfo()
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.fps_num == 0 || ovi.fps_den == 0)
		return false;
	fpsNum_ = ovi.fps_num;
	fpsDen_ = ovi.fps_den;
	// 1e9 * den / num: exact for fractional rates (59.94 -> 16683333ns).
	// Never 0, it's a divisor in Render().
	frameIntervalNs_ = std::max<uint64_t>(1, bufferfit::FrameIntervalNs(ovi.fps_num, ovi.fps_den));
	mixBaseWidth_ = ovi.base_width;
	mixBaseHeight_ = ovi.base_height;
	mixOutputWidth_ = ovi.output_width;
	mixOutputHeight_ = ovi.output_height;
	obsAdapterIndex_ = ovi.adapter;
	return true;
}

void VideoDelayFilter::InvalidatePlaybackCache()
{
	havePlaybackFrame_ = false;
	lastShownCaptureNs_ = 0;
	lastDueCaptureNs_ = 0;
}

void VideoDelayFilter::StopCycle(bool releaseCodecs)
{
	// Our reference goes to the pipeline's reaper thread along with its own
	// and the queued jobs': none is dropped here, so the ring (and its arena,
	// hundreds of MB) is never freed on the graphics thread, whichever
	// reference turns out to be the last (design B.6: G never frees gigabytes
	// nor waits for a worker; with codec_workers=false too).
	if (pipeline_)
		pipeline_->Detach(releaseCodecs, std::move(ring_));
	ring_.reset();
	for (auto &slot : stagingSlots_)
		slot.pending = false;
	stagingNext_ = 0;
	lastCaptureCell_ = UINT64_MAX;
	InvalidatePlaybackCache();
	effectiveDelaySeconds_ = 0;
	policy_ = degradation::Policy();
	interSteps_ = false;
	captureDivisor_ = 1;
	lastCause_ = PipelineLoad::None;
	ClearPolicyWindows();
	// Disable: the next Enable starts the storage codec over (a hard resize
	// within the same Enable keeps its state, an evacuation included).
	if (releaseCodecs)
		codecCycleFresh_ = true;
}

void VideoDelayFilter::PerfWindow::Reset()
{
	startNs = 0;
	lastSampleNs = 0;
	render.Reset();
	own.Reset();
	readback.Reset();
	upload.Reset();
	calls = 0;
	captures = 0;
	uploads = 0;
	holds = 0;
	holdsLateDecode = 0;
	holdsDropped = 0;
	holdsEncoding = 0;
	holdsCatchUp = 0;
	holdsNoKey = 0;
	reserveFailures = 0;
	backpressureDrops = 0;
	captureFailures = 0;
	obsTotalFramesStart = 0;
	obsLaggedFramesStart = 0;
	obsSkippedFramesStart = 0;
	obsTotalFramesLast = 0;
	obsLaggedFramesLast = 0;
	obsSkippedFramesLast = 0;
}

void VideoDelayFilter::BeginRenderSample(uint64_t renderStartNs)
{
	// Render() stopped for a while (filter hidden, OBS stalled...): close the
	// window at the last sample instead of averaging the pause into its rates.
	if (perf_.startNs != 0 && renderStartNs - perf_.lastSampleNs > kPerfGapResetNs)
		LogPerfWindow("resumed after a pause");
}

void VideoDelayFilter::FinishRenderSample(uint64_t renderStartNs)
{
	const uint64_t endNs = os_gettime_ns();
	// OBS's own counters, snapshotted every call (plain reads) so the deltas
	// cover exactly [startNs, lastSampleNs] even when a pause closes the window.
	video_t *video = obs_get_video();
	perf_.obsTotalFramesLast = obs_get_total_frames();
	perf_.obsLaggedFramesLast = obs_get_lagged_frames();
	perf_.obsSkippedFramesLast = video ? video_output_get_skipped_frames(video) : 0;
	if (perf_.startNs == 0) {
		perf_.startNs = renderStartNs;
		perf_.obsTotalFramesStart = perf_.obsTotalFramesLast;
		perf_.obsLaggedFramesStart = perf_.obsLaggedFramesLast;
		perf_.obsSkippedFramesStart = perf_.obsSkippedFramesLast;
	}
	// The policy judges only what the plugin adds to the graphics thread:
	// everything but rendering the scene itself into the capture texture,
	// which OBS would render anyway (and whose cost, game captures and GPU
	// waits included, the 25/40% thresholds were never calibrated for).
	const uint64_t totalNs = endNs - renderStartNs;
	const uint64_t ownNs = totalNs - std::min(totalNs, sceneRenderNs_);
	perf_.render.Push(totalNs);
	perf_.own.Push(ownNs);
	renderRecent_.Push(endNs, ownNs);
	++perf_.calls;
	perf_.lastSampleNs = endNs;
	if (endNs - perf_.startNs >= kPerfLogIntervalNs)
		LogPerfWindow("periodic");
}

void VideoDelayFilter::LogPerfWindow(const char *reason)
{
	// Always drain the workers' window, so the next line starts clean.
	if (pipeline_)
		pipeline_->TakeWindowStats(workerStats_);
	if (perf_.calls == 0 || perf_.lastSampleNs - perf_.startNs < kPerfMinWindowNs) {
		perf_.Reset();
		return;
	}
	const uint64_t windowNs = perf_.lastSampleNs - perf_.startNs;
	const double seconds = static_cast<double>(windowNs) / 1e9;

	double renderP50, renderP95, renderMax;
	double ownP50, ownP95, ownMax;
	double readbackP50, readbackP95, readbackMax;
	double uploadP50, uploadP95, uploadMax;
	perf_.render.Summarize(renderP50, renderP95, renderMax);
	perf_.own.Summarize(ownP50, ownP95, ownMax);
	perf_.readback.Summarize(readbackP50, readbackP95, readbackMax);
	perf_.upload.Summarize(uploadP50, uploadP95, uploadMax);

	auto delta = [](uint64_t last, uint64_t start) {
		return static_cast<unsigned long long>(last >= start ? last - start : 0);
	};

	char geometry[160];
	if (!lastFitInput_.capToOutput) {
		std::snprintf(geometry, sizeof(geometry),
			      "cap off (buffer_output_cap=false), source %ux%u, output %ux%u%s",
			      lastFitInput_.sourceWidth, lastFitInput_.sourceHeight, lastFitInput_.outputWidth,
			      lastFitInput_.outputHeight, lastFit_.reducedByRam ? ", reduced for RAM" : "");
	} else if (lastFit_.cappedByOutput) {
		std::snprintf(geometry, sizeof(geometry), "capped to OBS output %ux%u, canvas %ux%u%s",
			      lastFitInput_.outputWidth, lastFitInput_.outputHeight, lastFitInput_.baseWidth,
			      lastFitInput_.baseHeight, lastFit_.reducedByRam ? ", reduced for RAM" : "");
	} else {
		std::snprintf(geometry, sizeof(geometry), "not capped, source %ux%u, output %ux%u%s",
			      lastFitInput_.sourceWidth, lastFitInput_.sourceHeight, lastFitInput_.outputWidth,
			      lastFitInput_.outputHeight, lastFit_.reducedByRam ? ", reduced for RAM" : "");
	}

	const double fps = static_cast<double>(fpsNum_) / std::max<uint32_t>(1, fpsDen_);
	TRIGGLOW_LOG_INFO(kComponent,
			  "perf check (%s, %.1fs): buf=%ux%u (%s) fps=%.2f | render p50/p95/max=%.2f/%.2f/%.2fms "
			  "own=%.2f/%.2f/%.2f readback=%.2f/%.2f/%.2f upload=%.2f/%.2f/%.2f | calls=%.1f/s "
			  "captures=%.1f/s uploads=%.1f/s holds=%llu (late decode %llu, dropped %llu, encoding %llu, "
			  "catch-up %llu, no key %llu) | obs frames+%llu lagged+%llu skipped+%llu",
			  reason, seconds, bufferWidth_, bufferHeight_, geometry, fps, renderP50, renderP95, renderMax,
			  ownP50, ownP95, ownMax, readbackP50, readbackP95, readbackMax, uploadP50, uploadP95,
			  uploadMax, static_cast<double>(perf_.calls) / seconds,
			  static_cast<double>(perf_.captures) / seconds, static_cast<double>(perf_.uploads) / seconds,
			  static_cast<unsigned long long>(perf_.holds),
			  static_cast<unsigned long long>(perf_.holdsLateDecode),
			  static_cast<unsigned long long>(perf_.holdsDropped),
			  static_cast<unsigned long long>(perf_.holdsEncoding),
			  static_cast<unsigned long long>(perf_.holdsCatchUp),
			  static_cast<unsigned long long>(perf_.holdsNoKey),
			  delta(perf_.obsTotalFramesLast, perf_.obsTotalFramesStart),
			  delta(perf_.obsLaggedFramesLast, perf_.obsLaggedFramesStart),
			  delta(perf_.obsSkippedFramesLast, perf_.obsSkippedFramesStart));

	// B.9: workers, queues, drops, decode-ahead, degradation rung, arena RAM.
	CodecPipeline::WindowStats &w = workerStats_;
	double e0P50, e0P95, e0Max, e1P50, e1P95, e1Max, dP50, dP95, dMax;
	w.encode[0].frameNs.Summarize(e0P50, e0P95, e0Max);
	w.encode[1].frameNs.Summarize(e1P50, e1P95, e1Max);
	w.decode.frameNs.Summarize(dP50, dP95, dMax);
	auto busy = [&](const PerfSeries &series) {
		return 100.0 * static_cast<double>(series.totalNs) / static_cast<double>(windowNs);
	};
	const double ratio =
		w.encodedBytes > 0 ? static_cast<double>(w.encodedRawBytes) / static_cast<double>(w.encodedBytes) : 0.0;
	FrameArena::Stats arena;
	FrameRing::Counts counts;
	if (ring_) {
		arena = ring_->Arena().GetStats();
		counts = ring_->GetCounts();
	}
	const bool threaded = pipeline_ && pipeline_->Threaded();
	const size_t rung = policy_.Valid() ? policy_.RungIndex() : 0;
	const size_t rungs = policy_.Valid() ? policy_.Ladder().size() : 1;
	constexpr double kMiB = 1024.0 * 1024.0;
	TRIGGLOW_LOG_INFO(
		kComponent,
		"pipeline check (%s): mode=%s rung=%zu/%zu capture=%ux%u@%.2ffps enc=%ux%u L=%u S=%zu | "
		"enc0 p50/p95/max=%.2f/%.2f/%.2fms busy=%.0f%% enc1=%.2f/%.2f/%.2f busy=%.0f%% "
		"dec=%.2f/%.2f/%.2f busy=%.0f%% | drops reserve=%llu backpressure=%llu arena=%llu capture=%llu "
		"rawFallback=%llu encFail=%llu decFail=%llu staleEnc=%llu decJumps=%llu staleDec=%llu | queue enc "
		"max=%u avg=%.2f dec ahead avg=%.2f min=%u | ratio=%.1fx arena=%.0fMB committed (live %.0fMB, peak "
		"%.0fMB, %u blocks of %.0fMB, limit %.0fMB) ring=%zu slots (ready %zu, dropped %zu, in flight %zu, "
		"pinned %zu)",
		reason, threaded ? "workers" : "sync", rung, rungs - 1, captureWidth_, captureHeight_,
		fps / std::max<uint32_t>(1, captureDivisor_), pipeline_ ? pipeline_->ActiveEncodeContexts() : 0,
		pipeline_ ? pipeline_->SlicesPerContext() : 0, pipeline_ ? pipeline_->LookaheadFrames() : 0,
		kStagingSlotCount, e0P50, e0P95, e0Max, busy(w.encode[0].frameNs), e1P50, e1P95, e1Max,
		busy(w.encode[1].frameNs), dP50, dP95, dMax, busy(w.decode.frameNs),
		static_cast<unsigned long long>(perf_.reserveFailures),
		static_cast<unsigned long long>(perf_.backpressureDrops), static_cast<unsigned long long>(w.arenaDrops),
		static_cast<unsigned long long>(perf_.captureFailures), static_cast<unsigned long long>(w.rawFallbacks),
		static_cast<unsigned long long>(w.encodeFailures), static_cast<unsigned long long>(w.decodeFailures),
		static_cast<unsigned long long>(w.staleEncodes), static_cast<unsigned long long>(w.decodeJumps),
		static_cast<unsigned long long>(w.staleDecoded), w.queueDepthMax,
		w.queueDepthSamples ? static_cast<double>(w.queueDepthSum) / static_cast<double>(w.queueDepthSamples)
				    : 0.0,
		w.aheadSamples ? static_cast<double>(w.aheadSum) / static_cast<double>(w.aheadSamples) : 0.0,
		w.aheadSamples ? w.aheadMin : 0, ratio, static_cast<double>(arena.committedBytes) / kMiB,
		static_cast<double>(arena.liveBytes) / kMiB, static_cast<double>(arena.peakCommittedBytes) / kMiB,
		arena.blocks, ring_ ? static_cast<double>(ring_->Arena().GetConfig().blockBytes) / kMiB : 0.0,
		ring_ && ring_->Arena().GetConfig().limitBytes != UINT64_MAX
			? static_cast<double>(ring_->Arena().GetConfig().limitBytes) / kMiB
			: 0.0,
		ring_ ? ring_->SlotCount() : 0, counts.ready, counts.dropped, counts.reserved + counts.encoding,
		counts.pinned);

	const uint64_t drops = perf_.reserveFailures + perf_.backpressureDrops + perf_.captureFailures + w.arenaDrops;
	lastDropsPerMinute_ = static_cast<double>(drops) * 60.0 / seconds;
	lastHoldsPerMinute_ = static_cast<double>(perf_.holds) * 60.0 / seconds;
	PublishHealth(ring_ != nullptr);
	perf_.Reset();
}

void VideoDelayFilter::ReleaseGpuObjects()
{
	ReleaseSizedGpuObjects();
	// effect_ is just compiled shader code -- unlike everything in
	// ReleaseSizedGpuObjects() it isn't sized to any frame, so only the real
	// destructor tears it down.
	if (effect_) {
		gs_effect_destroy(effect_);
		effect_ = nullptr;
	}
}

void VideoDelayFilter::ReleaseSizedGpuObjects()
{
	// The playback textures are destroyed below: nothing left to redraw.
	InvalidatePlaybackCache();

	if (captureTexrender_) {
		gs_texrender_destroy(captureTexrender_);
		captureTexrender_ = nullptr;
	}
	for (auto &texrender : planeTexrenders_) {
		if (texrender) {
			gs_texrender_destroy(texrender);
			texrender = nullptr;
		}
	}
	for (auto &texture : playbackTextures_) {
		if (texture) {
			gs_texture_destroy(texture);
			texture = nullptr;
		}
	}
	playbackWidth_ = 0;
	playbackHeight_ = 0;
	for (auto &slot : stagingSlots_) {
		for (auto &surface : slot.surfaces) {
			if (surface) {
				gs_stagesurface_destroy(surface);
				surface = nullptr;
			}
		}
		slot.width = 0;
		slot.height = 0;
		slot.pending = false;
	}
}

bool VideoDelayFilter::EnsureEffect()
{
	if (effect_)
		return true;
	char *errorString = nullptr;
	effect_ = gs_effect_create(kEffectSource, "trigglow-i420-convert.effect", &errorString);
	if (!effect_ && !loggedEffectError_) {
		TRIGGLOW_LOG_WARN(kComponent, "failed to compile the I420 conversion shader: %s",
				  errorString ? errorString : "(no error string)");
		loggedEffectError_ = true;
	}
	bfree(errorString);
	return effect_ != nullptr;
}

bool VideoDelayFilter::EnsurePlaybackTextures(uint32_t width, uint32_t height)
{
	if (playbackTextures_[0] && playbackWidth_ == width && playbackHeight_ == height)
		return playbackTextures_[1] && playbackTextures_[2];
	// A new size (first frame, or a frame from another degradation rung).
	for (auto &texture : playbackTextures_) {
		if (texture) {
			gs_texture_destroy(texture);
			texture = nullptr;
		}
	}
	for (int p = 0; p < 3; ++p)
		playbackTextures_[p] =
			gs_texture_create(PlaneWidth(p, width), PlaneHeight(p, height), GS_R8, 1, nullptr, GS_DYNAMIC);
	playbackWidth_ = width;
	playbackHeight_ = height;
	// The old frame went with the old textures.
	havePlaybackFrame_ = false;
	return playbackTextures_[0] && playbackTextures_[1] && playbackTextures_[2];
}

void VideoDelayFilter::EnsureRingSized(uint32_t origWidth, uint32_t origHeight)
{
	if (origWidth == 0 || origHeight == 0)
		return;

	// The constructor and every Tick() already read these; this only retries
	// if obs_get_video_info() wasn't available yet back then.
	if (mixBaseWidth_ == 0 || mixOutputWidth_ == 0)
		RefreshVideoInfo();

	const uint32_t delaySeconds = configuredDelaySeconds_.load();
	const uint32_t minResolutionHeight = configuredMinResolutionHeight_.load();
	bufferfit::Input in;
	in.sourceWidth = origWidth;
	in.sourceHeight = origHeight;
	in.baseWidth = mixBaseWidth_;
	in.baseHeight = mixBaseHeight_;
	in.outputWidth = mixOutputWidth_;
	in.outputHeight = mixOutputHeight_;
	in.fpsNum = fpsNum_;
	in.fpsDen = fpsDen_;
	in.delaySeconds = delaySeconds;
	in.minResolutionHeight = minResolutionHeight;
	in.budgetBytes = GetBufferBudget();
	in.capToOutput = configuredOutputCap_.load();
	// v0.7.0 (design 6.4): the cycle's storage class -- the same one
	// PlannedEffectiveDelaySeconds() reads, so audio gets this whole-second delay.
	bufferfit::ApplyStorageClass(in, StorageInter(configuredStorageInter_.load()), kAssumedCompressionRatio);
	const bufferfit::Result fit = bufferfit::Compute(in);
	if (fit.bufferWidth == 0 || fit.bufferHeight == 0)
		return;

	const bool threaded = configuredCodecWorkers_.load();
	const bool resolutionChanged = bufferWidth_ != fit.bufferWidth || bufferHeight_ != fit.bufferHeight;
	if (ring_ && !resolutionChanged && ring_->SlotCount() == fit.actualFrames && pipelineThreaded_ == threaded &&
	    ringFrameIntervalNs_ == frameIntervalNs_ && effectiveDelaySeconds_.load() == fit.effectiveDelaySeconds)
		return; // Already correctly sized.

	const double fps = static_cast<double>(in.fpsNum) / in.fpsDen;
	bool restarted = false;
	if (ring_) {
		const size_t buffered = ring_->GetCounts().buffered;
		if (buffered > 0) {
			// Only possible with real frames in the ring: OBS Settings > Video
			// (canvas, output or fps -- OBS only allows that with no outputs
			// running), the delay/quality changed while the delay was on, or
			// the kill switch flipped. The ring restarts from empty, so the
			// delayed view is blank for `delay` seconds while it refills:
			// kBufferRestartedSignal below has the controller show the loading
			// scene meanwhile.
			restarted = true;
			TRIGGLOW_LOG_WARN(
				kComponent,
				"buffer geometry changed (%ux%u, %zu slots -> %ux%u, %llu slots) with %zu frames "
				"in the ring -- restarting and refilling it",
				bufferWidth_, bufferHeight_, ring_->SlotCount(), fit.bufferWidth, fit.bufferHeight,
				static_cast<unsigned long long>(fit.actualFrames), buffered);
		}
	}
	if (fit.cappedByOutput) {
		TRIGGLOW_LOG_INFO(kComponent, "buffering at %ux%u (capped to OBS output %ux%u, canvas %ux%u)",
				  fit.bufferWidth, fit.bufferHeight, in.outputWidth, in.outputHeight, in.baseWidth,
				  in.baseHeight);
	} else if (!in.capToOutput) {
		TRIGGLOW_LOG_INFO(kComponent,
				  "buffering at %ux%u with the output-resolution cap off (buffer_output_cap=false; "
				  "OBS output %ux%u, canvas %ux%u)",
				  fit.bufferWidth, fit.bufferHeight, in.outputWidth, in.outputHeight, in.baseWidth,
				  in.baseHeight);
	}
	if (fit.reducedByRam) {
		TRIGGLOW_LOG_INFO(kComponent,
				  "buffering at %ux%u (%.0f%% of the source's %ux%u) to fit %us at %.2ffps within "
				  "the %lluMB budget",
				  fit.bufferWidth, fit.bufferHeight, fit.scale * 100.0, origWidth, origHeight,
				  delaySeconds, fps,
				  static_cast<unsigned long long>(GetBufferBudget() / (1024 * 1024)));
	}
	if (!fit.fitsFullDuration) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "even at %ux%u the full %us doesn't fit (the ring spans ~%.1fs): video and audio are "
				  "both delayed by %us",
				  fit.bufferWidth, fit.bufferHeight, delaySeconds,
				  static_cast<double>(fit.actualFrames - 1) / fps, fit.effectiveDelaySeconds);
	}

	// Hard resize: end the old cycle (its ring goes to a worker to be freed).
	if (ring_)
		StopCycle(false);
	// The playback/staging objects are fixed-resolution GPU objects (unlike
	// the texrenders, which auto-resize on gs_texrender_begin) -- they're
	// recreated lazily at the new size.
	if (resolutionChanged)
		ReleaseSizedGpuObjects();

	// The pipeline lives as long as the filter. A new one only when the kill
	// switch flipped (read at OBS startup, so in practice never mid-session):
	// the old one's threads are joined in the destructor, never here.
	if (pipeline_ && pipelineThreaded_ != threaded) {
		pipeline_->Detach(true);
		retiredPipelines_.push_back(std::move(pipeline_));
	}
	if (!pipeline_) {
#ifdef TRIGGLOW_HAVE_HW_STORAGE
		ProviderRef provider = hwProvider_;
#else
		ProviderRef provider;
#endif
		pipeline_ = std::make_unique<CodecPipeline>(
			MakePipelineOptions(threaded, fpsNum_, fpsDen_, interControl_, std::move(provider)));
		pipelineThreaded_ = threaded;
	}
#ifdef TRIGGLOW_HAVE_HW_STORAGE
	if (hwProvider_)
		hwProvider_->SetFrameRate(fpsNum_, fpsDen_);
#endif

	bufferWidth_ = fit.bufferWidth;
	bufferHeight_ = fit.bufferHeight;
	lastFitInput_ = in;
	lastFit_ = fit;
	effectiveDelaySeconds_ = fit.effectiveDelaySeconds;
	ringFrameIntervalNs_ = frameIntervalNs_;

	const FrameArena::Config arena = ArenaConfigFor(fit.actualFrames, fit.bufferWidth, fit.bufferHeight,
							in.budgetBytes, in.compressionRatio);
	ring_ = std::make_shared<FrameRing>(static_cast<size_t>(fit.actualFrames), arena);

	// Degradation ladder (B.8): from the fitted size down to the user's
	// quality floor (in buffer pixels: the same floor bufferfit used), then
	// half the frame rate when the output runs at 48fps or more.
	const bool runsThreaded = pipeline_->Threaded();
	const double floorScale = std::min(fit.capScale, std::min(1.0, static_cast<double>(minResolutionHeight) /
									       static_cast<double>(origHeight)));
	const uint32_t floorHeight = std::min(fit.bufferHeight, bufferfit::EvenDim(origHeight, floorScale));
	const bool allowHalfRate = static_cast<uint64_t>(fpsNum_) >= 48ULL * fpsDen_;
	std::vector<degradation::Parallelism> steps = degradation::BuildParallelismSteps(runsThreaded);
	const size_t initialStep = degradation::InitialParallelism(
		steps, runsThreaded, fit.bufferWidth, fit.bufferHeight, frameIntervalNs_, EncodeSliceThreadCount());
	policy_ = degradation::Policy(degradation::BuildLadder(fit.bufferWidth, fit.bufferHeight, floorHeight,
							       allowHalfRate),
				      std::move(steps), initialStep, runsThreaded);
	ladderTopWidth_ = fit.bufferWidth;
	ladderTopHeight_ = fit.bufferHeight;
	ladderFloorHeight_ = floorHeight;
	ladderHalfRate_ = allowHalfRate;
	ApplyEncoderCeiling(true); // The ceiling OBS's encoders put on it (design 6.3), if any.
	ClearPolicyWindows();
	nextPolicyEvalNs_ = 0;
	ApplyCurrentRung();
	pipeline_->Attach(ring_, fit.bufferWidth, fit.bufferHeight, frameIntervalNs_ * captureDivisor_);
	interSteps_ = false; // A new ladder on MJPEG's steps: the next evaluation follows the codec in use.
	if (codecCycleFresh_) {
		// A new Enable: the storage codec starts over, on hardware if it may
		// (MJPEG bridges while the setup thread opens and checks it).
		codecCycleFresh_ = false;
		bool hardware = configuredBufferCodecAuto_.load() && runsThreaded;
#ifdef TRIGGLOW_HAVE_HW_STORAGE
		hardware = hardware && hwProvider_ && hwProvider_->AnyHardware();
#else
		hardware = false;
#endif
		codecPolicy_ = storagepolicy::Policy(hardware);
		interControl_->yielded = false;
		pipeline_->SetSoftwareDecode(false);
		pipeline_->SetStorageTarget(hardware);
		lastCodecStatus_ = pipeline_->GetCodecStatus();
		skippedEvents_.Clear();
		lastObsSkippedFrames_ = UINT64_MAX;
		lastMixSkipped_ = reportedMixSkipped_.load();
		TRIGGLOW_LOG_INFO(kComponent, "storage codec: %s (buffer_codec=%s%s)",
				  hardware ? "hardware inter-frame where it works, MJPEG bridging meanwhile" : "MJPEG",
				  configuredBufferCodecAuto_.load() ? "auto" : "mjpeg",
				  !runsThreaded ? ", codec_workers=false: MJPEG only"
						: (configuredBufferCodecAuto_.load() && !hardware ? ", no hardware here"
												  : ""));
	}

	stagingNext_ = 0;
	lastCaptureCell_ = UINT64_MAX;
	InvalidatePlaybackCache();

	const degradation::Rung &lowest = policy_.Ladder().back();
	const degradation::Parallelism &par = policy_.CurrentParallelism();
	char storageClass[96];
	if (in.keyframeIntervalFrames > 0)
		std::snprintf(
			storageClass, sizeof(storageClass), "inter (assumed %.0fx, ring +%llu slots of GOP margin)",
			in.compressionRatio,
			static_cast<unsigned long long>(bufferfit::SlackFrames(in.delaySeconds, in.fpsNum, in.fpsDen,
									       in.keyframeIntervalFrames) -
							bufferfit::SlackFrames(in.delaySeconds, in.fpsNum, in.fpsDen)));
	else
		std::snprintf(storageClass, sizeof(storageClass), "intra (assumed %.0fx)", in.compressionRatio);
	TRIGGLOW_LOG_INFO(
		kComponent,
		"delay pipeline: %s, buffer %ux%u, %llu slots, delay %us, storage class %s, %u encode "
		"context%s x %u slice thread%s, decode-ahead %u frames, %zu staging slots, degradation ladder "
		"%zu rungs down to %ux%u%s, arena blocks %zuMB (limit %lluMB)",
		runsThreaded ? "codec workers" : "sync (codec on the graphics thread, codec_workers=false)",
		fit.bufferWidth, fit.bufferHeight, static_cast<unsigned long long>(fit.actualFrames),
		fit.effectiveDelaySeconds, storageClass, par.contexts, par.contexts == 1 ? "" : "s", par.slices,
		par.slices == 1 ? "" : "s", pipeline_->LookaheadFrames(), kStagingSlotCount, policy_.Ladder().size(),
		lowest.width, lowest.height, lowest.captureDivisor > 1 ? " at half frame rate" : "",
		arena.blockBytes / (1024 * 1024), static_cast<unsigned long long>(arena.limitBytes / (1024 * 1024)));
	PublishHealth(true);

	if (restarted) {
		// ObsFrontendBridge hops this to the UI thread for BufferModeController.
		uint8_t stack[128];
		calldata_t data;
		calldata_init_fixed(&data, stack, sizeof(stack));
		calldata_set_ptr(&data, "source", filterSource_);
		signal_handler_signal(obs_source_get_signal_handler(filterSource_), kBufferRestartedSignal, &data);
	}
}

void VideoDelayFilter::ApplyCurrentRung()
{
	if (!policy_.Valid())
		return;
	const degradation::Rung &rung = policy_.CurrentRung();
	captureWidth_ = rung.width;
	captureHeight_ = rung.height;
	if (captureDivisor_ != rung.captureDivisor) {
		captureDivisor_ = rung.captureDivisor;
		lastCaptureCell_ = UINT64_MAX; // Cells are numbered per interval: start counting again.
	}
	if (pipeline_) {
		pipeline_->SetCaptureInterval(frameIntervalNs_ * captureDivisor_);
		const degradation::Parallelism &par = policy_.CurrentParallelism();
		pipeline_->SetEncodeParallelism(par.contexts, par.slices);
	}
}

void VideoDelayFilter::ApplyEncoderCeiling(bool force)
{
	const uint64_t serial = ceilingSerial_.load();
	if (!force && serial == appliedCeilingSerial_)
		return;
	appliedCeilingSerial_ = serial;
	if (!policy_.Valid() || bufferWidth_ == 0 || bufferHeight_ == 0)
		return;
	// Only below the ring's own size, and only for the live scene the size of
	// the canvas (the case the output cap applies to): a soft ceiling.
	uint32_t topWidth = bufferWidth_;
	uint32_t topHeight = bufferHeight_;
	const uint32_t width = ceilingWidth_.load();
	const uint32_t height = ceilingHeight_.load();
	const bool canvasSized = lastFitInput_.sourceWidth == mixBaseWidth_ &&
				 lastFitInput_.sourceHeight == mixBaseHeight_;
	if (configuredOutputCap_.load() && canvasSized && width > 0 && height > 0 && height < bufferHeight_ &&
	    width <= bufferWidth_) {
		topWidth = width;
		topHeight = height;
	}
	if (topWidth == ladderTopWidth_ && topHeight == ladderTopHeight_ && !force)
		return;
	const bool changed = topWidth != ladderTopWidth_ || topHeight != ladderTopHeight_;
	ladderTopWidth_ = topWidth;
	ladderTopHeight_ = topHeight;
	if (!changed)
		return;
	std::vector<degradation::Rung> ladder =
		degradation::BuildLadder(topWidth, topHeight, std::min(ladderFloorHeight_, topHeight), ladderHalfRate_);
	// The same capture size or a smaller one: never up because of the ceiling
	// unless the buffer was at its top already.
	size_t rung = 0;
	if (policy_.RungIndex() > 0) {
		rung = ladder.size() - 1;
		for (size_t i = 0; i < ladder.size(); ++i) {
			if (ladder[i].height <= captureHeight_ && ladder[i].captureDivisor == captureDivisor_) {
				rung = i;
				break;
			}
		}
	}
	policy_.Rebase(std::move(ladder), rung);
	ApplyCurrentRung();
	ClearPolicyWindows();
	++healthChanges_;
	if (topWidth != bufferWidth_ || topHeight != bufferHeight_)
		TRIGGLOW_LOG_INFO(kComponent,
				  "encoder ceiling: capturing at most %ux%u (the largest encoder of the main canvas) "
				  "instead of %ux%u -- the ring, its RAM limit and the delay are unchanged",
				  topWidth, topHeight, bufferWidth_, bufferHeight_);
	else
		TRIGGLOW_LOG_INFO(kComponent, "encoder ceiling lifted: capturing up to %ux%u again", topWidth,
				  topHeight);
}

void VideoDelayFilter::ClearPolicyWindows()
{
	renderRecent_.Clear();
	captureEvents_.Clear();
	dropEvents_.Clear();
	dueEvents_.Clear();
	missEvents_.Clear();
	laggedEvents_.Clear();
	if (pipeline_)
		pipeline_->ResetRecentLoad();
}

void VideoDelayFilter::MaybeEvaluateDegradation(uint64_t nowNs)
{
	if (!policy_.Valid() || !pipeline_ || nowNs < nextPolicyEvalNs_)
		return;
	nextPolicyEvalNs_ = nowNs + kPolicyEvalIntervalNs;
	// v0.7.0: a new ceiling from OBS's encoders, then the storage codec; a
	// rung or steps change resets the degradation policy's windows, so it
	// waits for the next evaluation.
	const uint64_t ladderSerial = healthChanges_;
	ApplyEncoderCeiling(false);
	if (EvaluateCodecPolicy(nowNs) || healthChanges_ != ladderSerial)
		return;

	// OBS's lagged frames since the last evaluation.
	const uint64_t lagged = obs_get_lagged_frames();
	if (lastObsLaggedFrames_ != UINT64_MAX && lagged > lastObsLaggedFrames_)
		laggedEvents_.Push(nowNs, lagged - lastObsLaggedFrames_);
	lastObsLaggedFrames_ = lagged;

	degradation::Sample sample;
	sample.nowNs = nowNs;
	sample.frameIntervalNs = frameIntervalNs_;
	const RollingWindow::Summary render = renderRecent_.Summarize(nowNs, kPolicyWindowNs);
	const RollingWindow::Summary renderShort = renderRecent_.Summarize(nowNs, kPolicyShortWindowNs);
	sample.graphicsSamples = render.samples;
	sample.graphicsP95Ms = render.p95Ms;
	sample.graphicsShortSamples = renderShort.samples;
	sample.graphicsShortP50Ms = renderShort.p50Ms;
	// Every frame is tagged with the rung it was captured at (CaptureFrame()).
	// Encode metrics: this rung's frames only. Decode metrics: the frames the
	// decoder works on now, captured `delay` seconds ago -- right after a rung
	// change still the previous rung's; the policy only acts on them once
	// they're this rung's (degradation::Sample::decodeRung).
	const auto rung = static_cast<uint32_t>(policy_.RungIndex());
	const CodecPipeline::RecentLoad load = pipeline_->GetRecentLoad(nowNs, kPolicyWindowNs, rung);
	// Decode samples are tagged (rung, codec class): the hardware decoder's
	// load says nothing about MJPEG's and the other way round, so only the
	// frames of the codec stored now count (v0.7.0, design 4.3).
	const uint32_t classBit = ClassBit(interSteps_);
	uint32_t decodeTag = load.decodeTag;
	if (decodeTag == RollingWindow::kAnyTag)
		decodeTag = dueEvents_.NewestTag(nowNs);
	if (decodeTag == RollingWindow::kAnyTag)
		decodeTag = rung | classBit;
	sample.encodeSamples = load.encode.samples;
	sample.encodeP95Ms = load.encode.p95Ms;
	sample.decodeSamples = load.decode.samples;
	sample.decodeP95Ms = load.decode.p95Ms;
	sample.decodeRung = (decodeTag & CodecPipeline::kInterTagBit) == classBit
				    ? (decodeTag & ~CodecPipeline::kInterTagBit)
				    : degradation::Sample::kSameRung - 1; // Another codec's frames: never this rung.
	sample.blockUp = codecPolicy_.BlockUp(nowNs);
	sample.captures = captureEvents_.Count(nowNs, kPolicyWindowNs);
	sample.encodeDrops = dropEvents_.Count(nowNs, kPolicyWindowNs);
	sample.dueFrames = dueEvents_.Count(nowNs, kPolicyWindowNs, decodeTag);
	sample.decodeMisses = missEvents_.Count(nowNs, kPolicyWindowNs, decodeTag);
	sample.obsLaggedFrames = static_cast<uint32_t>(
		std::min<uint64_t>(laggedEvents_.Summarize(nowNs, kPolicyWindowNs).sumNs, UINT32_MAX));

	const degradation::Decision decision = policy_.Evaluate(sample);
	if (decision.action != degradation::Action::None)
		ApplyDegradation(decision, sample);
}

void VideoDelayFilter::ApplyDegradation(const degradation::Decision &decision, const degradation::Sample &sample)
{
	ApplyCurrentRung();
	ClearPolicyWindows();
	++healthChanges_;
	const PipelineLoad cause = LoadFromCause(decision.cause);
	if (cause != PipelineLoad::None)
		lastCause_ = cause;
	if (policy_.RungIndex() == 0)
		lastCause_ = PipelineLoad::None;

	const degradation::Rung &rung = policy_.CurrentRung();
	const degradation::Parallelism &par = policy_.CurrentParallelism();
	const double fps = static_cast<double>(fpsNum_) / std::max<uint32_t>(1, fpsDen_);
	const double intervalMs = static_cast<double>(frameIntervalNs_) / 1e6;
	char metrics[320];
	std::snprintf(metrics, sizeof(metrics),
		      "last 2s: graphics thread (own work) p95 %.2fms (%.0f%% of a frame), encode p95 %.2fms, "
		      "decode p95 %.2fms, "
		      "%u of %u captures dropped, %u of %u due frames decoded late, OBS lagged %u",
		      sample.graphicsP95Ms, 100.0 * sample.graphicsP95Ms / intervalMs, sample.encodeP95Ms,
		      sample.decodeP95Ms, sample.encodeDrops, sample.captures, sample.decodeMisses, sample.dueFrames,
		      sample.obsLaggedFrames);
	const bool rungChange = decision.action == degradation::Action::LowerQuality ||
				decision.action == degradation::Action::RaiseQuality;
	// A rung change is what the user can see (a softer or choppier delayed
	// view): WARN, so it stands out in a log attached to a bug report.
	if (rungChange) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "delay buffer %s (%s, load %.0f%%): now %ux%u at %.2f fps capture, rung %zu of %zu "
				  "(%u encode context%s x %u slice thread%s) -- %s. The delay itself is unchanged.",
				  degradation::ActionName(decision.action), degradation::CauseName(decision.cause),
				  decision.load * 100.0, rung.width, rung.height, fps / rung.captureDivisor,
				  policy_.RungIndex(), policy_.Ladder().size() - 1, par.contexts,
				  par.contexts == 1 ? "" : "s", par.slices, par.slices == 1 ? "" : "s", metrics);
	} else {
		TRIGGLOW_LOG_INFO(kComponent,
				  "delay buffer: %s (%s, load %.0f%%): %u encode context%s x %u slice thread%s at "
				  "%ux%u -- %s",
				  degradation::ActionName(decision.action), degradation::CauseName(decision.cause),
				  decision.load * 100.0, par.contexts, par.contexts == 1 ? "" : "s", par.slices,
				  par.slices == 1 ? "" : "s", rung.width, rung.height, metrics);
	}
	PublishHealth(true);
}

storage::EngineShare VideoDelayFilter::CurrentEngineShare(const CodecPipeline::CodecStatus &status)
{
	ObsEncoderSet obs;
	{
		std::lock_guard<std::mutex> lock(obsEncodersMutex_);
		obs = obsEncoders_;
	}
	const std::vector<storage::EncoderFamily> families(obs.families,
							   obs.families + std::min(obs.count, ObsEncoderSet::kMax));
	storage::GpuVendor decodeVendor = storage::GpuVendor::Unknown;
#ifdef TRIGGLOW_HAVE_HW_STORAGE
	if (hwProvider_)
		decodeVendor = hwProvider_->DecodeVendor();
#endif
	return storage::ComputeEngineShare(status.backend, status.hardwareDecode, decodeVendor, obs.known, families);
}

bool VideoDelayFilter::EvaluateCodecPolicy(uint64_t nowNs)
{
	const CodecPipeline::CodecStatus status = pipeline_->GetCodecStatus();
	const bool threaded = pipeline_->Threaded();
	bool changed = false;
	// The parallelism steps follow the codec in use (design 4.3): the
	// hardware encoder is one context; MJPEG starts again from its initial step.
	if (status.interActive != interSteps_) {
		interSteps_ = status.interActive;
		std::vector<degradation::Parallelism> steps = degradation::BuildParallelismSteps(threaded, interSteps_);
		const size_t initial = interSteps_ ? 0
						   : degradation::InitialParallelism(steps, threaded, captureWidth_,
										     captureHeight_,
										     frameIntervalNs_ * captureDivisor_,
										     EncodeSliceThreadCount());
		policy_.SetParallelism(std::move(steps), initial);
		ApplyCurrentRung();
		ClearPolicyWindows();
		changed = true;
	}

	// Frames OBS skipped for encoding lag: the main mix's counter (read here)
	// and those of its encoders' own mixes (reported by the UI).
	video_t *video = obs_get_video();
	const uint64_t mainSkipped = video ? video_output_get_skipped_frames(video) : 0;
	if (lastObsSkippedFrames_ != UINT64_MAX && mainSkipped > lastObsSkippedFrames_)
		skippedEvents_.Push(nowNs, mainSkipped - lastObsSkippedFrames_);
	lastObsSkippedFrames_ = mainSkipped;
	const uint64_t mixSkipped = reportedMixSkipped_.load();
	if (mixSkipped > lastMixSkipped_)
		skippedEvents_.Push(nowNs, mixSkipped - lastMixSkipped_);
	lastMixSkipped_ = mixSkipped;

	const storage::EngineShare share = CurrentEngineShare(status);
	const CodecPipeline::InterLoad inter = pipeline_->GetInterLoad(nowNs, kCodecLatencyWindowNs);
	storagepolicy::Sample s;
	s.nowNs = nowNs;
	s.captureIntervalNs = frameIntervalNs_ * captureDivisor_;
	s.interActive = status.interActive;
	s.setupFailed = status.setupFailures > lastCodecStatus_.setupFailures;
	s.hardErrors = static_cast<uint32_t>((status.hardErrors - lastCodecStatus_.hardErrors) +
					     (status.decoderFailures - lastCodecStatus_.decoderFailures));
	s.yielded = interControl_->yielded.load();
	s.engineShared = share.Any();
	s.obsSkipped = static_cast<uint32_t>(
		std::min<uint64_t>(skippedEvents_.Summarize(nowNs, kPolicyWindowNs).sumNs, UINT32_MAX));
	s.latencySamples = inter.latency.samples;
	s.latencyP95Ms = inter.latency.p95Ms;
	s.inFlightMax = inter.inFlightMax;
	const std::vector<degradation::Rung> &ladder = policy_.Ladder();
	const size_t rung = policy_.RungIndex();
	s.canLowerRung = rung + 1 < ladder.size() && ladder[rung + 1].captureDivisor == ladder[rung].captureDivisor;
	lastCodecStatus_ = status;

	const storagepolicy::Decision d = codecPolicy_.Evaluate(s);
	bool nvidia = false;
#ifdef TRIGGLOW_HAVE_HW_STORAGE
	nvidia = hwProvider_ && hwProvider_->HasVendor(storage::GpuVendor::Nvidia);
#endif
	// What yield_hw_encoder acts on: an NVENC session open, or one a setup may open.
	mayUseNvenc_ = codecPolicy_.Hardware() && nvidia &&
		       (!status.interActive || status.backend == storage::Backend::Nvenc);
	if (d.action == storagepolicy::Action::None)
		return changed;

	++codecChanges_;
	const char *encoder = status.encoder[0] ? status.encoder : "the hardware encoder";
	switch (d.action) {
	case storagepolicy::Action::Adopted:
		TRIGGLOW_LOG_INFO(
			kComponent,
			"storage codec: %s (%s) took over from the MJPEG bridge -- engine shared with OBS: %s", encoder,
			BackendLabel(status.backend),
			!share.known ? "unknown" : (share.encode || share.decode ? "yes" : "no"));
		break;
	case storagepolicy::Action::NoHardware:
		pipeline_->SetStorageTarget(false);
		TRIGGLOW_LOG_INFO(kComponent, "storage codec: staying on MJPEG (%s)",
				  storagepolicy::ReasonText(d.reason));
		break;
	case storagepolicy::Action::LowerRung: {
		const degradation::Decision lowered = policy_.ForceLower(
			degradation::Cause::Encode,
			d.value / std::max(1.0, static_cast<double>(s.captureIntervalNs) / 1e6), nowNs);
		TRIGGLOW_LOG_WARN(
			kComponent,
			"storage codec %s: %s (%s %.1f%s, last 1 s) -- %s, no going back up for 5 minutes. The "
			"delay itself is unchanged.",
			encoder, storagepolicy::ReasonText(d.reason),
			d.reason == storagepolicy::Reason::Latency ? "latency p95" : "frames in flight", d.value,
			d.reason == storagepolicy::Reason::Latency ? " ms" : "",
			lowered.action != degradation::Action::None ? "one rung down" : "no rung left to lower");
		if (lowered.action != degradation::Action::None) {
			ApplyCurrentRung();
			ClearPolicyWindows();
			++healthChanges_;
			lastCause_ = PipelineLoad::Encode;
		}
		changed = true;
		break;
	}
	case storagepolicy::Action::Evacuate: {
		pipeline_->SetStorageTarget(false);
		// The decode shares the engine with OBS (AMD, Intel): what's left of the
		// hardware stretch in the ring is decoded in software.
		const bool softwareDecode = d.reason == storagepolicy::Reason::ObsSkipped && share.decode;
		if (softwareDecode)
			pipeline_->SetSoftwareDecode(true);
#ifdef TRIGGLOW_HAVE_HW_STORAGE
		if (d.reason == storagepolicy::Reason::HardError && hwProvider_ && status.encoder[0])
			hwProvider_->Blacklist(status.encoder, status.lastError);
#endif
		char detail[160] = "";
		if (d.reason == storagepolicy::Reason::ObsSkipped)
			std::snprintf(detail, sizeof(detail), " (%.0f frames skipped in 2 s)", d.value);
		else if (d.reason == storagepolicy::Reason::HardError && status.lastError[0])
			std::snprintf(detail, sizeof(detail), " (%s)", status.lastError);
		TRIGGLOW_LOG_WARN(
			kComponent,
			"delay buffer codec %s -> mjpeg: %s%s; buffer encoder latency p95 %.0f ms%s -- the delay "
			"itself is unchanged, the buffer only takes more RAM until the delay is turned off",
			encoder, storagepolicy::ReasonText(d.reason), detail, s.latencyP95Ms,
			softwareDecode ? "; the hardware frames left in the buffer are decoded in software" : "");
		break;
	}
	case storagepolicy::Action::Yield:
		TRIGGLOW_LOG_INFO(kComponent,
				  "storage codec: %s yielded its engine to an OBS output starting -- MJPEG "
				  "meanwhile",
				  encoder);
		break;
	case storagepolicy::Action::Resume:
		TRIGGLOW_LOG_INFO(kComponent,
				  "storage codec: the OBS output is up, setting the hardware codec up again");
		break;
	case storagepolicy::Action::VetoLifted:
		TRIGGLOW_LOG_INFO(kComponent,
				  "storage codec %s: 5 minutes without an engine warning -- the buffer may "
				  "go back up again",
				  encoder);
		break;
	case storagepolicy::Action::None:
		break;
	}
	PublishHealth(true);
	return changed;
}

void VideoDelayFilter::PublishHealth(bool running)
{
	PipelineHealth health;
	health.running = running && ring_ != nullptr;
	health.codecWorkers = pipeline_ ? pipeline_->Threaded() : configuredCodecWorkers_.load();
	const double fps = static_cast<double>(fpsNum_) / std::max<uint32_t>(1, fpsDen_);
	health.outputFps = fps;
	if (health.running && policy_.Valid()) {
		health.level = static_cast<uint32_t>(policy_.RungIndex());
		health.maxLevel = static_cast<uint32_t>(policy_.Ladder().size() - 1);
		health.width = captureWidth_;
		health.height = captureHeight_;
		health.fullWidth = bufferWidth_;
		health.fullHeight = bufferHeight_;
		health.captureFps = fps / std::max<uint32_t>(1, captureDivisor_);
		health.encodeContexts = policy_.CurrentParallelism().contexts;
		health.encodeSlices = policy_.CurrentParallelism().slices;
		health.cause = health.level > 0 ? lastCause_ : PipelineLoad::None;
		health.droppedPerMinute = lastDropsPerMinute_;
		health.repeatedPerMinute = lastHoldsPerMinute_;
	}
	health.changes = healthChanges_;
	std::lock_guard<std::mutex> lock(healthMutex_);
	health_ = health;
}

void VideoDelayFilter::EnsureAudioRingSized(uint32_t channels, uint32_t samplesPerSec)
{
	if (channels == 0 || samplesPerSec == 0)
		return;

	uint64_t desiredFrames = static_cast<uint64_t>(configuredDelaySeconds_.load()) * samplesPerSec +
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
	uint64_t delayFrames = static_cast<uint64_t>(configuredDelaySeconds_.load()) * sampleRate;
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

void VideoDelayFilter::ApplyDelaySeconds(uint32_t seconds)
{
	if (seconds == configuredDelaySeconds_.load())
		return;
	configuredDelaySeconds_ = seconds;
	if (seconds == 0)
		TRIGGLOW_LOG_INFO(kComponent, "delay set to 0s: the video passes through, no buffer");
	else
		TRIGGLOW_LOG_INFO(kComponent, "delay set to %us", seconds);
}

void VideoDelayFilter::ApplyMinResolutionHeight(uint32_t heightPixels)
{
	if (heightPixels == 0)
		heightPixels = kDefaultMinResolutionHeight;
	if (heightPixels == configuredMinResolutionHeight_.load())
		return;
	configuredMinResolutionHeight_ = heightPixels;
	TRIGGLOW_LOG_INFO(kComponent, "minimum resolution height set to %up", heightPixels);
}

void VideoDelayFilter::SetManagedSetting(const char *key, uint32_t value)
{
	std::lock_guard<std::mutex> lock(managedMutex_);
	if (!managed_)
		TRIGGLOW_LOG_INFO(kComponent,
				  "\"%s\": delay and minimum quality managed by the Trigglow dock from now on",
				  obs_source_get_name(filterSource_));
	managed_ = true;
	warnedForeignSettings_ = false; // One WARN per push of the dock's values, at most.
	if (std::strcmp(key, kSettingDelaySeconds) == 0)
		ApplyDelaySeconds(value);
	else
		ApplyMinResolutionHeight(value);
	// The settings say the same: what the scene collection saves, and what
	// Update() compares with. Under the mutex, so Update() never sees the new
	// value applied but the old one in the settings.
	if (obs_data_t *settings = obs_source_get_settings(filterSource_)) {
		obs_data_set_int(settings, key, value);
		obs_data_release(settings);
	}
}

void VideoDelayFilter::Update(obs_data_t *settings)
{
	{
		// The delay and the quality floor are the dock's once it has pushed
		// them (SetManagedSetting()): a different value in the settings came
		// from somewhere else -- OBS's Defaults button, an older properties
		// dialog, obs-websocket -- and is ignored, so this filter never
		// delays by anything but what the dock shows, and never passes the
		// video through (0 s) while the delay is on. Until then (a filter
		// added by hand, or loaded with a collection before the first Enable)
		// the settings apply, as before.
		std::lock_guard<std::mutex> lock(managedMutex_);
		const auto seconds = static_cast<uint32_t>(obs_data_get_int(settings, kSettingDelaySeconds));
		auto minHeight = static_cast<uint32_t>(obs_data_get_int(settings, kSettingMinResolutionHeight));
		if (minHeight == 0)
			minHeight = kDefaultMinResolutionHeight;
		if (!managed_) {
			ApplyDelaySeconds(seconds);
			ApplyMinResolutionHeight(minHeight);
		} else if ((seconds != configuredDelaySeconds_.load() ||
			    minHeight != configuredMinResolutionHeight_.load()) &&
			   !warnedForeignSettings_) {
			warnedForeignSettings_ = true;
			TRIGGLOW_LOG_WARN(
				kComponent,
				"\"%s\": delay_seconds=%u / min_resolution_height=%u set outside the Trigglow "
				"dock -- ignored, the dock manages this filter (%us, %up)",
				obs_source_get_name(filterSource_), seconds, minHeight, configuredDelaySeconds_.load(),
				configuredMinResolutionHeight_.load());
		}
	}

	// GetDefaults() makes this true unless the plugin pushed false.
	const bool outputCap = obs_data_get_bool(settings, kSettingBufferOutputCap);
	if (outputCap != configuredOutputCap_.load()) {
		configuredOutputCap_ = outputCap;
		TRIGGLOW_LOG_INFO(kComponent, "output-resolution cap %s (buffer_output_cap=%s)",
				  outputCap ? "on" : "off", outputCap ? "true" : "false");
	}

	// Same: true unless the plugin pushed the kill switch. A change while the
	// delay is on rebuilds the cycle (EnsureRingSized()).
	const bool codecWorkers = obs_data_get_bool(settings, kSettingCodecWorkers);
	if (codecWorkers != configuredCodecWorkers_.load()) {
		configuredCodecWorkers_ = codecWorkers;
		TRIGGLOW_LOG_INFO(kComponent, "codec workers %s (codec_workers=%s)", codecWorkers ? "on" : "off",
				  codecWorkers ? "true" : "false");
	}

	// v0.7.0 buffer_codec, as strict as in the plugin settings file: only
	// "auto" or "mjpeg"; anything else keeps auto with a WARN. Takes effect at
	// the next Enable (a codec cycle starts there).
	const storage::BufferCodecSetting bufferCodec = storage::ParseBufferCodec(
		storage::SettingType::String, obs_data_get_string(settings, kSettingBufferCodec));
	if (bufferCodec.warn)
		TRIGGLOW_LOG_WARN(kComponent,
				  "\"%s\" is neither \"auto\" nor \"mjpeg\" -- keeping auto; only the literal string "
				  "\"mjpeg\" forces MJPEG",
				  kSettingBufferCodec);
	const bool bufferCodecAuto = bufferCodec.mode == storage::BufferCodecMode::Auto;
	const bool storageInter =
		std::strcmp(obs_data_get_string(settings, kSettingStorageClass), kStorageClassInter) == 0;
	if (storageInter != configuredStorageInter_.load()) {
		configuredStorageInter_ = storageInter;
		TRIGGLOW_LOG_INFO(kComponent, "storage class %s for the next delay cycle",
				  storageInter ? "inter" : "intra");
	}
	if (bufferCodecAuto != configuredBufferCodecAuto_.load()) {
		configuredBufferCodecAuto_ = bufferCodecAuto;
		TRIGGLOW_LOG_INFO(kComponent, "storage codec %s (buffer_codec=%s)",
				  bufferCodecAuto ? "auto: hardware where it works" : "MJPEG",
				  storage::BufferCodecName(bufferCodec.mode));
	}

	// Always reset, even if seconds didn't change: Enable() calls this via
	// SetBufferFilterDelaySeconds at the start of every cycle, so this gives
	// fresh Render()/FilterAudio() diagnostics each time instead of only
	// the very first time this filter instance was ever created.
	loggedNoTarget_ = false;
	loggedZeroSize_ = false;
	loggedFirstRender_ = false;
	loggedPassthrough_ = false;
}

void VideoDelayFilter::Tick(float /*secondsSinceLastTick*/)
{
	RefreshVideoInfo();
}

void VideoDelayFilter::Render()
{
	const uint64_t renderStartNs = os_gettime_ns();
	BeginRenderSample(renderStartNs);
	sceneRenderNs_ = 0;

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

	// A 0s delay (only on a filter the dock doesn't manage, e.g. one added by
	// hand: the dock's minimum is 1, and Update() ignores a 0 written into a
	// managed filter's settings) means no delay: pass the video straight
	// through, with no ring and no codec. Before v0.6.0 it built a 1-slot
	// ring, encoded every frame into it and never drew anything (black output
	// at full CPU cost).
	if (configuredDelaySeconds_.load() == 0) {
		if (ring_) {
			LogPerfWindow("delay set to 0");
			StopCycle(true);
			ReleaseSizedGpuObjects();
			bufferWidth_ = 0;
			bufferHeight_ = 0;
			PublishHealth(false);
		}
		if (!loggedPassthrough_) {
			TRIGGLOW_LOG_INFO(kComponent, "Render(): delay is 0s -- passing the video through undelayed");
			loggedPassthrough_ = true;
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
	if (!ring_ || !pipeline_) {
		obs_source_skip_video_filter(filterSource_);
		return;
	}
	// Without the shader nothing can be captured or drawn: draw nothing (as
	// before), never the undelayed scene while its audio is delayed.
	if (!EnsureEffect())
		return;

	const uint64_t nowNs = os_gettime_ns();
	MaybeEvaluateDegradation(nowNs);

	// Capture at most once per 1/fps wall-clock cell (2/fps on the half-rate
	// rung). The old design wrote one ring slot per Render() CALL and assumed
	// that was exactly fps per second; measured live (support log,
	// 2026-10-04) it ran 48-90 slots/s at 60fps. frameIntervalNs_ is exact
	// (16.683ms at 59.94fps, v0.5.1).
	const uint64_t captureIntervalNs = frameIntervalNs_ * captureDivisor_;
	const uint64_t captureCell = nowNs / captureIntervalNs;
	if (captureCell != lastCaptureCell_) {
		lastCaptureCell_ = captureCell;
		CaptureFrame(target, width, height, nowNs);
	}

	DrawDelayed(width, height, nowNs);

	FinishRenderSample(renderStartNs);
}

void VideoDelayFilter::CaptureFrame(obs_source_t *target, uint32_t sourceWidth, uint32_t sourceHeight, uint64_t nowNs)
{
	++perf_.captures;
	captureEvents_.Push(nowNs, 0);

	// 1. The capture staged two captures ago has had two frames to land: map
	// it now (never stalls) and hand it to the encoders. Then the slot this
	// capture is staged into (staged three captures ago) -- normally already
	// harvested by the previous capture.
	StagingSlot &harvest = stagingSlots_[(stagingNext_ + 1) % kStagingSlotCount];
	if (harvest.pending)
		HarvestStagingSlot(harvest, nowNs);
	StagingSlot &kick = stagingSlots_[stagingNext_];
	if (kick.pending)
		HarvestStagingSlot(kick, nowNs);
	stagingNext_ = (stagingNext_ + 1) % kStagingSlotCount;

	// 2. This capture's ring slot, stamped now and tagged with the current
	// rung (the policy tells the decoder's load per rung by it). Busy slot (an
	// encoder or the decoder a whole ring behind): the capture is lost, the
	// delayed view repeats a frame, the timestamps stay monotonic.
	FrameRing::Reservation reservation;
	if (!ring_->Reserve(nowNs, reservation, static_cast<uint32_t>(policy_.RungIndex()))) {
		++perf_.reserveFailures;
		dropEvents_.Push(nowNs, 0);
		return;
	}

	// 3. Render the target into the capture texrender at the current rung's
	// size. Its ortho projection still spans the source's real size: that
	// mismatch is what makes the GPU rasterize the scene down to fit.
	const uint32_t width = captureWidth_;
	const uint32_t height = captureHeight_;
	if (!captureTexrender_)
		captureTexrender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (auto &texrender : planeTexrenders_) {
		if (!texrender)
			texrender = gs_texrender_create(GS_R8, GS_ZS_NONE);
	}
	bool ok = captureTexrender_ && planeTexrenders_[0] && planeTexrenders_[1] && planeTexrenders_[2];
	if (ok) {
		gs_texrender_reset(captureTexrender_);
		ok = gs_texrender_begin(captureTexrender_, width, height);
		if (ok) {
			struct vec4 clearColor = {};
			gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
			gs_matrix_push();
			gs_ortho(0.0f, static_cast<float>(sourceWidth), 0.0f, static_cast<float>(sourceHeight), -100.0f,
				 100.0f);
			const uint64_t sceneStartNs = os_gettime_ns();
			obs_source_video_render(target); // The scene's own cost: not ours (FinishRenderSample()).
			sceneRenderNs_ += os_gettime_ns() - sceneStartNs;
			gs_matrix_pop();
			gs_texrender_end(captureTexrender_);
		}
	}
	gs_texture_t *captured = ok ? gs_texrender_get_texture(captureTexrender_) : nullptr;
	ok = captured != nullptr;

	// 4. RGBA -> Y, U, V: one small full-screen-quad pass per plane.
	if (ok) {
		gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect_, "image");
		gs_eparam_t *texelParam = gs_effect_get_param_by_name(effect_, "texel");
		struct vec2 texel;
		vec2_set(&texel, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
		for (int p = 0; p < 3 && ok; ++p) {
			const uint32_t planeWidth = PlaneWidth(p, width);
			const uint32_t planeHeight = PlaneHeight(p, height);
			gs_texrender_reset(planeTexrenders_[p]);
			if (!gs_texrender_begin(planeTexrenders_[p], planeWidth, planeHeight)) {
				ok = false;
				break;
			}
			gs_matrix_push();
			gs_ortho(0.0f, static_cast<float>(planeWidth), 0.0f, static_cast<float>(planeHeight), -100.0f,
				 100.0f);
			gs_effect_set_texture(imageParam, captured);
			gs_effect_set_vec2(texelParam, &texel);
			while (gs_effect_loop(effect_, kPlaneTechniques[p]))
				gs_draw_sprite(captured, 0, planeWidth, planeHeight);
			gs_matrix_pop();
			gs_texrender_end(planeTexrenders_[p]);
		}
	}

	// 5. Async readback: stage the three planes; mapped two captures later.
	if (ok && (kick.width != width || kick.height != height)) {
		// First use, or another rung: staging surfaces are fixed-size.
		for (auto &surface : kick.surfaces) {
			if (surface) {
				gs_stagesurface_destroy(surface);
				surface = nullptr;
			}
		}
		for (int p = 0; p < 3; ++p)
			kick.surfaces[p] = gs_stagesurface_create(PlaneWidth(p, width), PlaneHeight(p, height), GS_R8);
		kick.width = width;
		kick.height = height;
	}
	gs_texture_t *planes[3] = {nullptr, nullptr, nullptr};
	for (int p = 0; p < 3 && ok; ++p) {
		planes[p] = gs_texrender_get_texture(planeTexrenders_[p]);
		ok = planes[p] && kick.surfaces[p];
	}
	if (ok) {
		for (int p = 0; p < 3; ++p)
			gs_stage_texture(kick.surfaces[p], planes[p]);
		kick.pending = true;
		kick.reservation = reservation;
	} else {
		ring_->MarkDropped(reservation.index, reservation.generation);
		++perf_.captureFailures;
	}
}

void VideoDelayFilter::HarvestStagingSlot(StagingSlot &slot, uint64_t nowNs)
{
	slot.pending = false;
	const FrameRing::Reservation reservation = slot.reservation;
	RawFrame *frame = pipeline_->AcquireRawFrame(slot.width, slot.height);
	if (!frame) {
		// Every RawFrame is queued or being encoded: the encoders are behind.
		// Drop the NEWEST capture (design B.4): nothing already done is
		// wasted, the queue never grows, and age-based playback just repeats
		// the previous frame -- the delay doesn't move. Sustained, it's the
		// degradation policy's job (more encode threads, then a lower rung).
		ring_->MarkDropped(reservation.index, reservation.generation);
		pipeline_->NoteBackpressureDrop();
		++perf_.backpressureDrops;
		dropEvents_.Push(nowNs, 0);
		return;
	}
	const uint64_t startNs = os_gettime_ns();
	bool ok = true;
	for (int p = 0; p < 3 && ok; ++p) {
		uint8_t *mapped = nullptr;
		uint32_t linesize = 0;
		if (!slot.surfaces[p] || !gs_stagesurface_map(slot.surfaces[p], &mapped, &linesize)) {
			ok = false;
			break;
		}
		const uint32_t rowBytes = PlaneWidth(p, slot.width);
		const uint32_t rows = PlaneHeight(p, slot.height);
		// bufferfit keeps every plane row a multiple of 4 bytes. libobs-opengl
		// reports linesize = width while glReadPixels() pads each row to
		// GL_PACK_ALIGNMENT (4): with any other width the rows would come out
		// sheared. Never trust less than that padding on OpenGL, and never
		// copy past a row D3D11 says is shorter than the plane.
		assert(rowBytes % 4 == 0);
		uint32_t srcStride = linesize;
		if (gs_get_device_type() == GS_DEVICE_OPENGL)
			srcStride = std::max(srcStride, (rowBytes + 3u) & ~3u);
		if (srcStride < rowBytes) {
			if (!loggedShortLinesize_) {
				TRIGGLOW_LOG_WARN(
					kComponent,
					"readback: plane %d of a %ux%u capture maps with %u-byte rows, fewer than "
					"its %u bytes of width -- dropping such captures",
					p, slot.width, slot.height, linesize, rowBytes);
				loggedShortLinesize_ = true;
			}
			gs_stagesurface_unmap(slot.surfaces[p]);
			ok = false;
			break;
		}
		for (uint32_t row = 0; row < rows; ++row) {
			std::memcpy(frame->plane[p] + static_cast<size_t>(row) * frame->stride[p],
				    mapped + static_cast<size_t>(row) * srcStride, rowBytes);
		}
		gs_stagesurface_unmap(slot.surfaces[p]);
	}
	perf_.readback.Push(os_gettime_ns() - startNs);
	if (!ok) {
		pipeline_->ReleaseRawFrame(frame);
		ring_->MarkDropped(reservation.index, reservation.generation);
		++perf_.captureFailures;
		return;
	}
	// Threaded: queued for an encode worker. Sync path: encoded right here.
	pipeline_->SubmitEncode(ring_, frame, reservation);
}

void VideoDelayFilter::DrawDelayed(uint32_t sourceWidth, uint32_t sourceHeight, uint64_t nowNs)
{
	// Design A.4: the whole-second delay audio uses too. 0 only when the RAM
	// budget left less than a second of ring: then the ring's own span.
	uint64_t delayNs = static_cast<uint64_t>(effectiveDelaySeconds_.load()) * 1000000000ULL;
	if (delayNs == 0) {
		delayNs = std::min<uint64_t>(static_cast<uint64_t>(configuredDelaySeconds_.load()) * 1000000000ULL,
					     static_cast<uint64_t>(ring_->SlotCount() - 1) * frameIntervalNs_);
	}
	if (nowNs <= delayNs)
		return;
	const uint64_t captureIntervalNs = frameIntervalNs_ * captureDivisor_;
	// The playback target, chosen by capture AGE (wall clock), never by
	// counting slots back: Render() can run slower than fps under load, and
	// counting slots would then stretch the delay away from the audio's.
	uint64_t targetNs = nowNs - delayNs;
	uint64_t oldestNs = 0;
	if (!ring_->OldestCaptureNs(oldestNs))
		return;
	if (oldestNs > targetNs) {
		// Buffer still filling (first `delay` seconds after Enable): draw
		// nothing. BufferModeController pairs this with a loading scene on
		// Program for exactly this window. Right as the window ends the
		// oldest frame can be a couple of frames younger than the delay:
		// show it rather than flash nothing.
		if (oldestNs - targetNs > 2 * captureIntervalNs) {
			// The last quarter second of the fill: have the decoder start
			// on the oldest frames now, so the first delayed frame is ready
			// the moment it's due instead of one decode later.
			if (oldestNs - targetNs <= kDecodePrimeWindowNs)
				pipeline_->PrimeDecode(oldestNs);
			return;
		}
		targetNs = oldestNs;
	}

	// The newest decoded frame not younger than the target, if it isn't the
	// one already in the textures (Render() runs once per view of the scene:
	// only the first call for a new frame uploads it).
	DecodedLease lease;
	const bool acquired = pipeline_->AcquireForDisplay(targetNs, lastShownCaptureNs_, lease);
	// Kept past ReleaseDisplay(): due frames the decoder holds on purpose.
	const DecodeWait decodeWait = lease.wait;
	const uint64_t decodeWaitUntilNs = lease.waitUntilNs;
	if (acquired) {
		const DecodedImage &image = lease.image;
		if (EnsurePlaybackTextures(image.width, image.height)) {
			const uint64_t uploadStartNs = os_gettime_ns();
			for (int p = 0; p < 3; ++p)
				gs_texture_set_image(playbackTextures_[p], image.plane[p], image.stride[p], false);
			perf_.upload.Push(os_gettime_ns() - uploadStartNs);
			havePlaybackFrame_ = true;
			++perf_.uploads;
		}
		lastShownCaptureNs_ = lease.captureNs;
		pipeline_->ReleaseDisplay(lease);
	}

	// Holds: once per frame that became due, if it isn't what's on screen.
	// The next frame is still picked by age, so a hold is a repeated frame,
	// never a drift in the delay.
	const FrameRing::DueSlot due = ring_->Due(targetNs);
	if (due.exists && due.captureNs > lastDueCaptureNs_) {
		lastDueCaptureNs_ = due.captureNs;
		// Tagged with the rung that frame was captured at and its codec's class
		// (the one stored now when it isn't Ready).
		const uint32_t tag = due.tag |
				     (due.state == SlotState::Ready ? ClassBitOf(due.codec) : ClassBit(interSteps_));
		dueEvents_.Push(nowNs, 0, tag);
		if (havePlaybackFrame_ && lastShownCaptureNs_ < due.captureNs) {
			++perf_.holds;
			if (due.state == SlotState::Ready && decodeWait != DecodeWait::None &&
			    due.captureNs <= decodeWaitUntilNs) {
				// Held by the decoder catching up from a key, or waiting for
				// the next one: on purpose, never a slow decoder.
				if (decodeWait == DecodeWait::CatchUp)
					++perf_.holdsCatchUp;
				else
					++perf_.holdsNoKey;
			} else if (due.state == SlotState::Ready) {
				++perf_.holdsLateDecode; // Encoded, but the decoder didn't get to it in time.
				missEvents_.Push(nowNs, 0, tag);
			} else if (due.state == SlotState::Dropped) {
				++perf_.holdsDropped;
			} else {
				++perf_.holdsEncoding;
			}
		}
	}

	if (!havePlaybackFrame_)
		return; // Nothing from this cycle uploaded yet: draw nothing.
	gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect_, "image");
	gs_eparam_t *imageUParam = gs_effect_get_param_by_name(effect_, "image_u");
	gs_eparam_t *imageVParam = gs_effect_get_param_by_name(effect_, "image_v");
	gs_effect_set_texture(imageParam, playbackTextures_[0]);
	gs_effect_set_texture(imageUParam, playbackTextures_[1]);
	gs_effect_set_texture(imageVParam, playbackTextures_[2]);
	while (gs_effect_loop(effect_, "DrawI420"))
		gs_draw_sprite(playbackTextures_[0], 0, sourceWidth, sourceHeight);
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
	return Str("Filter.VideoDelay.Name");
}

void *VideoDelayFilter::Create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new VideoDelayFilter(source);
	filter->Update(settings);

	if (signal_handler_t *signals = obs_source_get_signal_handler(source))
		signal_handler_add(signals, "void buffer_restarted(ptr source)");

	proc_handler_t *procHandler = obs_source_get_proc_handler(source);
	if (procHandler) {
		proc_handler_add(procHandler, "void release_buffers()", &VideoDelayFilter::ReleaseBuffersProc, filter);
		proc_handler_add(procHandler, "void get_effective_delay_seconds(out int seconds)",
				 &VideoDelayFilter::GetEffectiveDelaySecondsProc, filter);
		proc_handler_add(procHandler, "void get_pipeline_state(in ptr health)",
				 &VideoDelayFilter::GetPipelineStateProc, filter);
		proc_handler_add(procHandler, "void set_managed_delay_seconds(in int seconds)",
				 &VideoDelayFilter::SetManagedDelaySecondsProc, filter);
		proc_handler_add(procHandler, "void set_managed_min_resolution_height(in int height)",
				 &VideoDelayFilter::SetManagedMinResolutionHeightProc, filter);
		proc_handler_add(procHandler, "void set_encoder_ceiling(in int width, in int height)",
				 &VideoDelayFilter::SetEncoderCeilingProc, filter);
		proc_handler_add(procHandler, "void set_obs_encoders(in ptr encoders)",
				 &VideoDelayFilter::SetObsEncodersProc, filter);
		proc_handler_add(procHandler, "void report_encoder_lag(in int skipped)",
				 &VideoDelayFilter::ReportEncoderLagProc, filter);
		proc_handler_add(procHandler, "void yield_hw_encoder(in bool wait)",
				 &VideoDelayFilter::YieldHwEncoderProc, filter);
		proc_handler_add(procHandler, "void resume_hw_encoder()", &VideoDelayFilter::ResumeHwEncoderProc,
				 filter);
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
	// button-click thread) -- but the ring, the pipeline and the GPU objects
	// belong to the graphics thread, so releasing them right here would race
	// a Render() call still in flight. Queueing onto OBS_TASK_GRAPHICS runs it
	// strictly after any such call (the filter's already disabled by the time
	// this fires, so nothing else gets queued behind it). wait=true: Disable()
	// returns once everything is released from the filter.
	obs_queue_task(OBS_TASK_GRAPHICS, &VideoDelayFilter::ReleaseBuffersTask, data, true);
}

void VideoDelayFilter::ReleaseBuffersTask(void *param)
{
	auto *filter = static_cast<VideoDelayFilter *>(param);
	// Last partial telemetry window of this cycle (needs the ring for its
	// memory figures, so before StopCycle()); the next cycle starts a fresh one.
	filter->LogPerfWindow("filter disabled");
	// Codecs closed (by the threads that own them), the ring and the RawFrame
	// pool handed to the reaper thread, and every GPU object freed while the
	// delay is off (perf2 #4: ~100MB of RAM and ~37MB of VRAM at 1440p);
	// reopening them on the next Enable costs about a millisecond.
	filter->StopCycle(true);
	filter->ReleaseSizedGpuObjects();
	filter->bufferWidth_ = 0;
	filter->bufferHeight_ = 0;
	filter->PublishHealth(false);
	TRIGGLOW_LOG_INFO(kComponent, "released the delay buffer (filter disabled): ring and frame pool handed to "
				      "the reaper thread to free, codecs closed, GPU objects freed");
}

void VideoDelayFilter::GetEffectiveDelaySecondsProc(void *data, calldata_t *params)
{
	// Design A.4 (v0.6.0): bufferfit's whole-second effective delay, which is
	// exactly what Render() delays video by too. BufferModeController gives
	// audio this value when it's below the requested delay, so a RAM-trimmed
	// ring no longer leaves video up to ~1s away from audio (v0.5.1 used the
	// fractional span for video). Computed here from the current settings, so
	// audio gets the new value the moment a setting changes, not a Render()
	// later.
	calldata_set_int(params, "seconds", static_cast<VideoDelayFilter *>(data)->PlannedEffectiveDelaySeconds());
}

uint32_t VideoDelayFilter::PlannedEffectiveDelaySeconds() const
{
	// What EnsureRingSized() reads, read the same way: the target's base size,
	// OBS's video info (Tick() keeps the filter's copy current) and the
	// filter's settings -- obs_source_update() merges new settings into them
	// right away, it only defers calling Update().
	obs_source_t *target = obs_filter_get_target(filterSource_);
	if (!target)
		return 0;
	const uint32_t sourceWidth = obs_source_get_base_width(target);
	const uint32_t sourceHeight = obs_source_get_base_height(target);
	obs_video_info ovi = {};
	if (sourceWidth == 0 || sourceHeight == 0 || !obs_get_video_info(&ovi) || ovi.fps_num == 0 || ovi.fps_den == 0)
		return 0;
	obs_data_t *settings = obs_source_get_settings(filterSource_);
	if (!settings)
		return 0;
	uint32_t delaySeconds = 0;
	uint32_t minResolutionHeight = 0;
	{
		// Managed: the dock's values, which the procs applied right away
		// (whatever the settings say). Otherwise the settings, as Update()
		// will apply them.
		std::lock_guard<std::mutex> lock(managedMutex_);
		if (managed_) {
			delaySeconds = configuredDelaySeconds_.load();
			minResolutionHeight = configuredMinResolutionHeight_.load();
		} else {
			delaySeconds = static_cast<uint32_t>(obs_data_get_int(settings, kSettingDelaySeconds));
			minResolutionHeight =
				static_cast<uint32_t>(obs_data_get_int(settings, kSettingMinResolutionHeight));
		}
	}
	if (minResolutionHeight == 0)
		minResolutionHeight = kDefaultMinResolutionHeight;
	const bool capToOutput = obs_data_get_bool(settings, kSettingBufferOutputCap);
	// The same storage class EnsureRingSized() sizes with (invariant I9).
	const bool storageInter =
		std::strcmp(obs_data_get_string(settings, kSettingStorageClass), kStorageClassInter) == 0;
	obs_data_release(settings);
	if (delaySeconds == 0)
		return 0;
	return EstimateBufferFit(delaySeconds, minResolutionHeight, sourceWidth, sourceHeight, ovi.base_width,
				 ovi.base_height, ovi.output_width, ovi.output_height, ovi.fps_num, ovi.fps_den,
				 capToOutput, storageInter)
		.effectiveSeconds;
}

void VideoDelayFilter::GetPipelineStateProc(void *data, calldata_t *params)
{
	auto *filter = static_cast<VideoDelayFilter *>(data);
	void *out = nullptr;
	if (!calldata_get_ptr(params, "health", &out) || !out)
		return;
	std::lock_guard<std::mutex> lock(filter->healthMutex_);
	*static_cast<PipelineHealth *>(out) = filter->health_;
}

void VideoDelayFilter::SetManagedDelaySecondsProc(void *data, calldata_t *params)
{
	const long long seconds = calldata_int(params, "seconds");
	static_cast<VideoDelayFilter *>(data)->SetManagedSetting(
		kSettingDelaySeconds, static_cast<uint32_t>(std::clamp<long long>(seconds, 0, UINT32_MAX)));
}

void VideoDelayFilter::SetManagedMinResolutionHeightProc(void *data, calldata_t *params)
{
	const long long height = calldata_int(params, "height");
	static_cast<VideoDelayFilter *>(data)->SetManagedSetting(
		kSettingMinResolutionHeight, static_cast<uint32_t>(std::clamp<long long>(height, 0, UINT32_MAX)));
}

void VideoDelayFilter::SetEncoderCeilingProc(void *data, calldata_t *params)
{
	auto *filter = static_cast<VideoDelayFilter *>(data);
	long long width = calldata_int(params, "width");
	long long height = calldata_int(params, "height");
	obs_video_info ovi = {};
	// Never above the canvas, never odd: anything else is no ceiling.
	if (!obs_get_video_info(&ovi) || width <= 0 || height <= 0 || width > static_cast<long long>(ovi.base_width) ||
	    height > static_cast<long long>(ovi.base_height) || width % 8 != 0 || height % 2 != 0) {
		width = 0;
		height = 0;
	}
	if (static_cast<uint32_t>(width) == filter->ceilingWidth_.load() &&
	    static_cast<uint32_t>(height) == filter->ceilingHeight_.load())
		return;
	filter->ceilingWidth_ = static_cast<uint32_t>(width);
	filter->ceilingHeight_ = static_cast<uint32_t>(height);
	filter->ceilingSerial_.fetch_add(1);
}

void VideoDelayFilter::SetObsEncodersProc(void *data, calldata_t *params)
{
	auto *filter = static_cast<VideoDelayFilter *>(data);
	void *in = nullptr;
	if (!calldata_get_ptr(params, "encoders", &in) || !in)
		return;
	ObsEncoderSet set = *static_cast<const ObsEncoderSet *>(in);
	set.count = std::min(set.count, ObsEncoderSet::kMax);
	{
		std::lock_guard<std::mutex> lock(filter->obsEncodersMutex_);
		filter->obsEncoders_ = set;
	}
#ifdef TRIGGLOW_HAVE_HW_STORAGE
	if (filter->hwProvider_)
		filter->hwProvider_->SetObsEncoders(
			set.known, std::vector<storage::EncoderFamily>(set.families, set.families + set.count));
#endif
}

void VideoDelayFilter::ReportEncoderLagProc(void *data, calldata_t *params)
{
	const long long skipped = calldata_int(params, "skipped");
	if (skipped > 0)
		static_cast<VideoDelayFilter *>(data)->reportedMixSkipped_.fetch_add(static_cast<uint64_t>(skipped));
}

void VideoDelayFilter::YieldHwEncoderProc(void *data, calldata_t *params)
{
	auto *filter = static_cast<VideoDelayFilter *>(data);
	// Only NVENC has a session limit worth giving way for (design 4.5).
	if (!filter->mayUseNvenc_.load() || filter->interControl_->yielded.load())
		return;
	const bool wait = calldata_bool(params, "wait");
	filter->interControl_->yielded = true;
	filter->interControl_->Wake();
	if (!wait) {
		TRIGGLOW_LOG_INFO(kComponent, "storage codec: giving the NVENC engine to an OBS output starting");
		return;
	}
	// Near the session limit: give the encode worker up to 100 ms to close it.
	const uint64_t startNs = os_gettime_ns();
	while (filter->interControl_->openEncoders.load() > 0 && os_gettime_ns() - startNs < 100000000ULL)
		os_sleep_ms(5);
	const int open = filter->interControl_->openEncoders.load();
	TRIGGLOW_LOG_INFO(kComponent,
			  "storage codec: gave the NVENC session to an OBS output starting (%s after %.0f ms)",
			  open == 0 ? "closed" : "still closing", static_cast<double>(os_gettime_ns() - startNs) / 1e6);
}

void VideoDelayFilter::ResumeHwEncoderProc(void *data, calldata_t * /*params*/)
{
	auto *filter = static_cast<VideoDelayFilter *>(data);
	if (!filter->interControl_->yielded.load())
		return;
	filter->interControl_->yielded = false;
	filter->interControl_->Wake();
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
	// No delay or quality fields: the dock manages this filter. Editing them
	// here (0-60, while the dock allows 1-60) let the filter delay by another
	// value than the dock showed and audio used, or pass the video through
	// undelayed while the dock said the delay was on. Update() ignores such
	// values anyway once the dock has pushed its own (see SetManagedSetting()).
	// buffer_output_cap and codec_workers are deliberately not listed either
	// (see kSettingBufferOutputCap and kSettingCodecWorkers).
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, kPropertyManagedInfo, Str("Filter.VideoDelay.Managed"), OBS_TEXT_INFO);
	return props;
}

void VideoDelayFilter::GetDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kSettingDelaySeconds, 0);
	obs_data_set_default_int(settings, kSettingMinResolutionHeight, kDefaultMinResolutionHeight);
	obs_data_set_default_bool(settings, kSettingBufferOutputCap, true);
	obs_data_set_default_bool(settings, kSettingCodecWorkers, true);
	obs_data_set_default_string(settings, kSettingBufferCodec, kDefaultBufferCodec);
	obs_data_set_default_string(settings, kSettingStorageClass, "intra");
}

} // namespace trigglow
