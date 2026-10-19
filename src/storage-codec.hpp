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

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

// Storage codecs of the delay buffer (v0.7.0): how the frames waiting out the
// delay are compressed in the ring. Pure C++17 (no OBS, no FFmpeg), so tests
// drive the pipeline with fake codecs.
//
// Since v0.7.0 encode and decode are two separate objects, and the encoder is
// asynchronous: Send() hands it a frame, Receive() takes out the packets that
// are ready, in Send() order, and a frame may stay "in flight" for a while (a
// hardware encoder's latency). Every packet carries the seq it was sent with,
// whether it is a key (decodable on its own) and its codec. Implementations:
//   - Raw (here): the frame itself, uncompressed (platforms without FFmpeg, and
//     the fallback when an encode fails).
//   - MJPEG (mjpeg-codec.hpp): all-intra, synchronous: every packet is a key
//     and comes out of the Receive() right after its Send().
//   - Inter-frame ones: hardware H.264/HEVC on Windows (hw-storage-codec.hpp),
//     opened and checked on the pipeline's setup thread (InterSetupFn below).
//     They rely on the chain bookkeeping below (ChainTracker, InFlightQueue)
//     and the ring's per-packet metadata (FrameRing::Payload), which the
//     pipeline keeps for every codec.
//
// A "chain" is one bitstream session: an encoder from its first key on, for
// one ring. Inside a chain packets are numbered by seq (consecutive in the
// bitstream); a P-frame can only be decoded right after its predecessor in the
// same chain (ringselect::Decodable() in frame-ring.hpp).
//
// Threads: every Encoder and Decoder is used by one thread at a time, never
// the graphics thread (the sync kill switch excepted, where G is the codec
// thread). Intra ones are opened, used and closed by that thread; hardware
// ones are opened by the setup thread, handed over under the pipeline's mutex
// and closed by the worker using them or by the reaper (invariant I8).
//
// Invariants of the inter-frame path, cited by number across the pipeline:
//   I1  Inside a chain the bitstream follows capture order: one inter encode
//       context, packets matched in Send() order (InFlightQueue); a packet out
//       of order is a hard error.
//   I2  A P-frame is never decoded without its reference: a key, or the next
//       seq of the chain the decoder decoded last (ringselect::Decodable()).
//   I3  Captures dropped before their encode consume no seq; once a packet
//       already encoded is lost, the next frame is a forced key and no P-frame
//       depending on the lost one is stored (ChainTracker).
//   I4  With an inter-frame codec, the key of the frame due is always still in
//       the ring: a GOP of slack (bufferfit::SlackFrames()).
//   I5  Playback (re)starts on a key: it catches up from the key unshown, or
//       waits for the next one, holding the view meanwhile
//       (FrameRing::PlanDecodeStart()).
//   I6  An encoder's latency never counts as load: the degradation policy only
//       sees the worker's own time in its calls; the latency (Send() to
//       packet) goes to the codec policy.
//   I7  No change of codec, chain or resolution empties the ring or moves the
//       delay: rings hold several codecs, decoded frames are always I420.
//   I8  Every encoder and decoder is used by one thread at a time, never the
//       graphics thread; a hardware one is opened on the setup thread, handed
//       over under the pipeline's mutex, and closed by its worker or the reaper.
//   I9  The storage class that sizes a cycle's ring is fixed for that cycle,
//       so the filter (graphics thread) and the planned delay (UI thread) get
//       the same whole-second delay.
//
// Header-only: the interface, the raw codec and the bookkeeping helpers are
// all inline, so every target that uses the ring or the pipeline (the plugin,
// the tests, the benchmarks) gets them without another source file.
namespace trigglow {

enum class PipelineLogLevel { Info, Warning };
using PipelineLogFn = std::function<void(PipelineLogLevel, const char *)>;

// One I420 frame (Y full size, U and V at half width and height).
struct I420View {
	const uint8_t *plane[3] = {nullptr, nullptr, nullptr};
	uint32_t stride[3] = {0, 0, 0};
	uint32_t width = 0;
	uint32_t height = 0;
};

// A decoded frame: always I420, whatever the codec, so playback doesn't know
// which one was behind it. Planes kept alive by `owner` (an AVFrame, or a
// plain buffer).
struct DecodedImage {
	const uint8_t *plane[3] = {nullptr, nullptr, nullptr};
	uint32_t stride[3] = {0, 0, 0};
	uint32_t width = 0;
	uint32_t height = 0;
	std::shared_ptr<void> owner;
};

// Tightly packed I420 size: w*h + 2*(w/2)*(h/2). Dimensions are always even here.
inline size_t I420FrameBytes(uint32_t width, uint32_t height)
{
	const size_t luma = static_cast<size_t>(width) * height;
	const size_t chroma = static_cast<size_t>(width / 2) * (height / 2);
	return luma + 2 * chroma;
}

// Copies `frame` into `dst` (I420FrameBytes() bytes), tightly packed.
inline void PackI420(const I420View &frame, uint8_t *dst)
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

// A view of a tightly packed I420 buffer.
inline I420View PackedI420View(const uint8_t *data, uint32_t width, uint32_t height)
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

// NV12's interleaved chroma (U V U V..., what hardware decoders hand back)
// split into separate U and V planes, chromaWidth x chromaHeight samples each.
inline void DeinterleaveUv(const uint8_t *uv, uint32_t uvStride, uint32_t chromaWidth, uint32_t chromaHeight,
			   uint8_t *u, uint32_t uStride, uint8_t *v, uint32_t vStride)
{
	for (uint32_t row = 0; row < chromaHeight; ++row) {
		const uint8_t *src = uv + static_cast<size_t>(row) * uvStride;
		uint8_t *du = u + static_cast<size_t>(row) * uStride;
		uint8_t *dv = v + static_cast<size_t>(row) * vStride;
		for (uint32_t col = 0; col < chromaWidth; ++col) {
			du[col] = src[2 * col];
			dv[col] = src[2 * col + 1];
		}
	}
}

// The reverse: U and V planes interleaved into NV12's chroma plane (Intel
// Quick Sync only takes NV12).
inline void InterleaveUv(const uint8_t *u, uint32_t uStride, const uint8_t *v, uint32_t vStride, uint32_t chromaWidth,
			 uint32_t chromaHeight, uint8_t *uv, uint32_t uvStride)
{
	for (uint32_t row = 0; row < chromaHeight; ++row) {
		const uint8_t *su = u + static_cast<size_t>(row) * uStride;
		const uint8_t *sv = v + static_cast<size_t>(row) * vStride;
		uint8_t *dst = uv + static_cast<size_t>(row) * uvStride;
		for (uint32_t col = 0; col < chromaWidth; ++col) {
			dst[2 * col] = su[col];
			dst[2 * col + 1] = sv[col];
		}
	}
}

namespace storage {

enum class CodecId : uint8_t { Raw = 0, Mjpeg = 1, H264 = 2, Hevc = 3 };
constexpr size_t kCodecCount = 4;
enum class Backend : uint8_t { Cpu, Nvenc, Amf, Qsv, D3d12va };
// Which compression ratio sizes the ring (bufferfit::RingClass): intra
// codecs (every packet a key) or inter-frame ones.
enum class StorageClass : uint8_t { Intra, Inter };

// "raw", "mjpeg", "h264", "hevc".
inline const char *CodecName(CodecId codec)
{
	switch (codec) {
	case CodecId::Raw:
		return "raw";
	case CodecId::Mjpeg:
		return "mjpeg";
	case CodecId::H264:
		return "h264";
	case CodecId::Hevc:
		return "hevc";
	}
	return "?";
}

inline StorageClass ClassOf(CodecId codec)
{
	return codec == CodecId::H264 || codec == CodecId::Hevc ? StorageClass::Inter : StorageClass::Intra;
}

// What an encoder is opened for. The pipeline fills the frame geometry and
// slice threads; the factory the rest.
struct EncoderConfig {
	uint32_t width = 0; // Multiple of 8, like every buffer width (bufferfit).
	uint32_t height = 0;
	uint32_t fpsNum = 60; // Capture rate (time base).
	uint32_t fpsDen = 1;
	uint32_t keyframeInterval = 0; // In frames; 0 = every frame is a key.
	int quality = 0;               // QP (hardware) or qscale (MJPEG).
	uint32_t sliceThreads = 1;     // MJPEG only, <= CodecPipeline::kMaxSlicesPerContext.
};

// Same frame size and slice threads: the pipeline keeps the encoder it has.
inline bool SameGeometry(const EncoderConfig &a, const EncoderConfig &b)
{
	return a.width == b.width && a.height == b.height && a.sliceThreads == b.sliceThreads;
}

// One encoded frame from Encoder::Receive(). Valid until the next call into
// that encoder.
struct Packet {
	const uint8_t *data = nullptr;
	size_t bytes = 0;
	uint64_t seq = 0; // The seq passed to Send() for this frame.
	bool key = false; // Decodable on its own (every intra packet).
	CodecId codec = CodecId::Raw;
	// The payload is the very frame passed to Send(), to be stored tightly
	// packed (I420FrameBytes()); `data` is null. Only an encoder whose packet
	// comes out of the Receive() right after its Send() may set it (the
	// pipeline holds that frame until then): the raw encoder, which then copies
	// nothing itself -- the pipeline packs the frame straight into the arena.
	bool rawInput = false;
};

enum class SendResult : uint8_t {
	Ok,
	Busy,  // Can't take a frame now (too many in flight): that capture is dropped.
	Failed // This frame wasn't encoded: the pipeline stores it raw.
};

class Encoder {
public:
	virtual ~Encoder() = default; // Closes the contexts (never on the graphics thread, sync mode aside).
	virtual CodecId Codec() const = 0;
	virtual Backend GetBackend() const = 0;
	virtual const char *Name() const = 0; // "mjpeg", "raw", "hevc_nvenc"...
	virtual bool InterFrame() const = 0;  // false: every packet is a key.
	virtual const EncoderConfig &Config() const = 0;
	// Takes `frame` (Config()'s size) to be encoded as `seq`. The encoder
	// copies what it needs before returning: the frame goes back to the
	// pipeline's pool afterwards (Packet::rawInput aside). forceKey: this
	// frame's packet must be a key (a new chain, or the frame after a lost
	// packet).
	virtual SendResult Send(const I420View &frame, uint64_t seq, bool forceKey) = 0;
	// The oldest packet ready, if any; packets come out in Send() order. Never
	// blocks longer than the driver takes to hand over a finished packet.
	virtual bool Receive(Packet &out) = 0;
	// End of the session: hands every packet still in flight to `sink`, in
	// order, until deadlineNs (the pipeline's clock); the rest is lost.
	virtual void Drain(uint64_t deadlineNs, const std::function<void(const Packet &)> &sink) = 0;
	virtual uint32_t InFlight() const = 0; // Frames sent and not received yet.
	// The encoder hit an error it can't recover from (a hardware encoder's
	// send or receive failing, a lost device): the pipeline closes it and
	// stores the frames that follow with the intra codec. Intra encoders never
	// break; a failed Send() of theirs only costs that frame.
	virtual bool Broken() const { return false; }
};

// One stored packet to decode: the bytes the ring holds and the frame's size.
struct EncodedFrame {
	const uint8_t *data = nullptr;
	size_t bytes = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t seq = 0;
	bool key = false;
};

enum class DecodeStatus : uint8_t {
	Frame,      // `out` holds the frame of outSeq.
	NoFrameYet, // Accepted, no picture this time (catch-up, or decoder latency).
	Failed      // Nothing usable: what follows needs a key.
};

class Decoder {
public:
	virtual ~Decoder() = default;
	virtual CodecId Codec() const = 0;
	virtual const char *Name() const = 0;
	// wantImage false: catch-up -- decode only to keep the references (an
	// intra decoder skips the packet), no picture is produced.
	virtual DecodeStatus Decode(const EncodedFrame &in, bool wantImage, DecodedImage &out, uint64_t &outSeq) = 0;
	// Discontinuity (new ring, decoder skipped ahead, a failure): drops the
	// references and anything buffered; the next packet must be a key.
	virtual void Flush() = 0;
	// The decoder itself failed (a hardware decoder's device, not a bad
	// packet): the pipeline replaces it with a software one.
	virtual bool Broken() const { return false; }
	// Decoding runs on the GPU (a hardware decoder): what the codec policy
	// weighs when the GPU's video engine also encodes for OBS.
	virtual bool Hardware() const { return false; }
};

// The pipeline fills config's geometry and slice threads; nullptr: the frames
// are stored raw (like a failed encode).
using EncoderFactory = std::function<std::unique_ptr<Encoder>(const EncoderConfig &config)>;
// A decoder for `codec` (never Raw: the pipeline has its own); nullptr if
// this build can't decode it.
using DecoderFactory = std::function<std::unique_ptr<Decoder>(CodecId codec)>;

// --- The inter-frame (hardware) storage codec, v0.7.0 ---
//
// Opening a hardware encoder and checking it (the setup probe: a few frames,
// forced keys, a decode from a key) takes 50-1000 ms, so the pipeline does it
// on a setup thread of its own while the cycle goes on storing frames with
// the intra codec (the MJPEG "bridge"); the encoder and the decoder it
// warmed up are then handed over to the workers that use them (invariant I8:
// one thread at a time, the handover under the pipeline's mutex).

// What a setup hands back: an open, checked encoder for the requested frame
// size (nullptr: no hardware encoder works for it), and optionally the
// decoder the probe used, flushed (the decode worker adopts it). cancelled:
// the setup stopped because it was no longer wanted (see InterSetupFn), not
// because nothing works -- no encoder, and not a failure.
struct InterSession {
	std::unique_ptr<Encoder> encoder;
	std::unique_ptr<Decoder> decoder;
	bool cancelled = false;
};

// True once the setup under way is no longer wanted: Disable, shutdown, an
// intra target, or a yield to an OBS output starting. Cheap, any thread.
using SetupCancelled = std::function<bool()>;

// Runs on the pipeline's setup thread (it may block). config: the frame size
// (the rest is the provider's choice: codec, quality, keyframe interval).
// It asks `cancelled` between its steps (each encoder it tries, before and
// after opening one, during the probe) and stops at the first true, closing
// on its own thread whatever it opened, so a yield frees the engine within
// one step and Disable or shutdown never wait for a whole setup.
using InterSetupFn = std::function<InterSession(const EncoderConfig &config, const SetupCancelled &cancelled)>;

// State shared between the UI thread and the pipeline's encode worker, for what
// can't wait for the graphics thread's next policy evaluation (an OBS output
// starting): while `yielded`, the worker closes its hardware encoder and stores
// with the intra codec, so OBS can open an encoder of its own on that engine
// (the NVENC session limit); clearing it lets the worker set up hardware again.
// openEncoders: hardware encoders alive, counted from before their session is
// opened (the UI waits for 0 after a yield; a setup opening one then closes it
// as soon as it's open).
struct InterControl {
	std::atomic<bool> yielded{false};
	std::atomic<int> openEncoders{0};

	// Wakes the pipeline's encode worker (set by the pipeline for its life),
	// so a yield doesn't wait for the next frame. Any thread.
	void Wake()
	{
		std::lock_guard<std::mutex> lock(wakeMutex);
		if (wake)
			wake();
	}
	void SetWake(std::function<void()> fn)
	{
		std::lock_guard<std::mutex> lock(wakeMutex);
		wake = std::move(fn);
	}

private:
	std::mutex wakeMutex;
	std::function<void()> wake;
};

// The frame itself: no copy here (Packet::rawInput), the pipeline packs it
// into the arena.
class RawEncoder final : public Encoder {
public:
	explicit RawEncoder(const EncoderConfig &config) : config_(config) {}

	CodecId Codec() const override { return CodecId::Raw; }
	Backend GetBackend() const override { return Backend::Cpu; }
	const char *Name() const override { return "raw"; }
	bool InterFrame() const override { return false; }
	const EncoderConfig &Config() const override { return config_; }
	SendResult Send(const I420View &frame, uint64_t seq, bool /*forceKey*/) override
	{
		if (frame.width != config_.width || frame.height != config_.height)
			return SendResult::Failed;
		pending_ = true;
		pendingSeq_ = seq;
		return SendResult::Ok;
	}
	bool Receive(Packet &out) override
	{
		if (!pending_)
			return false;
		pending_ = false;
		out = Packet{};
		out.bytes = I420FrameBytes(config_.width, config_.height);
		out.seq = pendingSeq_;
		out.key = true;
		out.codec = CodecId::Raw;
		out.rawInput = true;
		return true;
	}
	// Nothing to hand over: a rawInput packet is only valid right after its Send().
	void Drain(uint64_t /*deadlineNs*/, const std::function<void(const Packet &)> & /*sink*/) override
	{
		pending_ = false;
	}
	uint32_t InFlight() const override { return pending_ ? 1 : 0; }

private:
	EncoderConfig config_;
	bool pending_ = false;
	uint64_t pendingSeq_ = 0;
};

// Copies the packed frame out of the ring into a buffer of its own (the slot
// is unpinned right after, and that copy is all the "decoding" there is).
class RawDecoder final : public Decoder {
public:
	CodecId Codec() const override { return CodecId::Raw; }
	const char *Name() const override { return "raw"; }
	DecodeStatus Decode(const EncodedFrame &in, bool wantImage, DecodedImage &out, uint64_t &outSeq) override
	{
		outSeq = in.seq;
		const size_t need = I420FrameBytes(in.width, in.height);
		if (!in.data || need == 0 || in.bytes < need)
			return DecodeStatus::Failed;
		if (!wantImage)
			return DecodeStatus::NoFrameYet;
		std::shared_ptr<uint8_t> buffer(new (std::nothrow) uint8_t[need], std::default_delete<uint8_t[]>());
		if (!buffer)
			return DecodeStatus::Failed;
		std::memcpy(buffer.get(), in.data, need);
		const I420View packed = PackedI420View(buffer.get(), in.width, in.height);
		for (int p = 0; p < 3; ++p) {
			out.plane[p] = packed.plane[p];
			out.stride[p] = packed.stride[p];
		}
		out.width = in.width;
		out.height = in.height;
		out.owner = buffer;
		return DecodeStatus::Frame;
	}
	void Flush() override {}
};

inline std::unique_ptr<Encoder> MakeRawEncoder(const EncoderConfig &config)
{
	return std::make_unique<RawEncoder>(config);
}

inline std::unique_ptr<Decoder> MakeRawDecoder()
{
	return std::make_unique<RawDecoder>();
}

// Chain ids: process-wide, unique, never kNoChain.
constexpr uint32_t kNoChain = 0;
inline std::atomic<uint32_t> g_nextChainId{1};
inline uint32_t NewChainId()
{
	uint32_t id = g_nextChainId.fetch_add(1, std::memory_order_relaxed);
	while (id == kNoChain) // Wrapped around (after ~4e9 sessions).
		id = g_nextChainId.fetch_add(1, std::memory_order_relaxed);
	return id;
}

// The encode side of one chain (invariant I3). Seqs are only
// consumed by frames the encoder accepted (captures dropped before the encode,
// or whose Send() failed, never leave a gap). Once a packet already encoded is
// lost -- no room in the arena, its slot reused, unusable bytes -- every later
// packet up to the next key references it, so those are not stored
// (ShouldStore() false: their slots end Dropped) and the next Send() forces a
// key.
class ChainTracker {
public:
	// A new session: chain id, seq from 0, the first frame forced to a key.
	void Start(uint32_t chain)
	{
		chain_ = chain;
		nextSeq_ = 0;
		wantKey_ = true;
		broken_ = false;
	}
	bool Started() const { return chain_ != kNoChain; }
	uint32_t Chain() const { return chain_; }
	uint64_t NextSeq() const { return nextSeq_; }
	bool WantKey() const { return wantKey_; }
	// The encoder accepted frame NextSeq() (with forceKey == forcedKey).
	void CommitSend(bool forcedKey)
	{
		++nextSeq_;
		if (forcedKey)
			wantKey_ = false;
	}
	bool ShouldStore(bool key) const { return !broken_ || key; }
	// A packet of this chain was stored: a key mends the chain.
	void OnStored(bool key)
	{
		if (key)
			broken_ = false;
	}
	// A packet of this chain was lost after its encode.
	void OnLost()
	{
		broken_ = true;
		wantKey_ = true;
	}
	// The next frame must be a key, though nothing sent so far was lost (an
	// encode that failed: that frame never entered the bitstream, but the
	// encoder's state can't be trusted).
	void RequestKey() { wantKey_ = true; }
	bool Broken() const { return broken_; }

private:
	uint32_t chain_ = kNoChain;
	uint64_t nextSeq_ = 0;
	bool wantKey_ = true;
	bool broken_ = false;
};

// Frames sent to an asynchronous encoder and not received yet, oldest first
// (invariant I1): packets come out in Send() order, so each one
// must match the oldest entry; anything else means the bitstream can't be
// trusted (a hard error). T is the caller's bookkeeping (ring slot, ...).
template<typename T> class InFlightQueue {
public:
	struct Entry {
		uint64_t seq = 0;
		uint64_t submitNs = 0;
		T value{};
	};
	enum class Match : uint8_t {
		Ok,      // `out` is the entry of that packet.
		Empty,   // Nothing in flight: a packet nobody sent.
		Mismatch // Not the oldest one: out of order, or one went missing. Nothing popped.
	};

	void Push(uint64_t seq, uint64_t submitNs, T value)
	{
		if (size_ == entries_.size())
			Grow();
		entries_[(head_ + size_) % entries_.size()] = Entry{seq, submitNs, std::move(value)};
		++size_;
		maxDepth_ = std::max(maxDepth_, size_);
	}
	Match Pop(uint64_t seq, Entry &out)
	{
		if (size_ == 0)
			return Match::Empty;
		if (entries_[head_].seq != seq)
			return Match::Mismatch;
		out = TakeFront();
		return Match::Ok;
	}
	// Drops every entry, oldest first, handing each to fn.
	template<typename Fn> void Clear(Fn &&fn)
	{
		while (size_ > 0) {
			Entry entry = TakeFront();
			fn(entry);
		}
	}
	template<typename Fn> void ForEach(Fn &&fn)
	{
		for (size_t i = 0; i < size_; ++i)
			fn(entries_[(head_ + i) % entries_.size()]);
	}
	bool Empty() const { return size_ == 0; }
	size_t Size() const { return size_; }
	size_t MaxDepth() const { return maxDepth_; }
	const Entry *Front() const { return size_ == 0 ? nullptr : &entries_[head_]; } // The oldest, or null.

private:
	// A ring of entries that only grows: nothing is allocated per frame once
	// it holds the encoder's latency.
	void Grow()
	{
		std::vector<Entry> grown(std::max<size_t>(4, 2 * entries_.size()));
		for (size_t i = 0; i < size_; ++i)
			grown[i] = std::move(entries_[(head_ + i) % entries_.size()]);
		entries_ = std::move(grown);
		head_ = 0;
	}
	Entry TakeFront()
	{
		Entry entry = std::move(entries_[head_]);
		entries_[head_] = Entry{}; // Lets go of whatever the value held.
		head_ = (head_ + 1) % entries_.size();
		--size_;
		return entry;
	}

	std::vector<Entry> entries_;
	size_t head_ = 0;
	size_t size_ = 0;
	size_t maxDepth_ = 0;
};

} // namespace storage
} // namespace trigglow
