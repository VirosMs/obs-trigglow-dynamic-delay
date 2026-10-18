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

// Pure sizing math for VideoDelayFilter's RAM ring: which resolution to buffer
// at and how many frames fit the RAM budget. Deliberately free of any libobs
// (or FFmpeg) dependency so tests/buffer_fit_tests.cpp can exercise it
// without OBS. VideoDelayFilter::EnsureRingSized() (actually resizes the ring)
// and VideoDelayFilter::EstimateBufferFit() (the dock's prediction) both call
// Compute() with the same Input, so what the dock predicts is what happens.
//
// Rules (v0.5.1, docs: buffer pipeline design A.1/A.6):
//   - Never buffer more pixels than OBS's main mix outputs. Anything above the
//     output resolution is thrown away by OBS's own output rescale anyway, but
//     it cost the plugin CPU (MJPEG encode/decode scale ~linearly with pixels).
//     The cap only applies when the target is canvas-sized (the normal live
//     scene case); anything else is left uncapped, the conservative choice.
//   - Quality floor second: never go below minResolutionHeight rows, unless
//     the output itself is smaller (storing more rows than OBS emits buys
//     nothing).
//   - Duration last: if the full delay doesn't fit the budget even at the
//     floor, the ring gets fewer frames instead.
//   - Exact frame rate (fpsNum/fpsDen), so 59.94 doesn't truncate to 59.
//   - Escape hatch: capToOutput = false (the hidden "buffer_output_cap"
//     plugin setting) drops the output cap and sizes like v0.5.0 did:
//     source resolution, only reduced for RAM, never below minResolutionHeight.
//     Not bit-identical to v0.5.0, though: the exact fps and the quarter
//     second of slack rounded up give one more slot at 25/30/50fps, and when
//     RAM runs short the row walk below can land up to 2px away from it.
namespace trigglow::bufferfit {

struct Input {
	uint32_t sourceWidth = 0; // obs_source_get_base_width/height(target)
	uint32_t sourceHeight = 0;
	uint32_t baseWidth = 0; // obs_video_info::base_* (canvas). 0 = unknown -> no output cap.
	uint32_t baseHeight = 0;
	uint32_t outputWidth = 0; // obs_video_info::output_*. 0 = unknown -> no output cap.
	uint32_t outputHeight = 0;
	uint32_t fpsNum = 0; // obs_video_info::fps_num / fps_den
	uint32_t fpsDen = 1;
	uint32_t delaySeconds = 0;
	uint32_t minResolutionHeight = 0;
	uint64_t budgetBytes = 0;
	double compressionRatio = 1.0; // Assumed bytes(raw NV12) / bytes(stored) per frame.
	// false = no output cap at all (OutputCapScale() returns 1.0). Support's
	// escape hatch for a case the cap gets wrong, see the header comment.
	bool capToOutput = true;
};

struct Result {
	uint32_t bufferWidth = 0; // Always even and >= 2 (NV12 chroma subsampling). 0 only for invalid input.
	uint32_t bufferHeight = 0;
	uint64_t actualFrames = 0;    // Ring slots that fit the budget (<= desiredFrames, >= 1).
	uint64_t desiredFrames = 0;   // Slots the full delay needs (delay*fps + ~0.25s slack + 1).
	double scale = 1.0;           // Final scale relative to the source.
	double capScale = 1.0;        // Ceiling from the OBS output resolution (1.0 = no cap).
	bool cappedByOutput = false;  // capScale < 1
	bool reducedByRam = false;    // scale < capScale
	bool fitsFullDuration = true; // actualFrames >= desiredFrames
	// v0.6.0 (design A.4): the delay video AND audio use, in whole seconds.
	// delaySeconds when the ring fits; otherwise the ring's span minus the
	// quarter second of slack, floored to whole seconds (without the slack if
	// that would leave 0). Before, video used the fractional span and audio
	// its floor: up to ~1 s apart when RAM trimmed the ring.
	uint32_t effectiveDelaySeconds = 0;
};

Result Compute(const Input &in);

// Exposed for tests and for VideoDelayFilter's own bookkeeping.
double OutputCapScale(const Input &in);
// llround, clamp to [2, orig], then forced even. The floor of 2 (one NV12
// chroma sample) wins over the orig ceiling: orig 0 or 1 gives 2, never 0.
uint32_t EvenDim(uint32_t orig, double scale);
uint64_t DesiredFrames(uint32_t delaySeconds, uint32_t fpsNum, uint32_t fpsDen);
// The quarter second of slack DesiredFrames() includes: ceil(fps / 4), 0 for a 0s delay.
uint64_t SlackFrames(uint32_t delaySeconds, uint32_t fpsNum, uint32_t fpsDen);
uint64_t FrameIntervalNs(uint32_t fpsNum, uint32_t fpsDen); // 1e9 * den / num; 0 if num == 0.
uint64_t Nv12FrameBytes(uint32_t width, uint32_t height);   // width * height * 3 / 2.

} // namespace trigglow::bufferfit
