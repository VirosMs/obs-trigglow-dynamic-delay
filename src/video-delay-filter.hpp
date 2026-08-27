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

#ifdef TRIGGLOW_HAVE_FFMPEG
// v0.3.0 real compression (docs/ROADMAP.md) -- only defined on platforms
// CMakeLists.txt actually links FFmpeg for (Windows/Linux; not macOS yet,
// see that file). Every use below is guarded the same way, with a raw-NV12
// fallback so this file still compiles and works (just uncompressed) on
// macOS.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#endif

// Delays video by a configured number of seconds, with the streaming OUTPUT
// never touched — no reconnection, ever, at any point. A standard OBS
// VIDEO-ONLY filter, attached automatically by BufferModeController to the
// live scene via ObsFrontendBridge — the user never opens OBS's own Filters
// dialog for it.
//
// Video mechanism (rewritten 2026-08-25 from "one persistent GPU texture per
// buffered frame" to a hybrid RAM+fixed-GPU-object design, after live
// feedback asked whether VRAM was really required and a real "Device
// Remove/Reset" game crash raised the cost of holding dozens of large GPU
// textures alive at once): the ring buffer itself lives in ordinary system
// RAM (Slot::pixels below, one plain byte vector per buffered frame), stored
// as NV12 rather than RGBA since 2026-08-26 (see Slot's comment -- ~2.7x
// less RAM for the same buffered duration/quality). There are only ever a
// small FIXED number of actual GPU objects, regardless of how many seconds
// are buffered:
//   1. captureTexrender_ -- one reusable RGBA render target the live frame
//      is rendered into each tick (same one every frame, just reset/reused).
//   2. nv12Effect_ + yTexrender_/uvTexrender_ -- a tiny custom shader
//      (see kNv12EffectSource in the .cpp) that converts captureTexrender_'s
//      RGBA into Y/UV planes via two full-screen-quad passes -- this is the
//      GPU-side half of the NV12 conversion.
//   3. stagingSlots_[kStagingSlotCount] -- a small (2) rotating pool of
//      Y+UV gs_stagesurf_t pairs used for ASYNC GPU->CPU readback
//      (gs_stage_texture + a deferred gs_stagesurface_map one frame later,
//      so the CPU never stalls waiting on the copy the way mapping the SAME
//      frame's staging surface immediately would).
//   4. yPlaybackTexture_/uvPlaybackTexture_ -- reusable upload textures:
//      whichever RAM slot is `delaySeconds` old gets pushed into these via
//      gs_texture_set_image, then nv12Effect_'s "DrawNV12" technique
//      samples both and writes RGBA straight to the output.
// Memory is still the hard constraint, not CPU: even at NV12's 1.5
// bytes/pixel, 1920x1080@60fps is still ~178MB PER SECOND of buffered
// history. EnsureRingSized() picks a buffer budget from the machine's total
// system RAM (src/hardware-info.hpp) and never captures below a
// configurable minimum resolution (quality floor, user-chosen) -- if the
// full requested delay doesn't fit the budget at that floor, the ACTUAL
// buffered duration shortens instead (flipped 2026-08-25 after live feedback
// that always honoring the full duration made long delays look terrible).
//
// v0.3.0 (2026-08-27, TRIGGLOW_HAVE_FFMPEG platforms only -- Windows/Linux
// for now, see CMakeLists.txt): each ring slot additionally gets MJPEG-
// encoded before storage and decoded back on playback (EncodeScratchNv12Into
// / DecodeSlotIntoScratchNv12 below), cutting real RAM use further on top of
// the NV12 win above. Falls back to storing raw NV12 (identical to the
// pre-v0.3.0 behavior) if the encoder isn't open/available or fails on a
// given frame -- never a hard requirement for the buffer to work at all.
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

	// The RAM budget EnsureRingSized() is actually using right now (bytes)
	// -- computed once from the machine's detected total system RAM
	// (src/hardware-info.hpp) or a safe fallback if that's unavailable.
	// Exposed so the dock can show the user what their hardware allows, per
	// BufferModeController's "aconsejar segun el hardware, pero a su
	// eleccion" requirement -- this never restricts
	// delaySeconds/minResolutionHeight, it's informational.
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
		// the current RAM budget.
		bool fitsFullDuration = true;
	};
	static BufferFitEstimate EstimateBufferFit(uint32_t requestedDelaySeconds, uint32_t minResolutionHeight,
						   uint32_t sourceWidth, uint32_t sourceHeight, uint32_t fps);

private:
	// One buffered historical frame, plain RAM -- NOT a GPU object. See this
	// file's header comment: the ring itself lives here, in system memory;
	// the only GPU objects involved (captureTexrender_, stagingSlots_,
	// yPlaybackTexture_/uvPlaybackTexture_ below) are a small FIXED set
	// shared by every slot, not one-per-slot. Stored as NV12 (Y plane, then
	// interleaved U/V --
	// yBytes = bufferWidth_*bufferHeight_, uvBytes = yBytes/2), not RGBA:
	// 1.5 bytes/pixel instead of 4 (2026-08-26, after live feedback that
	// RGBA storage alone was eating ~98% of system RAM for a 30s@1080p
	// buffer) -- ~2.7x less RAM for the same buffered duration/quality, at
	// the cost of two small extra GPU conversion passes per frame (see
	// nv12Effect_ below). Chroma subsampling requires even
	// bufferWidth_/bufferHeight_ -- EnsureRingSized() enforces that.
	//
	// v0.3.0 (2026-08-27): `pixels` is always allocated at the raw NV12
	// worst-case size (a compressed MJPEG packet can never exceed that in
	// practice), but on platforms with TRIGGLOW_HAVE_FFMPEG only
	// `pixels[0..usedBytes)` is meaningful, and it's an MJPEG packet rather
	// than raw NV12 bytes -- `compressed` says which. Both fields are simply
	// unused (pixels always fully raw NV12) on platforms without FFmpeg.
	struct Slot {
		std::vector<uint8_t> pixels;
		size_t usedBytes = 0;
		bool compressed = false;
		bool valid = false; // false until first successfully captured.
	};

	// One in-flight (or just-finished) async GPU->CPU readback -- BOTH
	// planes of one frame, staged and harvested together in lockstep since
	// they always come from the same source frame. See kStagingSlotCount's
	// comment for why there are exactly two of these.
	struct StagingSlot {
		gs_stagesurf_t *ySurface = nullptr;  // GS_R8, full bufferWidth_ x bufferHeight_.
		gs_stagesurf_t *uvSurface = nullptr; // GS_R8G8, half resolution (chroma subsampled).
		bool pending = false;       // true from gs_stage_texture() until the deferred harvest reads it back.
		size_t targetRingIndex = 0; // Which ring_ slot the pending readback belongs to.
	};

	explicit VideoDelayFilter(obs_source_t *filterSource);
	~VideoDelayFilter();

	void Update(obs_data_t *settings);
	void Tick(float secondsSinceLastTick);
	void Render();
	obs_audio_data *FilterAudio(obs_audio_data *audio);
	uint32_t GetWidth() const;
	uint32_t GetHeight() const;

	// Resizes ring_ (RAM-budget-capped) for the given target resolution and
	// current output frame rate. No-op if the ring is already correct for
	// this width/height/frame count. Must be called from within a
	// video_render (or otherwise graphics-context-active) call: a resolution
	// change also tears down and lazily-recreates the fixed GPU object pool
	// (via ReleaseGpuObjects()), since gs_stagesurf_t/gs_texture_t can't be
	// resized in place the way gs_texrender_t can.
	void EnsureRingSized(uint32_t width, uint32_t height);
	void ReleaseRing();            // Frees the CPU-side ring_ (Slot::pixels) only.
	void ReleaseGpuObjects();      // Frees the ENTIRE GPU pool, effect included -- destructor only.
	void ReleaseSizedGpuObjects(); // Frees just the resolution-sized objects -- also used on resize.

	// Resizes audioRing_ for the given channel count/sample rate and the
	// current configuredDelaySeconds_. Plain CPU memory (no graphics
	// context needed, unlike EnsureRingSized). No-op if already correctly
	// sized.
	void EnsureAudioRingSized(uint32_t channels, uint32_t samplesPerSec);

#ifdef TRIGGLOW_HAVE_FFMPEG
	// Opens encoderCtx_/decoderCtx_ (MJPEG, all-intra -- see this file's
	// header comment) at the CURRENT bufferWidth_/bufferHeight_. Called from
	// EnsureRingSized() on resolutionChanged, alongside the GPU pool
	// recreation those same dimensions drive -- codec contexts are just as
	// tied to a fixed frame size as gs_stagesurf_t/gs_texture_t are. No-op
	// if already open at the right size.
	void EnsureCodecContextsOpen();
	void ReleaseCodecContexts();

	// Encodes scratchNv12_ (this tick's freshly-harvested raw NV12 frame)
	// into `dst.pixels`/`usedBytes`/`compressed`. Returns false (dst
	// untouched) if the encoder isn't open or genuinely failed to produce a
	// packet this tick -- callers fall back to storing scratchNv12_ raw in
	// that case, same as platforms without TRIGGLOW_HAVE_FFMPEG do always.
	bool EncodeScratchNv12Into(Slot &dst);

	// Reverse: decodes `src` (an MJPEG packet, src.compressed must be true)
	// back into scratchNv12_ as plain NV12 bytes, ready for the existing
	// gs_texture_set_image() playback path exactly as if it had never been
	// compressed. Returns false (scratchNv12_ untouched) on decode failure.
	bool DecodeSlotIntoScratchNv12(const Slot &src);
#endif

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

	// "release_buffers" proc handler, registered on the filter's own
	// obs_source_t in Create() -- ObsFrontendBridge::SetBufferFilterEnabled
	// calls it when Disable() runs, since OBS bypasses a disabled filter's
	// video_render entirely (Render() otherwise never runs again to shrink/
	// free its RAM ring). ReleaseBuffersProc can be invoked from ANY thread
	// (in practice the Qt dock's button-click thread), but ring_ is only
	// ever touched from the graphics/video thread, so it hops onto that
	// thread via obs_queue_task rather than clearing ring_ directly.
	static void ReleaseBuffersProc(void *data, calldata_t *params);
	static void ReleaseBuffersTask(void *param);

	obs_source_t *filterSource_; // Not owned; valid for this object's lifetime.
	uint32_t configuredDelaySeconds_ = 0;
	// Floor on the ring's capture height, in pixels -- quality wins over
	// duration: EnsureRingSized() never captures shorter than this, and
	// shortens the ACTUAL buffered seconds instead if the full requested
	// delay doesn't fit the RAM budget at this floor. Defaults to 720
	// (kDefaultMinResolutionHeight); user-configurable via the dock.
	uint32_t configuredMinResolutionHeight_ = 720;

	std::vector<Slot> ring_;
	uint32_t bufferWidth_ = 0; // Resolution every ring_ slot and the fixed GPU pool are currently sized for.
	uint32_t bufferHeight_ = 0;
	size_t writeIndex_ = 0;
	size_t bufferedCount_ = 0; // How many ring_ slots hold a real rendered frame so far.
	uint32_t currentFps_ = 30; // Updated from obs_get_video_info() each Tick().

	// The fixed, small GPU object pool -- see this file's header comment.
	// Independent of ring_.size(): a 5s buffer and a 60s buffer both use
	// exactly this same set of objects, just with a bigger RAM ring_ behind
	// them.
	gs_texrender_t *captureTexrender_ = nullptr; // RGBA: obs_source_video_render() renders in here first.
	// nv12Effect_ converts captureTexrender_'s RGBA into Y (yTexrender_,
	// GS_R8) and UV (uvTexrender_, GS_R8G8, half resolution) via two small
	// full-screen-quad shader passes -- see video-delay-filter.cpp's
	// kNv12EffectSource for the actual conversion math. Public libobs
	// exposes no compute/dispatch API at all (checked 2026-08-25) and no
	// built-in RGBA->NV12 conversion a plugin can call, but ordinary pixel
	// shaders via a custom gs_effect_t are exactly how OBS's own filters
	// (color correction, chroma key, etc.) do custom per-pixel work, so
	// that's the mechanism used here too.
	gs_effect_t *nv12Effect_ = nullptr;
	gs_texrender_t *yTexrender_ = nullptr;
	gs_texrender_t *uvTexrender_ = nullptr;
	// Playback: nv12Effect_'s "DrawNV12" technique samples both of these
	// (uploaded fresh from the delayed ring slot's RAM each frame) and
	// writes out RGBA directly -- no separate RGBA reconstruction buffer.
	gs_texture_t *yPlaybackTexture_ = nullptr;
	gs_texture_t *uvPlaybackTexture_ = nullptr;
	// 2, not more: double-buffering is enough headroom for a GPU->CPU copy
	// to finish one frame later without the CPU ever having to stall on
	// gs_stagesurface_map() waiting for it -- see Render()'s capture step.
	static constexpr size_t kStagingSlotCount = 2;
	StagingSlot stagingSlots_[kStagingSlotCount];
	uint64_t captureFrameCounter_ = 0; // Parity counter picking which staging slot kicks off vs. gets harvested.

	// One frame's raw NV12 bytes (tightly packed, same layout as Slot::pixels
	// used to always have) -- the harvest step below writes here every tick
	// BEFORE either encoding it into a ring slot (TRIGGLOW_HAVE_FFMPEG) or
	// copying it straight into one (fallback path). Also reused as the
	// decode-output scratch buffer on playback, since it's never read and
	// written in the same tick.
	std::vector<uint8_t> scratchNv12_;

#ifdef TRIGGLOW_HAVE_FFMPEG
	// v0.3.0 real compression (docs/ROADMAP.md) -- MJPEG, all-intra: every
	// frame is encoded independently with no P/B-frame prediction at all, so
	// there's no GOP/keyframe-seek complexity to reconcile with a ring that
	// resets abruptly (EnsureRingSized()) and reads/writes by plain index
	// arithmetic. Costs compression ratio vs. a real GOP structure, but MJPEG
	// frames are bitstream-independent by construction, matching this
	// architecture's invariants exactly. Sized to bufferWidth_/bufferHeight_
	// like the GPU pool -- opened in EnsureCodecContextsOpen(), torn down in
	// ReleaseCodecContexts() (called from the same two places
	// ReleaseSizedGpuObjects() is: resolutionChanged and the destructor).
	AVCodecContext *encoderCtx_ = nullptr;
	AVCodecContext *decoderCtx_ = nullptr;
	// Reused every tick (av_frame_unref()/av_packet_unref() between calls,
	// never reallocated) -- same "one small fixed pool, not one per frame"
	// principle as the GPU objects above.
	AVFrame *encodeFrame_ = nullptr;
	AVPacket *encodePacket_ = nullptr;
	AVFrame *decodeFrame_ = nullptr;
	AVPacket *decodePacket_ = nullptr;
	// MJPEG's encoder only accepts PLANAR 4:2:0 (Y, then a separate U plane,
	// then a separate V plane) -- NV12 is semi-planar (Y, then interleaved
	// UV), a different memory layout. These hold the de-interleaved U/V
	// planes for one frame, reused every tick like everything else here.
	std::vector<uint8_t> encodeScratchU_;
	std::vector<uint8_t> encodeScratchV_;
#endif

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
#ifdef TRIGGLOW_HAVE_FFMPEG
	// Proves a real frame actually got MJPEG-compressed (not just that the
	// codec opened -- EnsureCodecContextsOpen() succeeding says nothing about
	// whether EncodeScratchNv12Into() then actually succeeds per-frame).
	// Reset in EnsureCodecContextsOpen() alongside opening fresh contexts, so
	// a resolution change logs its own new compression ratio too.
	bool loggedFirstEncode_ = false;
#endif
};

} // namespace trigglow
