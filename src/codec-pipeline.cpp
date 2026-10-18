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

class RawCodec final : public VideoCodec {
public:
	bool Encode(const I420View &frame, uint32_t /*sliceThreads*/, const uint8_t *&packet, size_t &bytes,
		    bool &compressed) override
	{
		// The pipeline packs the frame into the arena itself: no copy here.
		packet = nullptr;
		bytes = I420FrameBytes(frame.width, frame.height);
		compressed = false;
		return true;
	}
	bool Decode(const uint8_t * /*packet*/, size_t /*bytes*/, DecodedImage & /*out*/) override { return false; }
	void Release() override {}
};

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
} // namespace

size_t I420FrameBytes(uint32_t width, uint32_t height)
{
	const size_t luma = static_cast<size_t>(width) * height;
	const size_t chroma = static_cast<size_t>(width / 2) * (height / 2);
	return luma + 2 * chroma;
}

void PackI420(const I420View &frame, uint8_t *dst)
{
	const uint32_t widths[3] = {frame.width, frame.width / 2, frame.width / 2};
	const uint32_t heights[3] = {frame.height, frame.height / 2, frame.height / 2};
	for (int p = 0; p < 3; ++p) {
		for (uint32_t row = 0; row < heights[p]; ++row) {
			std::memcpy(dst, frame.plane[p] + static_cast<size_t>(row) * frame.stride[p], widths[p]);
			dst += widths[p];
		}
	}
}

I420View PackedI420View(const uint8_t *data, uint32_t width, uint32_t height)
{
	I420View view;
	view.width = width;
	view.height = height;
	view.plane[0] = data;
	view.stride[0] = width;
	view.plane[1] = data + static_cast<size_t>(width) * height;
	view.stride[1] = width / 2;
	view.plane[2] = view.plane[1] + static_cast<size_t>(width / 2) * (height / 2);
	view.stride[2] = width / 2;
	return view;
}

std::unique_ptr<VideoCodec> MakeRawCodec()
{
	return std::make_unique<RawCodec>();
}

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
	if (!options_.codecFactory)
		options_.codecFactory = &MakeRawCodec;
	for (uint32_t i = 0; i < options_.rawFramePoolSize; ++i) {
		rawFrames_.push_back(std::make_unique<RawFrame>());
		freeRaw_.push_back(rawFrames_.back().get());
	}
	decoded_.resize(options_.lookaheadFrames + 2);
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
		++wakeSerial_;
	}
	encodeCv_.notify_all();
	decodeCv_.notify_all();
	if (releaseCodecs && inlineCodec_) {
		inlineCodec_->Release();
		inlineScratch_ = std::vector<uint8_t>();
	}
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

uint32_t CodecPipeline::ActiveEncodeContexts() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return contexts_;
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
		if (!inlineCodec_)
			inlineCodec_ = options_.codecFactory();
		uint32_t slices;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			slices = slices_;
		}
		EncodeOne(*inlineCodec_, EncodeJob{ring, reservation, frame}, slices, kInlineStage);
		ReleaseRawFrame(frame);
		return;
	}
	size_t depth;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back(EncodeJob{ring, reservation, frame});
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

void CodecPipeline::EncodeOne(VideoCodec &codec, const EncodeJob &job, uint32_t slices, uint32_t stage)
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
	const size_t rawBytes = I420FrameBytes(view.width, view.height);
	const uint8_t *packet = nullptr;
	size_t bytes = 0;
	bool compressed = false;
	const bool encoded = codec.Encode(view, slices, packet, bytes, compressed);
	// A packet can't be trusted past the raw size (never happens with MJPEG);
	// anything odd is stored raw, like a failed encode.
	const bool usable = encoded && (!compressed || (packet && bytes > 0 && bytes <= rawBytes));
	const bool storeRaw = !usable || !compressed;
	const size_t storedBytes = storeRaw ? rawBytes : bytes;

	FrameArena::Allocation allocation = ring.Arena().Allocate(storedBytes);
	bool published = false;
	if (!allocation) {
		ring.MarkDropped(reservation.index, reservation.generation);
	} else {
		if (storeRaw)
			PackI420(view, allocation.data);
		else
			std::memcpy(allocation.data, packet, bytes);
		FrameRing::Payload payload;
		payload.allocation = allocation;
		payload.bytes = static_cast<uint32_t>(storedBytes);
		payload.width = view.width;
		payload.height = view.height;
		payload.compressed = !storeRaw;
		published = ring.Publish(reservation.index, reservation.generation, payload);
		if (!published)
			ring.Arena().Free(allocation);
	}
	const uint64_t endNs = options_.clockNs();
	// Emptied blocks beyond the spare quota (the window shrank, e.g. after a
	// lower-resolution rung) go back to the OS on the reaper: never on G (sync
	// mode included), nor on an encoder.
	if (ring.Arena().TrimWanted())
		RequestTrim(job.ring);

	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		const uint32_t index = stage < kMaxEncodeContexts ? stage : 0;
		window_.encode[index].frameNs.Push(endNs - startNs);
		encodeRecent_.Push(endNs, endNs - startNs);
		if (published && !storeRaw) {
			window_.encodedBytes += bytes;
			window_.encodedRawBytes += rawBytes;
		}
		if (!usable) {
			++window_.rawFallbacks;
			++window_.encodeFailures;
		}
		if (!allocation)
			++window_.arenaDrops;
		else if (!published)
			++window_.staleEncodes;
	}
}

bool CodecPipeline::DecodePinned(FrameRing &ring, const FrameRing::ReadView &view, VideoCodec &codec,
				 std::vector<uint8_t> &scratch, DecodedImage &out)
{
	bool ok = view.data != nullptr && view.bytes > 0;
	if (ok && view.compressed) {
		// Copy the packet and let go of the slot before decoding: the pin only
		// lasts a memcpy (tens of microseconds), not the whole decode.
		if (scratch.size() < view.bytes)
			scratch.resize(view.bytes);
		std::memcpy(scratch.data(), view.data, view.bytes);
	} else if (ok) {
		const size_t need = I420FrameBytes(view.width, view.height);
		ok = view.bytes >= need;
		if (ok) {
			std::shared_ptr<uint8_t> buffer(new (std::nothrow) uint8_t[need],
							std::default_delete<uint8_t[]>());
			ok = buffer != nullptr;
			if (ok) {
				std::memcpy(buffer.get(), view.data, need);
				const I420View packed = PackedI420View(buffer.get(), view.width, view.height);
				for (int p = 0; p < 3; ++p) {
					out.plane[p] = packed.plane[p];
					out.stride[p] = packed.stride[p];
				}
				out.width = view.width;
				out.height = view.height;
				out.owner = buffer;
			}
		}
	}
	ring.Unpin(view);
	if (ok && view.compressed) {
		ok = codec.Decode(scratch.data(), view.bytes, out) && out.width == view.width &&
		     out.height == view.height;
		if (!ok)
			out = DecodedImage{};
	}
	return ok;
}

bool CodecPipeline::AcquireForDisplay(uint64_t targetNs, uint64_t lastShownNs, DecodedLease &lease)
{
	lease = DecodedLease{};
	if (!Threaded())
		return AcquireInline(targetNs, lastShownNs, lease);

	std::array<std::shared_ptr<void>, kMaxLookaheadFrames + 2> doomed;
	size_t doomedCount = 0;
	uint32_t ahead = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
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
	if (!inlineCodec_)
		inlineCodec_ = options_.codecFactory();
	const uint64_t startNs = options_.clockNs();
	DecodedImage image;
	const bool ok = DecodePinned(*ring, view, *inlineCodec_, inlineScratch_, image);
	const uint64_t endNs = options_.clockNs();
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		window_.decode.frameNs.Push(endNs - startNs);
		decodeRecent_.Push(endNs, endNs - startNs);
		if (!ok)
			++window_.decodeFailures;
	}
	if (!ok) {
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
	std::unique_ptr<VideoCodec> codec;
	bool codecUsed = false;
	std::unique_lock<std::mutex> lock(mutex_);
	uint64_t handledRelease = releaseSerial_;
	std::vector<std::shared_ptr<void>> fallback; // Stays empty: workers only run with a reaper.
	for (;;) {
		encodeCv_.wait(lock, [&] {
			return stop_ || releaseSerial_ != handledRelease || (worker < contexts_ && !queue_.empty()) ||
			       (worker >= contexts_ && codecUsed);
		});
		if (stop_)
			break;
		if (releaseSerial_ != handledRelease || (worker >= contexts_ && codecUsed)) {
			// Disable, or this context is no longer needed: close it (on the
			// thread that opened it). Reopened lazily by the next job.
			handledRelease = releaseSerial_;
			if (codecUsed) {
				lock.unlock();
				codec->Release();
				codecUsed = false;
				lock.lock();
			}
			continue;
		}
		if (worker >= contexts_ || queue_.empty())
			continue;
		EncodeJob job = std::move(queue_.front());
		queue_.pop_front();
		const uint32_t slices = slices_;
		lock.unlock();
		if (!codec)
			codec = options_.codecFactory();
		EncodeOne(*codec, job, slices, worker);
		codecUsed = true;
		lock.lock();
		// Still the pipeline's ring: ring_ holds it too, so this is never the
		// last reference. Detached meanwhile: it may be, and freeing an arena
		// takes tens of ms -- the reaper does that, not the next encode.
		if (job.ring == ring_)
			job.ring.reset();
		else
			ReapLocked(std::move(job.ring), fallback);
		ReturnRawFrameLocked(job.frame, fallback);
		++wakeSerial_;
		decodeCv_.notify_one();
		if (!fallback.empty()) {
			lock.unlock();
			fallback.clear();
			lock.lock();
		}
	}
	lock.unlock();
	if (codec)
		codec->Release();
}

void CodecPipeline::DecodeLoop()
{
	if (options_.nameThread)
		options_.nameThread("trigglow-dec");
	std::unique_ptr<VideoCodec> codec;
	bool codecUsed = false;
	std::vector<uint8_t> scratch;
	uint64_t cursorNs = 0;
	uint64_t cursorEpoch = 0;
	std::unique_lock<std::mutex> lock(mutex_);
	uint64_t handledRelease = releaseSerial_;
	while (!stop_) {
		if (releaseSerial_ != handledRelease) {
			handledRelease = releaseSerial_;
			if (codecUsed) {
				lock.unlock();
				codec->Release();
				codecUsed = false;
				scratch = std::vector<uint8_t>();
				lock.lock();
			}
			continue;
		}
		if (epoch_ != cursorEpoch) {
			cursorEpoch = epoch_;
			cursorNs = 0;
		}
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

		const bool late = cursorNs != 0 && cursorNs + intervalNs < targetNs;
		FrameRing::ReadView view;
		const bool found = ring->PinNextForDecode(cursorNs, targetNs, UINT64_MAX, intervalNs, view);
		DecodedImage image;
		bool ok = false;
		uint64_t elapsedNs = 0;
		if (found) {
			if (!codec)
				codec = options_.codecFactory();
			const uint64_t startNs = options_.clockNs();
			ok = DecodePinned(*ring, view, *codec, scratch, image);
			const uint64_t endNs = options_.clockNs();
			elapsedNs = endNs - startNs;
			codecUsed = true;
			std::lock_guard<std::mutex> statsLock(statsMutex_);
			window_.decode.frameNs.Push(elapsedNs);
			decodeRecent_.Push(endNs, elapsedNs);
			if (!ok)
				++window_.decodeFailures;
			if (late)
				++window_.decodeJumps;
		}
		lock.lock();
		// Never the last reference while it's still the pipeline's ring;
		// detached meanwhile, the reaper takes it (see EncodeLoop()).
		if (ring == ring_) {
			ring.reset();
		} else {
			std::vector<std::shared_ptr<void>> fallback;
			ReapLocked(std::move(ring), fallback);
			if (!fallback.empty()) {
				lock.unlock();
				fallback.clear();
				lock.lock();
			}
		}
		if (!found) {
			entry->state = DecodedEntry::State::Free;
			// Nothing decodable yet: sleep until G publishes a new target, an
			// encoder publishes a slot, or one interval passes.
			decodeCv_.wait_for(lock, std::chrono::nanoseconds(intervalNs), [&] {
				return stop_ || wakeSerial_ != serial || releaseSerial_ != handledRelease;
			});
			continue;
		}
		cursorNs = view.captureNs;
		if (ok && epoch_ == epoch) {
			entry->state = DecodedEntry::State::Ready;
			entry->captureNs = view.captureNs;
			entry->image = std::move(image);
		} else {
			entry->state = DecodedEntry::State::Free;
			entry->image = DecodedImage{};
		}
	}
	lock.unlock();
	if (codec)
		codec->Release();
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
	window_.queueDepthMax = 0;
	window_.queueDepthSum = 0;
	window_.queueDepthSamples = 0;
	window_.aheadSum = 0;
	window_.aheadSamples = 0;
	window_.aheadMin = UINT32_MAX;
}

CodecPipeline::RecentLoad CodecPipeline::GetRecentLoad(uint64_t nowNs, uint64_t windowNs) const
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	RecentLoad load;
	load.encode = encodeRecent_.Summarize(nowNs, windowNs);
	load.decode = decodeRecent_.Summarize(nowNs, windowNs);
	return load;
}

void CodecPipeline::ResetRecentLoad()
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	encodeRecent_.Clear();
	decodeRecent_.Clear();
}

void CodecPipeline::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	encodeCv_.notify_all();
	decodeCv_.notify_all();
	for (std::thread &thread : encodeThreads_) {
		if (thread.joinable())
			thread.join();
	}
	encodeThreads_.clear();
	if (decodeThread_.joinable())
		decodeThread_.join();
	// Last: it frees whatever the workers handed it on their way out.
	reapCv_.notify_all();
	if (reapThread_.joinable())
		reapThread_.join();

	// The rest is freed on this thread (OBS's destruction thread).
	std::deque<EncodeJob> dropped;
	std::vector<std::shared_ptr<void>> doomed;
	std::vector<std::shared_ptr<FrameRing>> trimmed;
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
	}
	if (inlineCodec_) {
		inlineCodec_->Release();
		inlineCodec_.reset();
	}
}

} // namespace trigglow
