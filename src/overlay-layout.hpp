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
#include <string>

// Geometry of the live "Delay 30 s" overlay: one generated image
// (overlay-badge.hpp) placed in a corner of OBS's canvas. Pure C++, no
// libobs/Qt: tests/overlay_layout_tests.cpp.
namespace trigglow::overlaylayout {

// The badge as designed for a 1920x1080 canvas, in canvas pixels: a 200x72
// plate including its 1 px outer ring ("DELAY 30 s" over the "By Trigglow"
// signature; 200x58 before the signature).
inline constexpr float kDesignWidth = 200.0f;
inline constexpr float kDesignHeight = 72.0f;
inline constexpr float kDesignCanvasHeight = 1080.0f;
// The PNG is rendered at 2.5x the design size (500x180) so it stays sharp when
// OBS scales it, at 1080p and above.
inline constexpr float kRenderScale = 2.5f;
// Pixel size of the "By Trigglow" signature, in design units: 15 px on a
// 1080p canvas and 10 px on a 720p one, where the badge is drawn at 0.67x.
inline constexpr float kSignaturePixelSize = 15.0f;
// Bumped whenever the drawing changes, and part of the PNG's file name, so a
// badge an older build left in the folder is never what OBS shows (nor reused
// off the UI thread). 2: the "By Trigglow" signature and the taller plate.
inline constexpr uint32_t kRenderVersion = 2;
// Seconds the badge can show: the dock's spin box range.
inline constexpr uint32_t kMinSeconds = 1;
inline constexpr uint32_t kMaxSeconds = 60;

enum class Corner : uint32_t {
	TopLeft = 0,
	TopRight = 1,
	BottomLeft = 2,
	BottomRight = 3,
};

struct Placement {
	float x = 0.0f;
	float y = 0.0f;
	float scale = 1.0f; // Applied to the rendered PNG (kRenderScale x design size).
	float width = 0.0f; // Size on the canvas after scaling.
	float height = 0.0f;
	float margin = 0.0f;
};

// Distance from the canvas edges, so the platform's player controls and the
// zoom of clip tools don't crop the badge: 2.2 % of the canvas's short side
// (24 px at 1080p and on a 1080x1920 vertical canvas, 16 px at 720p).
float SafetyMargin(float canvasShortSide);

// Where the badge goes for `corner` (0-3; anything else is top-left, like
// BufferModeController::SetOverlayCorner) on a canvasWidth x canvasHeight
// canvas. The badge keeps the share of the canvas's SHORT side it has on
// 1080p, so it reads the same at 720p, 1440p and 4K and doesn't spread across
// a vertical (1080x1920) canvas; the scale is clamped so it never becomes
// unreadably small nor upscales the PNG beyond its rendered size. Invalid
// (zero) canvas sizes fall back to 1920x1080.
Placement Place(float canvasWidth, float canvasHeight, uint32_t corner);

// What the plugin last applied to the badge's scene item: the inputs of
// Place() and its result. Kept in the item's private settings, so it travels
// with the scene collection.
struct AppliedLayout {
	bool valid = false; // False: never placed by this version of the plugin.
	uint32_t corner = 0;
	float canvasWidth = 0.0f;
	float canvasHeight = 0.0f;
	float x = 0.0f;
	float y = 0.0f;
	float scale = 0.0f;
	// kDesignHeight of the badge it was placed for. 0: placed by a build that
	// didn't store it (the 58 px plate, before the signature).
	float designHeight = 0.0f;
};

// The item's position and scale as OBS has them now.
struct ItemTransform {
	float x = 0.0f;
	float y = 0.0f;
	float scaleX = 0.0f;
	float scaleY = 0.0f;
};

// True while `current` is still what the plugin applied: the user hasn't moved
// or scaled the badge in OBS since (half a pixel / 0.1 % of tolerance).
bool IsUntouched(const AppliedLayout &applied, const ItemTransform &current);

// Whether showing the badge has to (re)place it with Place(). The position and
// size the user gives it in OBS are kept: it is placed the first time, again
// whenever the corner chosen in the dock changes, and on a canvas size change
// or a new badge size (kDesignHeight) only while the user hasn't moved or
// scaled it (left where the plugin put it, it follows the new canvas and keeps
// its margin in a bottom corner; placed by the user, it stays put).
bool ShouldPlace(const AppliedLayout &applied, const ItemTransform &current, float canvasWidth, float canvasHeight,
		 uint32_t corner);

// "delay-badge-r2-30s.png" (render version, seconds): one file per value, so
// OBS reloads the image source when the value changes. Values outside
// [kMinSeconds, kMaxSeconds] are clamped.
std::string BadgeFileName(uint32_t seconds);
uint32_t ClampSeconds(uint32_t seconds);

// True for a badge PNG of another render version ("delay-badge-30s.png" from
// before versioned names, "delay-badge-r1-30s.png"...): left over in the
// overlay folder, to be deleted. False for the current version and for any
// file that isn't a badge.
bool IsStaleBadgeFile(const std::string &fileName);

} // namespace trigglow::overlaylayout
