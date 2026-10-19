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
// no OBS, no FFmpeg: the codecs are injected, storage-codec.hpp), so tests drive
// it without either.
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
//     publish the slot. Each owns its own encoder (never shared) and its
//     bitstream chain (storage::ChainTracker); since v0.7.0 the encoder may
//     keep frames in flight (storage::Encoder::Send()/Receive()): their
//     packets are matched to their slots in Send() order and published when
//     they come out, polled every kInFlightPollNs while nothing else arrives.
//     With MJPEG and raw every packet comes out with its own Send().
//   Setup thread (v0.7.0, "trigglow-codec-setup", only with
//     Options::interSetup): opens and checks the inter-frame (hardware)
//     encoder for a frame size when worker 0 asks for one (InterSetupFn),
//     while worker 0 goes on encoding with the intra codec (the MJPEG bridge).
//     Worker 0 adopts the encoder at a frame boundary, on a new chain from a
//     key; the decoder the probe warmed up goes to the decode worker. While
//     worker 0 encodes inter-frame, it is the only encode context (the
//     bitstream must follow capture order, invariant I1).
//   Decode worker (1, "trigglow-dec"): decodes, in capture order, up to
//     `lookahead` frames ahead of what's on screen, starting from the playback
//     target G publishes (target = now - delay is predictable from the clock;
//     when it falls behind it jumps straight to the frame due now), into a
//     pool of lookahead + 2 decoded frames. One decoder per codec in the ring
//     (raw is the pipeline's own); a P-frame is only decoded right after its
//     reference (ringselect::Decodable(), per codec), anything else is skipped
//     until the next key -- never the case with raw and MJPEG, all keys. When
//     it starts, or jumps, onto a P-frame, it decodes from that frame's key
//     without showing anything (catch-up) or waits for the next key, whichever
//     shows a fresh frame sooner (FrameRing::PlanDecodeStart(), invariant I5),
//     and tells G meanwhile (DecodedLease::wait), so those repeated frames
//     aren't taken for a slow decoder.
//   Reaper (1, "trigglow-reap"): frees what must never be freed on G nor hold
//     up an encode -- a detached ring and its arena (600-770 MB at 1440p, tens
//     of ms to give back), the RawFrame storage after Disable, decoded frames
//     dropped by a detach, hardware encoders and decoders retired -- and runs
//     the arena's Trim() (the only call that returns blocks to the OS). G and
//     the workers only ever move such references into its queue; a worker
//     drops a ring reference itself only while the pipeline still holds the
//     same ring (so never the last one).
//
// Synchronization: mutex_ guards the job queue, the RawFrame pool, the decoded
// pool, the ring pointer, the published target, the setup mailbox and the
// reaper's queues; statsMutex_ guards the telemetry. Neither is ever held
// during a copy, an encode, a decode or a free, and they're never nested with
// each other or with the ring's own mutex.
//
// Sync mode (Options::threaded == false, the hidden codec_workers=false kill
// switch): no codec threads. SubmitEncode() and AcquireForDisplay() run the
// codec inline on the caller (the graphics thread), like v0.5.1 did, on the
// same ring/arena, intra only (a hardware encoder needs the workers). The
// reaper still frees memory off the graphics thread (if it can't be started,
// memory is freed inline, like v0.5.1).
namespace trigglow {

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

// Why the decoder can't show the frame due yet, on purpose (v0.7.0): G
// counts those repeated frames apart, never as a slow decoder.
enum class DecodeWait : uint8_t {
	None,
	CatchUp, // Decoding from a key, unshown, up to the frame due.
	NoKey,   // Skipping to the next key (catching up would take longer).
};

// What AcquireForDisplay() hands G: upload `image`, then ReleaseDisplay().
struct DecodedLease {
	bool valid = false;
	uint64_t captureNs = 0;
	DecodedImage image;
	// Filled even without a frame: due frames up to waitUntilNs are held by
	// the decoder's catch-up or wait for a key, not decoded late.
	DecodeWait wait = DecodeWait::None;
	uint64_t waitUntilNs = 0;
};

class CodecPipeline {
public:
	static constexpr uint32_t kMaxEncodeContexts = 2;
	static constexpr uint32_t kMaxSlicesPerContext = 2; // 4 until v0.6.0 (each slice's buffers cost RAM).
	static constexpr uint32_t kStaleIntervals = 4;      // See AcquireForDisplay().
	// A worker with frames in flight and no new job polls its encoder this often.
	static constexpr uint64_t kInFlightPollNs = 2000000ULL;
	// What an encoder being replaced or closed gets to hand over what it has in flight.
	static constexpr uint64_t kDrainDeadlineNs = 50000000ULL;
	// An inter-frame encoder with its oldest frame in flight this long, newer
	// ones sent after it, and no packet: a stall (a hard error). Only time
	// while frames keep coming counts: an encoder like AMF hands a frame's
	// packet out once more frames are in, so during a render pause it holds
	// what it has, and that's no stall.
	static constexpr uint64_t kStallNs = 500000000ULL;
	// No frame sent for this long: a render pause (the stall clock stops, and
	// starts over from the next frame).
	static constexpr uint64_t kRenderPauseNs = 250000000ULL;
	// A decoder that decoded nothing for this long while others did is closed (its
	// codec left the ring: after a fallback, or MJPEG once the hardware took over).
	static constexpr uint64_t kDecoderIdleNs = 3000000000ULL;

	struct Options {
		bool threaded = true;
		// Opens an encoder for the frame geometry and slice threads the
		// pipeline fills in. Empty: frames are stored raw. With interSetup,
		// this is the intra codec: the bridge and the fallback.
		storage::EncoderFactory encoderFactory;
		// Decoders for every codec but raw (the pipeline's own). Empty: only
		// raw payloads decode. For the inter codecs: the hardware decoders.
		storage::DecoderFactory decoderFactory;
		// v0.7.0: the inter-frame (hardware) storage codec, set up on the
		// setup thread (see the header comment). Empty: intra only.
		storage::InterSetupFn interSetup;
		// Software decoders for the inter codecs, for what's left of an
		// inter-frame stretch in the ring when its hardware decoder can't be
		// used (it failed, or the codec policy wants the GPU's video engine
		// free: SetSoftwareDecode()). Empty: none.
		storage::DecoderFactory softwareDecoderFactory;
		std::shared_ptr<storage::InterControl> interControl; // Optional: yields from the UI.
		PipelineLogFn log;                                   // Optional.
		std::function<void(const char *)> nameThread;        // Optional; on the new thread.
		std::function<uint64_t()> clockNs;                   // Required: same clock as G's captureNs.
		uint32_t lookaheadFrames = 3;                        // Decode-ahead depth (L).
		uint32_t rawFramePoolSize = kMaxEncodeContexts + 2;  // Bounds the encode queue too.
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
		// v0.7.0 chain bookkeeping (raw and MJPEG: all 0, and inFlightMax 1).
		uint64_t chainBreaks = 0;   // Packets lost after their encode: the next frame is a forced key.
		uint64_t orphanDrops = 0;   // Packets not stored: their reference was lost (slot Dropped).
		uint64_t busyDrops = 0;     // Captures the encoder couldn't take (SendResult::Busy).
		uint64_t encoderResets = 0; // Encoders closed on a hard error (packet out of order).
		uint64_t noKeySkips = 0;    // Stored frames the decoder skipped: not decodable without a key first.
		uint32_t inFlightMax = 0;   // Most frames one encoder held in flight.
		// v0.7.0 inter-frame codec.
		PerfSeries interLatencyNs;    // Inter encoder: Send() to its packet out.
		PerfSeries interSendNs;       // Inter encoder: the Send() call itself.
		uint64_t interKeys = 0;       // Keys the inter encoder stored...
		uint64_t forcedKeys = 0;      // ...and keys it was asked for (chain starts, after a loss).
		uint64_t bridgeFrames = 0;    // Frames worker 0 stored intra while it wanted the inter encoder.
		uint64_t codecChanges = 0;    // Inter encoder adopted or retired.
		uint64_t interHardErrors = 0; // Inter encoder failed (closed, intra from then on).
		uint64_t catchUps = 0;        // Decoder (re)starts decoded from a key, unshown...
		uint64_t catchUpFrames = 0;   // ...these many frames...
		uint64_t catchUpMaxNs = 0;    // ...the longest taking this long.
		uint64_t noKeyWaits = 0;      // Decoder (re)starts that skipped to the next key instead.
		uint32_t queueDepthMax = 0;
		uint64_t queueDepthSum = 0;
		uint64_t queueDepthSamples = 0;
		uint64_t aheadSum = 0; // Decoded frames ready ahead of what's shown, sampled per acquire.
		uint64_t aheadSamples = 0;
		uint32_t aheadMin = UINT32_MAX;
	};
	// Trailing-window load for the degradation policy. Every sample carries
	// its frame's FrameRing::Reservation::tag (the rung it was captured at);
	// since v0.7.0 decode samples add kInterTagBit for inter-frame codecs.
	struct RecentLoad {
		RollingWindow::Summary encode; // All encode contexts, frames tagged encodeTag only.
		// Frames tagged decodeTag only: the tag of the newest frame decoded.
		// The decoder works `delay` seconds behind the encoders, so right after
		// a rung (or codec) change it still decodes the previous one's frames.
		RollingWindow::Summary decode;
		uint32_t decodeTag = RollingWindow::kAnyTag; // kAnyTag: nothing decoded in the window.
	};
	static constexpr uint32_t kInterTagBit = 1u << 16;
	// The inter encoder's latency over a trailing window, for the codec policy.
	struct InterLoad {
		RollingWindow::Summary latency; // Send() to packet out.
		uint32_t inFlightMax = 0;       // Most frames in flight at a Send().
	};
	// The inter-frame codec's state, for the codec policy and the telemetry.
	// Counters are cumulative over the pipeline's life.
	struct CodecStatus {
		bool interWanted = false; // SetStorageTarget(true), not yielded, not stopped by a hard error.
		bool interActive = false; // Worker 0 encodes with the inter encoder right now.
		bool setupBusy = false;
		uint64_t adoptions = 0;
		uint64_t setupFailures = 0;   // Setups that found no working encoder.
		uint64_t hardErrors = 0;      // Inter encoders that failed.
		uint64_t decoderFailures = 0; // Hardware decoders that failed (software from then on).
		bool hardwareDecode = false;  // The inter codec's decoder in use is a hardware one.
		bool softwareDecode = false;  // SetSoftwareDecode(true), or a hardware decoder failed.
		storage::CodecId codec = storage::CodecId::Raw; // The last inter encoder adopted...
		storage::Backend backend = storage::Backend::Cpu;
		char encoder[24] = "";
		char lastError[96] = "";
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
	// allocates it again); a setup under way stops at its next step and
	// closes what it opened.
	// Pass the caller's own reference to the ring as `retiredRing` instead of
	// dropping it: then no reference the caller held can be the last one, and
	// the ring is always freed on the reaper. Never waits for a worker.
	void Detach(bool releaseCodecs, std::shared_ptr<FrameRing> retiredRing = nullptr);
	// Capture cadence (1/fps, or 2/fps at the 30fps degradation rung).
	void SetCaptureInterval(uint64_t captureIntervalNs);
	// Encode contexts (1..kMaxEncodeContexts, threaded only) and slice threads
	// per context (1..kMaxSlicesPerContext). Takes effect on the next frame.
	// While the inter encoder is in use there is one context whatever this says.
	void SetEncodeParallelism(uint32_t contexts, uint32_t slicesPerContext);
	// v0.7.0: store with the inter-frame codec (true: set it up, bridging with
	// the intra codec meanwhile) or the intra one (false: an inter encoder in
	// use hands over what it has in flight, up to kDrainDeadlineNs, and is
	// closed on the reaper; the frames after it are intra). O(1), takes effect
	// at the next frame. True also lifts the block a hard error put on it.
	// Ignored without Options::interSetup or in sync mode.
	void SetStorageTarget(bool inter);
	// v0.7.0: decode the inter codecs in software from now on (true), or with
	// the hardware decoders (false). The switch restarts those codecs' chains:
	// from a key, catching up or waiting for the next one.
	void SetSoftwareDecode(bool software);

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
	// due frame when it falls behind, has that one ready (one decode later,
	// or a catch-up from its key). Without a pause nothing is dropped: a
	// decoder that's merely slow still gets every frame it manages shown.
	bool AcquireForDisplay(uint64_t targetNs, uint64_t lastShownNs, DecodedLease &lease);
	void ReleaseDisplay(DecodedLease &lease);
	// Only publishes a target (nothing is shown): lets the decoder start on
	// the first frames before playback begins, e.g. right before the fill
	// window ends. No-op in sync mode.
	void PrimeDecode(uint64_t targetNs);

	// --- Telemetry (any thread) ---
	void TakeWindowStats(WindowStats &out);
	RecentLoad GetRecentLoad(uint64_t nowNs, uint64_t windowNs, uint32_t encodeTag = RollingWindow::kAnyTag) const;
	InterLoad GetInterLoad(uint64_t nowNs, uint64_t windowNs) const;
	CodecStatus GetCodecStatus() const;
	void ResetRecentLoad();
	uint32_t ActiveEncodeContexts() const;
	uint32_t SlicesPerContext() const;

	// Stops and joins every worker (each closes its own codec), the setup
	// thread (a setup under way stops at its next step) and the reaper (after
	// it frees what's queued). Destruction thread only, never the graphics
	// thread: a join can wait for one frame's encode or decode, one step of a
	// setup (a hardware encoder being opened), or a ring being freed.
	// Idempotent.
	void Shutdown();

private:
	struct EncodeJob {
		std::shared_ptr<FrameRing> ring;
		FrameRing::Reservation reservation;
		RawFrame *frame = nullptr;
		uint64_t epoch = 0; // The cycle it was captured in: a new one starts a new chain.
	};
	// A frame sent to an encoder whose packet hasn't been stored yet.
	struct InFlightFrame {
		std::shared_ptr<FrameRing> ring;
		FrameRing::Reservation reservation;
		uint32_t chain = storage::kNoChain;
		uint32_t width = 0;
		uint32_t height = 0;
		I420View frame; // The job's frame, only while that job is being encoded.
	};
	// One encode context: a worker's (or, in sync mode, G's) encoder, its
	// chain and what it has in flight. Only its owner thread touches it.
	struct EncodeContext {
		std::unique_ptr<storage::Encoder> encoder;
		bool fromSetup = false; // `encoder` is the inter one the setup thread opened (worker 0 only).
		storage::ChainTracker chain;
		storage::InFlightQueue<InFlightFrame> inFlight;
		uint64_t chainEpoch = 0;
		// Stall detection (inter encoders): the last Send() attempt, and when
		// the stall clock started (the encoder's first frame, or the first
		// one after a render pause).
		uint64_t lastSendNs = 0;
		uint64_t stallFromNs = 0;
	};
	// Counters an encode adds up without the stats lock, committed once.
	struct EncodeTally {
		uint64_t encodedBytes = 0;
		uint64_t encodedRawBytes = 0;
		uint64_t rawFallbacks = 0;
		uint64_t encodeFailures = 0;
		uint64_t arenaDrops = 0;
		uint64_t staleEncodes = 0;
		uint64_t chainBreaks = 0;
		uint64_t orphanDrops = 0;
		uint64_t busyDrops = 0;
		uint64_t encoderResets = 0;
		uint32_t inFlightMax = 0;
		uint64_t interKeys = 0;
		uint64_t forcedKeys = 0;
		uint64_t bridgeFrames = 0;
		uint64_t codecChanges = 0;
		uint64_t interHardErrors = 0;
	};
	// A decode context: one decoder per codec (created on first use) and the
	// chain position of the last packet each decoded. Only its owner thread
	// touches it.
	struct DecodeContext {
		std::unique_ptr<storage::Decoder> decoders[storage::kCodecCount];
		bool unavailable[storage::kCodecCount] = {}; // The factory had none: not asked again.
		ringselect::ChainCursor cursors[storage::kCodecCount];
		uint64_t lastUsedNs[storage::kCodecCount] = {};
		uint64_t decodeNs[storage::kCodecCount] = {}; // Average decode (with a picture), for catch-up plans.
		uint64_t lastDecodeNs = 0;                    // Any codec.
		bool software = false;                        // Inter codecs decode in software.
		bool hardwareFailed = false;                  // A hardware decoder broke: software from then on.
		bool flushPending = false;
		std::vector<uint8_t> scratch; // Reusable packet copy.
	};
	enum class DecodeOutcome : uint8_t { Decoded, Skipped, Failed };
	struct DecodedEntry {
		enum class State : uint8_t { Free, Decoding, Ready };
		State state = State::Free;
		uint64_t captureNs = 0;
		uint64_t epoch = 0;
		DecodedImage image;
	};
	// The setup thread's mailbox (under mutex_).
	struct SetupState {
		bool requested = false; // A request waiting for the setup thread.
		uint32_t wantWidth = 0;
		uint32_t wantHeight = 0;
		bool busy = false; // The setup thread is on one.
		uint32_t busyWidth = 0;
		uint32_t busyHeight = 0;
		uint64_t generation = 0; // ++ throws away whatever is under way (target off, Disable).
		std::unique_ptr<storage::Encoder> result;
		uint32_t resultWidth = 0;
		uint32_t resultHeight = 0;
		uint32_t failedWidth = 0; // The last size no encoder worked for: not asked again.
		uint32_t failedHeight = 0;
		std::unique_ptr<storage::Decoder> decoders[storage::kCodecCount]; // Warmed up, for the decode worker.
	};
	enum : uint32_t { kInlineStage = UINT32_MAX };

	void EncodeLoop(uint32_t worker);
	void DecodeLoop();
	void ReapLoop();
	void SetupLoop();
	void EnsureThreadsLocked(); // Spawns the reaper, then missing workers (threaded mode).
	bool EnsureReaperLocked();  // Spawns the reaper if needed; false if it can't run.
	bool EnsureSetupThreadLocked();
	// Under mutex_: queues `object` for the reaper. Without a reaper thread it
	// goes to `fallback` instead, which the caller drops once it unlocks.
	void ReapLocked(std::shared_ptr<void> object, std::vector<std::shared_ptr<void>> &fallback);
	template<typename T>
	void ReapOwnedLocked(std::unique_ptr<T> object, std::vector<std::shared_ptr<void>> &fallback)
	{
		if (object)
			ReapLocked(std::shared_ptr<T>(std::move(object)), fallback);
	}
	// Under mutex_: back to the pool; after Disable its storage goes to the reaper.
	void ReturnRawFrameLocked(RawFrame *frame, std::vector<std::shared_ptr<void>> &fallback);
	// Has the reaper Trim() the ring's arena (inline without a reaper). No lock held.
	void RequestTrim(const std::shared_ptr<FrameRing> &ring);
	// Under mutex_: encode contexts in use (1 while the inter encoder is).
	uint32_t ActiveContextsLocked() const { return interActive_ ? 1 : contexts_; }
	// Under mutex_: whether worker 0 should use the inter encoder.
	bool InterWantedLocked() const;
	// Encodes one job on the calling thread; doesn't touch the RawFrame pool.
	// Ring references it lets go of (packets stored) go to `released`.
	// interWanted: worker 0 should use the inter encoder (read under mutex_).
	void EncodeOne(EncodeContext &ctx, const EncodeJob &job, uint32_t slices, uint32_t stage, bool interWanted,
		       std::vector<std::shared_ptr<FrameRing>> &released);
	// Worker 0 with interWanted: keeps, adopts or asks for the inter encoder of
	// this frame size; on return ctx.encoder is it, or (bridge) whatever
	// EncodeOne() falls back to. No lock held.
	void ChooseInterEncoder(EncodeContext &ctx, uint32_t width, uint32_t height, EncodeTally &tally,
				std::vector<std::shared_ptr<FrameRing>> &released);
	// Closes an inter encoder that's no longer wanted: hands over what it has
	// in flight first; the encoder goes to the reaper (inline: closed here,
	// for a yield, so its session is free right away). No lock held.
	void RetireInter(EncodeContext &ctx, bool inline_, EncodeTally &tally,
			 std::vector<std::shared_ptr<FrameRing>> &released);
	// The inter encoder failed: what it had in flight is lost, it goes to the
	// reaper, and worker 0 stays intra until SetStorageTarget(true). No lock held.
	void FailInter(EncodeContext &ctx, const char *reason, EncodeTally &tally,
		       std::vector<std::shared_ptr<FrameRing>> &released);
	// After a Send or a poll: a broken or stalled inter encoder fails.
	void CheckInterHealth(EncodeContext &ctx, EncodeTally &tally,
			      std::vector<std::shared_ptr<FrameRing>> &released);
	// Stores every packet the encoder has ready. No lock held.
	void ReceiveReady(EncodeContext &ctx, EncodeTally &tally, std::vector<std::shared_ptr<FrameRing>> &released);
	// Stores one packet in its slot (or drops it), keeping the chain's books.
	// False: it isn't the oldest frame in flight (a hard error, nothing done).
	bool StorePacket(EncodeContext &ctx, const storage::Packet &packet, EncodeTally &tally,
			 std::vector<std::shared_ptr<FrameRing>> &released);
	// Stores `frame` uncompressed (codec raw, a key of no chain).
	void StoreRaw(FrameRing &ring, const FrameRing::Reservation &reservation, const I420View &frame,
		      EncodeTally &tally);
	// Ends what ctx has in flight: every frame's slot is Dropped, its ring released.
	void DropInFlight(EncodeContext &ctx, std::vector<std::shared_ptr<FrameRing>> &released);
	// Closes ctx's encoder: drain hands over what it has in flight first (up to
	// kDrainDeadlineNs); whatever is left ends Dropped.
	void CloseEncoder(EncodeContext &ctx, bool drain, EncodeTally &tally,
			  std::vector<std::shared_ptr<FrameRing>> &released);
	void CommitTally(const EncodeTally &tally);    // Takes statsMutex_.
	void AddTallyLocked(const EncodeTally &tally); // statsMutex_ held.
	// Under mutex_: drops ring references; ones no longer the pipeline's go to the reaper.
	void ReleaseRingsLocked(std::vector<std::shared_ptr<FrameRing>> &rings,
				std::vector<std::shared_ptr<void>> &fallback);
	// The decoder for `codec` (created on first use), or nullptr.
	storage::Decoder *DecoderFor(DecodeContext &ctx, storage::CodecId codec);
	void ResetDecoders(DecodeContext &ctx); // Closes them all (Disable).
	void FlushDecoders(DecodeContext &ctx); // A new cycle: no references any more.
	// Under mutex_ (decode worker): software/hardware switch, warmed-up
	// decoders from the setup thread, idle decoders closed.
	void SyncDecodersLocked(DecodeContext &ctx, uint64_t nowNs, std::vector<std::shared_ptr<void>> &fallback);
	// Decodes the pinned slot when ringselect::Decodable() allows it -- a raw
	// payload is copied out while pinned (that copy is its decode); anything
	// else is copied to ctx.scratch, unpinned, then decoded. Always unpins.
	// wantImage false: a catch-up decode (references only, no picture).
	DecodeOutcome DecodePinned(FrameRing &ring, const FrameRing::ReadView &view, DecodeContext &ctx,
				   DecodedImage &out, bool wantImage = true);
	// The decoder landed on a P-frame it can't start on (it just started, or
	// jumped): catches up from its key and decodes the frame due into `image`
	// (true, `view` then describes it), or settles for waiting for the next
	// key (false, cursorNs moved to the frame due). Decode worker, no lock.
	bool StartFromKey(FrameRing &ring, DecodeContext &ctx, storage::CodecId codec, uint64_t targetNs,
			  uint64_t intervalNs, uint64_t epoch, FrameRing::ReadView &view, DecodedImage &image,
			  uint64_t &cursorNs);
	void SetDecodeWait(DecodeWait wait, uint64_t untilNs); // Takes mutex_.
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

	// v0.7.0 inter-frame codec (under mutex_).
	bool interTarget_ = false;    // SetStorageTarget().
	bool interBlocked_ = false;   // A hard error: intra until SetStorageTarget(true).
	bool interActive_ = false;    // Worker 0's encoder is the inter one.
	bool softwareDecode_ = false; // SetSoftwareDecode().
	bool setupFailedToStart_ = false;
	SetupState setup_;
	std::thread setupThread_;
	std::condition_variable setupCv_;
	DecodeWait decodeWait_ = DecodeWait::None;
	uint64_t decodeWaitUntilNs_ = 0;
	CodecStatus status_; // The cumulative counters and names (setupBusy etc. filled on read).

	// Sync mode / fallback: graphics thread only.
	EncodeContext inlineEncode_;
	DecodeContext inlineDecode_;
	uint64_t inlineFailedNs_ = 0; // captureNs whose decode failed (or was skipped): not retried every Render().

	mutable std::mutex statsMutex_;
	WindowStats window_;
	RollingWindow encodeRecent_;
	RollingWindow decodeRecent_;
	RollingWindow interLatencyRecent_;
	RollingWindow interInFlightRecent_; // Value: frames in flight (not nanoseconds).
};

} // namespace trigglow
