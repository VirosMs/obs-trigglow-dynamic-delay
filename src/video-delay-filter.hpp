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
#include <vector>

extern "C" {
#include <obs.h>
#include <graphics/graphics.h>
}

// PHASE 1 (issue #173, "modo sin reconexion") — video-only, no audio yet.
//
// A standard OBS video-effect FILTER (not a source of its own): attached by
// the user to whatever source/scene they want delayed, via OBS's own
// Filters dialog — same UX as any other filter (Color Correction, Chroma
// Key, etc). NOT wired into DelayController/settings-ui yet; that
// integration (a hotkey/dock toggle that arms this + switches to it) is
// follow-up work once this core mechanism is verified live.
//
// Mechanism: every video_render call, renders the filter's target source
// into the NEXT slot of a ring buffer of GPU textures (gs_texrender_t),
// then draws whichever slot is `delaySeconds` old. The streaming OUTPUT
// itself is never touched — no reconnection, ever. This is the same
// technique real-world "no reconnect" OBS delay filters use (GPU texture
// ring buffer via gs_texrender, not libobs's async obs_source_frame
// pipeline) — verified against the real OBS 31.1.1 headers before writing
// this, not guessed.
//
// Memory is the hard constraint here, not CPU: uncompressed RGBA at
// 1920x1080@60fps is ~475MB PER SECOND of buffer (1920*1080*4 bytes *
// 60). A careless "just buffer N seconds" implementation is how you end up
// with the multi-GB RAM/VRAM blowups real users report against similar
// tools. So the ring buffer is sized from an actual VRAM budget
// (kMaxBufferBytes), not directly from the requested delay — requesting
// more than the budget allows silently clamps (logged), it never grows
// unbounded.
namespace trigglow {

class VideoDelayFilter {
public:
	static const char *Id();

	// Registers this filter type with OBS. Call once from obs_module_load().
	static void Register();

private:
	// One buffered historical frame.
	struct Slot {
		gs_texrender_t *texrender = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		bool valid = false; // false until first successfully rendered.
	};

	explicit VideoDelayFilter(obs_source_t *filterSource);
	~VideoDelayFilter();

	void Update(obs_data_t *settings);
	void Tick(float secondsSinceLastTick);
	void Render();
	uint32_t GetWidth() const;
	uint32_t GetHeight() const;

	// Resizes ring_ (VRAM-budget-capped) for the given target resolution and
	// current output frame rate. No-op if slotCapacity_ is already correct
	// for this width/height. Must be called from within a video_render (or
	// otherwise graphics-context-active) call, since it creates/destroys
	// gs_texrender_t objects.
	void EnsureRingSized(uint32_t width, uint32_t height);
	void ReleaseRing();

	// --- obs_source_info callback trampolines (static: OBS calls C
	// function pointers, not member functions) ---
	static const char *GetName(void *typeData);
	static void *Create(obs_data_t *settings, obs_source_t *source);
	static void Destroy(void *data);
	static void UpdateCb(void *data, obs_data_t *settings);
	static void TickCb(void *data, float seconds);
	static void RenderCb(void *data, gs_effect_t *effect);
	static uint32_t GetWidthCb(void *data);
	static uint32_t GetHeightCb(void *data);
	static obs_properties_t *GetProperties(void *data);
	static void GetDefaults(obs_data_t *settings);

	obs_source_t *filterSource_; // Not owned; valid for this object's lifetime.
	uint32_t configuredDelaySeconds_ = 0;

	std::vector<Slot> ring_;
	size_t writeIndex_ = 0;
	size_t bufferedCount_ = 0; // How many ring_ slots hold a real rendered frame so far.
	uint32_t currentFps_ = 30; // Updated from obs_get_video_info() each Tick().
};

} // namespace trigglow
