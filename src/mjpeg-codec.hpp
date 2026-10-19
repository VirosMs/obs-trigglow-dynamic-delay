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

#include "storage-codec.hpp"

#include <memory>

// MJPEG over the vendored FFmpeg (n9.0.2), as the delay buffer's storage
// codec (storage-codec.hpp). Only built where CMakeLists.txt links FFmpeg
// (TRIGGLOW_HAVE_FFMPEG); other platforms store frames raw. No OBS
// dependency, so the benchmarks link it as is.
//
// All-intra and synchronous: Send() encodes right away (as v0.6.0's Encode()
// did) and the packet comes out of the next Receive(), always a key. One
// encoder per frame size and slice-thread count (the pipeline opens a new one
// when either changes, about 1 ms); its context is opened lazily by the first
// Send(), used and closed by the thread that owns the object. Planar I420 in,
// planar I420 out: since v0.6.0 the GPU hands back separate Y/U/V planes and
// playback uploads three planes, so there's no NV12 (de)interleave on the CPU.
// A decoded frame is the decoder's own AVFrame (no copy), returned to its
// buffer pool when the DecodedImage's owner goes away.
namespace trigglow {

// config: width, height and sliceThreads (the pipeline's), quality (qscale,
// AV_CODEC_FLAG_QSCALE with qmin = qmax) and fpsNum/fpsDen (only a valid
// time base: all-intra, pts is a counter). keyframeInterval is ignored.
std::unique_ptr<storage::Encoder> MakeMjpegEncoder(const storage::EncoderConfig &config, PipelineLogFn log);
std::unique_ptr<storage::Decoder> MakeMjpegDecoder(PipelineLogFn log);

// Whether the linked FFmpeg has both an MJPEG encoder and decoder.
bool MjpegAvailable();

} // namespace trigglow
