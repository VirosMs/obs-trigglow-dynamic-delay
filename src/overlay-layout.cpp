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
} // namespace

float SafetyMargin(float canvasHeight)
{
	if (!(canvasHeight > 0.0f))
		canvasHeight = kDesignCanvasHeight;
	return std::round(canvasHeight * 0.022f);
}

Placement Place(float canvasWidth, float canvasHeight, uint32_t corner)
{
	if (!(canvasWidth > 0.0f) || !(canvasHeight > 0.0f)) {
		canvasWidth = 1920.0f;
		canvasHeight = kDesignCanvasHeight;
	}

	Placement p;
	p.scale = std::clamp(canvasHeight / kDesignCanvasHeight / kRenderScale, kMinScale, kMaxScale);
	p.width = kDesignWidth * kRenderScale * p.scale;
	p.height = kDesignHeight * kRenderScale * p.scale;
	p.margin = SafetyMargin(canvasHeight);

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

uint32_t ClampSeconds(uint32_t seconds)
{
	return std::clamp(seconds, kMinSeconds, kMaxSeconds);
}

std::string BadgeFileName(uint32_t seconds)
{
	return "delay-badge-" + std::to_string(ClampSeconds(seconds)) + "s.png";
}

} // namespace trigglow::overlaylayout
