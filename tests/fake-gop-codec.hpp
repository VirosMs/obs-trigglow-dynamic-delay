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

// A fake inter-frame storage codec for the tests: no FFmpeg, but it behaves
// like a hardware HEVC encoder as far as the pipeline can tell -- a key every
// `gop` frames (or when forced), P-frames that reference the frame before them,
// packets coming out `latency` later than their Send() (frames in flight), and
// failures or Busy on demand. Each packet carries the stamp of its own frame
// and of the frame it references, so FakeGopDecoder knows when it is handed a
// P-frame without its reference: it then returns a picture visibly marked as
// corrupt (like a real decoder would return garbage) and counts it. The same
// encoder can also pose as an intra codec (inter = false: every packet a key),
// for rings that mix both.
//
// Frames are the stress tests' stamped I420 frames: the first 8 Y bytes are
// the capture time ("stamp").

#include "storage-codec.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace fakegop {

using trigglow::DecodedImage;
using trigglow::I420View;
namespace storage = trigglow::storage;

constexpr uint32_t kMagic = 0x504F4747;                  // "GGOP"
constexpr uint64_t kCorruptMask = 0xC0FFEE0000000000ULL; // XORed into a corrupt picture's stamp.
constexpr size_t kHeaderBytes = 4 + 4 + 4 + 8 + 8 + 8 + 1;

// Shared counters a test checks afterwards.
struct Counters {
	std::atomic<uint64_t> encodes{0};
	std::atomic<uint64_t> keys{0};
	std::atomic<uint64_t> forcedKeys{0};
	std::atomic<uint64_t> decodes{0};
	std::atomic<uint64_t> corruptDecodes{0};   // A P-frame decoded without its reference: must stay 0.
	std::atomic<uint64_t> badPackets{0};       // Malformed bytes reached a decoder: must stay 0.
	std::atomic<uint64_t> threadViolations{0}; // A codec object used from two threads, or from G.
	std::atomic<uint64_t> wrongSize{0};        // A frame of another size than the encoder's config.
	std::thread::id graphicsThread;            // Codec objects must never run here (threaded mode).
	std::atomic<uint32_t> decodeExtraUs{0};    // Added to every decode: a slow decoder.
	// Every hardware decoder fails from its next decode on and says it's
	// broken (a lost device); software ones go on.
	std::atomic<bool> breakHardwareDecoders{false};
};

struct EncoderOptions {
	storage::CodecId codec = storage::CodecId::Hevc;
	bool inter = true;
	uint32_t gop = 4;           // A key every gop frames (forced keys restart the count).
	uint64_t latencyNs = 0;     // A packet comes out this long after its Send() (needs clockNs)...
	uint32_t latencyFrames = 0; // ...and only after this many later Send()s (0: right away).
	uint32_t busyAbove = 0;     // Send() returns Busy with this many in flight (0: never).
	uint32_t failOneIn = 0;     // Send() fails once in this many (0: never).
	uint32_t fillerBytes = 16;  // Packet size: the header, this, and stamp % 97 more.
	uint32_t seed = 1;
	// Opened and checked on one thread, used on another (the setup thread's
	// handover, invariant I8): the guard then checks exclusive use, not one owner.
	bool handover = false;
	uint32_t breakAfter = 0; // After this many sends: Broken(), every Send() fails (0: never).
	uint32_t stallAfter = 0; // After this many sends: no packet comes out any more (0: never).
	std::function<uint64_t()> clockNs;
	Counters *counters = nullptr;
};

inline uint64_t ReadStamp(const uint8_t *y)
{
	uint64_t stamp;
	std::memcpy(&stamp, y, sizeof(stamp));
	return stamp;
}

// Checks every call comes from the thread of the first one (and, when set,
// never from the graphics thread). With `handover`, the object may move from
// one thread to another (the setup thread opens it, a worker uses it), so the
// check is that no two calls ever overlap instead.
class ThreadGuard {
public:
	explicit ThreadGuard(bool handover = false) : handover_(handover) {}

	class Scope {
	public:
		Scope(ThreadGuard &guard, Counters *counters) : guard_(guard)
		{
			const std::thread::id self = std::this_thread::get_id();
			bool bad = counters && counters->graphicsThread != std::thread::id() &&
				   self == counters->graphicsThread;
			if (guard.handover_) {
				bad = bad || guard.inUse_.fetch_add(1) != 0;
			} else {
				if (guard.owner_ == std::thread::id())
					guard.owner_ = self;
				bad = bad || self != guard.owner_;
			}
			if (bad && counters)
				++counters->threadViolations;
		}
		~Scope()
		{
			if (guard_.handover_)
				guard_.inUse_.fetch_sub(1);
		}
		Scope(const Scope &) = delete;
		Scope &operator=(const Scope &) = delete;

	private:
		ThreadGuard &guard_;
	};

private:
	bool handover_;
	std::thread::id owner_;
	std::atomic<int> inUse_{0};
};

class FakeGopEncoder final : public storage::Encoder {
public:
	FakeGopEncoder(const storage::EncoderConfig &config, const EncoderOptions &options)
		: config_(config),
		  options_(options),
		  rng_(options.seed),
		  guard_(options.handover)
	{
		config_.keyframeInterval = options.inter ? options.gop : 0;
	}

	storage::CodecId Codec() const override { return options_.codec; }
	storage::Backend GetBackend() const override { return storage::Backend::Cpu; }
	const char *Name() const override { return options_.inter ? "fake-gop" : "fake-intra"; }
	bool InterFrame() const override { return options_.inter; }
	const storage::EncoderConfig &Config() const override { return config_; }

	storage::SendResult Send(const I420View &frame, uint64_t seq, bool forceKey) override
	{
		ThreadGuard::Scope scope(guard_, options_.counters);
		if (broken_)
			return storage::SendResult::Failed;
		if (options_.breakAfter > 0 && sends_ >= options_.breakAfter) {
			broken_ = true;
			return storage::SendResult::Failed;
		}
		if (frame.width != config_.width || frame.height != config_.height) {
			if (options_.counters)
				++options_.counters->wrongSize;
			return storage::SendResult::Failed;
		}
		if (options_.busyAbove > 0 && pending_.size() >= options_.busyAbove)
			return storage::SendResult::Busy;
		if (options_.failOneIn > 0 && std::uniform_int_distribution<uint32_t>(1, options_.failOneIn)(rng_) == 1)
			return storage::SendResult::Failed;
		const uint64_t stamp = ReadStamp(frame.plane[0]);
		const bool key = !options_.inter || forceKey || sinceKey_ >= options_.gop || !haveRef_;
		Pending p;
		p.seq = seq;
		p.key = key;
		p.readyNs = options_.clockNs ? options_.clockNs() + options_.latencyNs : 0;
		p.sendsLeft = options_.latencyFrames;
		p.bytes.resize(kHeaderBytes + options_.fillerBytes + static_cast<size_t>(stamp % 97));
		uint8_t *d = p.bytes.data();
		const uint64_t ref = key ? 0 : lastStamp_;
		std::memcpy(d, &kMagic, 4);
		std::memcpy(d + 4, &frame.width, 4);
		std::memcpy(d + 8, &frame.height, 4);
		std::memcpy(d + 12, &seq, 8);
		std::memcpy(d + 20, &stamp, 8);
		std::memcpy(d + 28, &ref, 8);
		d[36] = key ? 1 : 0;
		std::memset(d + kHeaderBytes, static_cast<int>(stamp & 0xFF), p.bytes.size() - kHeaderBytes);
		for (Pending &older : pending_) {
			if (older.sendsLeft > 0)
				--older.sendsLeft;
		}
		pending_.push_back(std::move(p));
		++sends_;
		sinceKey_ = key ? 1 : sinceKey_ + 1;
		lastStamp_ = stamp;
		haveRef_ = true;
		if (options_.counters) {
			++options_.counters->encodes;
			if (key)
				++options_.counters->keys;
			if (forceKey)
				++options_.counters->forcedKeys;
		}
		return storage::SendResult::Ok;
	}

	bool Receive(storage::Packet &out) override
	{
		ThreadGuard::Scope scope(guard_, options_.counters);
		if (pending_.empty() || (options_.stallAfter > 0 && sends_ >= options_.stallAfter))
			return false;
		const Pending &front = pending_.front();
		if (front.sendsLeft > 0 || (options_.clockNs && options_.clockNs() < front.readyNs))
			return false;
		TakeFront(out);
		return true;
	}

	void Drain(uint64_t /*deadlineNs*/, const std::function<void(const storage::Packet &)> &sink) override
	{
		ThreadGuard::Scope scope(guard_, options_.counters);
		if (options_.stallAfter > 0 && sends_ >= options_.stallAfter)
			return; // Stalled: nothing comes out.
		while (!pending_.empty()) {
			storage::Packet packet;
			TakeFront(packet);
			sink(packet);
		}
	}

	uint32_t InFlight() const override { return static_cast<uint32_t>(pending_.size()); }
	bool Broken() const override { return broken_; }

private:
	struct Pending {
		uint64_t seq = 0;
		bool key = false;
		uint64_t readyNs = 0;
		uint32_t sendsLeft = 0;
		std::vector<uint8_t> bytes;
	};

	void TakeFront(storage::Packet &out)
	{
		current_ = std::move(pending_.front());
		pending_.pop_front();
		out = storage::Packet{};
		out.data = current_.bytes.data();
		out.bytes = current_.bytes.size();
		out.seq = current_.seq;
		out.key = current_.key;
		out.codec = options_.codec;
	}

	storage::EncoderConfig config_;
	EncoderOptions options_;
	std::mt19937 rng_;
	ThreadGuard guard_;
	std::deque<Pending> pending_;
	Pending current_; // The packet Receive() handed out last (valid until the next call).
	uint32_t sinceKey_ = 0;
	uint64_t lastStamp_ = 0;
	bool haveRef_ = false;
	uint32_t sends_ = 0;
	bool broken_ = false;
};

// Decodes FakeGopEncoder packets into a stamped 4:2:0 picture: Y bytes 0-7
// the stamp, every other Y byte stamp & 0xFF, U/V (stamp >> 8) & 0xFF. A P-frame
// whose reference isn't the last frame decoded gets the stamp XOR kCorruptMask.
class FakeGopDecoder final : public storage::Decoder {
public:
	FakeGopDecoder(storage::CodecId codec, Counters *counters, bool handover = false, bool hardware = false)
		: codec_(codec),
		  counters_(counters),
		  hardware_(hardware),
		  guard_(handover)
	{
	}

	storage::CodecId Codec() const override { return codec_; }
	const char *Name() const override { return "fake-gop"; }
	bool Hardware() const override { return hardware_; }
	bool Broken() const override { return broken_; }
	void Flush() override
	{
		ThreadGuard::Scope scope(guard_, counters_);
		haveRef_ = false;
	}

	storage::DecodeStatus Decode(const storage::EncodedFrame &in, bool wantImage, DecodedImage &out,
				     uint64_t &outSeq) override
	{
		ThreadGuard::Scope scope(guard_, counters_);
		outSeq = in.seq;
		if (hardware_ && counters_ && counters_->breakHardwareDecoders.load())
			broken_ = true;
		if (broken_)
			return storage::DecodeStatus::Failed;
		uint32_t magic = 0, width = 0, height = 0;
		uint64_t seq = 0, stamp = 0, ref = 0;
		if (!in.data || in.bytes < kHeaderBytes) {
			Bad();
			return storage::DecodeStatus::Failed;
		}
		std::memcpy(&magic, in.data, 4);
		std::memcpy(&width, in.data + 4, 4);
		std::memcpy(&height, in.data + 8, 4);
		std::memcpy(&seq, in.data + 12, 8);
		std::memcpy(&stamp, in.data + 20, 8);
		std::memcpy(&ref, in.data + 28, 8);
		const bool key = in.data[36] != 0;
		bool ok = magic == kMagic && width == in.width && height == in.height && width > 0 && height > 0 &&
			  in.bytes >= kHeaderBytes + static_cast<size_t>(stamp % 97) &&
			  static_cast<uint32_t>(seq) == static_cast<uint32_t>(in.seq) && key == in.key;
		for (size_t k = kHeaderBytes; ok && k < in.bytes; k += 13)
			ok = in.data[k] == static_cast<uint8_t>(stamp & 0xFF);
		if (!ok) {
			Bad();
			haveRef_ = false;
			return storage::DecodeStatus::Failed;
		}
		const uint32_t extraUs = counters_ ? counters_->decodeExtraUs.load() : 0;
		if (extraUs > 0) {
			const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(extraUs);
			while (std::chrono::steady_clock::now() < end) {
			}
		}
		const bool corrupt = !key && (!haveRef_ || ref != lastStamp_);
		if (counters_) {
			++counters_->decodes;
			if (corrupt)
				++counters_->corruptDecodes;
		}
		lastStamp_ = stamp;
		haveRef_ = true;
		if (!wantImage)
			return storage::DecodeStatus::NoFrameYet;
		const uint64_t shown = corrupt ? stamp ^ kCorruptMask : stamp;
		const size_t need = trigglow::I420FrameBytes(width, height);
		std::shared_ptr<uint8_t> buffer(new uint8_t[need], std::default_delete<uint8_t[]>());
		const size_t luma = static_cast<size_t>(width) * height;
		std::memset(buffer.get(), static_cast<int>(shown & 0xFF), luma);
		std::memset(buffer.get() + luma, static_cast<int>((shown >> 8) & 0xFF), need - luma);
		std::memcpy(buffer.get(), &shown, 8);
		const I420View view = trigglow::PackedI420View(buffer.get(), width, height);
		for (int p = 0; p < 3; ++p) {
			out.plane[p] = view.plane[p];
			out.stride[p] = view.stride[p];
		}
		out.width = width;
		out.height = height;
		out.owner = buffer;
		return storage::DecodeStatus::Frame;
	}

private:
	void Bad()
	{
		if (counters_)
			++counters_->badPackets;
	}

	storage::CodecId codec_;
	Counters *counters_;
	bool hardware_;
	ThreadGuard guard_;
	uint64_t lastStamp_ = 0;
	bool haveRef_ = false;
	bool broken_ = false;
};

} // namespace fakegop
