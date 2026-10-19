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

#include "hw-storage-codec.hpp"

#include "annexb.hpp"
#include "buffer-fit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

namespace trigglow::hwstorage {

namespace {

using storage::Backend;
using storage::CodecId;

constexpr uint32_t kProbeFrames = 8;
constexpr uint32_t kProbeSecondKey = 5;
constexpr uint32_t kQuickProbeFrames = 2;
constexpr uint64_t kProbePacketTimeoutNs = 500000000ULL;
constexpr double kProbeMinPsnr = 30.0;
// Pictures a hardware decoder keeps ready for reuse (decode-ahead + 2, one on
// screen and one being decoded fit).
constexpr size_t kPictureCacheSize = 8;

uint64_t SteadyNs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					     std::chrono::steady_clock::now().time_since_epoch())
					     .count());
}

void Logf(const PipelineLogFn &log, PipelineLogLevel level, const char *fmt, ...)
{
	if (!log)
		return;
	char line[1024];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	log(level, line);
}

using InjectFn = std::function<bool(const char *step)>;

// ProviderSettings::injectFailure: whether a test wants `step` to fail.
bool Injected(const InjectFn &inject, const char *step)
{
	return inject && inject(step);
}

std::string AvError(int code)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = "";
	av_strerror(code, text, sizeof(text));
	return text;
}

const char *BackendName(Backend backend)
{
	switch (backend) {
	case Backend::Nvenc:
		return "NVENC";
	case Backend::Amf:
		return "AMF";
	case Backend::Qsv:
		return "Quick Sync";
	case Backend::Cpu:
		return "CPU";
	case Backend::D3d12va:
		return "D3D12";
	}
	return "?";
}

AVCodecID AvCodecOf(CodecId codec)
{
	return codec == CodecId::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

// --- The session's registry (design 2.7 step 3) ---

struct RegistryEntry {
	std::string encoder;
	bool ok = false;          // Passed a full probe this session.
	bool blacklisted = false; // Failed hard while in use: never again this session.
	uint32_t failedWidth = 0; // Failed its probe at this size: not retried there.
	uint32_t failedHeight = 0;
	std::string reason;
	bool copyParameterSets = false; // Only its first key carries them.
};

std::mutex g_registryMutex;
std::vector<RegistryEntry> g_registry;

RegistryEntry &EntryLocked(const std::string &encoder)
{
	for (RegistryEntry &entry : g_registry) {
		if (entry.encoder == encoder)
			return entry;
	}
	g_registry.push_back(RegistryEntry{});
	g_registry.back().encoder = encoder;
	return g_registry.back();
}

RegistryEntry Lookup(const std::string &encoder)
{
	std::lock_guard<std::mutex> lock(g_registryMutex);
	return EntryLocked(encoder);
}

std::vector<std::string> Blacklisted()
{
	std::lock_guard<std::mutex> lock(g_registryMutex);
	std::vector<std::string> out;
	for (const RegistryEntry &entry : g_registry) {
		if (entry.blacklisted)
			out.push_back(entry.encoder);
	}
	return out;
}

// --- Probe frames and checks ---

// A moving diagonal ramp with mild noise (and chroma ramps): content an
// encoder can't get away with skipping, and a decoder can be checked on.
void FillProbeFrame(std::vector<uint8_t> &buffer, uint32_t width, uint32_t height, uint32_t t)
{
	buffer.resize(I420FrameBytes(width, height));
	uint8_t *y = buffer.data();
	uint8_t *u = y + static_cast<size_t>(width) * height;
	uint8_t *v = u + static_cast<size_t>(width / 2) * (height / 2);
	uint32_t seed = 0x9E3779B9u ^ (t * 2654435761u);
	for (uint32_t row = 0; row < height; ++row) {
		uint8_t *line = y + static_cast<size_t>(row) * width;
		for (uint32_t col = 0; col < width; ++col) {
			seed = seed * 1664525u + 1013904223u;
			const int noise = static_cast<int>(seed >> 29) - 4;
			const int base = 16 + static_cast<int>((col * 3 + row * 2 + t * 24) % 219);
			line[col] = static_cast<uint8_t>(std::clamp(base + noise, 16, 235));
		}
	}
	for (uint32_t row = 0; row < height / 2; ++row) {
		for (uint32_t col = 0; col < width / 2; ++col) {
			u[static_cast<size_t>(row) * (width / 2) + col] =
				static_cast<uint8_t>(64 + (col + t * 8) % 128);
			v[static_cast<size_t>(row) * (width / 2) + col] =
				static_cast<uint8_t>(64 + (row + t * 4) % 128);
		}
	}
}

double PsnrY(const uint8_t *source, uint32_t width, uint32_t height, const DecodedImage &image)
{
	if (!image.plane[0] || image.width != width || image.height != height)
		return 0.0;
	double sum = 0.0;
	for (uint32_t row = 0; row < height; ++row) {
		const uint8_t *a = source + static_cast<size_t>(row) * width;
		const uint8_t *b = image.plane[0] + static_cast<size_t>(row) * image.stride[0];
		for (uint32_t col = 0; col < width; ++col) {
			const double d = static_cast<double>(a[col]) - static_cast<double>(b[col]);
			sum += d * d;
		}
	}
	const double mse = sum / (static_cast<double>(width) * height);
	return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// --- Encoder ---

class HwEncoder final : public storage::Encoder {
public:
	HwEncoder(const storage::HwCandidate &candidate, const storage::EncoderConfig &config,
		  std::shared_ptr<storage::InterControl> control, std::function<uint64_t()> clockNs, InjectFn inject)
		: candidate_(candidate),
		  config_(config),
		  nv12_(candidate.backend == Backend::Qsv),
		  control_(std::move(control)),
		  clockNs_(std::move(clockNs)),
		  inject_(std::move(inject))
	{
	}
	~HwEncoder() override
	{
		if (ctx_)
			avcodec_free_context(&ctx_);
		if (packet_)
			av_packet_free(&packet_);
		if (pool_)
			av_buffer_pool_uninit(&pool_);
		if (device_)
			av_buffer_unref(&device_);
		if (counted_ && control_)
			control_->openEncoders.fetch_sub(1);
	}
	HwEncoder(const HwEncoder &) = delete;
	HwEncoder &operator=(const HwEncoder &) = delete;

	// Opens the context; `device` (D3D11VA, may be null) is referenced.
	// options: consumed as avcodec_open2() does; what's left wasn't known.
	bool Open(AVBufferRef *device, AVDictionary **options, std::string &error);

	CodecId Codec() const override { return candidate_.codec; }
	Backend GetBackend() const override { return candidate_.backend; }
	const char *Name() const override { return candidate_.encoder; }
	bool InterFrame() const override { return true; }
	const storage::EncoderConfig &Config() const override { return config_; }
	storage::SendResult Send(const I420View &frame, uint64_t seq, bool forceKey) override;
	bool Receive(storage::Packet &out) override;
	void Drain(uint64_t deadlineNs, const std::function<void(const storage::Packet &)> &sink) override;
	uint32_t InFlight() const override { return static_cast<uint32_t>(pending_.size()) - discards_; }
	bool Broken() const override { return broken_; }

	// What's in flight now is the probe's: its packets are thrown away as they
	// come out. Encoders like AMF only hand a frame's packet out once the next
	// frame is in (or on a drain), so the probe sends one frame more than it
	// checks, and that one is never the pipeline's.
	void DiscardInFlight()
	{
		for (Pending &entry : pending_)
			entry.discard = true;
		discards_ = static_cast<uint32_t>(pending_.size());
	}

	// Keys without their parameter sets get a copy of the first key's (design
	// 2.7); `sets`: those already seen (the probe's first key), if any.
	void CopyParameterSets(std::vector<uint8_t> sets = {})
	{
		copyParameterSets_ = true;
		if (parameterSets_.empty())
			parameterSets_ = std::move(sets);
	}
	bool OnDevice() const { return device_ != nullptr; }

private:
	AVFrame *NewInputFrame();

	storage::HwCandidate candidate_;
	storage::EncoderConfig config_;
	bool nv12_;
	std::shared_ptr<storage::InterControl> control_;
	std::function<uint64_t()> clockNs_;
	InjectFn inject_;
	AVCodecContext *ctx_ = nullptr;
	AVBufferRef *device_ = nullptr;
	AVPacket *packet_ = nullptr;
	AVBufferPool *pool_ = nullptr;
	int lineY_ = 0;
	int lineC_ = 0;
	uint32_t rowsY_ = 0;
	struct Pending {
		int64_t pts = 0;
		uint64_t seq = 0;
		bool discard = false;
	};
	std::deque<Pending> pending_; // In Send() order.
	uint32_t discards_ = 0;       // Entries of pending_ to throw away.
	int64_t nextPts_ = 0;
	bool broken_ = false;
	bool draining_ = false;
	bool counted_ = false;
	bool copyParameterSets_ = false;
	std::vector<uint8_t> parameterSets_;
	std::vector<uint8_t> scratch_; // A key with the parameter sets put in front.
};

bool HwEncoder::Open(AVBufferRef *device, AVDictionary **options, std::string &error)
{
	const AVCodec *codec = avcodec_find_encoder_by_name(candidate_.encoder);
	if (!codec) {
		error = "not in this FFmpeg";
		return false;
	}
	ctx_ = avcodec_alloc_context3(codec);
	packet_ = av_packet_alloc();
	if (!ctx_ || !packet_) {
		error = "out of memory";
		return false;
	}
	ctx_->width = static_cast<int>(config_.width);
	ctx_->height = static_cast<int>(config_.height);
	ctx_->time_base = AVRational{static_cast<int>(config_.fpsDen), static_cast<int>(config_.fpsNum)};
	ctx_->framerate = AVRational{static_cast<int>(config_.fpsNum), static_cast<int>(config_.fpsDen)};
	ctx_->pix_fmt = nv12_ ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
	ctx_->gop_size = static_cast<int>(config_.keyframeInterval);
	ctx_->max_b_frames = 0;
	// Labels only, never a conversion: BT.601 studio range is what the
	// capture shader writes and what playback expects (like MJPEG's).
	ctx_->color_range = AVCOL_RANGE_MPEG;
	ctx_->colorspace = AVCOL_SPC_SMPTE170M;
	ctx_->color_primaries = AVCOL_PRI_SMPTE170M;
	ctx_->color_trc = AVCOL_TRC_SMPTE170M;
	if (candidate_.backend == Backend::Qsv) {
		// Constant QP through FFmpeg's QSCALE: I and P at the same QP.
		ctx_->flags |= AV_CODEC_FLAG_QSCALE;
		ctx_->global_quality = config_.quality * FF_QP2LAMBDA;
		ctx_->i_quant_factor = 1.0f;
		ctx_->i_quant_offset = 0.0f;
		ctx_->b_quant_factor = 1.0f;
	}
	if (device) {
		device_ = av_buffer_ref(device);
		ctx_->hw_device_ctx = av_buffer_ref(device);
	}
	const int ret = Injected(inject_, "encoder-open") ? AVERROR(EINVAL) : avcodec_open2(ctx_, codec, options);
	if (ret < 0) {
		error = "open failed: " + AvError(ret);
		return false;
	}
	// Input frames from a pool: the encoder may hold a reference to one past
	// its Send() (Quick Sync does), so a frame is never written while queued.
	lineY_ = static_cast<int>((config_.width + 63) / 64 * 64);
	lineC_ = nv12_ ? lineY_ : static_cast<int>((config_.width / 2 + 63) / 64 * 64);
	rowsY_ = (config_.height + 31) / 32 * 32;
	const size_t chroma = static_cast<size_t>(lineC_) * (rowsY_ / 2) * (nv12_ ? 1 : 2);
	pool_ = av_buffer_pool_init(static_cast<size_t>(lineY_) * rowsY_ + chroma + 64, &av_buffer_alloc);
	if (!pool_) {
		error = "out of memory";
		return false;
	}
	if (control_) {
		control_->openEncoders.fetch_add(1);
		counted_ = true;
	}
	return true;
}

AVFrame *HwEncoder::NewInputFrame()
{
	AVFrame *frame = av_frame_alloc();
	if (!frame)
		return nullptr;
	frame->buf[0] = av_buffer_pool_get(pool_);
	if (!frame->buf[0]) {
		av_frame_free(&frame);
		return nullptr;
	}
	frame->format = nv12_ ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
	frame->width = static_cast<int>(config_.width);
	frame->height = static_cast<int>(config_.height);
	frame->data[0] = frame->buf[0]->data;
	frame->linesize[0] = lineY_;
	frame->data[1] = frame->data[0] + static_cast<size_t>(lineY_) * rowsY_;
	frame->linesize[1] = lineC_;
	if (!nv12_) {
		frame->data[2] = frame->data[1] + static_cast<size_t>(lineC_) * (rowsY_ / 2);
		frame->linesize[2] = lineC_;
	}
	return frame;
}

storage::SendResult HwEncoder::Send(const I420View &view, uint64_t seq, bool forceKey)
{
	if (broken_ || draining_ || view.width != config_.width || view.height != config_.height)
		return storage::SendResult::Failed;
	AVFrame *frame = NewInputFrame();
	if (!frame)
		return storage::SendResult::Failed;
	for (uint32_t row = 0; row < view.height; ++row)
		std::memcpy(frame->data[0] + static_cast<size_t>(row) * lineY_,
			    view.plane[0] + static_cast<size_t>(row) * view.stride[0], view.width);
	const uint32_t cw = view.width / 2;
	const uint32_t ch = view.height / 2;
	if (nv12_) {
		InterleaveUv(view.plane[1], view.stride[1], view.plane[2], view.stride[2], cw, ch, frame->data[1],
			     static_cast<uint32_t>(lineC_));
	} else {
		for (int p = 1; p < 3; ++p) {
			for (uint32_t row = 0; row < ch; ++row)
				std::memcpy(frame->data[p] + static_cast<size_t>(row) * lineC_,
					    view.plane[p] + static_cast<size_t>(row) * view.stride[p], cw);
		}
	}
	frame->pts = nextPts_;
	if (forceKey) {
		// With forced-idr/forced_idr, an I picture asked for is an IDR.
		frame->pict_type = AV_PICTURE_TYPE_I;
		frame->flags |= AV_FRAME_FLAG_KEY;
	}
	const int ret = avcodec_send_frame(ctx_, frame);
	av_frame_free(&frame);
	if (ret == AVERROR(EAGAIN))
		return storage::SendResult::Busy; // Its queue is full: this capture is dropped.
	if (ret < 0) {
		broken_ = true;
		return storage::SendResult::Failed;
	}
	pending_.push_back(Pending{nextPts_++, seq, false});
	return storage::SendResult::Ok;
}

bool HwEncoder::Receive(storage::Packet &out)
{
	for (;;) {
		if (broken_ || pending_.empty())
			return false;
		av_packet_unref(packet_);
		const int ret = avcodec_receive_packet(ctx_, packet_);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return false;
		if (ret < 0) {
			broken_ = true;
			return false;
		}
		if (!pending_.front().discard || pending_.front().pts != packet_->pts)
			break;
		pending_.pop_front(); // The probe's last frame: not the pipeline's.
		--discards_;
	}
	out = storage::Packet{};
	// The seq the pipeline sent it as; a pts out of order maps to none (the
	// pipeline then closes this encoder: invariant I1).
	out.seq = UINT64_MAX;
	if (pending_.front().pts == packet_->pts) {
		out.seq = pending_.front().seq;
		pending_.pop_front();
	}
	out.key = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
	out.codec = candidate_.codec;
	out.data = packet_->data;
	out.bytes = static_cast<size_t>(packet_->size);
	if (out.key && copyParameterSets_) {
		if (annexb::HasInBandParameterSets(candidate_.codec, out.data, out.bytes)) {
			if (parameterSets_.empty())
				parameterSets_ = annexb::ExtractParameterSets(candidate_.codec, out.data, out.bytes);
		} else if (annexb::PrependParameterSets(candidate_.codec, parameterSets_, out.data, out.bytes,
							scratch_)) {
			out.data = scratch_.data();
			out.bytes = scratch_.size();
		}
	}
	return true;
}

void HwEncoder::Drain(uint64_t deadlineNs, const std::function<void(const storage::Packet &)> &sink)
{
	if (broken_ || pending_.empty())
		return;
	if (!draining_) {
		draining_ = true;
		if (avcodec_send_frame(ctx_, nullptr) < 0) {
			broken_ = true;
			return;
		}
	}
	const auto now = [&] {
		return clockNs_ ? clockNs_() : SteadyNs();
	};
	while (!pending_.empty() && now() < deadlineNs) {
		storage::Packet packet;
		if (Receive(packet)) {
			if (sink)
				sink(packet);
			continue;
		}
		if (broken_)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	pending_.clear();
	discards_ = 0;
}

// --- Decoder ---

struct Picture {
	AVFrame *nv12 = nullptr; // The download target, NV12 at the picture's size.
	std::unique_ptr<uint8_t[]> chroma;
	uint32_t width = 0;
	uint32_t height = 0;
	~Picture()
	{
		if (nv12)
			av_frame_free(&nv12);
	}
};

// Pictures for a hardware decoder's downloads, reused: a 1440p one is 5.5 MB,
// and the decoder hands one out per frame shown.
class PicturePool : public std::enable_shared_from_this<PicturePool> {
public:
	std::shared_ptr<Picture> Get(uint32_t width, uint32_t height)
	{
		std::unique_ptr<Picture> picture;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			while (!free_.empty() && !picture) {
				std::unique_ptr<Picture> candidate = std::move(free_.back());
				free_.pop_back();
				if (candidate->width == width && candidate->height == height)
					picture = std::move(candidate);
			}
		}
		if (!picture) {
			picture = std::make_unique<Picture>();
			picture->width = width;
			picture->height = height;
			picture->nv12 = av_frame_alloc();
			if (!picture->nv12)
				return nullptr;
			picture->nv12->format = AV_PIX_FMT_NV12;
			picture->nv12->width = static_cast<int>(width);
			picture->nv12->height = static_cast<int>(height);
			if (av_frame_get_buffer(picture->nv12, 0) < 0)
				return nullptr;
			picture->chroma.reset(new (std::nothrow)
						      uint8_t[static_cast<size_t>(width / 2) * (height / 2) * 2]);
			if (!picture->chroma)
				return nullptr;
		}
		std::weak_ptr<PicturePool> pool = weak_from_this();
		return std::shared_ptr<Picture>(picture.release(), [pool](Picture *released) {
			if (std::shared_ptr<PicturePool> alive = pool.lock())
				alive->Return(released);
			else
				delete released;
		});
	}

private:
	void Return(Picture *picture)
	{
		std::unique_ptr<Picture> owned(picture);
		std::lock_guard<std::mutex> lock(mutex_);
		if (free_.size() < kPictureCacheSize)
			free_.push_back(std::move(owned));
		// Past the cache it's freed here, after the lock (`owned` outlives it: declared first).
	}

	std::mutex mutex_;
	std::vector<std::unique_ptr<Picture>> free_;
};

enum AVPixelFormat PickD3d11(AVCodecContext * /*ctx*/, const enum AVPixelFormat *formats)
{
	for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; ++f) {
		if (*f == AV_PIX_FMT_D3D11)
			return *f;
	}
	return AV_PIX_FMT_NONE; // No D3D11VA decode for this stream: a failure, never a silent software one.
}

class HwDecoder final : public storage::Decoder {
public:
	HwDecoder(CodecId codec, bool hardware, uint32_t adapterIndex, PipelineLogFn log, InjectFn inject)
		: codec_(codec),
		  hardware_(hardware),
		  adapterIndex_(adapterIndex),
		  log_(std::move(log)),
		  inject_(std::move(inject)),
		  pictures_(std::make_shared<PicturePool>())
	{
		std::snprintf(name_, sizeof(name_), "%s %s", storage::CodecName(codec), hardware ? "d3d11va" : "sw");
	}
	~HwDecoder() override { Close(); }
	HwDecoder(const HwDecoder &) = delete;
	HwDecoder &operator=(const HwDecoder &) = delete;

	CodecId Codec() const override { return codec_; }
	const char *Name() const override { return name_; }
	bool Hardware() const override { return hardware_; }
	bool Broken() const override { return broken_; }
	storage::DecodeStatus Decode(const storage::EncodedFrame &in, bool wantImage, DecodedImage &out,
				     uint64_t &outSeq) override;
	// Never on a context that isn't open: Open() leaves none behind when it
	// fails, and avcodec_flush_buffers() on an allocated but unopened context
	// crashes (a decoder whose D3D11 device couldn't be created after a TDR,
	// say, is flushed right after its failed Decode()).
	void Flush() override
	{
		if (ctx_ && avcodec_is_open(ctx_))
			avcodec_flush_buffers(ctx_);
	}
	// Opens the context now (the probe, on the setup thread); otherwise the
	// first Decode() does, on the decode worker. On failure nothing is left
	// allocated (ctx_ stays null) and later calls fail without retrying.
	bool Open(std::string &error);

private:
	void Close(); // Frees whatever Open() allocated.

	CodecId codec_;
	bool hardware_;
	uint32_t adapterIndex_;
	PipelineLogFn log_;
	InjectFn inject_;
	char name_[24] = "";
	AVCodecContext *ctx_ = nullptr;
	AVBufferRef *device_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;
	bool openFailed_ = false;
	bool broken_ = false;
	std::shared_ptr<PicturePool> pictures_;
};

void HwDecoder::Close()
{
	if (ctx_)
		avcodec_free_context(&ctx_);
	if (frame_)
		av_frame_free(&frame_);
	if (packet_)
		av_packet_free(&packet_);
	if (device_)
		av_buffer_unref(&device_);
}

bool HwDecoder::Open(std::string &error)
{
	if (ctx_)
		return true;
	if (openFailed_) {
		error = "open failed before";
		return false;
	}
	openFailed_ = true;
	const AVCodec *codec = avcodec_find_decoder(AvCodecOf(codec_));
	if (!codec) {
		error = "no decoder in this FFmpeg";
		return false;
	}
	// Every failure below frees what was allocated: a context left allocated
	// but unopened would look open to Decode() and crash Flush().
	ctx_ = avcodec_alloc_context3(codec);
	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	if (!ctx_ || !frame_ || !packet_) {
		error = "out of memory";
		Close();
		return false;
	}
	if (hardware_) {
		char device[16];
		std::snprintf(device, sizeof(device), "%u", adapterIndex_);
		const int ret =
			Injected(inject_, "decoder-device")
				? AVERROR(ENODEV)
				: av_hwdevice_ctx_create(&device_, AV_HWDEVICE_TYPE_D3D11VA, device, nullptr, 0);
		if (ret < 0) {
			error = "no D3D11 device on adapter " + std::string(device) + ": " + AvError(ret);
			Close();
			return false;
		}
		ctx_->hw_device_ctx = av_buffer_ref(device_);
		ctx_->get_format = &PickD3d11;
		ctx_->extra_hw_frames = 2;
		ctx_->thread_count = 1;
	} else {
		// Slice threads only: frame threads would hold pictures back.
		ctx_->thread_count = 2;
		ctx_->thread_type = FF_THREAD_SLICE;
	}
	ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	const int ret = Injected(inject_, "decoder-open") ? AVERROR(EINVAL) : avcodec_open2(ctx_, codec, nullptr);
	if (ret < 0) {
		error = "open failed: " + AvError(ret);
		Close();
		return false;
	}
	openFailed_ = false;
	return true;
}

storage::DecodeStatus HwDecoder::Decode(const storage::EncodedFrame &in, bool wantImage, DecodedImage &out,
					uint64_t &outSeq)
{
	outSeq = in.seq;
	if (!ctx_) {
		std::string error;
		if (!Open(error)) {
			if (!broken_)
				Logf(log_, PipelineLogLevel::Warning, "buffer decoder %s unavailable (%s)", name_,
				     error.c_str());
			broken_ = true;
			return storage::DecodeStatus::Failed;
		}
	}
	if (!in.data || in.bytes == 0 || in.bytes > static_cast<size_t>(INT32_MAX))
		return storage::DecodeStatus::Failed;
	av_packet_unref(packet_);
	if (av_new_packet(packet_, static_cast<int>(in.bytes)) < 0)
		return storage::DecodeStatus::Failed;
	std::memcpy(packet_->data, in.data, in.bytes);
	packet_->pts = static_cast<int64_t>(in.seq);
	if (in.key)
		packet_->flags |= AV_PKT_FLAG_KEY;
	int ret = avcodec_send_packet(ctx_, packet_);
	av_packet_unref(packet_);
	if (ret < 0) {
		// Our own packets, in order, from a key: a hardware decoder failing
		// here is the decoder, not the stream.
		broken_ = broken_ || hardware_;
		return storage::DecodeStatus::Failed;
	}
	ret = avcodec_receive_frame(ctx_, frame_);
	if (ret == AVERROR(EAGAIN))
		return storage::DecodeStatus::NoFrameYet;
	if (ret < 0) {
		broken_ = broken_ || hardware_;
		return storage::DecodeStatus::Failed;
	}
	if (frame_->pts != AV_NOPTS_VALUE)
		outSeq = static_cast<uint64_t>(frame_->pts);
	if (!wantImage) {
		av_frame_unref(frame_); // Catch-up: the references are kept, the picture isn't needed.
		return storage::DecodeStatus::NoFrameYet;
	}
	const auto width = static_cast<uint32_t>(frame_->width);
	const auto height = static_cast<uint32_t>(frame_->height);
	if (hardware_) {
		if (frame_->format != AV_PIX_FMT_D3D11) {
			av_frame_unref(frame_);
			broken_ = true;
			return storage::DecodeStatus::Failed;
		}
		std::shared_ptr<Picture> picture = pictures_->Get(width, height);
		if (!picture) {
			av_frame_unref(frame_);
			return storage::DecodeStatus::Failed;
		}
		ret = av_hwframe_transfer_data(picture->nv12, frame_, 0);
		av_frame_unref(frame_);
		if (ret < 0) {
			broken_ = true;
			return storage::DecodeStatus::Failed;
		}
		// NV12 -> I420: luma stays where the download put it, chroma is split.
		const uint32_t cw = width / 2;
		const uint32_t ch = height / 2;
		uint8_t *u = picture->chroma.get();
		uint8_t *v = u + static_cast<size_t>(cw) * ch;
		DeinterleaveUv(picture->nv12->data[1], static_cast<uint32_t>(picture->nv12->linesize[1]), cw, ch, u, cw,
			       v, cw);
		out.plane[0] = picture->nv12->data[0];
		out.stride[0] = static_cast<uint32_t>(picture->nv12->linesize[0]);
		out.plane[1] = u;
		out.stride[1] = cw;
		out.plane[2] = v;
		out.stride[2] = cw;
		out.width = width;
		out.height = height;
		out.owner = std::move(picture);
		return storage::DecodeStatus::Frame;
	}
	if (frame_->format != AV_PIX_FMT_YUV420P && frame_->format != AV_PIX_FMT_YUVJ420P) {
		av_frame_unref(frame_);
		return storage::DecodeStatus::Failed;
	}
	// Software: the decoder's own frame, no copy (like MJPEG's).
	AVFrame *kept = av_frame_alloc();
	if (!kept) {
		av_frame_unref(frame_);
		return storage::DecodeStatus::Failed;
	}
	av_frame_move_ref(kept, frame_);
	for (int p = 0; p < 3; ++p) {
		out.plane[p] = kept->data[p];
		out.stride[p] = static_cast<uint32_t>(kept->linesize[p]);
	}
	out.width = width;
	out.height = height;
	out.owner = std::shared_ptr<void>(kept, [](void *p) {
		AVFrame *f = static_cast<AVFrame *>(p);
		av_frame_free(&f);
	});
	return storage::DecodeStatus::Frame;
}

// --- Opening and probing (setup thread) ---

struct Opened {
	std::unique_ptr<HwEncoder> encoder;
	std::string error;
	std::string ignored; // Options the encoder didn't know.
	double openMs = 0.0;
};

void SetOptions(const storage::HwCandidate &c, int qp, uint32_t gop, AVDictionary **o)
{
	switch (c.backend) {
	case Backend::Nvenc:
		av_dict_set(o, "preset", "p1", 0);
		av_dict_set(o, "tune", "ll", 0);
		av_dict_set(o, "rc", "constqp", 0);
		av_dict_set_int(o, "qp", qp, 0);
		av_dict_set(o, "zerolatency", "1", 0);
		av_dict_set(o, "delay", "0", 0);
		av_dict_set(o, "rc-lookahead", "0", 0);
		av_dict_set(o, "multipass", "disabled", 0);
		av_dict_set(o, "spatial-aq", "0", 0);
		av_dict_set(o, "forced-idr", "1", 0);
		av_dict_set(o, "surfaces", "4", 0);
		break;
	case Backend::Amf:
		av_dict_set(o, "usage", "lowlatency_high_quality", 0);
		av_dict_set(o, "quality", "speed", 0);
		av_dict_set(o, "rc", "cqp", 0);
		av_dict_set_int(o, "qp_i", qp, 0);
		av_dict_set_int(o, "qp_p", qp, 0);
		av_dict_set(o, "preanalysis", "0", 0);
		av_dict_set(o, "forced_idr", "1", 0);
		av_dict_set(o, "enforce_hrd", "0", 0);
		av_dict_set(o, "filler_data", "0", 0);
		av_dict_set(o, "vbaq", "0", 0);
		if (c.codec == CodecId::Hevc) {
			av_dict_set(o, "header_insertion_mode", "idr", 0);
			av_dict_set(o, "skip_frame", "0", 0);
		} else {
			av_dict_set_int(o, "header_spacing", gop, 0);
			av_dict_set(o, "frame_skipping", "0", 0);
			av_dict_set(o, "bf", "0", 0);
		}
		break;
	case Backend::Qsv:
		av_dict_set(o, "preset", "veryfast", 0);
		av_dict_set(o, "forced_idr", "1", 0);
		av_dict_set(o, "idr_interval", "0", 0);
		av_dict_set(o, "async_depth", "1", 0);
		if (c.codec == CodecId::H264)
			av_dict_set(o, "look_ahead", "0", 0);
		break;
	case Backend::Cpu:
	case Backend::D3d12va:
		break;
	}
}

std::string Leftovers(AVDictionary *options)
{
	std::string out;
	const AVDictionaryEntry *entry = nullptr;
	while ((entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX))) {
		if (!out.empty())
			out += ",";
		out += entry->key;
	}
	return out;
}

Opened OpenEncoder(const storage::HwCandidate &c, const storage::EncoderConfig &config, int adapterIndex,
		   const ProviderSettings &settings)
{
	Opened result;
	const uint64_t startNs = SteadyNs();
	// NVENC and AMF on a D3D11 device of their GPU's own adapter (the
	// adapter picked, not whatever the driver defaults to); if the encoder
	// won't open on it, once more on its default device.
	AVBufferRef *device = nullptr;
	if ((c.backend == Backend::Nvenc || c.backend == Backend::Amf) && adapterIndex >= 0) {
		char index[16];
		std::snprintf(index, sizeof(index), "%d", adapterIndex);
		if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, index, nullptr, 0) < 0)
			device = nullptr;
	}
	for (int attempt = 0; attempt < 2 && !result.encoder; ++attempt) {
		if (attempt == 1 && !device)
			break;
		AVBufferRef *use = attempt == 0 ? device : nullptr;
		auto encoder = std::make_unique<HwEncoder>(c, config, settings.control, settings.clockNs,
							   settings.injectFailure);
		AVDictionary *options = nullptr;
		SetOptions(c, config.quality, config.keyframeInterval, &options);
		std::string error;
		if (encoder->Open(use, &options, error)) {
			result.ignored = Leftovers(options);
			result.encoder = std::move(encoder);
			result.error.clear();
		} else {
			result.error = error + (use ? " (on its adapter's D3D11 device)" : "");
		}
		av_dict_free(&options);
	}
	if (device)
		av_buffer_unref(&device);
	result.openMs = static_cast<double>(SteadyNs() - startNs) / 1e6;
	return result;
}

struct ProbeOutcome {
	bool ok = false;
	std::string reason;
	uint32_t packets = 0;
	bool copyParameterSets = false;
	std::vector<uint8_t> parameterSets; // The first key's, when copyParameterSets.
	bool freshStart = false;
	double psnr = 0.0;
	std::unique_ptr<HwDecoder> decoder;
};

// Sends `frames` synthetic frames (keys forced at 0 and kProbeSecondKey),
// plus one more, and collects the first `frames` packets, in order, within
// kProbePacketTimeoutNs of the last send (an encoder may only hand a packet
// out once the next frame is in: the extra one's is thrown away later).
// Copies kept: an encoder's packet is only valid until its next call.
bool ProbeEncode(HwEncoder &encoder, uint32_t frames, std::vector<std::vector<uint8_t>> &source,
		 std::vector<std::vector<uint8_t>> &packets, std::vector<bool> &keys, std::string &reason)
{
	const storage::EncoderConfig &config = encoder.Config();
	source.resize(frames + 1);
	auto collect = [&](uint64_t untilNs, size_t want) {
		storage::Packet packet;
		while (packets.size() < want) {
			if (encoder.Receive(packet)) {
				if (packet.seq != packets.size()) {
					reason = "packet out of order";
					return false;
				}
				packets.emplace_back(packet.data, packet.data + packet.bytes);
				keys.push_back(packet.key);
				continue;
			}
			if (encoder.Broken()) {
				reason = "receive failed";
				return false;
			}
			if (SteadyNs() >= untilNs)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	};
	for (uint32_t i = 0; i <= frames; ++i) {
		FillProbeFrame(source[i], config.width, config.height, i);
		const I420View view = PackedI420View(source[i].data(), config.width, config.height);
		storage::SendResult sent = encoder.Send(view, i, i == 0 || i == kProbeSecondKey);
		for (int retry = 0; sent == storage::SendResult::Busy && retry < 50; ++retry) {
			if (!collect(SteadyNs() + 10000000ULL, i))
				return false;
			sent = encoder.Send(view, i, i == 0 || i == kProbeSecondKey);
		}
		if (sent != storage::SendResult::Ok) {
			reason = "send failed";
			return false;
		}
		// Whatever is out already (a quick encoder hands each one back now).
		if (!collect(SteadyNs() + 1000000ULL, std::min(i + 1, frames)))
			return false;
	}
	if (!collect(SteadyNs() + kProbePacketTimeoutNs, frames))
		return false;
	encoder.DiscardInFlight();
	if (packets.size() != frames) {
		char text[96];
		std::snprintf(text, sizeof(text), "%zu of %u packets within 500 ms", packets.size(), frames);
		reason = text;
		return false;
	}
	return true;
}

ProbeOutcome Probe(HwEncoder &encoder, bool quick, const ProviderSettings &settings)
{
	ProbeOutcome outcome;
	const storage::EncoderConfig &config = encoder.Config();
	const CodecId codec = encoder.Codec();
	const uint32_t frames = quick ? kQuickProbeFrames : kProbeFrames;
	std::vector<std::vector<uint8_t>> source, packets;
	std::vector<bool> keys;
	if (!ProbeEncode(encoder, frames, source, packets, keys, outcome.reason))
		return outcome;
	outcome.packets = static_cast<uint32_t>(packets.size());
	if (!keys[0]) {
		outcome.reason = "the first frame, forced, is no key";
		return outcome;
	}
	if (!annexb::HasInBandParameterSets(codec, packets[0].data(), packets[0].size())) {
		outcome.reason = "no parameter sets in band on the first key";
		return outcome;
	}
	if (quick) {
		outcome.ok = true;
		return outcome;
	}
	if (!keys[kProbeSecondKey]) {
		outcome.reason = "a forced key came out as no key";
		return outcome;
	}
	if (!annexb::HasInBandParameterSets(codec, packets[kProbeSecondKey].data(), packets[kProbeSecondKey].size())) {
		// Only the first key carries them: every later key gets a copy.
		outcome.copyParameterSets = true;
		outcome.parameterSets = annexb::ExtractParameterSets(codec, packets[0].data(), packets[0].size());
		std::vector<uint8_t> patched;
		annexb::PrependParameterSets(codec, outcome.parameterSets, packets[kProbeSecondKey].data(),
					     packets[kProbeSecondKey].size(), patched);
		packets[kProbeSecondKey] = std::move(patched);
	}

	// The decoder: 0-7 in order, then from the second key on a flushed decoder.
	auto decoder = std::make_unique<HwDecoder>(codec, true, settings.obsAdapterIndex, settings.log,
						   settings.injectFailure);
	std::string error;
	if (!decoder->Open(error)) {
		outcome.reason = "hardware decode: " + error;
		return outcome;
	}
	auto decodeRange = [&](uint32_t from, uint32_t to, double *psnr) {
		for (uint32_t i = from; i < to; ++i) {
			storage::EncodedFrame in;
			in.data = packets[i].data();
			in.bytes = packets[i].size();
			in.width = config.width;
			in.height = config.height;
			in.seq = i;
			in.key = keys[i];
			DecodedImage image;
			uint64_t outSeq = 0;
			if (decoder->Decode(in, true, image, outSeq) != storage::DecodeStatus::Frame || outSeq != i ||
			    image.width != config.width || image.height != config.height) {
				char text[96];
				std::snprintf(text, sizeof(text), "hardware decode of frame %u failed or held back", i);
				outcome.reason = text;
				return false;
			}
			if (psnr && (i == from || i + 1 == to))
				*psnr = std::min(*psnr, PsnrY(source[i].data(), config.width, config.height, image));
		}
		return true;
	};
	double psnr = 99.0;
	if (!decodeRange(0, kProbeFrames, &psnr))
		return outcome;
	outcome.psnr = psnr;
	if (psnr < kProbeMinPsnr) {
		char text[64];
		std::snprintf(text, sizeof(text), "decoded picture wrong (PSNR-Y %.1f dB)", psnr);
		outcome.reason = text;
		return outcome;
	}
	decoder->Flush();
	if (!decodeRange(kProbeSecondKey, kProbeFrames, nullptr)) {
		outcome.reason += " (from the second key, on a flushed decoder)";
		return outcome;
	}
	outcome.freshStart = true;
	decoder->Flush();
	outcome.decoder = std::move(decoder);
	outcome.ok = true;
	return outcome;
}

} // namespace

bool RegistryHasWorkingEncoder()
{
	std::lock_guard<std::mutex> lock(g_registryMutex);
	for (const RegistryEntry &entry : g_registry) {
		if (entry.ok && !entry.blacklisted)
			return true;
	}
	return false;
}

Provider::Provider(ProviderSettings settings) : settings_(std::move(settings))
{
	adapters_ = settings_.adapters.empty() ? QueryGpuAdapters() : settings_.adapters;
	fpsNum_ = std::max<uint32_t>(1, settings_.fpsNum);
	fpsDen_ = std::max<uint32_t>(1, settings_.fpsDen);
	for (const GpuAdapterInfo &adapter : adapters_) {
		if (adapter.index == settings_.obsAdapterIndex)
			decodeAdapterName_ = adapter.name;
	}
	if (!settings_.clockNs)
		settings_.clockNs = &SteadyNs;
}

void Provider::SetFrameRate(uint32_t fpsNum, uint32_t fpsDen)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (fpsNum > 0 && fpsDen > 0) {
		fpsNum_ = fpsNum;
		fpsDen_ = fpsDen;
	}
}

uint32_t Provider::KeyframeInterval() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return std::max<uint32_t>(1, bufferfit::KeyframeIntervalFrames(fpsNum_, fpsDen_));
}

bool Provider::HasVendor(storage::GpuVendor vendor) const
{
	for (const GpuAdapterInfo &adapter : adapters_) {
		if (storage::VendorFromPciId(adapter.vendorId) == vendor)
			return true;
	}
	return false;
}

storage::GpuVendor Provider::DecodeVendor() const
{
	for (const GpuAdapterInfo &adapter : adapters_) {
		if (adapter.index == settings_.obsAdapterIndex)
			return storage::VendorFromPciId(adapter.vendorId);
	}
	return storage::GpuVendor::Unknown;
}

int Provider::QpFor(CodecId codec) const
{
	return codec == CodecId::Hevc ? settings_.hevcQp : settings_.h264Qp;
}

void Provider::SetObsEncoders(bool known, std::vector<storage::EncoderFamily> families)
{
	std::lock_guard<std::mutex> lock(mutex_);
	obsKnown_ = known;
	obsFamilies_ = std::move(families);
}

void Provider::ObsEncoders(bool &known, std::vector<storage::EncoderFamily> &families) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	known = obsKnown_;
	families = obsFamilies_;
}

void Provider::Blacklist(const std::string &encoder, const char *reason)
{
	std::lock_guard<std::mutex> lock(g_registryMutex);
	RegistryEntry &entry = EntryLocked(encoder);
	entry.blacklisted = true;
	entry.reason = reason ? reason : "";
}

std::unique_ptr<storage::Decoder> Provider::MakeHardwareDecoder(CodecId codec) const
{
	if (codec != CodecId::Hevc && codec != CodecId::H264)
		return nullptr;
	return std::make_unique<HwDecoder>(codec, true, settings_.obsAdapterIndex, settings_.log,
					   settings_.injectFailure);
}

std::unique_ptr<storage::Decoder> Provider::MakeSoftwareDecoder(CodecId codec) const
{
	if (codec != CodecId::Hevc && codec != CodecId::H264)
		return nullptr;
	return std::make_unique<HwDecoder>(codec, false, 0, settings_.log, settings_.injectFailure);
}

storage::InterSession Provider::Setup(const storage::EncoderConfig &frame)
{
	const uint64_t startNs = SteadyNs();
	storage::RankInput rank;
	rank.mode = storage::BufferCodecMode::Auto;
	rank.workers = true;
	rank.platformSupported = true;
	for (const GpuAdapterInfo &adapter : adapters_) {
		const storage::GpuVendor vendor = storage::VendorFromPciId(adapter.vendorId);
		if (std::find(rank.vendors.begin(), rank.vendors.end(), vendor) == rank.vendors.end())
			rank.vendors.push_back(vendor);
	}
	ObsEncoders(rank.obsEncodersKnown, rank.obsEncoders);
	rank.blacklist = Blacklisted();
	const std::vector<storage::HwCandidate> candidates = storage::RankCandidates(rank);
	const uint32_t obsNvenc = storage::CountFamily(rank.obsEncoders, storage::EncoderFamily::Nvenc);
	uint32_t fpsNum = 60, fpsDen = 1;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		fpsNum = fpsNum_;
		fpsDen = fpsDen_;
	}
	const uint32_t keyframeInterval = std::max<uint32_t>(1, bufferfit::KeyframeIntervalFrames(fpsNum, fpsDen));

	std::string tried;
	auto note = [&](const char *encoder, const std::string &why) {
		if (!tried.empty())
			tried += ", ";
		tried += encoder;
		tried += " (" + why + ")";
	};
	for (const storage::HwCandidate &candidate : candidates) {
		const RegistryEntry entry = Lookup(candidate.encoder);
		if (!entry.ok && entry.failedWidth == frame.width && entry.failedHeight == frame.height) {
			note(candidate.encoder, "failed here before: " + entry.reason);
			continue;
		}
		// The adapter of the candidate's vendor: OBS's own if it is that vendor's.
		int adapterIndex = -1;
		for (const GpuAdapterInfo &adapter : adapters_) {
			if (storage::VendorFromPciId(adapter.vendorId) != candidate.vendor)
				continue;
			if (adapterIndex < 0 || adapter.index == settings_.obsAdapterIndex)
				adapterIndex = static_cast<int>(adapter.index);
		}
		storage::EncoderConfig config;
		config.width = frame.width;
		config.height = frame.height;
		config.fpsNum = fpsNum;
		config.fpsDen = fpsDen;
		config.keyframeInterval = keyframeInterval;
		config.quality = QpFor(candidate.codec);
		const uint64_t candidateNs = SteadyNs();
		Opened opened = OpenEncoder(candidate, config, adapterIndex, settings_);
		if (!opened.encoder) {
			note(candidate.encoder, opened.error);
			std::lock_guard<std::mutex> lock(g_registryMutex);
			RegistryEntry &e = EntryLocked(candidate.encoder);
			e.failedWidth = frame.width;
			e.failedHeight = frame.height;
			e.reason = opened.error;
			continue;
		}
		// Probed already this session: a short check is enough at a new size.
		const bool quick = entry.ok;
		if (entry.copyParameterSets)
			opened.encoder->CopyParameterSets();
		ProbeOutcome probe = Probe(*opened.encoder, quick, settings_);
		const double probeMs = static_cast<double>(SteadyNs() - candidateNs) / 1e6;
		if (!probe.ok) {
			note(candidate.encoder, probe.reason);
			std::lock_guard<std::mutex> lock(g_registryMutex);
			RegistryEntry &e = EntryLocked(candidate.encoder);
			e.failedWidth = frame.width;
			e.failedHeight = frame.height;
			e.reason = probe.reason;
			continue;
		}
		if (probe.copyParameterSets)
			opened.encoder->CopyParameterSets(std::move(probe.parameterSets));
		{
			std::lock_guard<std::mutex> lock(g_registryMutex);
			RegistryEntry &e = EntryLocked(candidate.encoder);
			e.ok = true;
			e.failedWidth = 0;
			e.failedHeight = 0;
			e.copyParameterSets = e.copyParameterSets || probe.copyParameterSets;
		}
		std::string shared = candidate.sharesEngine ? "yes" : "no";
		if (!rank.obsEncodersKnown)
			shared = "unknown";
		const double gopSeconds =
			static_cast<double>(keyframeInterval) * fpsDen / std::max<uint32_t>(1, fpsNum);
		if (quick) {
			Logf(settings_.log, PipelineLogLevel::Info,
			     "storage codec: %s reopened at %ux%u in %.0f ms (open %.0f ms, %u/%u packets, key in band ok)",
			     candidate.encoder, frame.width, frame.height, probeMs, opened.openMs, probe.packets,
			     kQuickProbeFrames);
		} else {
			Logf(settings_.log, PipelineLogLevel::Info,
			     "storage codec: auto -> %s (%s, QP %d, GOP %u = %.2f s, no B-frames, decode d3d11va on \"%s\") "
			     "at %ux%u; probe %.0f ms (open %.0f ms%s, %u/%u packets, IDR in-band %s, fresh-decoder start "
			     "ok, psnr %.1f dB)%s%s; tried: %s; engine shared with OBS: %s; NVENC sessions by OBS: %u",
			     candidate.encoder, BackendName(candidate.backend), config.quality, keyframeInterval,
			     gopSeconds, decodeAdapterName_.empty() ? "?" : decodeAdapterName_.c_str(), frame.width,
			     frame.height, probeMs, opened.openMs, opened.encoder->OnDevice() ? " on its adapter" : "",
			     probe.packets, kProbeFrames,
			     probe.copyParameterSets ? "on the first key only (copied)" : "ok", probe.psnr,
			     opened.ignored.empty() ? "" : "; options it ignored: ", opened.ignored.c_str(),
			     tried.empty() ? "-" : tried.c_str(), shared.c_str(), obsNvenc);
		}
		storage::InterSession session;
		session.encoder = std::move(opened.encoder);
		session.decoder = std::move(probe.decoder);
		return session;
	}
	const double totalMs = static_cast<double>(SteadyNs() - startNs) / 1e6;
	if (candidates.empty()) {
		Logf(settings_.log, PipelineLogLevel::Info,
		     "storage codec: auto -> mjpeg at %ux%u (no hardware encoder to try: GPUs %zu, NVENC sessions by OBS "
		     "%u%s)",
		     frame.width, frame.height, adapters_.size(), obsNvenc,
		     rank.blacklist.empty() ? "" : ", some blacklisted after failing this session");
	} else {
		Logf(settings_.log, PipelineLogLevel::Warning,
		     "storage codec: auto -> mjpeg at %ux%u (no hardware encoder works, %.0f ms; tried: %s) -- the delay "
		     "is unchanged, the buffer just takes more RAM",
		     frame.width, frame.height, totalMs, tried.c_str());
	}
	return {};
}

} // namespace trigglow::hwstorage
