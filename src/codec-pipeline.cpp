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

#include "codec-pipeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <system_error>

namespace trigglow {

namespace {
// Most decoded frames G ever has to sift through at once (lookahead + 2 <= 16).
constexpr uint32_t kMaxLookaheadFrames = 14;
// Sync path: how far back past a dropped/late due slot it looks for an older
// Ready frame to show instead.
constexpr size_t kDisplayMaxBack = 4;
// How far back past non-Ready slots the decoder's jump to the frame due looks
// (FrameRing::PinNextForDecode() uses the same).
constexpr size_t kDecodeJumpBack = 8;
// A catch-up plan's guess at one decode before any was measured, and right
// after a switch to the software decoders (1440p HEVC: ~15 ms on one thread).
constexpr uint64_t kDefaultDecodeNs = 4000000ULL;
constexpr uint64_t kSoftwareDecodeNs = 15000000ULL;

// A RawFrame's storage as a reference the reaper can drop. The frame keeps no
// storage (capacity 0): AcquireRawFrame()/Attach() allocate it again.
std::shared_ptr<void> TakeStorage(RawFrame &frame)
{
	std::shared_ptr<uint8_t> storage(frame.storage.release(), std::default_delete<uint8_t[]>());
	frame.capacity = 0;
	for (auto &plane : frame.plane)
		plane = nullptr;
	return storage;
}

bool InterCodec(storage::CodecId codec)
{
	return storage::ClassOf(codec) == storage::StorageClass::Inter;
}

size_t CodecIndex(storage::CodecId codec)
{
	return static_cast<size_t>(codec);
}

// The tag a decode sample carries: the capture rung, plus the codec class.
uint32_t DecodeTag(const FrameRing::ReadView &view)
{
	return view.tag | (InterCodec(view.codec) ? CodecPipeline::kInterTagBit : 0u);
}
} // namespace

I420View RawFrame::View() const
{
	I420View view;
	view.width = width;
	view.height = height;
	for (int p = 0; p < 3; ++p) {
		view.plane[p] = plane[p];
		view.stride[p] = stride[p];
	}
	return view;
}

CodecPipeline::CodecPipeline(Options options) : options_(std::move(options))
{
	options_.lookaheadFrames = std::clamp<uint32_t>(options_.lookaheadFrames, 1, kMaxLookaheadFrames);
	options_.rawFramePoolSize = std::max<uint32_t>(options_.rawFramePoolSize, 1);
	if (!options_.clockNs) {
		options_.clockNs = [] {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
							     std::chrono::steady_clock::now().time_since_epoch())
							     .count());
		};
	}
	if (!options_.encoderFactory)
		options_.encoderFactory = &storage::MakeRawEncoder;
	for (uint32_t i = 0; i < options_.rawFramePoolSize; ++i) {
		rawFrames_.push_back(std::make_unique<RawFrame>());
		freeRaw_.push_back(rawFrames_.back().get());
	}
	decoded_.resize(options_.lookaheadFrames + 2);
	if (options_.interControl)
		options_.interControl->SetWake([this] { encodeCv_.notify_all(); });
}

CodecPipeline::~CodecPipeline()
{
	Shutdown();
}

bool CodecPipeline::Threaded() const
{
	return options_.threaded && !threadsFailed_;
}

void CodecPipeline::Log(PipelineLogLevel level, const char *fmt, ...)
{
	if (!options_.log)
		return;
	char line[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	options_.log(level, line);
}

bool CodecPipeline::EnsureReaperLocked()
{
	if (reapThread_.joinable())
		return true;
	if (reaperFailed_ || stop_)
		return false;
	try {
		reapThread_ = std::thread(&CodecPipeline::ReapLoop, this);
	} catch (const std::system_error &error) {
		reaperFailed_ = true;
		Log(PipelineLogLevel::Warning,
		    "could not start the buffer reaper thread (%s) -- freeing old buffers on the calling thread instead",
		    error.what());
		return false;
	}
	return true;
}

bool CodecPipeline::EnsureSetupThreadLocked()
{
	if (setupThread_.joinable())
		return true;
	if (setupFailedToStart_ || stop_)
		return false;
	try {
		setupThread_ = std::thread(&CodecPipeline::SetupLoop, this);
	} catch (const std::system_error &error) {
		setupFailedToStart_ = true;
		Log(PipelineLogLevel::Warning,
		    "could not start the buffer codec setup thread (%s) -- the buffer stays on its intra codec",
		    error.what());
		return false;
	}
	return true;
}

void CodecPipeline::ReapLocked(std::shared_ptr<void> object, std::vector<std::shared_ptr<void>> &fallback)
{
	if (!object)
		return;
	if (reapThread_.joinable()) {
		reap_.push_back(std::move(object));
		reapCv_.notify_one();
	} else {
		fallback.push_back(std::move(object));
	}
}

void CodecPipeline::ReleaseRingsLocked(std::vector<std::shared_ptr<FrameRing>> &rings,
				       std::vector<std::shared_ptr<void>> &fallback)
{
	// Still the pipeline's ring: ring_ holds it too, so this is never the last
	// reference. Detached meanwhile: it may be, and freeing an arena takes tens
	// of ms -- the reaper does that, never G nor an encode.
	for (std::shared_ptr<FrameRing> &ring : rings) {
		if (ring == ring_)
			ring.reset();
		else
			ReapLocked(std::move(ring), fallback);
	}
	rings.clear();
}

void CodecPipeline::ReturnRawFrameLocked(RawFrame *frame, std::vector<std::shared_ptr<void>> &fallback)
{
	if (!frame)
		return;
	// Disabled: the pool keeps its frames but not their storage (~22 MB at 1440p).
	if (rawPoolReleased_ && frame->storage)
		ReapLocked(TakeStorage(*frame), fallback);
	freeRaw_.push_back(frame);
}

void CodecPipeline::RequestTrim(const std::shared_ptr<FrameRing> &ring)
{
	if (!ring)
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (reapThread_.joinable()) {
			if (std::find(trim_.begin(), trim_.end(), ring) == trim_.end())
				trim_.push_back(ring);
			reapCv_.notify_one();
			return;
		}
	}
	ring->Arena().Trim(); // No reaper thread: inline, like before.
}

void CodecPipeline::EnsureThreadsLocked()
{
	if (!options_.threaded || threadsFailed_ || stop_)
		return;
	if (!EnsureReaperLocked()) {
		// The workers hand every ring they let go of to the reaper: no
		// workers without one.
		threadsFailed_ = true;
		Log(PipelineLogLevel::Warning, "encoding and decoding on the graphics thread instead");
		return;
	}
	try {
		if (!decodeThread_.joinable())
			decodeThread_ = std::thread(&CodecPipeline::DecodeLoop, this);
		while (encodeThreads_.size() < std::max<uint32_t>(contexts_, 1)) {
			const auto worker = static_cast<uint32_t>(encodeThreads_.size());
			encodeThreads_.emplace_back(&CodecPipeline::EncodeLoop, this, worker);
		}
	} catch (const std::system_error &error) {
		// Whatever did start keeps running (and idles); from now on G does
		// the codec work itself, exactly like the kill switch.
		threadsFailed_ = true;
		Log(PipelineLogLevel::Warning,
		    "could not start a codec worker thread (%s) -- encoding and decoding on the graphics thread instead",
		    error.what());
	}
}

void CodecPipeline::Attach(std::shared_ptr<FrameRing> ring, uint32_t maxWidth, uint32_t maxHeight,
			   uint64_t captureIntervalNs)
{
	Detach(false);
	std::vector<RawFrame *> resize;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		ring_ = std::move(ring);
		captureIntervalNs_ = std::max<uint64_t>(1, captureIntervalNs);
		rawPoolReleased_ = false;
		EnsureThreadsLocked();
		EnsureReaperLocked(); // Sync mode too: memory is never freed on the graphics thread.
		resize = freeRaw_;
		freeRaw_.clear();
	}
	// Size the RawFrames up front for the largest geometry this cycle can use,
	// so the graphics thread doesn't allocate them mid-capture (allocating is
	// cheap: no page is touched). Only frames sitting in the pool: ones still
	// out (stale jobs) grow on their next use. Smaller storage they had goes
	// to the reaper.
	const size_t need = I420FrameBytes(maxWidth, maxHeight);
	std::vector<std::shared_ptr<void>> old;
	for (RawFrame *frame : resize) {
		if (frame->capacity < need) {
			if (frame->storage)
				old.push_back(TakeStorage(*frame));
			frame->storage.reset(new (std::nothrow) uint8_t[need]);
			frame->capacity = frame->storage ? need : 0;
		}
	}
	std::vector<std::shared_ptr<void>> fallback;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		freeRaw_.insert(freeRaw_.end(), resize.begin(), resize.end());
		for (auto &storage : old)
			ReapLocked(std::move(storage), fallback);
	}
	encodeCv_.notify_all();
	decodeCv_.notify_all();
	// `fallback` (only without a reaper thread) is freed here.
}

void CodecPipeline::Detach(bool releaseCodecs, std::shared_ptr<FrameRing> retiredRing)
{
	// Only without a reaper thread (it couldn't be started): freed on the
	// caller once the lock is released, like v0.5.1 did.
	std::vector<std::shared_ptr<void>> fallback;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		++epoch_;
		EnsureReaperLocked();
		// Every reference the caller and the pipeline hold goes to the reaper
		// -- the caller's, ours, the queued jobs' -- none is dropped here: if a
		// worker had already let go of its own, the one dropped here would be
		// the last, and the graphics thread would free the whole arena (QA:
		// 31-47 ms, in 273 of 1200 cycles). A job in flight (an encode or a
		// decode) keeps its own; that worker hands it over when it's done.
		ReapLocked(std::move(retiredRing), fallback);
		ReapLocked(std::move(ring_), fallback);
		ring_.reset();
		if (releaseCodecs) {
			++releaseSerial_;
			// Disable: the RawFrame storage goes too (frames still out drop
			// theirs when they come back, see ReturnRawFrameLocked()).
			rawPoolReleased_ = true;
			for (RawFrame *frame : freeRaw_) {
				if (frame->storage)
					ReapLocked(TakeStorage(*frame), fallback);
			}
			// A setup under way stops at its next step (the new generation
			// cancels it); one ready (an open hardware encoder, a warmed-up
			// decoder) goes now.
			++setup_.generation;
			setup_.requested = false;
			setup_.failedWidth = 0;
			setup_.failedHeight = 0;
			ReapOwnedLocked(std::move(setup_.result), fallback);
			for (auto &decoder : setup_.decoders)
				ReapOwnedLocked(std::move(decoder), fallback);
		}
		for (EncodeJob &job : queue_) {
			ReapLocked(std::move(job.ring), fallback);
			ReturnRawFrameLocked(job.frame, fallback);
		}
		queue_.clear();
		for (DecodedEntry &entry : decoded_) {
			if (entry.state == DecodedEntry::State::Ready) {
				// Back to the decoder's buffer pool -- or freed, once the
				// decoder closed its context: the reaper does either.
				entry.state = DecodedEntry::State::Free;
				ReapLocked(std::move(entry.image.owner), fallback);
				entry.image = DecodedImage{};
			}
			// Decoding: the decoder sees the new epoch and frees it.
		}
		targetNs_ = 0;
		lastShownNs_ = 0;
		lastAcquireNs_ = 0; // A new cycle's first frame isn't "after a pause".
		decodeWait_ = DecodeWait::None;
		decodeWaitUntilNs_ = 0;
		++wakeSerial_;
	}
	encodeCv_.notify_all();
	decodeCv_.notify_all();
	// The sync path's own codecs (graphics thread): closed on Disable, like the
	// workers' (v0.6.0 released them here too).
	if (releaseCodecs) {
		EncodeTally tally;
		std::vector<std::shared_ptr<FrameRing>> released;
		CloseEncoder(inlineEncode_, false, tally, released);
		ResetDecoders(inlineDecode_);
		inlineDecode_.scratch = std::vector<uint8_t>();
		if (!released.empty()) {
			std::lock_guard<std::mutex> lock(mutex_);
			ReleaseRingsLocked(released, fallback);
		}
		CommitTally(tally);
	}
	for (auto &cursor : inlineDecode_.cursors)
		cursor = ringselect::ChainCursor{};
	inlineDecode_.flushPending = true;
	inlineFailedNs_ = 0;
}

void CodecPipeline::SetCaptureInterval(uint64_t captureIntervalNs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	captureIntervalNs_ = std::max<uint64_t>(1, captureIntervalNs);
}

void CodecPipeline::SetEncodeParallelism(uint32_t contexts, uint32_t slicesPerContext)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		contexts_ = std::clamp<uint32_t>(contexts, 1, Threaded() ? kMaxEncodeContexts : 1);
		slices_ = std::clamp<uint32_t>(slicesPerContext, 1, kMaxSlicesPerContext);
		EnsureThreadsLocked();
	}
	encodeCv_.notify_all();
}

void CodecPipeline::SetStorageTarget(bool inter)
{
	std::vector<std::shared_ptr<void>> fallback; // Declared first: freed after the lock is released.
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!options_.interSetup || !Threaded())
			inter = false;
		if (inter) {
			interTarget_ = true;
			interBlocked_ = false;
			// Sizes no encoder worked for are worth another try.
			setup_.failedWidth = 0;
			setup_.failedHeight = 0;
		} else if (interTarget_) {
			interTarget_ = false;
			++setup_.generation;
			setup_.requested = false;
			ReapOwnedLocked(std::move(setup_.result), fallback);
		}
	}
	encodeCv_.notify_all();
}

void CodecPipeline::SetSoftwareDecode(bool software)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (softwareDecode_ == software)
			return;
		softwareDecode_ = software;
		++wakeSerial_;
	}
	decodeCv_.notify_one();
}

bool CodecPipeline::InterWantedLocked() const
{
	return interTarget_ && !interBlocked_ && !stop_ &&
	       !(options_.interControl && options_.interControl->yielded.load(std::memory_order_relaxed));
}

uint32_t CodecPipeline::ActiveEncodeContexts() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return ActiveContextsLocked();
}

uint32_t CodecPipeline::SlicesPerContext() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return slices_;
}

RawFrame *CodecPipeline::AcquireRawFrame(uint32_t width, uint32_t height)
{
	RawFrame *frame = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (freeRaw_.empty())
			return nullptr;
		frame = freeRaw_.back();
		freeRaw_.pop_back();
	}
	const size_t need = I420FrameBytes(width, height);
	if (frame->capacity < need) {
		// Came back from a stale job of a smaller cycle, or emptied by
		// Disable: grows here. Old storage goes to the reaper.
		std::shared_ptr<void> old = frame->storage ? TakeStorage(*frame) : nullptr;
		frame->storage.reset(new (std::nothrow) uint8_t[need]);
		frame->capacity = frame->storage ? need : 0;
		std::vector<std::shared_ptr<void>> fallback;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			ReapLocked(std::move(old), fallback);
			if (!frame->storage) {
				ReturnRawFrameLocked(frame, fallback);
				frame = nullptr;
			}
		}
		if (!frame)
			return nullptr;
	}
	frame->width = width;
	frame->height = height;
	frame->plane[0] = frame->storage.get();
	frame->stride[0] = width;
	frame->plane[1] = frame->plane[0] + static_cast<size_t>(width) * height;
	frame->stride[1] = width / 2;
	frame->plane[2] = frame->plane[1] + static_cast<size_t>(width / 2) * (height / 2);
	frame->stride[2] = width / 2;
	return frame;
}

void CodecPipeline::ReleaseRawFrame(RawFrame *frame)
{
	if (!frame)
		return;
	std::vector<std::shared_ptr<void>> fallback; // Declared first: freed after the lock is released.
	std::lock_guard<std::mutex> lock(mutex_);
	ReturnRawFrameLocked(frame, fallback);
}

void CodecPipeline::NoteBackpressureDrop()
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	++window_.backpressureDrops;
}

void CodecPipeline::SubmitEncode(const std::shared_ptr<FrameRing> &ring, RawFrame *frame,
				 const FrameRing::Reservation &reservation)
{
	if (!frame)
		return;
	if (!ring) {
		ReleaseRawFrame(frame);
		return;
	}
	if (!Threaded()) {
		uint32_t slices;
		uint64_t epoch;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			slices = slices_;
			epoch = epoch_;
		}
		std::vector<std::shared_ptr<FrameRing>> released;
		EncodeOne(inlineEncode_, EncodeJob{ring, reservation, frame, epoch}, slices, kInlineStage, false,
			  released);
		ReleaseRawFrame(frame);
		if (!released.empty()) {
			std::vector<std::shared_ptr<void>> fallback; // Declared first: freed after the lock is released.
			std::lock_guard<std::mutex> lock(mutex_);
			ReleaseRingsLocked(released, fallback);
		}
		return;
	}
	size_t depth;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back(EncodeJob{ring, reservation, frame, epoch_});
		depth = queue_.size();
	}
	// notify_all: with one active context, notify_one could wake only the
	// idle second worker, which would leave the job where it is.
	encodeCv_.notify_all();
	std::lock_guard<std::mutex> lock(statsMutex_);
	window_.queueDepthMax = std::max(window_.queueDepthMax, static_cast<uint32_t>(depth));
	window_.queueDepthSum += depth;
	++window_.queueDepthSamples;
}

void CodecPipeline::StoreRaw(FrameRing &ring, const FrameRing::Reservation &reservation, const I420View &frame,
			     EncodeTally &tally)
{
	const size_t rawBytes = I420FrameBytes(frame.width, frame.height);
	FrameArena::Allocation allocation = ring.Arena().Allocate(rawBytes);
	if (!allocation) {
		ring.MarkDropped(reservation.index, reservation.generation);
		++tally.arenaDrops;
		return;
	}
	PackI420(frame, allocation.data);
	FrameRing::Payload payload;
	payload.allocation = allocation;
	payload.bytes = static_cast<uint32_t>(rawBytes);
	payload.width = frame.width;
	payload.height = frame.height;
	payload.codec = storage::CodecId::Raw; // A key of no chain: decodable on its own, references nothing.
	payload.key = true;
	payload.chain = storage::kNoChain;
	payload.seq = 0;
	if (!ring.Publish(reservation.index, reservation.generation, payload)) {
		ring.Arena().Free(allocation);
		++tally.staleEncodes;
	}
}

bool CodecPipeline::StorePacket(EncodeContext &ctx, const storage::Packet &packet, EncodeTally &tally,
				std::vector<std::shared_ptr<FrameRing>> &released)
{
	storage::InFlightQueue<InFlightFrame>::Entry entry;
	if (ctx.inFlight.Pop(packet.seq, entry) != storage::InFlightQueue<InFlightFrame>::Match::Ok)
		return false; // Out of order, or nobody sent it: the caller closes the encoder.
	std::shared_ptr<FrameRing> ringRef = std::move(entry.value.ring);
	const InFlightFrame &sent = entry.value;
	FrameRing &ring = *ringRef;
	const FrameRing::Reservation &reservation = sent.reservation;
	const bool inter = ctx.encoder && ctx.encoder->InterFrame();
	if (inter) {
		// The encoder's latency (Send() to packet): the codec policy's, never
		// the degradation policy's load (invariant I6).
		const uint64_t nowNs = options_.clockNs();
		const uint64_t latencyNs = nowNs > entry.submitNs ? nowNs - entry.submitNs : 0;
		std::lock_guard<std::mutex> lock(statsMutex_);
		window_.interLatencyNs.Push(latencyNs);
		interLatencyRecent_.Push(nowNs, latencyNs);
	}
	// Chain bookkeeping only for an inter-frame encoder's current chain: an
	// intra packet references nothing, and an older chain (a packet of a ring
	// left behind) can't break the new one.
	const bool tracked = inter && sent.chain == ctx.chain.Chain();
	const size_t rawBytes = I420FrameBytes(sent.width, sent.height);
	[&] {
		// A packet can't be trusted past the raw size (never happens with
		// MJPEG); anything odd is stored raw while the frame is still at hand
		// (a synchronous encoder), like a failed encode.
		const bool usable = packet.rawInput
					    ? sent.frame.plane[0] != nullptr && packet.bytes == rawBytes
					    : packet.data != nullptr && packet.bytes > 0 && packet.bytes <= rawBytes;
		if (!usable) {
			++tally.encodeFailures;
			if (sent.frame.plane[0]) {
				++tally.rawFallbacks;
				StoreRaw(ring, reservation, sent.frame, tally);
			} else {
				ring.MarkDropped(reservation.index, reservation.generation);
			}
			if (tracked) {
				ctx.chain.OnLost();
				++tally.chainBreaks;
			}
			return;
		}
		if (tracked && !ctx.chain.ShouldStore(packet.key)) {
			// Its reference was lost: never stored, so the decoder never
			// meets a P-frame without its reference (invariant I3).
			ring.MarkDropped(reservation.index, reservation.generation);
			++tally.orphanDrops;
			return;
		}
		const size_t storedBytes = packet.rawInput ? rawBytes : packet.bytes;
		FrameArena::Allocation allocation = ring.Arena().Allocate(storedBytes);
		if (!allocation) {
			ring.MarkDropped(reservation.index, reservation.generation);
			++tally.arenaDrops;
			if (tracked) {
				ctx.chain.OnLost();
				++tally.chainBreaks;
			}
			return;
		}
		if (packet.rawInput)
			PackI420(sent.frame, allocation.data);
		else
			std::memcpy(allocation.data, packet.data, packet.bytes);
		FrameRing::Payload payload;
		payload.allocation = allocation;
		payload.bytes = static_cast<uint32_t>(storedBytes);
		payload.width = sent.width;
		payload.height = sent.height;
		payload.codec = packet.rawInput ? storage::CodecId::Raw : packet.codec;
		payload.key = packet.rawInput || packet.key;
		payload.chain = packet.rawInput ? storage::kNoChain : sent.chain;
		payload.seq = packet.rawInput ? 0 : static_cast<uint32_t>(packet.seq);
		if (!ring.Publish(reservation.index, reservation.generation, payload)) {
			ring.Arena().Free(allocation);
			++tally.staleEncodes;
			if (tracked) {
				ctx.chain.OnLost();
				++tally.chainBreaks;
			}
			return;
		}
		if (tracked)
			ctx.chain.OnStored(packet.key);
		if (inter && packet.key)
			++tally.interKeys;
		if (payload.codec != storage::CodecId::Raw) {
			tally.encodedBytes += packet.bytes;
			tally.encodedRawBytes += rawBytes;
		}
	}();
	released.push_back(std::move(ringRef));
	return true;
}

void CodecPipeline::DropInFlight(EncodeContext &ctx, std::vector<std::shared_ptr<FrameRing>> &released)
{
	ctx.inFlight.Clear([&](storage::InFlightQueue<InFlightFrame>::Entry &entry) {
		if (entry.value.ring) {
			entry.value.ring->MarkDropped(entry.value.reservation.index,
						      entry.value.reservation.generation);
			released.push_back(std::move(entry.value.ring));
		}
	});
}

void CodecPipeline::ReceiveReady(EncodeContext &ctx, EncodeTally &tally,
				 std::vector<std::shared_ptr<FrameRing>> &released)
{
	if (!ctx.encoder)
		return;
	storage::Packet packet;
	while (ctx.encoder->Receive(packet)) {
		if (StorePacket(ctx, packet, tally, released))
			continue;
		// A packet out of Send() order (invariant I1): the bitstream can't be
		// trusted. Close the encoder; the next job opens a new one, on a new
		// chain from a key. What was in flight is lost.
		++tally.encoderResets;
		if (ctx.fromSetup) {
			char reason[64];
			std::snprintf(reason, sizeof(reason), "packet %llu out of order",
				      static_cast<unsigned long long>(packet.seq));
			FailInter(ctx, reason, tally, released);
			return;
		}
		Log(PipelineLogLevel::Warning,
		    "buffer encoder %s handed back packet %llu out of order -- closing it (%zu frames in flight lost)",
		    ctx.encoder->Name(), static_cast<unsigned long long>(packet.seq), ctx.inFlight.Size());
		++tally.encodeFailures;
		DropInFlight(ctx, released);
		ctx.encoder.reset();
		return;
	}
}

void CodecPipeline::CloseEncoder(EncodeContext &ctx, bool drain, EncodeTally &tally,
				 std::vector<std::shared_ptr<FrameRing>> &released)
{
	if (ctx.encoder && drain && ctx.encoder->InFlight() > 0) {
		bool failed = false;
		ctx.encoder->Drain(options_.clockNs() + kDrainDeadlineNs, [&](const storage::Packet &packet) {
			if (!failed && !StorePacket(ctx, packet, tally, released))
				failed = true;
		});
	}
	DropInFlight(ctx, released); // Whatever didn't make it out.
	ctx.encoder.reset();         // Closes its contexts, on this (its) thread.
	ctx.fromSetup = false;
}

void CodecPipeline::RetireInter(EncodeContext &ctx, bool inline_, EncodeTally &tally,
				std::vector<std::shared_ptr<FrameRing>> &released)
{
	if (!ctx.encoder || !ctx.fromSetup)
		return;
	if (ctx.encoder->InFlight() > 0) {
		bool failed = false;
		ctx.encoder->Drain(options_.clockNs() + kDrainDeadlineNs, [&](const storage::Packet &packet) {
			if (!failed && !StorePacket(ctx, packet, tally, released))
				failed = true;
		});
	}
	DropInFlight(ctx, released);
	std::unique_ptr<storage::Encoder> retired = std::move(ctx.encoder);
	ctx.fromSetup = false;
	++tally.codecChanges;
	Log(PipelineLogLevel::Info, "buffer encoder %s closed (%s) -- storing with the intra codec meanwhile",
	    retired->Name(), inline_ ? "yielded to an OBS output starting" : "no longer wanted for this frame size");
	{
		std::vector<std::shared_ptr<void>> fallback; // Stays empty: workers only run with a reaper.
		std::lock_guard<std::mutex> lock(mutex_);
		interActive_ = false;
		// Closing a hardware session can take tens of ms: the reaper does it,
		// unless it must be free right away (a yield: OBS opens its own next).
		if (!inline_)
			ReapOwnedLocked(std::move(retired), fallback);
	}
	retired.reset();
	encodeCv_.notify_all(); // A second intra context may run again.
}

void CodecPipeline::FailInter(EncodeContext &ctx, const char *reason, EncodeTally &tally,
			      std::vector<std::shared_ptr<FrameRing>> &released)
{
	if (!ctx.encoder)
		return;
	Log(PipelineLogLevel::Warning,
	    "buffer encoder %s failed (%s): closing it, %zu frames in flight lost -- the frames that follow are "
	    "stored with the intra codec",
	    ctx.encoder->Name(), reason, ctx.inFlight.Size());
	DropInFlight(ctx, released);
	std::unique_ptr<storage::Encoder> failed = std::move(ctx.encoder);
	const bool fromSetup = ctx.fromSetup;
	ctx.fromSetup = false;
	++tally.interHardErrors;
	++tally.codecChanges;
	++tally.encodeFailures;
	std::vector<std::shared_ptr<void>> fallback; // Stays empty: workers only run with a reaper.
	std::lock_guard<std::mutex> lock(mutex_);
	if (fromSetup) {
		interActive_ = false;
		interBlocked_ = true;
	}
	++status_.hardErrors;
	std::snprintf(status_.lastError, sizeof(status_.lastError), "%s: %s", failed->Name(), reason);
	ReapOwnedLocked(std::move(failed), fallback);
}

void CodecPipeline::CheckInterHealth(EncodeContext &ctx, EncodeTally &tally,
				     std::vector<std::shared_ptr<FrameRing>> &released)
{
	if (!ctx.encoder || !ctx.encoder->InterFrame())
		return;
	if (ctx.encoder->Broken()) {
		FailInter(ctx, "encoder error", tally, released);
		return;
	}
	// A stall: frames went in after the oldest one, and for 500 ms none came
	// out. One frame alone may wait: an encoder like AMF hands a frame's packet
	// out only once the next one is in (a render pause, say).
	const auto *oldest = ctx.inFlight.Front();
	if (oldest && ctx.inFlight.Size() >= 2 && options_.clockNs() > oldest->submitNs + kStallNs)
		FailInter(ctx, "stalled: no packet for 500 ms", tally, released);
}

void CodecPipeline::ChooseInterEncoder(EncodeContext &ctx, uint32_t width, uint32_t height, EncodeTally &tally,
				       std::vector<std::shared_ptr<FrameRing>> &released)
{
	if (ctx.encoder && ctx.fromSetup) {
		const storage::EncoderConfig &have = ctx.encoder->Config();
		if (have.width == width && have.height == height)
			return; // Keep it.
		// Another frame size (a rung change): this one hands over and goes; the
		// bridge stores frames until the setup thread has one for the new size.
		RetireInter(ctx, false, tally, released);
	}
	std::unique_ptr<storage::Encoder> adopted;
	{
		std::vector<std::shared_ptr<void>> fallback; // Stays empty: workers only run with a reaper.
		std::lock_guard<std::mutex> lock(mutex_);
		SetupState &s = setup_;
		if (s.result && (s.resultWidth != width || s.resultHeight != height))
			ReapOwnedLocked(std::move(s.result), fallback); // Set up for a size no longer wanted.
		if (s.result) {
			adopted = std::move(s.result);
			interActive_ = true;
			++status_.adoptions;
			status_.codec = adopted->Codec();
			status_.backend = adopted->GetBackend();
			std::snprintf(status_.encoder, sizeof(status_.encoder), "%s", adopted->Name());
		} else if (!(s.failedWidth == width && s.failedHeight == height) &&
			   !(s.busy && s.busyWidth == width && s.busyHeight == height) &&
			   !(s.requested && s.wantWidth == width && s.wantHeight == height) &&
			   EnsureSetupThreadLocked()) {
			s.requested = true;
			s.wantWidth = width;
			s.wantHeight = height;
			setupCv_.notify_one();
		}
	}
	if (!adopted)
		return;
	// The bridge (intra: nothing in flight past its own frame) goes; the new
	// encoder starts a chain from a key (EncodeOne()).
	CloseEncoder(ctx, true, tally, released);
	ctx.encoder = std::move(adopted);
	ctx.fromSetup = true;
	++tally.codecChanges;
	encodeCv_.notify_all(); // A second encode context, if any, stops.
}

void CodecPipeline::AddTallyLocked(const EncodeTally &tally)
{
	window_.encodedBytes += tally.encodedBytes;
	window_.encodedRawBytes += tally.encodedRawBytes;
	window_.rawFallbacks += tally.rawFallbacks;
	window_.encodeFailures += tally.encodeFailures;
	window_.arenaDrops += tally.arenaDrops;
	window_.staleEncodes += tally.staleEncodes;
	window_.chainBreaks += tally.chainBreaks;
	window_.orphanDrops += tally.orphanDrops;
	window_.busyDrops += tally.busyDrops;
	window_.encoderResets += tally.encoderResets;
	window_.inFlightMax = std::max(window_.inFlightMax, tally.inFlightMax);
	window_.interKeys += tally.interKeys;
	window_.forcedKeys += tally.forcedKeys;
	window_.bridgeFrames += tally.bridgeFrames;
	window_.codecChanges += tally.codecChanges;
	window_.interHardErrors += tally.interHardErrors;
}

void CodecPipeline::CommitTally(const EncodeTally &tally)
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	AddTallyLocked(tally);
}

void CodecPipeline::EncodeOne(EncodeContext &ctx, const EncodeJob &job, uint32_t slices, uint32_t stage,
			      bool interWanted, std::vector<std::shared_ptr<FrameRing>> &released)
{
	FrameRing &ring = *job.ring;
	const FrameRing::Reservation &reservation = job.reservation;
	if (!ring.BeginEncode(reservation.index, reservation.generation)) {
		std::lock_guard<std::mutex> lock(statsMutex_);
		++window_.staleEncodes;
		return;
	}

	const uint64_t startNs = options_.clockNs();
	const I420View view = job.frame->View();
	EncodeTally tally;
	// v0.7.0: worker 0 keeps, adopts or asks for the inter encoder of this
	// frame size (until it has one the intra codec bridges); without it
	// wanted, an inter encoder still open hands over and goes.
	const storage::Encoder *before = ctx.encoder.get();
	if (interWanted) {
		ChooseInterEncoder(ctx, view.width, view.height, tally, released);
	} else {
		if (ctx.fromSetup)
			RetireInter(ctx, options_.interControl && options_.interControl->yielded.load(), tally,
				    released);
		if (stage == 0 && options_.interSetup) {
			// One set up meanwhile (it finished as a yield began): no session held for nothing.
			std::vector<std::shared_ptr<void>> fallback; // Stays empty: workers only run with a reaper.
			std::lock_guard<std::mutex> lock(mutex_);
			ReapOwnedLocked(std::move(setup_.result), fallback);
		}
	}
	const bool adopted = ctx.fromSetup && ctx.encoder.get() != before;

	storage::EncoderConfig config;
	config.width = view.width;
	config.height = view.height;
	config.sliceThreads = slices;
	// Another frame size or slice count: a new intra encoder (the old one
	// hands over what it has in flight first), on a new chain. A new cycle
	// (new ring): the same encoder, a new chain from a key.
	if (ctx.encoder && !ctx.fromSetup && !storage::SameGeometry(ctx.encoder->Config(), config))
		CloseEncoder(ctx, true, tally, released);
	bool fresh = adopted;
	if (!ctx.encoder) {
		ctx.encoder = options_.encoderFactory(config);
		fresh = true;
	}
	if (fresh || ctx.chainEpoch != job.epoch) {
		ctx.chain.Start(storage::NewChainId());
		ctx.chainEpoch = job.epoch;
	}
	const bool inter = ctx.encoder && ctx.encoder->InterFrame();
	if (interWanted && !ctx.fromSetup)
		++tally.bridgeFrames;

	const bool forceKey = ctx.chain.WantKey();
	const uint64_t seq = ctx.chain.NextSeq();
	const uint64_t sendNs = options_.clockNs();
	const storage::SendResult sent = ctx.encoder ? ctx.encoder->Send(view, seq, forceKey)
						     : storage::SendResult::Failed;
	const uint64_t sentNs = options_.clockNs();
	uint32_t inFlightAtSend = 0;
	if (sent == storage::SendResult::Ok) {
		ctx.chain.CommitSend(forceKey);
		if (inter && forceKey)
			++tally.forcedKeys;
		InFlightFrame frame;
		frame.ring = job.ring;
		frame.reservation = reservation;
		frame.chain = ctx.chain.Chain();
		frame.width = view.width;
		frame.height = view.height;
		frame.frame = view;
		ctx.inFlight.Push(seq, sendNs, std::move(frame));
		inFlightAtSend = static_cast<uint32_t>(ctx.inFlight.Size());
		tally.inFlightMax = inFlightAtSend;
		ReceiveReady(ctx, tally, released);
		// The frame goes back to the pool once this returns: a packet still in
		// flight can no longer fall back to it.
		ctx.inFlight.ForEach(
			[](storage::InFlightQueue<InFlightFrame>::Entry &entry) { entry.value.frame = I420View{}; });
	} else if (sent == storage::SendResult::Busy) {
		ring.MarkDropped(reservation.index, reservation.generation);
		++tally.busyDrops;
	} else {
		// Not encoded: stored raw, like v0.6.0 did when the codec failed. The
		// frame never entered the bitstream (no seq consumed, nothing in
		// flight references it), but an inter-frame encoder goes on from a key.
		++tally.rawFallbacks;
		++tally.encodeFailures;
		StoreRaw(ring, reservation, view, tally);
		if (inter)
			ctx.chain.RequestKey();
	}
	if (inter)
		CheckInterHealth(ctx, tally, released); // Broken by this send, or stalled.
	const uint64_t endNs = options_.clockNs();
	// Emptied blocks beyond the spare quota (the window shrank, e.g. after a
	// lower-resolution rung) go back to the OS on the reaper: never on G (sync
	// mode included), nor on an encoder.
	if (ring.Arena().TrimWanted())
		RequestTrim(job.ring);

	{
		// The worker's own time on this frame (Send, Receive, copies,
		// Publish), never an encoder's latency: what the degradation policy
		// weighs as load (invariant I6).
		std::lock_guard<std::mutex> lock(statsMutex_);
		const uint32_t index = stage < kMaxEncodeContexts ? stage : 0;
		window_.encode[index].frameNs.Push(endNs - startNs);
		encodeRecent_.Push(endNs, endNs - startNs, reservation.tag);
		if (inter) {
			window_.interSendNs.Push(sentNs - sendNs);
			interInFlightRecent_.Push(endNs, inFlightAtSend);
		}
		AddTallyLocked(tally);
	}
}

storage::Decoder *CodecPipeline::DecoderFor(DecodeContext &ctx, storage::CodecId codec)
{
	const size_t index = CodecIndex(codec);
	if (index >= storage::kCodecCount)
		return nullptr;
	if (!ctx.decoders[index] && !ctx.unavailable[index]) {
		if (codec == storage::CodecId::Raw)
			ctx.decoders[index] = storage::MakeRawDecoder();
		else if (InterCodec(codec) && ctx.software)
			ctx.decoders[index] = options_.softwareDecoderFactory ? options_.softwareDecoderFactory(codec)
									      : nullptr;
		else if (options_.decoderFactory)
			ctx.decoders[index] = options_.decoderFactory(codec);
		ctx.unavailable[index] = !ctx.decoders[index];
	}
	return ctx.decoders[index].get();
}

void CodecPipeline::ResetDecoders(DecodeContext &ctx)
{
	for (size_t i = 0; i < storage::kCodecCount; ++i) {
		ctx.decoders[i].reset(); // Closes its context, on this (its) thread.
		ctx.unavailable[i] = false;
		ctx.cursors[i] = ringselect::ChainCursor{};
		ctx.lastUsedNs[i] = 0;
		ctx.decodeNs[i] = 0;
	}
	ctx.lastDecodeNs = 0;
	ctx.software = false;
	ctx.hardwareFailed = false;
	ctx.flushPending = false;
}

void CodecPipeline::FlushDecoders(DecodeContext &ctx)
{
	// A new cycle: nothing decoded before can be referenced any more.
	for (auto &decoder : ctx.decoders) {
		if (decoder)
			decoder->Flush();
	}
	for (auto &cursor : ctx.cursors)
		cursor = ringselect::ChainCursor{};
	ctx.flushPending = false;
}

void CodecPipeline::SyncDecodersLocked(DecodeContext &ctx, uint64_t nowNs, std::vector<std::shared_ptr<void>> &fallback)
{
	// Software decoding wanted (or a hardware decoder failed): the inter
	// codecs' decoders are replaced; their chains restart from a key.
	const bool software = softwareDecode_ || ctx.hardwareFailed;
	if (software != ctx.software) {
		ctx.software = software;
		for (storage::CodecId codec : {storage::CodecId::H264, storage::CodecId::Hevc}) {
			const size_t i = CodecIndex(codec);
			ReapOwnedLocked(std::move(ctx.decoders[i]), fallback);
			ctx.unavailable[i] = false;
			ctx.cursors[i] = ringselect::ChainCursor{};
			ctx.decodeNs[i] = software ? kSoftwareDecodeNs : 0;
		}
	}
	// A decoder the setup thread warmed up: adopted if this codec has none
	// (handed over under mutex_, invariant I8); otherwise it isn't needed.
	for (size_t i = 0; i < storage::kCodecCount; ++i) {
		if (!setup_.decoders[i])
			continue;
		if (!ctx.decoders[i] && !ctx.software) {
			ctx.decoders[i] = std::move(setup_.decoders[i]);
			ctx.unavailable[i] = false;
			ctx.cursors[i] = ringselect::ChainCursor{};
		} else {
			ReapOwnedLocked(std::move(setup_.decoders[i]), fallback);
		}
	}
	// A codec that left the ring (MJPEG after the hardware took over, the
	// hardware after a fallback): its decoder and its frame pool go.
	for (size_t i = CodecIndex(storage::CodecId::Mjpeg); i < storage::kCodecCount; ++i) {
		if (ctx.decoders[i] && ctx.lastUsedNs[i] != 0 && ctx.lastDecodeNs > ctx.lastUsedNs[i] &&
		    nowNs > ctx.lastUsedNs[i] + kDecoderIdleNs) {
			ReapOwnedLocked(std::move(ctx.decoders[i]), fallback);
			ctx.cursors[i] = ringselect::ChainCursor{};
			ctx.lastUsedNs[i] = 0;
		}
	}
	bool hardware = false;
	for (storage::CodecId codec : {storage::CodecId::H264, storage::CodecId::Hevc}) {
		const size_t i = CodecIndex(codec);
		hardware = hardware || (ctx.decoders[i] && ctx.decoders[i]->Hardware());
	}
	status_.hardwareDecode = hardware;
	status_.softwareDecode = ctx.software;
}

CodecPipeline::DecodeOutcome CodecPipeline::DecodePinned(FrameRing &ring, const FrameRing::ReadView &view,
							 DecodeContext &ctx, DecodedImage &out, bool wantImage)
{
	if (ctx.flushPending)
		FlushDecoders(ctx);
	if (!view.data || view.bytes == 0) {
		ring.Unpin(view);
		return DecodeOutcome::Failed;
	}
	const size_t index = CodecIndex(view.codec);
	if (index >= storage::kCodecCount) {
		ring.Unpin(view);
		return DecodeOutcome::Failed;
	}
	ringselect::ChainCursor &cursor = ctx.cursors[index];
	if (!ringselect::Decodable(view.key, view.chain, view.seq, cursor)) {
		// A P-frame whose reference this decoder doesn't have (it skipped
		// ahead, or that packet was lost): never decoded, the delayed view
		// holds until the next key (invariant I2). Raw and MJPEG: never.
		ring.Unpin(view);
		return DecodeOutcome::Skipped;
	}
	storage::Decoder *decoder = DecoderFor(ctx, view.codec);
	if (!decoder) {
		ring.Unpin(view);
		cursor = ringselect::ChainCursor{};
		return DecodeOutcome::Failed;
	}
	storage::EncodedFrame in;
	in.bytes = view.bytes;
	in.width = view.width;
	in.height = view.height;
	in.seq = view.seq;
	in.key = view.key;
	uint64_t outSeq = 0;
	storage::DecodeStatus status;
	const uint64_t startNs = options_.clockNs();
	if (view.codec == storage::CodecId::Raw) {
		// Its decode is one copy out of the slot: done while pinned.
		in.data = view.data;
		status = decoder->Decode(in, wantImage, out, outSeq);
		ring.Unpin(view);
	} else {
		// Copy the packet and let go of the slot before decoding: the pin only
		// lasts a memcpy (tens of microseconds), not the whole decode.
		if (ctx.scratch.size() < view.bytes)
			ctx.scratch.resize(view.bytes);
		std::memcpy(ctx.scratch.data(), view.data, view.bytes);
		ring.Unpin(view);
		in.data = ctx.scratch.data();
		status = decoder->Decode(in, wantImage, out, outSeq);
	}
	const uint64_t endNs = options_.clockNs();
	ctx.lastUsedNs[index] = endNs;
	ctx.lastDecodeNs = endNs;
	const bool ok = wantImage ? status == storage::DecodeStatus::Frame && outSeq == view.seq &&
					    out.width == view.width && out.height == view.height
				  : status != storage::DecodeStatus::Failed;
	if (!ok) {
		out = DecodedImage{};
		cursor = ringselect::ChainCursor{}; // Whatever comes next needs a key.
		decoder->Flush();
		if (decoder->Broken() && decoder->Hardware() && !ctx.hardwareFailed) {
			// Software from the next frame on (SyncDecodersLocked()), which can't
			// keep up at every size (4K60 HEVC): counted, so the codec policy
			// takes the encoder off the hardware too and the frames that follow
			// are MJPEG, which decodes fast enough.
			ctx.hardwareFailed = true;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				++status_.decoderFailures;
				std::snprintf(status_.lastError, sizeof(status_.lastError), "decoder %s failed",
					      decoder->Name());
			}
			Log(PipelineLogLevel::Warning,
			    "buffer decoder %s failed -- what's left of its frames is decoded in software",
			    decoder->Name());
		}
		return DecodeOutcome::Failed;
	}
	if (wantImage) {
		const uint64_t took = endNs - startNs;
		ctx.decodeNs[index] = ctx.decodeNs[index] ? (ctx.decodeNs[index] * 7 + took) / 8 : took;
	}
	cursor.valid = true;
	cursor.chain = view.chain;
	cursor.seq = view.seq;
	return DecodeOutcome::Decoded;
}

void CodecPipeline::SetDecodeWait(DecodeWait wait, uint64_t untilNs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	decodeWait_ = wait;
	decodeWaitUntilNs_ = wait == DecodeWait::None ? 0 : untilNs;
}

bool CodecPipeline::StartFromKey(FrameRing &ring, DecodeContext &ctx, storage::CodecId codec, uint64_t targetNs,
				 uint64_t intervalNs, uint64_t epoch, FrameRing::ReadView &view, DecodedImage &image,
				 uint64_t &cursorNs)
{
	const size_t index = CodecIndex(codec);
	const uint64_t decodeNs = ctx.decodeNs[index] ? ctx.decodeNs[index] : kDefaultDecodeNs;
	const FrameRing::DecodePlan plan = ring.PlanDecodeStart(targetNs, kDecodeJumpBack, decodeNs, intervalNs);
	if (!plan.due)
		return false;
	if (!plan.found || plan.choice == ringselect::StartChoice::WaitForKey || plan.catchUp == 0) {
		// Skip to the next key: the P-frames before it are skipped as they
		// come (not decodable), the view holds meanwhile.
		const uint64_t untilNs = plan.nextKey ? plan.dueNs + plan.framesToNextKey * intervalNs : UINT64_MAX;
		SetDecodeWait(DecodeWait::NoKey, untilNs);
		{
			std::lock_guard<std::mutex> lock(statsMutex_);
			++window_.noKeyWaits;
		}
		cursorNs = plan.dueNs;
		return false;
	}
	// Catch-up: from the key, unshown, up to the frame due (invariant I5).
	SetDecodeWait(DecodeWait::CatchUp, plan.dueNs);
	const uint64_t startNs = options_.clockNs();
	uint64_t afterNs = plan.keyNs > 0 ? plan.keyNs - 1 : 0;
	uint64_t frames = 0;
	bool shown = false;
	for (;;) {
		FrameRing::ReadView next;
		if (!ring.PinNextInRange(afterNs, plan.dueNs, next))
			break;
		afterNs = next.captureNs;
		if (next.captureNs >= plan.dueNs) {
			// The frame due: this one with its picture.
			shown = DecodePinned(ring, next, ctx, image, true) == DecodeOutcome::Decoded;
			view = next;
			break;
		}
		if (next.codec != codec) {
			ring.Unpin(next); // Another codec's frame: older than the one due, never shown.
			continue;
		}
		DecodedImage unused;
		if (DecodePinned(ring, next, ctx, unused, false) != DecodeOutcome::Decoded)
			break;
		++frames;
		std::lock_guard<std::mutex> lock(mutex_);
		if (stop_ || epoch_ != epoch)
			break;
	}
	const uint64_t tookNs = options_.clockNs() - startNs;
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		++window_.catchUps;
		window_.catchUpFrames += frames;
		window_.catchUpMaxNs = std::max(window_.catchUpMaxNs, tookNs);
	}
	if (!shown) {
		// Something in between failed or went away: wait for the next key.
		SetDecodeWait(DecodeWait::NoKey, UINT64_MAX);
		cursorNs = std::max(cursorNs, plan.dueNs);
		return false;
	}
	return true;
}

bool CodecPipeline::AcquireForDisplay(uint64_t targetNs, uint64_t lastShownNs, DecodedLease &lease)
{
	lease = DecodedLease{};
	if (!Threaded())
		return AcquireInline(targetNs, lastShownNs, lease);

	// The frame that should be on screen now: the newest Ready slot at or
	// before the target. G is the only caller of Detach(), so this copy of the
	// ring is never its last reference.
	uint64_t dueNs = 0;
	{
		std::shared_ptr<FrameRing> ring;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			ring = ring_;
		}
		if (!ring || !ring->DisplayCandidateNs(targetNs, kDisplayMaxBack, dueNs))
			dueNs = 0;
	}

	const uint64_t nowNs = options_.clockNs();

	std::array<std::shared_ptr<void>, kMaxLookaheadFrames + 2> doomed;
	size_t doomedCount = 0;
	uint32_t ahead = 0;
	uint32_t stale = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		lease.wait = decodeWait_;
		lease.waitUntilNs = decodeWaitUntilNs_;
		const uint64_t staleNs = kStaleIntervals * captureIntervalNs_;
		const bool resumed = lastAcquireNs_ != 0 && nowNs > lastAcquireNs_ + staleNs;
		lastAcquireNs_ = nowNs;
		targetNs_ = targetNs;
		lastShownNs_ = lastShownNs;
		++wakeSerial_;
		DecodedEntry *best = nullptr;
		for (DecodedEntry &entry : decoded_) {
			if (entry.state != DecodedEntry::State::Ready)
				continue;
			if (entry.epoch != epoch_ || entry.captureNs <= lastShownNs) {
				entry.state = DecodedEntry::State::Free;
				doomed[doomedCount++] = std::move(entry.image.owner);
				entry.image = DecodedImage{};
				continue;
			}
			if (entry.captureNs > targetNs)
				++ahead;
			else if (!best || entry.captureNs > best->captureNs)
				best = &entry;
		}
		// Stale: rendering just resumed after a pause, and the frames decoded
		// ahead before it are now far older than the one due: QA saw one shown
		// ~280 ms older than due right after a 300 ms pause. None of the
		// decoded frames at or before the target will ever be shown -- the due
		// one is newer -- so they all go, freeing the pool for it. Only after a
		// pause: a decoder that is merely slow (frames arrive late but fresh)
		// must keep having them shown, or the delayed view would freeze.
		if (resumed && best && dueNs > best->captureNs + staleNs) {
			for (DecodedEntry &entry : decoded_) {
				if (entry.state == DecodedEntry::State::Ready && entry.captureNs <= targetNs) {
					entry.state = DecodedEntry::State::Free;
					doomed[doomedCount++] = std::move(entry.image.owner);
					entry.image = DecodedImage{};
					++stale;
				}
			}
			best = nullptr;
		}
		if (best) {
			// Anything older than the frame shown now will never be shown.
			for (DecodedEntry &entry : decoded_) {
				if (&entry != best && entry.state == DecodedEntry::State::Ready &&
				    entry.captureNs < best->captureNs) {
					entry.state = DecodedEntry::State::Free;
					doomed[doomedCount++] = std::move(entry.image.owner);
					entry.image = DecodedImage{};
				}
			}
			lease.valid = true;
			lease.captureNs = best->captureNs;
			lease.image = std::move(best->image);
			best->image = DecodedImage{};
			best->state = DecodedEntry::State::Free;
		}
	}
	decodeCv_.notify_one();
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		window_.aheadSum += ahead;
		++window_.aheadSamples;
		window_.aheadMin = std::min(window_.aheadMin, ahead);
		window_.staleDecoded += stale;
	}
	// `doomed` released here, outside both locks.
	return lease.valid;
}

bool CodecPipeline::AcquireInline(uint64_t targetNs, uint64_t lastShownNs, DecodedLease &lease)
{
	std::shared_ptr<FrameRing> ring;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		ring = ring_;
	}
	if (!ring)
		return false;
	FrameRing::ReadView view;
	if (!ring->PinForDisplay(targetNs, kDisplayMaxBack, view))
		return false;
	// Already on screen, or already failed once: don't decode it again on
	// every call (Render() runs once per view of the scene).
	if (view.captureNs <= lastShownNs || view.captureNs == inlineFailedNs_) {
		ring->Unpin(view);
		return false;
	}
	const uint64_t startNs = options_.clockNs();
	DecodedImage image;
	const DecodeOutcome outcome = DecodePinned(*ring, view, inlineDecode_, image);
	const uint64_t endNs = options_.clockNs();
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		if (outcome == DecodeOutcome::Skipped) {
			++window_.noKeySkips;
		} else {
			window_.decode.frameNs.Push(endNs - startNs);
			decodeRecent_.Push(endNs, endNs - startNs, DecodeTag(view));
			if (outcome == DecodeOutcome::Failed)
				++window_.decodeFailures;
		}
	}
	if (outcome != DecodeOutcome::Decoded) {
		inlineFailedNs_ = view.captureNs;
		return false;
	}
	lease.valid = true;
	lease.captureNs = view.captureNs;
	lease.image = std::move(image);
	return true;
}

void CodecPipeline::ReleaseDisplay(DecodedLease &lease)
{
	lease = DecodedLease{};
}

void CodecPipeline::PrimeDecode(uint64_t targetNs)
{
	if (!Threaded())
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (targetNs_ == targetNs)
			return;
		targetNs_ = targetNs;
		++wakeSerial_;
	}
	decodeCv_.notify_one();
}

void CodecPipeline::EncodeLoop(uint32_t worker)
{
	if (options_.nameThread) {
		char name[32];
		std::snprintf(name, sizeof(name), "trigglow-enc-%u", worker);
		options_.nameThread(name);
	}
	EncodeContext ctx;
	std::unique_lock<std::mutex> lock(mutex_);
	uint64_t handledRelease = releaseSerial_;
	std::vector<std::shared_ptr<void>> fallback; // Stays empty: workers only run with a reaper.
	std::vector<std::shared_ptr<FrameRing>> released;
	// Under the lock: lets go of the rings the last step was done with.
	auto settle = [&] {
		ReleaseRingsLocked(released, fallback);
		if (!fallback.empty()) {
			lock.unlock();
			fallback.clear();
			lock.lock();
		}
	};
	// An inter encoder still open that's no longer wanted (an intra target, a
	// yield): worker 0 retires it even with no job to do.
	auto interUnwanted = [&] {
		return worker == 0 && ctx.fromSetup && !InterWantedLocked();
	};
	// An encoder the setup thread left in the mailbox that's no longer wanted
	// (a yield while rendering is paused, so no job comes to adopt or drop
	// it): worker 0 closes it, so its session is free within the yield's wait.
	auto resultUnwanted = [&] {
		return worker == 0 && setup_.result && !InterWantedLocked();
	};
	auto ready = [&] {
		const uint32_t active = ActiveContextsLocked();
		return stop_ || releaseSerial_ != handledRelease || (worker < active && !queue_.empty()) ||
		       (worker >= active && ctx.encoder != nullptr) || interUnwanted() || resultUnwanted();
	};
	auto yielded = [&] {
		return options_.interControl && options_.interControl->yielded.load();
	};
	for (;;) {
		if (ctx.inFlight.Empty()) {
			encodeCv_.wait(lock, ready);
		} else if (!encodeCv_.wait_for(lock, std::chrono::nanoseconds(kInFlightPollNs), ready)) {
			// Frames in flight and nothing else to do: store what came out.
			lock.unlock();
			EncodeTally tally;
			ReceiveReady(ctx, tally, released);
			CheckInterHealth(ctx, tally, released);
			CommitTally(tally);
			lock.lock();
			settle();
			++wakeSerial_; // A slot may have turned Ready: the decoder may be waiting for it.
			decodeCv_.notify_one();
			continue;
		}
		if (stop_)
			break;
		if (releaseSerial_ != handledRelease || (worker >= ActiveContextsLocked() && ctx.encoder)) {
			// Disable, or this context is no longer needed: close it (on the
			// thread that opened it). Reopened lazily by the next job. On
			// Disable what's in flight belongs to a ring being thrown away;
			// otherwise it's handed over first.
			const bool disable = releaseSerial_ != handledRelease;
			handledRelease = releaseSerial_;
			if (ctx.encoder || !ctx.inFlight.Empty()) {
				lock.unlock();
				EncodeTally tally;
				CloseEncoder(ctx, !disable, tally, released);
				CommitTally(tally);
				lock.lock();
				if (worker == 0)
					interActive_ = false;
				settle();
			}
			continue;
		}
		if (interUnwanted()) {
			const bool inline_ = yielded();
			lock.unlock();
			EncodeTally tally;
			RetireInter(ctx, inline_, tally, released);
			CommitTally(tally);
			lock.lock();
			settle();
			continue;
		}
		if (resultUnwanted()) {
			std::unique_ptr<storage::Encoder> unwanted = std::move(setup_.result);
			lock.unlock();
			unwanted.reset(); // Closed on this thread, now (handed over under the lock).
			lock.lock();
			continue;
		}
		if (worker >= ActiveContextsLocked() || queue_.empty())
			continue;
		EncodeJob job = std::move(queue_.front());
		queue_.pop_front();
		const uint32_t slices = slices_;
		const bool interWanted = worker == 0 && InterWantedLocked() && options_.interSetup != nullptr;
		lock.unlock();
		EncodeOne(ctx, job, slices, worker, interWanted, released);
		lock.lock();
		released.push_back(std::move(job.ring));
		ReturnRawFrameLocked(job.frame, fallback);
		settle();
		++wakeSerial_;
		decodeCv_.notify_one();
	}
	lock.unlock();
	EncodeTally tally;
	CloseEncoder(ctx, false, tally, released);
	// `released` goes here, on this thread (shutting down: never G).
}

void CodecPipeline::SetupLoop()
{
	if (options_.nameThread)
		options_.nameThread("trigglow-codec-setup");
	std::unique_lock<std::mutex> lock(mutex_);
	for (;;) {
		setupCv_.wait(lock, [&] { return stop_ || setup_.requested; });
		if (stop_)
			break;
		SetupState &s = setup_;
		s.requested = false;
		s.busy = true;
		s.busyWidth = s.wantWidth;
		s.busyHeight = s.wantHeight;
		const uint64_t generation = s.generation;
		storage::EncoderConfig config;
		config.width = s.busyWidth;
		config.height = s.busyHeight;
		lock.unlock();
		// The setup asks this between its steps: Disable (a new generation),
		// shutdown, an intra target or a yield stop it within one step instead
		// of after a whole probe (up to about a second per encoder tried).
		const storage::SetupCancelled cancelled = [this, generation] {
			std::lock_guard<std::mutex> guard(mutex_);
			return stop_ || generation != setup_.generation || !InterWantedLocked();
		};
		storage::InterSession session;
		try {
			session = options_.interSetup(config, cancelled);
		} catch (...) {
			session = storage::InterSession{}; // Treated as no encoder working.
		}
		lock.lock();
		s.busy = false;
		std::vector<std::shared_ptr<void>> fallback;
		if (session.cancelled || stop_ || generation != s.generation || !InterWantedLocked()) {
			// No longer wanted (an intra target, a yield, Disable, shutdown):
			// closed right here, so its hardware session is gone at once (a
			// yield waits for that), and never counted as a failure. A setup
			// cancelled by a yield that is over by now is asked for again by
			// the next frame.
			lock.unlock();
			session.encoder.reset();
			session.decoder.reset();
			lock.lock();
		} else if (session.encoder) {
			ReapOwnedLocked(std::move(s.result), fallback);
			s.result = std::move(session.encoder);
			s.resultWidth = config.width;
			s.resultHeight = config.height;
			s.failedWidth = 0;
			s.failedHeight = 0;
			if (session.decoder) {
				const size_t i = CodecIndex(session.decoder->Codec());
				if (i < storage::kCodecCount) {
					ReapOwnedLocked(std::move(s.decoders[i]), fallback);
					s.decoders[i] = std::move(session.decoder);
				} else {
					ReapOwnedLocked(std::move(session.decoder), fallback);
				}
			}
		} else {
			ReapOwnedLocked(std::move(session.decoder), fallback);
			s.failedWidth = config.width;
			s.failedHeight = config.height;
			++status_.setupFailures;
		}
		++wakeSerial_;
		if (!fallback.empty()) {
			lock.unlock();
			fallback.clear();
			lock.lock();
		}
		decodeCv_.notify_one();
		encodeCv_.notify_all();
	}
}

void CodecPipeline::DecodeLoop()
{
	if (options_.nameThread)
		options_.nameThread("trigglow-dec");
	DecodeContext ctx;
	bool codecUsed = false;
	uint64_t cursorNs = 0;
	uint64_t cursorEpoch = 0;
	bool waitingForKey = false; // StartFromKey() chose to skip to the next key; a key decoded ends it.
	std::vector<std::shared_ptr<void>> fallback; // Stays empty: workers only run with a reaper.
	std::unique_lock<std::mutex> lock(mutex_);
	uint64_t handledRelease = releaseSerial_;
	while (!stop_) {
		if (releaseSerial_ != handledRelease) {
			handledRelease = releaseSerial_;
			if (codecUsed) {
				lock.unlock();
				ResetDecoders(ctx);
				ctx.scratch = std::vector<uint8_t>();
				codecUsed = false;
				lock.lock();
			}
			continue;
		}
		if (epoch_ != cursorEpoch) {
			cursorEpoch = epoch_;
			cursorNs = 0;
			waitingForKey = false;
			ctx.flushPending = true;
		}
		SyncDecodersLocked(ctx, options_.clockNs(), fallback);
		const uint64_t targetNs = targetNs_;
		if (!ring_ || targetNs == 0) {
			const uint64_t serial = wakeSerial_;
			decodeCv_.wait(lock, [&] {
				return stop_ || wakeSerial_ != serial || releaseSerial_ != handledRelease;
			});
			continue;
		}
		const uint64_t intervalNs = captureIntervalNs_;
		const uint64_t lastShownNs = lastShownNs_;
		DecodedEntry *entry = nullptr;
		uint32_t ahead = 0;
		for (DecodedEntry &candidate : decoded_) {
			if (candidate.state == DecodedEntry::State::Ready &&
			    (candidate.epoch != epoch_ || candidate.captureNs <= lastShownNs)) {
				candidate.state = DecodedEntry::State::Free; // Shown already (or stale): recycle.
				candidate.image = DecodedImage{};
			}
			if (candidate.state == DecodedEntry::State::Ready)
				++ahead;
			if (!entry && candidate.state == DecodedEntry::State::Free)
				entry = &candidate;
		}
		// The lookahead is a frame COUNT, not a time window: frames are
		// decoded in capture order, and the decoder stays at most `lookahead`
		// frames ahead of what's on screen. A time window (target + L
		// intervals) would starve whenever captures come further apart than
		// the nominal interval (OBS rendering slower than its fps).
		if (!entry || ahead >= options_.lookaheadFrames) {
			// Enough decoded ahead: wait for G to consume one.
			const uint64_t serial = wakeSerial_;
			decodeCv_.wait_for(lock, std::chrono::nanoseconds(intervalNs), [&] {
				return stop_ || wakeSerial_ != serial || releaseSerial_ != handledRelease;
			});
			continue;
		}
		entry->state = DecodedEntry::State::Decoding;
		entry->epoch = epoch_;
		const uint64_t epoch = epoch_;
		const uint64_t serial = wakeSerial_;
		std::shared_ptr<FrameRing> ring = ring_;
		lock.unlock();

		if (ctx.flushPending)
			FlushDecoders(ctx);
		const bool starting = cursorNs == 0;
		const bool late = !starting && cursorNs + intervalNs < targetNs;
		FrameRing::ReadView view;
		bool found = ring->PinNextForDecode(cursorNs, targetNs, UINT64_MAX, intervalNs, view);
		DecodedImage image;
		DecodeOutcome outcome = DecodeOutcome::Failed;
		bool skipAhead = false; // Waiting for the next key: go on at once, no sleep.
		if (found &&
		    !ringselect::Decodable(view.key, view.chain, view.seq,
					   ctx.cursors[std::min(CodecIndex(view.codec), storage::kCodecCount - 1)])) {
			// It (re)starts on a P-frame it has no reference for -- it just
			// started, it jumped, or its decoder lost them (a switch to the
			// software decoder, a failure): from the key, or the next one
			// (invariant I5). When it started or jumped, for the frame due;
			// otherwise for the frame it landed on, ahead of the target.
			const storage::CodecId codec = view.codec;
			const uint64_t landedNs = view.captureNs;
			ring->Unpin(view);
			codecUsed = true;
			const uint64_t planNs = starting || late ? targetNs : landedNs;
			if (waitingForKey && !starting && !late) {
				// Already skipping to the next key: no new plan per P-frame.
				found = false;
				skipAhead = true;
				cursorNs = std::max(cursorNs, landedNs);
				std::lock_guard<std::mutex> statsLock(statsMutex_);
				++window_.noKeySkips;
			} else if (StartFromKey(*ring, ctx, codec, planNs, intervalNs, epoch, view, image, cursorNs)) {
				outcome = DecodeOutcome::Decoded;
				waitingForKey = false;
			} else {
				found = false;
				skipAhead = true;
				waitingForKey = true;
				cursorNs = std::max(cursorNs, landedNs);
			}
			if (late) {
				std::lock_guard<std::mutex> statsLock(statsMutex_);
				++window_.decodeJumps;
			}
		} else if (found) {
			const uint64_t startNs = options_.clockNs();
			outcome = DecodePinned(*ring, view, ctx, image);
			const uint64_t endNs = options_.clockNs();
			codecUsed = true;
			std::lock_guard<std::mutex> statsLock(statsMutex_);
			if (outcome == DecodeOutcome::Skipped) {
				// Nothing was decoded: no decode time to weigh.
				++window_.noKeySkips;
			} else {
				window_.decode.frameNs.Push(endNs - startNs);
				decodeRecent_.Push(endNs, endNs - startNs, DecodeTag(view));
				if (outcome == DecodeOutcome::Failed)
					++window_.decodeFailures;
			}
			if (late)
				++window_.decodeJumps;
		}
		lock.lock();
		// Never the last reference while it's still the pipeline's ring;
		// detached meanwhile, the reaper takes it (see ReleaseRingsLocked()).
		if (ring == ring_) {
			ring.reset();
		} else {
			ReapLocked(std::move(ring), fallback);
			if (!fallback.empty()) {
				lock.unlock();
				fallback.clear();
				lock.lock();
			}
		}
		if (!found) {
			entry->state = DecodedEntry::State::Free;
			if (skipAhead)
				continue;
			// Nothing decodable yet: sleep until G publishes a new target, an
			// encoder publishes a slot, or one interval passes.
			decodeCv_.wait_for(lock, std::chrono::nanoseconds(intervalNs), [&] {
				return stop_ || wakeSerial_ != serial || releaseSerial_ != handledRelease;
			});
			continue;
		}
		cursorNs = view.captureNs;
		if (outcome == DecodeOutcome::Decoded && view.key)
			waitingForKey = false;
		if (outcome == DecodeOutcome::Decoded && decodeWait_ != DecodeWait::None &&
		    ((decodeWait_ == DecodeWait::CatchUp && view.captureNs >= decodeWaitUntilNs_) || view.key)) {
			decodeWait_ = DecodeWait::None; // A fresh frame again.
			decodeWaitUntilNs_ = 0;
		}
		if (outcome == DecodeOutcome::Decoded && epoch_ == epoch) {
			entry->state = DecodedEntry::State::Ready;
			entry->captureNs = view.captureNs;
			entry->image = std::move(image);
		} else {
			entry->state = DecodedEntry::State::Free;
			entry->image = DecodedImage{};
		}
	}
	lock.unlock();
	ResetDecoders(ctx);
}

void CodecPipeline::ReapLoop()
{
	if (options_.nameThread)
		options_.nameThread("trigglow-reap");
	std::unique_lock<std::mutex> lock(mutex_);
	for (;;) {
		reapCv_.wait(lock, [&] { return stop_ || !reap_.empty() || !trim_.empty(); });
		if (reap_.empty() && trim_.empty())
			break; // Stopping, with nothing left to free.
		std::vector<std::shared_ptr<FrameRing>> trim;
		trim.swap(trim_);
		std::vector<std::shared_ptr<void>> doomed;
		doomed.swap(reap_);
		lock.unlock();
		for (const auto &ring : trim)
			ring->Arena().Trim();
		// The frees (and the OS calls behind them) happen here, on this thread.
		trim.clear();
		doomed.clear();
		lock.lock();
	}
}

void CodecPipeline::TakeWindowStats(WindowStats &out)
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	out = window_;
	for (Stage &stage : window_.encode)
		stage.frameNs.Reset();
	window_.decode.frameNs.Reset();
	window_.encodedBytes = 0;
	window_.encodedRawBytes = 0;
	window_.rawFallbacks = 0;
	window_.encodeFailures = 0;
	window_.decodeFailures = 0;
	window_.arenaDrops = 0;
	window_.backpressureDrops = 0;
	window_.staleEncodes = 0;
	window_.decodeJumps = 0;
	window_.staleDecoded = 0;
	window_.chainBreaks = 0;
	window_.orphanDrops = 0;
	window_.busyDrops = 0;
	window_.encoderResets = 0;
	window_.noKeySkips = 0;
	window_.inFlightMax = 0;
	window_.interLatencyNs.Reset();
	window_.interSendNs.Reset();
	window_.interKeys = 0;
	window_.forcedKeys = 0;
	window_.bridgeFrames = 0;
	window_.codecChanges = 0;
	window_.interHardErrors = 0;
	window_.catchUps = 0;
	window_.catchUpFrames = 0;
	window_.catchUpMaxNs = 0;
	window_.noKeyWaits = 0;
	window_.queueDepthMax = 0;
	window_.queueDepthSum = 0;
	window_.queueDepthSamples = 0;
	window_.aheadSum = 0;
	window_.aheadSamples = 0;
	window_.aheadMin = UINT32_MAX;
}

CodecPipeline::RecentLoad CodecPipeline::GetRecentLoad(uint64_t nowNs, uint64_t windowNs, uint32_t encodeTag) const
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	RecentLoad load;
	load.encode = encodeRecent_.Summarize(nowNs, windowNs, encodeTag);
	load.decodeTag = decodeRecent_.NewestTag(nowNs);
	if (load.decodeTag != RollingWindow::kAnyTag)
		load.decode = decodeRecent_.Summarize(nowNs, windowNs, load.decodeTag);
	return load;
}

CodecPipeline::InterLoad CodecPipeline::GetInterLoad(uint64_t nowNs, uint64_t windowNs) const
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	InterLoad load;
	load.latency = interLatencyRecent_.Summarize(nowNs, windowNs);
	// Frames in flight were pushed as the samples' values: maxMs is the count / 1e6.
	const RollingWindow::Summary inFlight = interInFlightRecent_.Summarize(nowNs, windowNs);
	load.inFlightMax = static_cast<uint32_t>(inFlight.maxMs * 1e6 + 0.5);
	return load;
}

CodecPipeline::CodecStatus CodecPipeline::GetCodecStatus() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	CodecStatus status = status_;
	status.interWanted = InterWantedLocked() && options_.interSetup != nullptr;
	status.interActive = interActive_;
	status.setupBusy = setup_.busy || setup_.requested;
	return status;
}

void CodecPipeline::ResetRecentLoad()
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	encodeRecent_.Clear();
	decodeRecent_.Clear();
	interLatencyRecent_.Clear();
	interInFlightRecent_.Clear();
}

void CodecPipeline::Shutdown()
{
	if (options_.interControl)
		options_.interControl->SetWake(nullptr); // Before anything goes away: the UI may call it.
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	encodeCv_.notify_all();
	decodeCv_.notify_all();
	setupCv_.notify_all();
	for (std::thread &thread : encodeThreads_) {
		if (thread.joinable())
			thread.join();
	}
	encodeThreads_.clear();
	if (decodeThread_.joinable())
		decodeThread_.join();
	// A setup under way stops at its next step (stop_ cancels it) and closes
	// what it opened on its own thread.
	if (setupThread_.joinable())
		setupThread_.join();
	// Last: it frees whatever the workers handed it on their way out.
	reapCv_.notify_all();
	if (reapThread_.joinable())
		reapThread_.join();

	// The rest is freed on this thread (OBS's destruction thread).
	std::deque<EncodeJob> dropped;
	std::vector<std::shared_ptr<void>> doomed;
	std::vector<std::shared_ptr<FrameRing>> trimmed;
	std::unique_ptr<storage::Encoder> setupResult;
	std::unique_ptr<storage::Decoder> setupDecoders[storage::kCodecCount];
	{
		std::lock_guard<std::mutex> lock(mutex_);
		dropped.swap(queue_);
		for (EncodeJob &job : dropped)
			freeRaw_.push_back(job.frame);
		doomed.swap(reap_);
		trimmed.swap(trim_);
		doomed.push_back(std::move(ring_));
		ring_.reset();
		for (DecodedEntry &entry : decoded_) {
			entry.state = DecodedEntry::State::Free;
			entry.image = DecodedImage{};
		}
		setupResult = std::move(setup_.result);
		for (size_t i = 0; i < storage::kCodecCount; ++i)
			setupDecoders[i] = std::move(setup_.decoders[i]);
	}
	EncodeTally tally;
	std::vector<std::shared_ptr<FrameRing>> released;
	CloseEncoder(inlineEncode_, false, tally, released);
	ResetDecoders(inlineDecode_);
}

} // namespace trigglow
