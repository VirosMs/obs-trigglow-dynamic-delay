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

#include "codec-pipeline.hpp"

#include <cstdint>
#include <functional>
#include <memory>

// MJPEG over the vendored FFmpeg (n9.0.2), as a VideoCodec for CodecPipeline.
// Only built where CMakeLists.txt links FFmpeg (TRIGGLOW_HAVE_FFMPEG); other
// platforms store frames raw (MakeRawCodec()). No OBS dependency, so the
// benchmarks link it as is.
//
// One instance per thread (design invariant B.1.5): it opens, uses and closes
// its own encoder and decoder contexts, lazily, and reopens the encoder when
// the frame size or the slice-thread count changes (about 1 ms). Planar I420
// in, planar I420 out: since v0.6.0 the GPU hands back separate Y/U/V planes
// and playback uploads three planes, so there's no NV12 (de)interleave on the
// CPU any more. A decoded frame is the decoder's own AVFrame (no copy),
// returned to its buffer pool when the DecodedImage's owner goes away.
namespace trigglow {

struct MjpegCodecOptions {
	int quality = 5;           // qscale (AV_CODEC_FLAG_QSCALE, qmin = qmax).
	uint32_t timeBaseFps = 30; // Only has to be a valid time base (all-intra, pts is a counter).
	std::function<void(PipelineLogLevel, const char *)> log;
};

std::unique_ptr<VideoCodec> MakeMjpegCodec(const MjpegCodecOptions &options);

// Whether the linked FFmpeg has both an MJPEG encoder and decoder.
bool MjpegAvailable();

} // namespace trigglow
