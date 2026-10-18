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

#include "encoder-check.hpp"

#include <algorithm>

namespace trigglow::encodercheck {

bool SameAspectRatio(Size a, Size b)
{
	if (a.width == 0 || a.height == 0 || b.width == 0 || b.height == 0)
		return false;
	// Cross-multiplied in 64 bits (no overflow for any uint32_t pair), no
	// floating point: |aw*bh - bw*ah| <= tol% of the larger product. Dividing
	// first can't overflow either; it truncates by less than 100 units out of
	// products in the hundreds of thousands, which doesn't matter here.
	const uint64_t lhs = static_cast<uint64_t>(a.width) * b.height;
	const uint64_t rhs = static_cast<uint64_t>(b.width) * a.height;
	const uint64_t diff = lhs > rhs ? lhs - rhs : rhs - lhs;
	const uint64_t larger = std::max(lhs, rhs);
	return diff <= larger / 100 * kAspectTolerancePercent;
}

Scope Classify(VideoSource source, Size encoder, Size mainBase, Size mainOutput)
{
	switch (source) {
	case VideoSource::MainMix:
		return Scope::MainCanvas;
	case VideoSource::OtherCanvasMix:
		return Scope::OtherCanvas;
	case VideoSource::UnlistedMix:
		break;
	}
	if (SameAspectRatio(encoder, mainBase) || SameAspectRatio(encoder, mainOutput))
		return Scope::MainCanvasByAspect;
	return Scope::OtherCanvas;
}

bool CapLimitsBuffer(Size cappedBuffer, Size uncappedBuffer)
{
	if (cappedBuffer.width == 0 || cappedBuffer.height == 0)
		return false;
	return uncappedBuffer.width > cappedBuffer.width || uncappedBuffer.height > cappedBuffer.height;
}

bool ShouldWarn(Scope scope, Size encoder, Size cappedBuffer, Size uncappedBuffer, bool capToOutput)
{
	if (!capToOutput || !CapLimitsBuffer(cappedBuffer, uncappedBuffer))
		return false;
	if (scope == Scope::OtherCanvas)
		return false;
	return encoder.width > cappedBuffer.width || encoder.height > cappedBuffer.height;
}

} // namespace trigglow::encodercheck
