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

#include "mjpeg-codec.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

namespace trigglow {

namespace {

class MjpegCodec final : public VideoCodec {
public:
	explicit MjpegCodec(const MjpegCodecOptions &options) : options_(options) {}
	~MjpegCodec() override { Release(); }

	bool Encode(const I420View &frame, uint32_t sliceThreads, const uint8_t *&packet, size_t &bytes,
		    bool &compressed) override;
	bool Decode(const uint8_t *packet, size_t bytes, DecodedImage &out) override;
	void Release() override;

private:
	bool OpenEncoder(uint32_t width, uint32_t height, uint32_t sliceThreads);
	void CloseEncoder();
	bool OpenDecoder();
	void Log(PipelineLogLevel level, const char *fmt, ...);

	MjpegCodecOptions options_;
	AVCodecContext *encoder_ = nullptr;
	AVFrame *encodeFrame_ = nullptr;
	AVPacket *encodePacket_ = nullptr;
	uint32_t encodeWidth_ = 0;
	uint32_t encodeHeight_ = 0;
	uint32_t encodeSlices_ = 0;
	// Geometry the encoder last failed to open at: not retried every frame
	// (those frames are stored raw), only once it changes.
	uint32_t failedWidth_ = 0;
	uint32_t failedHeight_ = 0;
	uint32_t failedSlices_ = 0;
	int64_t pts_ = 0;
	bool loggedFirstEncode_ = false;

	AVCodecContext *decoder_ = nullptr;
	AVPacket *decodePacket_ = nullptr;
	bool decoderFailed_ = false;
};

void MjpegCodec::Log(PipelineLogLevel level, const char *fmt, ...)
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

bool MjpegCodec::OpenEncoder(uint32_t width, uint32_t height, uint32_t sliceThreads)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (!codec)
		return false;

	int huffmanResult = 0;
	auto open = [&](int threads) -> bool {
		encoder_ = avcodec_alloc_context3(codec);
		if (!encoder_)
			return false;
		// huffman=default (since v0.5.1): the standard JPEG Huffman tables
		// instead of FFmpeg's "optimal" ones, which cost a second pass over
		// every frame (-17% encode time for ~3% bigger packets; lossless, so
		// the decoded picture is identical). With several slices FFmpeg falls
		// back to the standard tables on its own anyway.
		huffmanResult = av_opt_set(encoder_->priv_data, "huffman", "default", 0);
		encoder_->width = static_cast<int>(width);
		encoder_->height = static_cast<int>(height);
		// Plain 4:2:0 planar, labelled with the range the shader really writes:
		// BT.601 studio range (Y 16-235: PSDrawY in video-delay-filter.cpp), not
		// JPEG's full range. MJPEG only carries that label (a "CS=ITU601"
		// comment), never converts a sample: the decoded planes are bit for bit
		// the same with either label (checked with the QA round trip at every
		// ladder size), and playback (DrawI420) expects studio range. FFmpeg
		// accepts it for MJPEG only below strict compliance.
		encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
		encoder_->color_range = AVCOL_RANGE_MPEG;
		encoder_->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
		encoder_->time_base = AVRational{1, static_cast<int>(std::max<uint32_t>(1, options_.timeBaseFps))};
		encoder_->flags |= AV_CODEC_FLAG_QSCALE;
		encoder_->qmin = options_.quality;
		encoder_->qmax = options_.quality;
		// thread_type MUST be explicit: with only thread_count > 1 FFmpeg picks
		// FRAME threading for MJPEG (AV_CODEC_FLAG_QSCALE skips its
		// single-thread exception), and frame threads hand packets back one or
		// two calls late (receive_packet() -> EAGAIN in the same call).
		encoder_->thread_type = FF_THREAD_SLICE;
		encoder_->thread_count = threads;
		if (avcodec_open2(encoder_, codec, nullptr) < 0) {
			avcodec_free_context(&encoder_);
			return false;
		}
		return true;
	};

	bool ok = open(static_cast<int>(sliceThreads));
	if (ok && (encoder_->active_thread_type & FF_THREAD_FRAME)) {
		Log(PipelineLogLevel::Warning,
		    "MJPEG encoder came up with frame threading (active_thread_type=%d) despite thread_type=slice -- "
		    "reopening it single-threaded",
		    encoder_->active_thread_type);
		avcodec_free_context(&encoder_);
		ok = open(1);
	} else if (!ok && sliceThreads > 1) {
		Log(PipelineLogLevel::Warning,
		    "failed to open the MJPEG encoder with %u slice threads -- retrying single-threaded", sliceThreads);
		ok = open(1);
	}
	if (!ok)
		return false;

	if (!encodeFrame_)
		encodeFrame_ = av_frame_alloc();
	if (!encodePacket_)
		encodePacket_ = av_packet_alloc();
	if (!encodeFrame_ || !encodePacket_) {
		CloseEncoder();
		return false;
	}
	// The frame buffer is reserved once per open and reused every frame:
	// receive_packet() always follows send_frame() in the same call (all-intra,
	// no lookahead), so the encoder never keeps a reference to it and
	// av_frame_make_writable() finds it writable.
	av_frame_unref(encodeFrame_);
	encodeFrame_->format = AV_PIX_FMT_YUV420P;
	encodeFrame_->width = static_cast<int>(width);
	encodeFrame_->height = static_cast<int>(height);
	if (av_frame_get_buffer(encodeFrame_, 0) < 0) {
		CloseEncoder();
		return false;
	}

	char huffman[96] = "default";
	if (huffmanResult < 0) {
		char error[64] = "";
		av_strerror(huffmanResult, error, sizeof(error));
		std::snprintf(huffman, sizeof(huffman), "FFmpeg's choice, setting default failed: %s", error);
	}
	Log(PipelineLogLevel::Info,
	    "MJPEG encoder open at %ux%u (qscale=%d, huffman=%s, slice threads=%d of %u requested, "
	    "active_thread_type=%d, %u logical CPUs)",
	    width, height, options_.quality, huffman, encoder_->thread_count, sliceThreads,
	    encoder_->active_thread_type, std::thread::hardware_concurrency());
	encodeWidth_ = width;
	encodeHeight_ = height;
	encodeSlices_ = sliceThreads;
	loggedFirstEncode_ = false;
	return true;
}

void MjpegCodec::CloseEncoder()
{
	if (encoder_)
		avcodec_free_context(&encoder_);
	if (encodeFrame_)
		av_frame_free(&encodeFrame_);
	if (encodePacket_)
		av_packet_free(&encodePacket_);
	encodeWidth_ = 0;
	encodeHeight_ = 0;
	encodeSlices_ = 0;
}

bool MjpegCodec::Encode(const I420View &frame, uint32_t sliceThreads, const uint8_t *&packet, size_t &bytes,
			bool &compressed)
{
	sliceThreads = std::max<uint32_t>(sliceThreads, 1);
	if (!encoder_ || encodeWidth_ != frame.width || encodeHeight_ != frame.height ||
	    encodeSlices_ != sliceThreads) {
		CloseEncoder();
		if (failedWidth_ == frame.width && failedHeight_ == frame.height && failedSlices_ == sliceThreads)
			return false;
		if (!OpenEncoder(frame.width, frame.height, sliceThreads)) {
			Log(PipelineLogLevel::Warning,
			    "failed to open the MJPEG encoder at %ux%u -- storing those frames uncompressed",
			    frame.width, frame.height);
			failedWidth_ = frame.width;
			failedHeight_ = frame.height;
			failedSlices_ = sliceThreads;
			return false;
		}
		failedWidth_ = failedHeight_ = failedSlices_ = 0;
	}
	if (av_frame_make_writable(encodeFrame_) < 0)
		return false;
	// av_frame_get_buffer() picks FFmpeg's own (padded) linesizes.
	const uint32_t widths[3] = {frame.width, frame.width / 2, frame.width / 2};
	const uint32_t heights[3] = {frame.height, frame.height / 2, frame.height / 2};
	for (int p = 0; p < 3; ++p) {
		for (uint32_t row = 0; row < heights[p]; ++row) {
			std::memcpy(encodeFrame_->data[p] + static_cast<size_t>(row) * encodeFrame_->linesize[p],
				    frame.plane[p] + static_cast<size_t>(row) * frame.stride[p], widths[p]);
		}
	}
	encodeFrame_->pts = pts_++;
	if (avcodec_send_frame(encoder_, encodeFrame_) < 0)
		return false;
	av_packet_unref(encodePacket_);
	// All-intra, no lookahead: the packet is always there right after the send.
	if (avcodec_receive_packet(encoder_, encodePacket_) < 0)
		return false;
	packet = encodePacket_->data;
	bytes = static_cast<size_t>(encodePacket_->size);
	compressed = true;
	if (!loggedFirstEncode_) {
		const size_t raw = I420FrameBytes(frame.width, frame.height);
		Log(PipelineLogLevel::Info, "first MJPEG encode at %ux%u: %zu bytes (raw I420 would be %zu -- %.1fx)",
		    frame.width, frame.height, bytes, raw, static_cast<double>(raw) / std::max<size_t>(1, bytes));
		loggedFirstEncode_ = true;
	}
	return true;
}

bool MjpegCodec::OpenDecoder()
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
	if (!codec)
		return false;
	decoder_ = avcodec_alloc_context3(codec);
	if (!decoder_)
		return false;
	// Explicit: n9.0's MJPEG decoder has no threading at all, and a future
	// default change must not start buffering frames across calls.
	decoder_->thread_count = 1;
	if (avcodec_open2(decoder_, codec, nullptr) < 0) {
		avcodec_free_context(&decoder_);
		return false;
	}
	if (!decodePacket_)
		decodePacket_ = av_packet_alloc();
	if (!decodePacket_) {
		avcodec_free_context(&decoder_);
		return false;
	}
	return true;
}

bool MjpegCodec::Decode(const uint8_t *packet, size_t bytes, DecodedImage &out)
{
	if (!decoder_) {
		if (decoderFailed_)
			return false;
		if (!OpenDecoder()) {
			decoderFailed_ = true;
			Log(PipelineLogLevel::Warning,
			    "failed to open the MJPEG decoder -- the delayed view repeats frames");
			return false;
		}
	}
	if (!packet || bytes == 0 || bytes > static_cast<size_t>(INT32_MAX))
		return false;
	av_packet_unref(decodePacket_);
	if (av_new_packet(decodePacket_, static_cast<int>(bytes)) < 0)
		return false;
	std::memcpy(decodePacket_->data, packet, bytes);
	if (avcodec_send_packet(decoder_, decodePacket_) < 0)
		return false;
	AVFrame *frame = av_frame_alloc();
	if (!frame)
		return false;
	if (avcodec_receive_frame(decoder_, frame) < 0 ||
	    (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) || frame->width <= 0 ||
	    frame->height <= 0) {
		av_frame_free(&frame);
		return false;
	}
	for (int p = 0; p < 3; ++p) {
		out.plane[p] = frame->data[p];
		out.stride[p] = static_cast<uint32_t>(frame->linesize[p]);
	}
	out.width = static_cast<uint32_t>(frame->width);
	out.height = static_cast<uint32_t>(frame->height);
	// The planes live in the decoder's buffer pool; freeing the frame (on
	// whichever thread drops the last reference) hands them back to it.
	out.owner = std::shared_ptr<void>(frame, [](void *p) {
		AVFrame *f = static_cast<AVFrame *>(p);
		av_frame_free(&f);
	});
	return true;
}

void MjpegCodec::Release()
{
	CloseEncoder();
	if (decoder_)
		avcodec_free_context(&decoder_);
	if (decodePacket_)
		av_packet_free(&decodePacket_);
	failedWidth_ = failedHeight_ = failedSlices_ = 0;
	decoderFailed_ = false;
}

} // namespace

std::unique_ptr<VideoCodec> MakeMjpegCodec(const MjpegCodecOptions &options)
{
	return std::make_unique<MjpegCodec>(options);
}

bool MjpegAvailable()
{
	return avcodec_find_encoder(AV_CODEC_ID_MJPEG) && avcodec_find_decoder(AV_CODEC_ID_MJPEG);
}

} // namespace trigglow
