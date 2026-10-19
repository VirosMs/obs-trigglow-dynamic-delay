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

// Health of the video delay buffer's pipeline, for the dock (v0.6.0, design
// B.8): which degradation rung it's on and why, so the UI can show a
// non-blocking notice such as "Buffer reduced to 1280x720 because of CPU load".
//
// Read it with BufferModeController::GetPipelineHealth() (or
// ObsFrontendBridge::GetBufferPipelineHealth()) from the UI thread, as often as
// the dock likes (e.g. once a second): VideoDelayFilter keeps a copy under a
// small mutex and its "get_pipeline_state" proc only copies it, never hopping
// to the graphics thread, so a lagging render can't block the UI. Plain data,
// no OBS or Qt types.
namespace trigglow {

enum class PipelineLoad : uint8_t {
	None = 0,
	Graphics = 1, // OBS's graphics thread (capture/readback/upload, or the codec in sync mode).
	Encode = 2,   // MJPEG encode workers.
	Decode = 3,   // MJPEG decode worker.
};

struct PipelineHealth {
	uint32_t version = 2;     // Bumped if fields change meaning (2: v0.7.0, the storage codec).
	bool running = false;     // The delay buffer exists (Filling or Active).
	bool codecWorkers = true; // false: the hidden codec_workers=false kill switch (codec on the graphics thread).
	// Degradation rung: 0 = full quality. Rungs only ever lower the resolution
	// (never below the user's minimum quality) and, last, the capture frame
	// rate; the delay itself is never shortened by them.
	uint32_t level = 0;
	uint32_t maxLevel = 0;
	uint32_t width = 0; // What's buffered right now.
	uint32_t height = 0;
	uint32_t fullWidth = 0; // Rung 0 (OBS output resolution or the RAM fit).
	uint32_t fullHeight = 0;
	double captureFps = 0.0; // Output fps, or half of it on the last rung.
	double outputFps = 0.0;
	uint32_t encodeContexts = 0;             // Encode workers in use (1-2), 1 in sync mode.
	uint32_t encodeSlices = 0;               // Slice threads per encode context (1-4).
	PipelineLoad cause = PipelineLoad::None; // What triggered the last step down (None at level 0).
	// Over the last telemetry window (10 s): captures that never made it into
	// the buffer, and output frames that repeated the previous one because
	// the due frame wasn't ready (both show as a repeated frame, never as a
	// shift of the delay).
	double droppedPerMinute = 0.0;
	double repeatedPerMinute = 0.0;
	uint64_t changes = 0; // ++ on every rung or thread change: a cheap "something changed" check.

	// v2 (v0.7.0): the storage codec, for support diagnostics only -- the dock
	// shows none of it.
	uint8_t codec = 1;     // What's stored now (storage::CodecId): 0 raw, 1 MJPEG, 2 H.264, 3 HEVC.
	char encoder[16] = ""; // "hevc_nvenc", "mjpeg"...
	bool hwDecode = false; // Its frames decode on the GPU.
	uint8_t codecPolicyState =
		0; // storagepolicy::State: 0 Mjpeg, 1 Probing, 2 Hw, 3 HwGuarded, 4 Yielded, 5 Evacuated.
	uint64_t codecChanges = 0; // ++ on every storage codec policy decision.

	bool Degraded() const { return level > 0; }
};

} // namespace trigglow
