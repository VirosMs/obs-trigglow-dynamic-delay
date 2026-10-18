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

#include "overlay-layout.hpp"

#include <algorithm>
#include <cmath>

namespace trigglow::overlaylayout {

namespace {
// Below ~0.5x of the 1080p size the label stops being legible on a phone;
// above 1.0 OBS would upscale the 2.5x PNG past its native resolution.
constexpr float kMinScale = 0.2f;
constexpr float kMaxScale = 1.0f;

// Anything but 1-3 is top-left, like Place() and SetOverlayCorner().
uint32_t NormalizedCorner(uint32_t corner)
{
	return corner > static_cast<uint32_t>(Corner::BottomRight) ? static_cast<uint32_t>(Corner::TopLeft) : corner;
}
} // namespace

float SafetyMargin(float canvasShortSide)
{
	if (!(canvasShortSide > 0.0f))
		canvasShortSide = kDesignCanvasHeight;
	return std::round(canvasShortSide * 0.022f);
}

Placement Place(float canvasWidth, float canvasHeight, uint32_t corner)
{
	if (!(canvasWidth > 0.0f) || !(canvasHeight > 0.0f)) {
		canvasWidth = 1920.0f;
		canvasHeight = kDesignCanvasHeight;
	}
	// Landscape: the height. Vertical (1080x1920): the width, so the badge
	// takes the same share of the frame's width as on a 1080p landscape canvas.
	const float shortSide = std::min(canvasWidth, canvasHeight);

	Placement p;
	p.scale = std::clamp(shortSide / kDesignCanvasHeight / kRenderScale, kMinScale, kMaxScale);
	p.width = kDesignWidth * kRenderScale * p.scale;
	p.height = kDesignHeight * kRenderScale * p.scale;
	p.margin = SafetyMargin(shortSide);

	const bool right = corner == static_cast<uint32_t>(Corner::TopRight) ||
			   corner == static_cast<uint32_t>(Corner::BottomRight);
	const bool bottom = corner == static_cast<uint32_t>(Corner::BottomLeft) ||
			    corner == static_cast<uint32_t>(Corner::BottomRight);
	p.x = right ? canvasWidth - p.width - p.margin : p.margin;
	p.y = bottom ? canvasHeight - p.height - p.margin : p.margin;
	// A canvas narrower than the badge plus both margins: pin it to the left/top.
	p.x = std::max(p.x, 0.0f);
	p.y = std::max(p.y, 0.0f);
	return p;
}

bool IsUntouched(const AppliedLayout &applied, const ItemTransform &current)
{
	if (!applied.valid)
		return false;
	constexpr float kPositionTolerance = 0.5f; // Canvas pixels.
	constexpr float kScaleTolerance = 0.001f;  // Relative.
	const float scaleTolerance = std::max(applied.scale, 0.0f) * kScaleTolerance + 1e-6f;
	return std::fabs(current.x - applied.x) <= kPositionTolerance &&
	       std::fabs(current.y - applied.y) <= kPositionTolerance &&
	       std::fabs(current.scaleX - applied.scale) <= scaleTolerance &&
	       std::fabs(current.scaleY - applied.scale) <= scaleTolerance;
}

bool ShouldPlace(const AppliedLayout &applied, const ItemTransform &current, float canvasWidth, float canvasHeight,
		 uint32_t corner)
{
	if (!applied.valid)
		return true; // First show (or a badge from v0.5.x, drawn at another size).
	if (NormalizedCorner(corner) != NormalizedCorner(applied.corner))
		return true; // The dock asked for another corner: that wins over a manual move.
	const bool canvasChanged = std::fabs(canvasWidth - applied.canvasWidth) >= 0.5f ||
				   std::fabs(canvasHeight - applied.canvasHeight) >= 0.5f;
	return canvasChanged && IsUntouched(applied, current);
}

uint32_t ClampSeconds(uint32_t seconds)
{
	return std::clamp(seconds, kMinSeconds, kMaxSeconds);
}

std::string BadgeFileName(uint32_t seconds)
{
	return "delay-badge-" + std::to_string(ClampSeconds(seconds)) + "s.png";
}

} // namespace trigglow::overlaylayout
