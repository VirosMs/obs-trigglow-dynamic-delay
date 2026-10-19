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

#include "buffer-fit.hpp"
#include "codec-pipeline.hpp"
#include "degradation-policy.hpp"
#include "frame-ring.hpp"
#include "perf-stats.hpp"
#include "pipeline-health.hpp"
#include "storage-codec-policy.hpp"
#include "storage-codec-select.hpp"

#ifdef TRIGGLOW_HAVE_HW_STORAGE
#include "hw-storage-codec.hpp"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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
// Video mechanism: the ring buffer lives in ordinary system RAM, never in
// VRAM (2026-08-25, after a real "Device Remove/Reset" game crash raised the
// cost of holding dozens of large GPU textures alive), and only a small FIXED
// set of GPU objects exists however many seconds are buffered:
//   1. captureTexrender_ -- the live frame is rendered into it each capture.
//   2. effect_ + planeTexrenders_ -- a tiny custom shader (kEffectSource in
//      the .cpp) converts that RGBA into three planes, Y (full size), U and V
//      (half width and height), I420, one full-screen-quad pass each. Since
//      v0.6.0 the planes come back separately (I420) instead of NV12, so the
//      CPU never (de)interleaves chroma in either direction (perf2 #5).
//   3. stagingSlots_[kStagingSlotCount] -- a rotating pool of staging
//      surfaces for ASYNC GPU->CPU readback: each capture is mapped two
//      captures after it was staged, so the map never stalls on the GPU.
//   4. playbackTextures_ -- the delayed frame's three planes, uploaded only
//      when the frame changes; the shader's "DrawI420" technique turns them
//      back into RGBA.
// Frames are stored MJPEG-compressed (v0.3.0, TRIGGLOW_HAVE_FFMPEG platforms:
// Windows/Linux; raw I420 elsewhere) in a FrameRing whose payloads live in a
// block FrameArena (v0.6.0: commit/resident memory follows the real packet
// sizes, and freeing it is one call per 32 MiB block).
//
// v0.6.0 (buffer pipeline design part B): encode and decode run on worker
// threads (CodecPipeline, see codec-pipeline.hpp for the thread model). The
// graphics thread only renders, converts, reads back, hands the frame to the
// encoders, and on playback uploads whatever decoded frame is due. The ring is
// read by AGE (capture timestamp vs. now - delay), so a frame that isn't
// ready in time is a repeated frame, never a shift of the delay. Under load
// the buffer degrades in steps that never shorten the delay (degradation-
// policy.hpp): more encode threads, then a lower capture resolution (never
// below the user's quality floor), then half the capture frame rate. The
// hidden "codec_workers" setting (false) is the kill switch back to doing the
// codec work on the graphics thread (same ring, no codec threads; memory is
// still freed on the pipeline's reaper thread).
//
// Memory is the hard constraint, not CPU: EnsureRingSized() picks a buffer
// budget from the machine's total system RAM (src/hardware-info.hpp) and
// never reduces below a configurable minimum resolution for RAM (quality
// floor, user-chosen) -- if the full requested delay doesn't fit the budget
// at that floor, the ACTUAL buffered duration shortens instead. Since v0.5.1
// it also never buffers above OBS's output resolution (src/buffer-fit.hpp).
// Since v0.6.0 a shortened delay is a whole number of seconds that video and
// audio both use (bufferfit::Result::effectiveDelaySeconds, design A.4).
//
// v0.7.0 (RAM design): on Windows the frames can be stored with a hardware
// inter-frame codec instead (HEVC or H.264 on NVENC, AMF or Quick Sync,
// decoded with D3D11VA: hw-storage-codec.hpp), 50-70% less RAM for a better
// final picture, set up off every hot thread while MJPEG bridges. A storage
// codec policy (storage-codec-policy.hpp) watches that it never costs OBS's
// own encoders their engine: early latency warnings lower a rung, OBS
// skipping frames for encoding lag (or any hard error) sends the buffer back
// to MJPEG for the rest of the cycle, in place, without touching the delay;
// an OBS output starting gets the engine first (yield_hw_encoder). The hidden
// "buffer_codec" setting ("mjpeg") keeps it on MJPEG.
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

	// Signal "void buffer_restarted(ptr source)" on the filter's own source,
	// emitted on the graphics thread when a hard resize threw away buffered
	// frames -- a delay or quality change, a reset of OBS's video settings --
	// so the delayed view stays blank until the new ring fills.
	// ObsFrontendBridge forwards it to BufferModeController, which shows the
	// loading scene meanwhile.
	static constexpr const char *kBufferRestartedSignal = "buffer_restarted";

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
	// (sourceWidth, sourceHeight) and OBS's current video settings (canvas
	// base*, output*, exact fpsNum/fpsDen), with the output-resolution cap on
	// or off (capToOutput, the hidden buffer_output_cap setting) -- the same
	// bufferfit::Compute() call, exposed as a pure query so the dock can warn
	// the user BEFORE they press Enable, not just after. Never restricts their
	// choice: this is purely informational, "aconsejar segun el hardware, pero
	// a su eleccion" per the 2026-08-25 live feedback that asked for exactly
	// this.
	struct BufferFitEstimate {
		uint32_t width = 0;
		uint32_t height = 0;
		double actualSeconds = 0.0;
		// False if actualSeconds ends up shorter than requestedDelaySeconds
		// -- the quality floor didn't leave room for the full duration at
		// the current RAM budget.
		bool fitsFullDuration = true;
		// v0.6.0 (design A.4): the delay video and audio will both really use,
		// in whole seconds (the requested delay when it fits).
		uint32_t effectiveSeconds = 0;
	};
	// storageInter (v0.7.0): the cycle's storage class (design 6.4, I9) --
	// the inter class assumes 12x and keeps a GOP of slack.
	static BufferFitEstimate EstimateBufferFit(uint32_t requestedDelaySeconds, uint32_t minResolutionHeight,
						   uint32_t sourceWidth, uint32_t sourceHeight, uint32_t baseWidth,
						   uint32_t baseHeight, uint32_t outputWidth, uint32_t outputHeight,
						   uint32_t fpsNum, uint32_t fpsDen, bool capToOutput,
						   bool storageInter);

	// v0.7.0: what the "set_obs_encoders(in ptr encoders)" proc takes (the UI
	// thread, through ObsFrontendBridge): the families of OBS's active video
	// encoders, for the setup's ranking (NVENC sessions, engines OBS doesn't
	// use first) and the codec policy's engine sharing.
	struct ObsEncoderSet {
		static constexpr uint32_t kMax = 16;
		bool known = false;
		uint32_t count = 0;
		storage::EncoderFamily families[kMax] = {};
	};

private:
	// One in-flight (or just-finished) async GPU->CPU readback: the three
	// planes of one capture, staged and harvested together. Sized per slot,
	// so a capture staged at one degradation rung is still read back right
	// after the next capture switched to another size.
	struct StagingSlot {
		gs_stagesurf_t *surfaces[3] = {nullptr, nullptr, nullptr}; // Y, U, V (GS_R8).
		uint32_t width = 0;                                        // Luma size the surfaces were created for.
		uint32_t height = 0;
		bool pending = false;               // From gs_stage_texture() until harvested.
		FrameRing::Reservation reservation; // The ring slot this capture goes to.
	};

	// G-side telemetry for one "perf check" window (kPerfLogIntervalNs in the
	// .cpp). The encode/decode side comes from CodecPipeline::WindowStats.
	struct PerfWindow {
		uint64_t startNs = 0;           // Start of the first Render() measured (0 = window empty).
		uint64_t lastSampleNs = 0;      // End of the last Render() measured.
		PerfSeries render;              // Whole Render() call (main path), EnsureRingSized() included.
		PerfSeries own;                 // The same minus rendering the scene itself: the plugin's own work.
		PerfSeries readback;            // Staging maps + memcpy into the RawFrame + unmaps (Y, U, V).
		PerfSeries upload;              // The three gs_texture_set_image() calls.
		uint64_t calls = 0;             // Render() calls that reached the main path.
		uint64_t captures = 0;          // 1/fps cells captured (ring reservations attempted).
		uint64_t uploads = 0;           // Delayed frames uploaded.
		uint64_t holds = 0;             // A new frame was due but the previous one was redrawn...
		uint64_t holdsLateDecode = 0;   // ...because it was encoded but not decoded in time,
		uint64_t holdsDropped = 0;      // ...because it was dropped,
		uint64_t holdsEncoding = 0;     // ...because it was still being encoded,
		uint64_t holdsCatchUp = 0;      // ...because the decoder was catching up from a key (v0.7.0),
		uint64_t holdsNoKey = 0;        // ...because it waited for the next key (v0.7.0).
		uint64_t reserveFailures = 0;   // Captures lost: the next ring slot was busy.
		uint64_t backpressureDrops = 0; // Captures lost: no free RawFrame (encoders behind).
		uint64_t captureFailures = 0;   // Captures lost: render/staging failed.
		// OBS's own counters (obs_get_total_frames/obs_get_lagged_frames,
		// video_output_get_skipped_frames) at the window's first and last
		// sample; the line logs the deltas.
		uint64_t obsTotalFramesStart = 0;
		uint64_t obsLaggedFramesStart = 0;
		uint64_t obsSkippedFramesStart = 0;
		uint64_t obsTotalFramesLast = 0;
		uint64_t obsLaggedFramesLast = 0;
		uint64_t obsSkippedFramesLast = 0;
		void Reset();
	};

	explicit VideoDelayFilter(obs_source_t *filterSource);
	~VideoDelayFilter();

	void Update(obs_data_t *settings);
	// Set configuredDelaySeconds_ / configuredMinResolutionHeight_, logging a
	// real change. Under managedMutex_.
	void ApplyDelaySeconds(uint32_t seconds);
	void ApplyMinResolutionHeight(uint32_t heightPixels);
	// The set_managed_* procs: the dock's value for `key` (kSettingDelaySeconds
	// or kSettingMinResolutionHeight), applied right away and written into the
	// filter's settings; from then on Update() ignores any other value there.
	void SetManagedSetting(const char *key, uint32_t value);
	void Tick(float secondsSinceLastTick);
	void Render();
	obs_audio_data *FilterAudio(obs_audio_data *audio);
	uint32_t GetWidth() const;
	uint32_t GetHeight() const;

	// Reads obs_get_video_info() into fpsNum_/fpsDen_/frameIntervalNs_ and the
	// mix geometry. From the constructor (before the filter renders) and from
	// Tick(). False (members untouched) if OBS has no video info yet.
	bool RefreshVideoInfo();

	// Sizes the delay cycle (ring, pipeline, degradation ladder) for the given
	// target resolution, delay, quality floor and frame rate. No-op if the
	// current ring already matches. A real change -- OBS video settings, delay
	// or quality changed, or the kill switch flipped -- is a "hard resize": the
	// old ring goes to the reaper thread to be freed and a new one fills from empty.
	// Graphics thread only (it can destroy resolution-sized GPU objects).
	void EnsureRingSized(uint32_t width, uint32_t height);
	// Ends the current cycle: the pipeline drops queued work and decoded
	// frames and hands the ring (ours included) to its reaper thread, which
	// frees it. Graphics thread only; never waits on a worker, never frees it here.
	void StopCycle(bool releaseCodecs);
	void ReleaseGpuObjects();      // Frees the ENTIRE GPU pool, effect included -- destructor only.
	void ReleaseSizedGpuObjects(); // Frees the resolution-sized objects (resize, Disable).
	// Forgets the frame in the playback textures (what a "hold" redraws).
	void InvalidatePlaybackCache();

	// Capture (one per 1/fps cell): harvest the staging slot staged two
	// captures ago into a RawFrame for the encoders, reserve a ring slot,
	// render + convert the target and stage it.
	void CaptureFrame(obs_source_t *target, uint32_t sourceWidth, uint32_t sourceHeight, uint64_t nowNs);
	void HarvestStagingSlot(StagingSlot &slot, uint64_t nowNs);
	// Playback: upload the newest decoded frame not younger than now - delay
	// (when it changed) and draw it, or redraw the last one.
	void DrawDelayed(uint32_t sourceWidth, uint32_t sourceHeight, uint64_t nowNs);
	bool EnsureEffect();
	bool EnsurePlaybackTextures(uint32_t width, uint32_t height);

	// Adaptive degradation (B.8): every kPolicyEvalIntervalNs, feeds the
	// trailing-window metrics to policy_ and applies its decision.
	void MaybeEvaluateDegradation(uint64_t nowNs);
	void ApplyDegradation(const degradation::Decision &decision, const degradation::Sample &sample);
	// v0.7.0: the storage codec policy's evaluation (from MaybeEvaluateDegradation()):
	// the codec's parallelism steps, OBS's skipped frames, and its decision.
	// True if it changed the rung or the steps (the degradation policy then
	// waits for the next evaluation).
	bool EvaluateCodecPolicy(uint64_t nowNs);
	// Whether the buffer's hardware codec shares an engine with OBS's encoders.
	storage::EngineShare CurrentEngineShare(const CodecPipeline::CodecStatus &status);
	void ApplyCurrentRung();
	void ClearPolicyWindows();
	// Copies the current state into health_ (under healthMutex_).
	void PublishHealth(bool running);

	// Telemetry. BeginRenderSample() closes the window early after a pause in
	// rendering; FinishRenderSample() records one Render() call and logs the
	// window every kPerfLogIntervalNs; LogPerfWindow() writes the "perf check"
	// and "pipeline check" lines and resets the window. Graphics thread only
	// (also the destructor, when nothing renders any more).
	void BeginRenderSample(uint64_t renderStartNs);
	void FinishRenderSample(uint64_t renderStartNs);
	void LogPerfWindow(const char *reason);

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

	// "release_buffers" proc handler, registered on the filter's own
	// obs_source_t in Create() -- ObsFrontendBridge::SetBufferFilterEnabled
	// calls it when Disable() runs, since OBS bypasses a disabled filter's
	// video_render entirely (Render() otherwise never runs again to release
	// anything). ReleaseBuffersProc can be invoked from ANY thread (in
	// practice the Qt dock's button-click thread), but the ring, the pipeline
	// and the GPU objects belong to the graphics thread, so it hops onto that
	// thread via obs_queue_task. Since v0.6.0 the task only DETACHES the ring
	// and the RawFrame pool (the pipeline's reaper thread frees their RAM a
	// moment later), closes the codecs (each worker closes its own) and frees
	// the GPU objects (perf2 #4): wait=true now means "released from the
	// filter", not "RAM already returned".
	static void ReleaseBuffersProc(void *data, calldata_t *params);
	static void ReleaseBuffersTask(void *param);

	// "get_effective_delay_seconds" proc handler -- lets
	// ObsFrontendBridge::GetVideoEffectiveDelaySeconds() find out what this
	// filter is ACTUALLY delaying by right now (whole seconds), which can be
	// less than configuredDelaySeconds_ if the RAM budget didn't fit the full
	// requested duration at the chosen quality floor (EnsureRingSized). Added
	// 2026-08-27 after a real live report of audio lagging noticeably behind
	// video on long/high-quality delays: AudioDelayFilter always buffers the
	// full requested seconds, so BufferModeController needs this to clamp
	// audio's delay down to what video really delivers -- see
	// BufferModeController::SyncAudioDelayToVideoEffective. Since v0.6.0
	// (design A.4) video uses exactly this whole-second value too, so the two
	// match. Since the QA fixes it's PlannedEffectiveDelaySeconds(), computed
	// on the calling thread from the filter's settings as they are right now:
	// before, it hopped to the graphics thread and read what the ring had been
	// sized for, and a change of delay reached it only on the next Render()
	// -- obs_source_update() defers a video filter's update to the next video
	// tick -- so a 30 -> 40 s change left audio at 30 s.
	static void GetEffectiveDelaySecondsProc(void *data, calldata_t *params);
	// The whole-second delay the ring will have once Render() sizes it for the
	// current settings, OBS video settings and target size: the same
	// bufferfit::Compute() call as EnsureRingSized(). 0 without a target, video
	// info or delay. Any thread.
	uint32_t PlannedEffectiveDelaySeconds() const;

	// "get_pipeline_state(in ptr health)" proc handler (v0.6.0, design B.8):
	// copies the current PipelineHealth into *health. Doesn't hop to the
	// graphics thread -- it only reads health_ under healthMutex_ -- so the
	// dock can poll it without ever blocking on a lagging render.
	static void GetPipelineStateProc(void *data, calldata_t *params);

	// "set_managed_delay_seconds(in int seconds)" and
	// "set_managed_min_resolution_height(in int height)" proc handlers: how
	// ObsFrontendBridge pushes the dock's delay and quality floor (every
	// Enable(), and on a change while the delay runs). They make this filter
	// "managed": its delay and floor are the dock's from then on, whatever its
	// settings say -- OBS's Defaults button, an older properties dialog or
	// obs-websocket can no longer make it delay by another value, or pass the
	// video through undelayed (0 s) while the dock says the delay is on.
	// Called on the caller's thread (the UI): the values apply at once, like
	// PlannedEffectiveDelaySeconds() reads them.
	static void SetManagedDelaySecondsProc(void *data, calldata_t *params);
	static void SetManagedMinResolutionHeightProc(void *data, calldata_t *params);

	// v0.7.0 storage codec procs, all on the caller's thread (the UI), O(1):
	//   "set_obs_encoders(in ptr encoders)": an ObsEncoderSet (Enable and
	//     every output start/stop);
	//   "report_encoder_lag(in int skipped)": frames OBS skipped for encoding
	//     lag in the mixes of its encoders other than the main one, since the
	//     last report (the main mix's own count the filter reads on G);
	//   "yield_hw_encoder(in bool wait)": an OBS output is starting -- the
	//     buffer closes its NVENC session (if it may hold one) and, with wait,
	//     the call waits up to 100 ms for it to be gone;
	//   "resume_hw_encoder()": that output started (or never will): the
	//     buffer may set up hardware again.
	static void SetObsEncodersProc(void *data, calldata_t *params);
	static void ReportEncoderLagProc(void *data, calldata_t *params);
	static void YieldHwEncoderProc(void *data, calldata_t *params);
	static void ResumeHwEncoderProc(void *data, calldata_t *params);

	obs_source_t *filterSource_; // Not owned; valid for this object's lifetime.
	// Written by Update() (UI thread), read by Render() (graphics thread).
	std::atomic<uint32_t> configuredDelaySeconds_{0};
	// Floor for the RAM reduction of the ring's capture height, in pixels --
	// quality wins over duration: EnsureRingSized() never reduces below this
	// for RAM (nor does the degradation ladder), and shortens the ACTUAL
	// buffered seconds instead if the full requested delay doesn't fit the
	// RAM budget at this floor. Not a floor above OBS's output resolution,
	// though: since v0.5.1 the buffer never exceeds that (a 720p output
	// buffers at 720p even with a 1080p minimum). Defaults to 720
	// (kDefaultMinResolutionHeight); user-configurable via the dock.
	std::atomic<uint32_t> configuredMinResolutionHeight_{720};
	// Hidden "buffer_output_cap" setting (bufferfit::Input::capToOutput):
	// false = v0.5.0 sizing, no cap at OBS's output resolution. Only ever set
	// from the plugin settings file, through ObsFrontendBridge.
	std::atomic<bool> configuredOutputCap_{true};
	// Hidden "codec_workers" setting (v0.6.0 kill switch): false = encode and
	// decode on the graphics thread (no codec threads), like v0.5.1. Only
	// ever set from the plugin settings file, through ObsFrontendBridge.
	std::atomic<bool> configuredCodecWorkers_{true};
	// Hidden "buffer_codec" setting (v0.7.0): true = "auto" (the hardware
	// codec where it works), false = "mjpeg". Read strictly in Update().
	std::atomic<bool> configuredBufferCodecAuto_{false};
	// "storage_class" (v0.7.0, design 6.4 and invariant I9): what sizes the
	// ring -- "inter" (assumed 12x, a GOP of slack) or "intra" (5x, a quarter
	// second). BufferModeController writes it at every Enable, fixed for the
	// cycle, so EnsureRingSized() and PlannedEffectiveDelaySeconds() read the
	// same value and video and audio get the same whole-second delay.
	std::atomic<bool> configuredStorageInter_{false};
	// Managed by the dock (set_managed_* procs, see SetManagedSetting()):
	// Update() keeps configuredDelaySeconds_ and configuredMinResolutionHeight_
	// instead of applying the settings' values, and logs one WARN per push if
	// they differ. managedMutex_ guards these two flags and keeps a proc's
	// value and its write into the settings together against Update() (graphics
	// thread) and PlannedEffectiveDelaySeconds() (UI).
	mutable std::mutex managedMutex_;
	bool managed_ = false;
	bool warnedForeignSettings_ = false;

	// --- The current delay cycle (graphics thread) ---
	std::shared_ptr<FrameRing> ring_;
	std::unique_ptr<CodecPipeline> pipeline_;
	bool pipelineThreaded_ = false; // What pipeline_ was created as.
	// Pipelines replaced when the kill switch flipped mid-session: their
	// threads are only joined in the destructor (never on the graphics thread).
	std::vector<std::unique_ptr<CodecPipeline>> retiredPipelines_;
	uint32_t bufferWidth_ = 0; // Rung 0: what bufferfit::Compute() chose.
	uint32_t bufferHeight_ = 0;
	uint32_t captureWidth_ = 0; // What's captured right now (the current rung).
	uint32_t captureHeight_ = 0;
	uint32_t captureDivisor_ = 1; // 2 on the half-frame-rate rung.
	// Design A.4: the delay video uses (and audio is told), whole seconds.
	// 0 until the ring is sized. Atomic only so the proc's task could read it
	// from anywhere; it's written on the graphics thread.
	std::atomic<uint32_t> effectiveDelaySeconds_{0};
	uint64_t ringFrameIntervalNs_ = 0; // frameIntervalNs_ the current ring was sized with.
	// Index of the capture cell (now / capture interval) of the last capture.
	// Render() can be called several times per OBS frame (preview,
	// projectors, a second canvas, the keep-alive callback...) or slower than
	// fps under load; capturing at most once per cell keeps the ring at <= fps
	// slots/second.
	uint64_t lastCaptureCell_ = UINT64_MAX;
	// Exact frame rate and frame interval from obs_get_video_info()
	// (RefreshVideoInfo(): constructor, then every Tick(); v0.5.1). The 30fps
	// values below only stand if OBS had no video info at creation.
	uint32_t fpsNum_ = 30;
	uint32_t fpsDen_ = 1;
	uint64_t frameIntervalNs_ = 1000000000ULL / 30;
	// OBS main mix geometry, also from RefreshVideoInfo(): base_* is the
	// canvas, output_* the scaled output. EnsureRingSized() never buffers
	// above the output (bufferfit::OutputCapScale) unless configuredOutputCap_
	// is false. 0 while OBS has no video info.
	uint32_t mixBaseWidth_ = 0;
	uint32_t mixBaseHeight_ = 0;
	uint32_t mixOutputWidth_ = 0;
	uint32_t mixOutputHeight_ = 0;
	// What the current ring was sized from and to, for the perf check line.
	bufferfit::Input lastFitInput_;
	bufferfit::Result lastFit_;
	uint32_t obsAdapterIndex_ = 0; // obs_video_info::adapter (where a hardware decoder runs).

	// --- v0.7.0 storage codec (graphics thread unless noted) ---
	// Shared with the pipeline's encode worker and the UI's yield procs.
	std::shared_ptr<storage::InterControl> interControl_ = std::make_shared<storage::InterControl>();
#ifdef TRIGGLOW_HAVE_HW_STORAGE
	std::shared_ptr<hwstorage::Provider> hwProvider_; // With the pipeline; thread-safe on its own.
#endif
	storagepolicy::Policy codecPolicy_;
	bool codecCycleFresh_ = true; // The next ring starts a codec cycle (the first one, or after Disable).
	bool interSteps_ = false;     // policy_ runs on the hardware encoder's parallelism steps.
	CodecPipeline::CodecStatus lastCodecStatus_;  // Its counters at the previous evaluation.
	RollingWindow skippedEvents_;                 // OBS encoding-lag skipped frames (value = frames).
	uint64_t lastObsSkippedFrames_ = UINT64_MAX;  // The main mix's counter (UINT64_MAX = not read yet).
	std::atomic<uint64_t> reportedMixSkipped_{0}; // report_encoder_lag, summed (UI thread).
	uint64_t lastMixSkipped_ = 0;
	uint64_t codecChanges_ = 0;
	// May hold an NVENC session (or be about to): what yield_hw_encoder acts on. Written on G.
	std::atomic<bool> mayUseNvenc_{false};
	// OBS's active video encoders (set_obs_encoders, UI thread).
	mutable std::mutex obsEncodersMutex_;
	ObsEncoderSet obsEncoders_;

	// --- Adaptive degradation (B.8), graphics thread ---
	degradation::Policy policy_;
	uint64_t nextPolicyEvalNs_ = 0;
	// Trailing windows the policy reads (timestamps from os_gettime_ns).
	RollingWindow renderRecent_;                // Render() own work per call (PerfWindow::own).
	RollingWindow captureEvents_;               // One per capture attempt.
	RollingWindow dropEvents_;                  // One per capture lost (busy slot, backpressure, staging).
	RollingWindow dueEvents_;                   // One per new frame due in playback (tag: its capture rung).
	RollingWindow missEvents_;                  // One per due frame encoded but not decoded in time (same tag).
	RollingWindow laggedEvents_;                // OBS lagged-frame increments (value = frames).
	uint64_t lastObsLaggedFrames_ = UINT64_MAX; // UINT64_MAX = not read yet.
	uint64_t healthChanges_ = 0;
	PipelineLoad lastCause_ = PipelineLoad::None;
	double lastDropsPerMinute_ = 0.0; // From the last telemetry window, for the dock.
	double lastHoldsPerMinute_ = 0.0;

	// --- GPU objects (graphics thread) ---
	gs_texrender_t *captureTexrender_ = nullptr; // RGBA: obs_source_video_render() renders in here first.
	// effect_ converts captureTexrender_'s RGBA into Y, U and V (GS_R8 each;
	// U and V at half width and height) with one full-screen-quad pass per
	// plane -- see kEffectSource in the .cpp. Public libobs exposes no
	// compute/dispatch API and no RGBA->YUV conversion a plugin can call, but
	// ordinary pixel shaders via a custom gs_effect_t are exactly how OBS's own
	// filters do per-pixel work.
	gs_effect_t *effect_ = nullptr;
	gs_texrender_t *planeTexrenders_[3] = {nullptr, nullptr, nullptr};
	// Playback: "DrawI420" samples these three and writes RGBA directly.
	gs_texture_t *playbackTextures_[3] = {nullptr, nullptr, nullptr};
	uint32_t playbackWidth_ = 0; // Luma size of playbackTextures_.
	uint32_t playbackHeight_ = 0;
	// The frame in playbackTextures_: what's redrawn (a "hold") when the frame
	// due can't be shown yet. Only frames of the current cycle: cleared with
	// the ring and with the textures.
	bool havePlaybackFrame_ = false;
	uint64_t lastShownCaptureNs_ = 0;
	uint64_t lastDueCaptureNs_ = 0; // Newest frame due so far (hold accounting).
	// 3 (v0.6.0, was 2): each capture is mapped two captures after it was
	// staged, so even with the GPU saturated by a game gs_stagesurface_map()
	// finds the copy done instead of stalling the graphics thread (design §1.1.7).
	static constexpr size_t kStagingSlotCount = 3;
	StagingSlot stagingSlots_[kStagingSlotCount];
	size_t stagingNext_ = 0; // Slot the next capture is staged into.

	PerfWindow perf_;                        // Graphics thread only.
	uint64_t sceneRenderNs_ = 0;             // This Render() call's obs_source_video_render(target) time.
	CodecPipeline::WindowStats workerStats_; // Scratch for LogPerfWindow().

	// Dock-facing state (design B.8), written on the graphics thread, read by
	// GetPipelineStateProc() from any thread.
	mutable std::mutex healthMutex_;
	PipelineHealth health_;

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
	bool loggedPassthrough_ = false; // Delay 0: passing the video through.
	bool loggedEffectError_ = false;
	bool loggedShortLinesize_ = false; // A staging map with rows shorter than the plane (never expected).
};

} // namespace trigglow
