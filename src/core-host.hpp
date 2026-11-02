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

// The core build's stand-in for the dock (TRIGGLOW_CORE_ONLY): the same
// BufferModeController and filters as the OBS build, driven by a host that
// can only create sources and read or write their settings (Trigglow Studio's
// engine, obs-studio-node, can't call proc handlers).
//
// The host creates one "trigglow_delay_controller" source (private, nothing
// to render) and talks to the controller through its settings:
//   in   live_scene, loading_scene (engine scene names), delay_seconds,
//        min_resolution_height, show_overlay, overlay_corner, the hidden
//        buffer_output_cap / codec_workers / buffer_codec ("auto" | "mjpeg",
//        read once, like buffer-mode-settings.json at OBS's start),
//        badge_prefix (the badge PNG for N seconds is <badge_prefix>N.png;
//        the host draws them, the core has no Qt) and badge_token (they
//        changed), host_* (the engine's video info, see
//        VideoDelayFilter::GetVideoInfo()), and command + command_token
//        (a new token runs it once: enable, disable, toggle, output_starting,
//        collection_changing);
//   out  status_* (state inactive|filling|active|error, message and its
//        locale key, fill generation and milliseconds left, delay, effective
//        and applied seconds, the wrapper scene's name), health_* (the
//        video buffer's PipelineHealth) and fit_* (EstimateBufferFit for the
//        settings), written into the same settings at the end of every
//        update -- the host polls with any update (e.g. a poll counter) and
//        reads the settings back. Every update waits for the controller
//        thread, so a command's result is there when it returns.
//
// The controller thread is the OBS build's UI thread: the controller, its
// fill timer (the dock's single-shot QTimer) and its once-a-second Tick()
// (the dock's health timer) all run on it.
namespace trigglow::corehost {

// Registers the controller source. obs_module_load().
void Register();

// Stops the controller thread and releases what the bridge holds.
// obs_module_unload().
void Shutdown();

// Runs task(param) on the controller thread. Any thread, never waits; dropped
// after Shutdown().
void QueueTask(void (*task)(void *), void *param);

} // namespace trigglow::corehost
