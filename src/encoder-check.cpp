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

#include "buffer-fit.hpp"

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

Ceiling EncoderCeiling(const std::vector<CeilingEncoder> &encoders, Size base, Size output, bool capToOutput,
		       bool rawOutputOnMainMix)
{
	Ceiling ceiling;
	if (!capToOutput) {
		ceiling.why = CeilingWhy::CapOff;
		return ceiling;
	}
	if (rawOutputOnMainMix) {
		ceiling.why = CeilingWhy::RawOutput;
		return ceiling;
	}
	if (base.width == 0 || base.height == 0 || output.width == 0 || output.height == 0) {
		ceiling.why = CeilingWhy::NoEncoders;
		return ceiling;
	}
	double need = 0.0;
	bool any = false;
	for (const CeilingEncoder &e : encoders) {
		if (e.source == VideoSource::UnlistedMix && e.scope == Scope::OtherCanvas) {
			ceiling.why = CeilingWhy::UnknownMix;
			return ceiling;
		}
		if (e.scope == Scope::OtherCanvas || e.size.width == 0 || e.size.height == 0)
			continue;
		any = true;
		need = std::max(need, std::max(static_cast<double>(e.size.width) / base.width,
					       static_cast<double>(e.size.height) / base.height));
	}
	if (!any) {
		ceiling.why = CeilingWhy::NoEncoders;
		return ceiling;
	}
	// The output cap's own scale (bufferfit::OutputCapScale() for a canvas-sized scene).
	const double cap = std::min(1.0, std::max(static_cast<double>(output.width) / base.width,
						  static_cast<double>(output.height) / base.height));
	const Size size{bufferfit::AlignedWidth(base.width, need), bufferfit::EvenDim(base.height, need)};
	const Size capped{bufferfit::AlignedWidth(base.width, cap), bufferfit::EvenDim(base.height, cap)};
	if (need >= cap || (size.width >= capped.width && size.height >= capped.height)) {
		ceiling.why = CeilingWhy::NotBelowOutput;
		return ceiling;
	}
	ceiling.applies = true;
	ceiling.size = size;
	ceiling.why = CeilingWhy::Applied;
	return ceiling;
}

const char *CeilingWhyText(CeilingWhy why)
{
	switch (why) {
	case CeilingWhy::Applied:
		return "every encoder of the main canvas is below OBS's output";
	case CeilingWhy::NoEncoders:
		return "no active encoder of the main canvas";
	case CeilingWhy::RawOutput:
		return "an output without an encoder takes the main mix as it is";
	case CeilingWhy::UnknownMix:
		return "a GPU-rescaled encoder of another aspect ratio might be the main canvas";
	case CeilingWhy::CapOff:
		return "buffer_output_cap=false";
	case CeilingWhy::NotBelowOutput:
		return "an encoder needs OBS's whole output";
	}
	return "?";
}

} // namespace trigglow::encodercheck
