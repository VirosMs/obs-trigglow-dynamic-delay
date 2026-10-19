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

#include "frame-ring.hpp"
#include "perf-stats.hpp"
#include "storage-codec.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The delay buffer's CPU pipeline (v0.6.0, buffer pipeline design B.2-B.6):
// MJPEG encode and decode moved off OBS's graphics thread. Pure C++ (std::thread,
// no OBS, no FFmpeg: the codec is injected), so tests drive it without either.
//
// Threads and who owns what:
//   Graphics thread (G, VideoDelayFilter::Render): reserves a ring slot per
//     capture, reads the GPU staging back into a RawFrame from a small pool
//     and SubmitEncode()s it; on playback asks AcquireForDisplay() for the
//     newest decoded frame not younger than the delay and uploads it. G never
//     waits on a worker: no free RawFrame means the capture is dropped (the
//     slot goes Dropped, the frame is repeated in the delayed view, the delay
//     doesn't move), no decoded frame means the last one is redrawn.
//   Encode workers (1-2, "trigglow-enc-N"): pop jobs from a bounded FIFO,
//     encode outside every lock, copy the packet into the ring's arena and
//     publish the slot. Each owns its own codec context (never shared).
//   Decode worker (1, "trigglow-dec"): decodes, in capture order, up to
//     `lookahead` frames ahead of what's on screen, starting from the playback
//     target G publishes (target = now - delay is predictable from the clock;
//     when it falls behind it jumps straight to the frame due now), into a
//     pool of lookahead + 2 decoded frames.
//   Reaper (1, "trigglow-reap"): frees what must never be freed on G nor hold
//     up an encode -- a detached ring and its arena (600-770 MB at 1440p, tens
//     of ms to give back), the RawFrame storage after Disable, decoded frames
//     dropped by a detach -- and runs the arena's Trim() (the only call that
//     returns blocks to the OS). G and the workers only ever move such
//     references into its queue; a worker drops a ring reference itself only
//     while the pipeline still holds the same ring (so never the last one).
//
// Synchronization: mutex_ guards the job queue, the RawFrame pool, the decoded
// pool, the ring pointer, the published target and the reaper's queues;
// statsMutex_ guards the telemetry. Neither is ever held during a copy, an
// encode, a decode or a free, and they're never nested with each other or with
// the ring's own mutex.
//
// Sync mode (Options::threaded == false, the hidden codec_workers=false kill
// switch): no codec threads. SubmitEncode() and AcquireForDisplay() run the
// codec inline on the caller (the graphics thread), like v0.5.1 did, on the
// same ring/arena. The reaper still frees memory off the graphics thread (if
// it can't be started, memory is freed inline, like v0.5.1).
namespace trigglow {

// One codec instance per thread, opened, used and closed by that thread only
// (design invariant B.1.5). Implementations: MJPEG over FFmpeg (mjpeg-codec.hpp),
// and MakeRawCodec() below. (Since v0.7.0 I420View, DecodedImage, the I420
// helpers and PipelineLogLevel live in storage-codec.hpp, with the storage
// codec interface that replaces this one.)
class VideoCodec {
public:
	virtual ~VideoCodec() = default;
	// Encodes `frame` with `sliceThreads` slice threads (1 = none). On success
	// the packet stays valid until the next call. `compressed` false means the
	// bytes are the raw packed I420 frame. False on failure: the pipeline then
	// stores the frame raw.
	virtual bool Encode(const I420View &frame, uint32_t sliceThreads, const uint8_t *&packet, size_t &bytes,
			    bool &compressed) = 0;
	// Decodes one packet this codec produced (compressed == true).
	virtual bool Decode(const uint8_t *packet, size_t bytes, DecodedImage &out) = 0;
	// Closes the codec contexts and frees their buffers; the next call reopens.
	virtual void Release() = 0;
};
using CodecFactory = std::function<std::unique_ptr<VideoCodec>()>;

// Stores frames uncompressed (platforms without FFmpeg, and tests).
std::unique_ptr<VideoCodec> MakeRawCodec();

// A capture waiting to be encoded: tightly packed I420 storage from a small
// pool, reused frame after frame.
struct RawFrame {
	uint32_t width = 0;
	uint32_t height = 0;
	uint8_t *plane[3] = {nullptr, nullptr, nullptr};
	uint32_t stride[3] = {0, 0, 0};
	I420View View() const;

	std::unique_ptr<uint8_t[]> storage;
	size_t capacity = 0;
};

// What AcquireForDisplay() hands G: upload `image`, then ReleaseDisplay().
struct DecodedLease {
	bool valid = false;
	uint64_t captureNs = 0;
	DecodedImage image;
};

class CodecPipeline {
public:
	static constexpr uint32_t kMaxEncodeContexts = 2;
	static constexpr uint32_t kMaxSlicesPerContext = 2; // 4 until v0.6.0 (design 6.2: RAM).
	static constexpr uint32_t kStaleIntervals = 4;      // See AcquireForDisplay().

	struct Options {
		bool threaded = true;
		CodecFactory codecFactory;                               // Required.
		std::function<void(PipelineLogLevel, const char *)> log; // Optional.
		std::function<void(const char *)> nameThread;            // Optional; on the new thread.
		std::function<uint64_t()> clockNs;                       // Required: same clock as G's captureNs.
		uint32_t lookaheadFrames = 3;                            // Decode-ahead depth (L).
		uint32_t rawFramePoolSize = kMaxEncodeContexts + 2;      // Bounds the encode queue too.
	};

	// One telemetry window ("perf check" line), taken and reset by G.
	struct Stage {
		PerfSeries frameNs; // Wall time per frame (encode or decode).
	};
	struct WindowStats {
		Stage encode[kMaxEncodeContexts];
		Stage decode;
		uint64_t encodedBytes = 0;    // Compressed packets stored.
		uint64_t encodedRawBytes = 0; // Raw I420 size of those frames.
		uint64_t rawFallbacks = 0;    // Frames stored raw because the codec failed.
		uint64_t encodeFailures = 0;
		uint64_t decodeFailures = 0;
		uint64_t arenaDrops = 0;        // Slots dropped: the arena reached its limit.
		uint64_t backpressureDrops = 0; // Captures dropped: no free RawFrame (encoders behind).
		uint64_t staleEncodes = 0;      // Jobs whose slot was superseded before encoding.
		uint64_t decodeJumps = 0;       // Decoder skipped ahead to catch up.
		uint64_t staleDecoded = 0;      // Decoded frames dropped unshown: far older than the frame due.
		uint32_t queueDepthMax = 0;
		uint64_t queueDepthSum = 0;
		uint64_t queueDepthSamples = 0;
		uint64_t aheadSum = 0; // Decoded frames ready ahead of what's shown, sampled per acquire.
		uint64_t aheadSamples = 0;
		uint32_t aheadMin = UINT32_MAX;
	};
	// Trailing-window load for the degradation policy. Every sample carries
	// its frame's FrameRing::Reservation::tag (the rung it was captured at).
	struct RecentLoad {
		RollingWindow::Summary encode; // All encode contexts, frames tagged encodeTag only.
		// Frames tagged decodeTag only: the tag of the newest frame decoded.
		// The decoder works `delay` seconds behind the encoders, so right after
		// a rung change it still decodes the previous rung's frames.
		RollingWindow::Summary decode;
		uint32_t decodeTag = RollingWindow::kAnyTag; // kAnyTag: nothing decoded in the window.
	};

	explicit CodecPipeline(Options options);
	~CodecPipeline(); // Shutdown().
	CodecPipeline(const CodecPipeline &) = delete;
	CodecPipeline &operator=(const CodecPipeline &) = delete;

	// False in sync mode, or after a worker thread failed to start.
	bool Threaded() const;
	uint32_t LookaheadFrames() const { return options_.lookaheadFrames; }

	// --- Graphics thread ---

	// Starts a cycle on `ring` (Enable, or a hard resize). Any previous ring is
	// detached first (codecs kept). RawFrames are sized for up to maxWidth x
	// maxHeight. Spawns the workers the first time (threaded mode).
	void Attach(std::shared_ptr<FrameRing> ring, uint32_t maxWidth, uint32_t maxHeight, uint64_t captureIntervalNs);
	// Ends the cycle: drops queued jobs and decoded frames and hands the ring
	// to the reaper. With releaseCodecs (Disable) every worker closes its codec
	// contexts and the RawFrame pool's storage is freed too (the next Attach()
	// allocates it again). Pass the caller's own reference to the ring as
	// `retiredRing` instead of dropping it: then no reference the caller held
	// can be the last one, and the ring is always freed on the reaper. Never
	// waits for a worker.
	void Detach(bool releaseCodecs, std::shared_ptr<FrameRing> retiredRing = nullptr);
	// Capture cadence (1/fps, or 2/fps at the 30fps degradation rung).
	void SetCaptureInterval(uint64_t captureIntervalNs);
	// Encode contexts (1..kMaxEncodeContexts, threaded only) and slice threads
	// per context (1..kMaxSlicesPerContext). Takes effect on the next frame.
	void SetEncodeParallelism(uint32_t contexts, uint32_t slicesPerContext);

	// A free RawFrame laid out for width x height, or nullptr when every one
	// is queued or being encoded (backpressure: the caller drops the capture).
	RawFrame *AcquireRawFrame(uint32_t width, uint32_t height);
	void ReleaseRawFrame(RawFrame *frame);
	// Hands `frame` and the slot reserved for it in `ring` over (sync mode:
	// encodes right here). The frame always goes back to the pool; if the slot
	// can't be filled it ends Dropped (or stale, when the ring was replaced).
	void SubmitEncode(const std::shared_ptr<FrameRing> &ring, RawFrame *frame,
			  const FrameRing::Reservation &reservation);
	// Counts a capture dropped because AcquireRawFrame() returned nullptr.
	void NoteBackpressureDrop();

	// Playback. Publishes targetNs (and what's on screen now) to the decoder,
	// then returns the newest decoded frame with lastShownNs < captureNs <=
	// targetNs, if any (sync mode: decodes it right here). After uploading it,
	// call ReleaseDisplay(). Right after a pause in rendering (more than
	// kStaleIntervals capture intervals since the previous call), a decoded
	// frame that much older than the frame due in the ring -- decoded ahead
	// before the pause -- is never returned: it's dropped, and the caller
	// holds the frame on screen until the decoder, which jumps straight to the
	// due frame when it falls behind, has that one ready (one decode later).
	// Without a pause nothing is dropped: a decoder that's merely slow still
	// gets every frame it manages shown.
	bool AcquireForDisplay(uint64_t targetNs, uint64_t lastShownNs, DecodedLease &lease);
	void ReleaseDisplay(DecodedLease &lease);
	// Only publishes a target (nothing is shown): lets the decoder start on
	// the first frames before playback begins, e.g. right before the fill
	// window ends. No-op in sync mode.
	void PrimeDecode(uint64_t targetNs);

	// --- Telemetry (any thread) ---
	void TakeWindowStats(WindowStats &out);
	RecentLoad GetRecentLoad(uint64_t nowNs, uint64_t windowNs, uint32_t encodeTag = RollingWindow::kAnyTag) const;
	void ResetRecentLoad();
	uint32_t ActiveEncodeContexts() const;
	uint32_t SlicesPerContext() const;

	// Stops and joins every worker (each closes its own codec) and the reaper
	// (after it frees what's queued). Destruction thread only, never the
	// graphics thread: a join can wait for one frame's encode or decode, or a
	// ring being freed. Idempotent.
	void Shutdown();

private:
	struct EncodeJob {
		std::shared_ptr<FrameRing> ring;
		FrameRing::Reservation reservation;
		RawFrame *frame = nullptr;
	};
	struct DecodedEntry {
		enum class State : uint8_t { Free, Decoding, Ready };
		State state = State::Free;
		uint64_t captureNs = 0;
		uint64_t epoch = 0;
		DecodedImage image;
	};
	enum : uint32_t { kInlineStage = UINT32_MAX };

	void EncodeLoop(uint32_t worker);
	void DecodeLoop();
	void ReapLoop();
	void EnsureThreadsLocked(); // Spawns the reaper, then missing workers (threaded mode).
	bool EnsureReaperLocked();  // Spawns the reaper if needed; false if it can't run.
	// Under mutex_: queues `object` for the reaper. Without a reaper thread it
	// goes to `fallback` instead, which the caller drops once it unlocks.
	void ReapLocked(std::shared_ptr<void> object, std::vector<std::shared_ptr<void>> &fallback);
	// Under mutex_: back to the pool; after Disable its storage goes to the reaper.
	void ReturnRawFrameLocked(RawFrame *frame, std::vector<std::shared_ptr<void>> &fallback);
	// Has the reaper Trim() the ring's arena (inline without a reaper). No lock held.
	void RequestTrim(const std::shared_ptr<FrameRing> &ring);
	// Encodes one job on the calling thread; doesn't touch the RawFrame pool.
	void EncodeOne(VideoCodec &codec, const EncodeJob &job, uint32_t slices, uint32_t stage);
	// Copies the pinned slot's payload, unpins it, then decodes it (or unpacks a
	// raw frame). `scratch` is the caller's reusable packet buffer.
	bool DecodePinned(FrameRing &ring, const FrameRing::ReadView &view, VideoCodec &codec,
			  std::vector<uint8_t> &scratch, DecodedImage &out);
	bool AcquireInline(uint64_t targetNs, uint64_t lastShownNs, DecodedLease &lease);
	void Log(PipelineLogLevel level, const char *fmt, ...);

	Options options_;

	mutable std::mutex mutex_;
	std::condition_variable encodeCv_;
	std::condition_variable decodeCv_;
	std::deque<EncodeJob> queue_;
	std::vector<std::unique_ptr<RawFrame>> rawFrames_;
	std::vector<RawFrame *> freeRaw_;
	std::vector<DecodedEntry> decoded_;
	std::shared_ptr<FrameRing> ring_;
	// The reaper's queues: references to drop, and rings whose arena to Trim().
	std::vector<std::shared_ptr<void>> reap_;
	std::vector<std::shared_ptr<FrameRing>> trim_;
	uint64_t epoch_ = 1;
	uint64_t releaseSerial_ = 0; // ++ when every worker must close its codec.
	uint32_t contexts_ = 1;
	uint32_t slices_ = 1;
	uint64_t captureIntervalNs_ = 16666667;
	uint64_t targetNs_ = 0;
	uint64_t lastShownNs_ = 0;
	uint64_t lastAcquireNs_ = 0; // Clock at the previous AcquireForDisplay() (pause detection).
	// ++ on anything the decoder may be waiting for (new target, new Ready
	// slot, detach), so a notification sent while it wasn't waiting yet is
	// never lost.
	uint64_t wakeSerial_ = 0;
	bool stop_ = false;
	bool threadsFailed_ = false;   // Couldn't spawn a worker: everything runs inline from then on.
	bool reaperFailed_ = false;    // Couldn't spawn the reaper: memory is freed inline from then on.
	bool rawPoolReleased_ = false; // Detach(true) freed the RawFrame storage; Attach() brings it back.
	std::vector<std::thread> encodeThreads_;
	std::thread decodeThread_;
	std::thread reapThread_;
	std::condition_variable reapCv_;

	// Sync mode / fallback: graphics thread only.
	std::unique_ptr<VideoCodec> inlineCodec_;
	std::vector<uint8_t> inlineScratch_;
	uint64_t inlineFailedNs_ = 0; // captureNs whose decode failed: not retried on every Render().

	mutable std::mutex statsMutex_;
	WindowStats window_;
	RollingWindow encodeRecent_;
	RollingWindow decodeRecent_;
};

} // namespace trigglow
