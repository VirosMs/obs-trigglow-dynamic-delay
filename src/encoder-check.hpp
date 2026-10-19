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

#include <cstdint>

// Pure decision behind BufferModeController's "video encoder above the delay
// buffer" WARN: which active encoders see OBS's main canvas (and so the
// delayed scene), and which of those encode above the buffer capped to OBS's
// output resolution. Free of libobs, like buffer-fit, so
// tests/encoder_check_tests.cpp can exercise it without OBS.
// ObsFrontendBridge::ListActiveVideoEncoders() gathers the inputs.
//
// How libobs 31.1.1 wires an encoder's video (obs-encoder.c, obs.c), which is
// what limits how exactly this can be known:
//   - An encoder renders the video_t it was given (obs_encoder_set_video():
//     obs_get_video() for the frontend's own outputs, obs_canvas_get_video()
//     for a canvas, or a plugin's own obs_view_add2() mix) at that mix's
//     output size, or scaled on the CPU to obs_encoder_set_scaled_size().
//     obs_encoder_parent_video() still returns that same video_t.
//   - With GPU scaling (Settings > Output > Advanced > Rescale output, every
//     Enhanced Broadcasting track, which also sets a preferred format), the
//     encoder is moved at start to an encoder-only mix of the SAME view at the
//     scaled size (maybe_set_up_gpu_rescale), so obs_encoder_parent_video()
//     returns a video_t that no public API maps back to its view or canvas.
// So: the main mix and the mixes of listed canvases are recognized exactly;
// any other mix is attributed by aspect ratio (see Classify()).
namespace trigglow::encodercheck {

struct Size {
	uint32_t width = 0;
	uint32_t height = 0;
};

// Which video_t obs_encoder_parent_video() returned, compared by
// ObsFrontendBridge against what libobs exposes.
enum class VideoSource {
	MainMix,        // == obs_get_video(): the main canvas's own mix. Exact.
	OtherCanvasMix, // == obs_canvas_get_video() of a non-main canvas from obs_enum_canvases(). Exact.
	// Anything else: an encoder-only GPU rescale mix of ANY canvas (the main
	// one, or e.g. a vertical canvas, through Rescale output or an Enhanced
	// Broadcasting extra canvas), a private canvas, or a plugin's own view.
	UnlistedMix,
};

enum class Scope {
	MainCanvas,         // Renders the main canvas, known exactly (VideoSource::MainMix).
	MainCanvasByAspect, // Heuristic: an unlisted mix with the main canvas's aspect ratio.
	OtherCanvas,        // Anything else; never warned about.
};

// Relative tolerance of SameAspectRatio(): 2%. Covers the even-rounded sizes
// OBS offers for rescaling (852x480 and 1366x768 for 16:9 are 0.2% and 0.04%
// off) while 16:10 (11% from 16:9), 4:3, 21:9 and 9:16 all stay distinct.
constexpr uint32_t kAspectTolerancePercent = 2;

// a.width / a.height within kAspectTolerancePercent of b.width / b.height.
// False if either size has a zero dimension.
bool SameAspectRatio(Size a, Size b);

// mainBase/mainOutput: obs_video_info base_* (canvas) and output_* of the main
// mix. An UnlistedMix counts as the main canvas when the encoder's size has
// the canvas's or the output's aspect ratio. Limits of that heuristic:
//   - another canvas or plugin view with the same aspect ratio as the main
//     canvas, GPU-rescaled, is taken for the main canvas (a WARN that doesn't
//     apply: say a second 16:9 canvas);
//   - a main-canvas encoder GPU-rescaled to another aspect ratio (stretched)
//     is missed (no WARN).
Scope Classify(VideoSource source, Size encoder, Size mainBase, Size mainOutput);

// The output cap is what limits the buffer: lifting it would buffer more
// pixels on some axis. False when no cap applies (output = canvas) or RAM
// sets the size anyway, and for an invalid (zero) capped size.
bool CapLimitsBuffer(Size cappedBuffer, Size uncappedBuffer);

// Whether to warn about one encoder: the cap is on (capToOutput, the hidden
// buffer_output_cap setting) and limits the buffer (CapLimitsBuffer), the
// encoder renders the main canvas (exactly or by aspect ratio), and it
// encodes above the capped buffer on either axis, so its delayed video is
// upscaled. cappedBuffer/uncappedBuffer are bufferfit::Compute() with the cap
// on and off, as the filter would size its ring.
bool ShouldWarn(Scope scope, Size encoder, Size cappedBuffer, Size uncappedBuffer, bool capToOutput);

} // namespace trigglow::encodercheck
