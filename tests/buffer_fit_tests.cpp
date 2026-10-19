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

// Standalone unit tests for trigglow::bufferfit (src/buffer-fit.hpp). No
// libobs, no test framework: a tiny CHECK macro that keeps going after a
// failure and a non-zero exit code if anything failed. Not assert(): CMake's
// RelWithDebInfo/Release define NDEBUG, which would silently compile every
// check away. Build with -DTRIGGLOW_BUILD_TESTS=ON, run with ctest.

#include "buffer-fit.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using trigglow::bufferfit::AlignedWidth;
using trigglow::bufferfit::ApplyStorageClass;
using trigglow::bufferfit::ParseRingClass;
using trigglow::bufferfit::RingClass;
using trigglow::bufferfit::RingClassName;
using trigglow::bufferfit::Compute;
using trigglow::bufferfit::DesiredFrames;
using trigglow::bufferfit::EvenDim;
using trigglow::bufferfit::FrameIntervalNs;
using trigglow::bufferfit::Input;
using trigglow::bufferfit::KeyframeIntervalFrames;
using trigglow::bufferfit::Nv12FrameBytes;
using trigglow::bufferfit::OutputCapScale;
using trigglow::bufferfit::Result;
using trigglow::bufferfit::SlackFrames;

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_currentTest = "";

void Check(bool ok, const char *expr, const char *file, int line)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("FAIL [%s] %s:%d: %s\n", g_currentTest, file, line, expr);
	}
}

#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                                                            \
	do {                                                                                                      \
		const auto checkA_ = (a);                                                                         \
		const auto checkB_ = (b);                                                                         \
		if (!(checkA_ == checkB_))                                                                        \
			std::printf("  %s = %llu, %s = %llu\n", #a, static_cast<unsigned long long>(checkA_), #b, \
				    static_cast<unsigned long long>(checkB_));                                    \
		Check(checkA_ == checkB_, #a " == " #b, __FILE__, __LINE__);                                      \
	} while (0)
#define CHECK_NEAR(a, b, eps) Check(std::fabs((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;
constexpr uint64_t kMiB = 1024ULL * 1024;

// Shared defaults from the design's test table: 60/1 fps, 30s delay, assumed
// 5x compression, 16 GiB budget.
Input MakeInput(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH, uint32_t minRes = 720)
{
	Input in;
	in.sourceWidth = srcW;
	in.sourceHeight = srcH;
	in.baseWidth = srcW;
	in.baseHeight = srcH;
	in.outputWidth = outW;
	in.outputHeight = outH;
	in.fpsNum = 60;
	in.fpsDen = 1;
	in.delaySeconds = 30;
	in.minResolutionHeight = minRes;
	in.budgetBytes = 16 * kGiB;
	in.compressionRatio = 5.0;
	return in;
}

bool IsEvenInRange(uint32_t dim, uint32_t src)
{
	return dim >= 2 && dim <= src && (dim % 2) == 0;
}

// Widths: a multiple of 8 (whole 4-byte rows in Y, U and V for OpenGL), never
// above the source unless the source is narrower than 8.
bool IsAlignedWidthInRange(uint32_t width, uint32_t src)
{
	return width >= 8 && width % 8 == 0 && (width <= src || src < 8);
}

// What the ring would really occupy, against the same assumed ratio.
bool WithinBudget(const Input &in, const Result &r)
{
	const double perFrame =
		static_cast<double>(Nv12FrameBytes(r.bufferWidth, r.bufferHeight)) / in.compressionRatio;
	return static_cast<double>(r.actualFrames) * perFrame <= static_cast<double>(in.budgetBytes);
}

void TestNoCap()
{
	g_currentTest = "1 canvas == output";
	Result r = Compute(MakeInput(1920, 1080, 1920, 1080));
	CHECK_EQ(r.bufferWidth, 1920u);
	CHECK_EQ(r.bufferHeight, 1080u);
	CHECK_EQ(r.desiredFrames, 1816u); // 30*60 + 15 + 1
	CHECK_EQ(r.actualFrames, 1816u);
	CHECK_NEAR(r.capScale, 1.0, 1e-12);
	CHECK(!r.cappedByOutput);
	CHECK(!r.reducedByRam);
	CHECK(r.fitsFullDuration);
}

void TestTicketCase()
{
	g_currentTest = "2 canvas 1440p -> output 1080p";
	Result r = Compute(MakeInput(2560, 1440, 1920, 1080, 1080));
	CHECK_EQ(r.bufferWidth, 1920u);
	CHECK_EQ(r.bufferHeight, 1080u);
	CHECK_EQ(r.actualFrames, 1816u);
	CHECK_NEAR(r.capScale, 0.75, 1e-12);
	CHECK(r.cappedByOutput);
	CHECK(!r.reducedByRam);
	CHECK(r.fitsFullDuration);
}

void Test4kTo1080()
{
	g_currentTest = "3 canvas 4K -> output 1080p";
	Result r = Compute(MakeInput(3840, 2160, 1920, 1080));
	CHECK_EQ(r.bufferWidth, 1920u);
	CHECK_EQ(r.bufferHeight, 1080u);
	CHECK_NEAR(r.capScale, 0.5, 1e-12);
	CHECK(r.cappedByOutput);
}

void TestOutputAboveCanvas()
{
	g_currentTest = "4 output > canvas";
	Result r = Compute(MakeInput(1280, 720, 1920, 1080));
	CHECK_EQ(r.bufferWidth, 1280u);
	CHECK_EQ(r.bufferHeight, 720u);
	CHECK_NEAR(r.capScale, 1.0, 1e-12);
	CHECK(!r.cappedByOutput);
}

void TestAspectMismatch()
{
	g_currentTest = "5 canvas/output aspect mismatch";
	Result r = Compute(MakeInput(2560, 1440, 1440, 1080));
	CHECK_NEAR(r.capScale, 0.75, 1e-12);
	CHECK_EQ(r.bufferWidth, 1920u);
	CHECK_EQ(r.bufferHeight, 1080u);
	CHECK(r.bufferWidth >= 1440u && r.bufferHeight >= 1080u); // Never below the output on either axis.

	Input wide = MakeInput(1920, 1200, 1920, 1080);
	CHECK_NEAR(OutputCapScale(wide), 1.0, 1e-12);
	Result w = Compute(wide);
	CHECK_EQ(w.bufferWidth, 1920u);
	CHECK_EQ(w.bufferHeight, 1200u);

	Input ultrawide = MakeInput(2560, 1080, 1920, 1080);
	CHECK_NEAR(OutputCapScale(ultrawide), 1.0, 1e-12);
}

void TestFloorAboveCeiling()
{
	g_currentTest = "6 output below min resolution";
	Result r = Compute(MakeInput(1920, 1080, 1280, 720, 1080));
	CHECK_EQ(r.bufferWidth, 1280u);
	CHECK_EQ(r.bufferHeight, 720u);
	CHECK(r.cappedByOutput);
	CHECK(r.fitsFullDuration);
}

void TestRamReductionInRange()
{
	g_currentTest = "7 RAM reduction between floor and ceiling";
	Input in = MakeInput(2560, 1440, 1920, 1080, 720);
	// 1080p needs ~1.13 GB for 1816 frames at 5x, 720p ~0.50 GB.
	in.budgetBytes = 800 * kMiB;
	Result r = Compute(in);
	CHECK(r.scale >= 0.5 - 1e-12 && r.scale <= 0.75 + 1e-12);
	CHECK(r.reducedByRam);
	CHECK(r.cappedByOutput);
	CHECK(IsAlignedWidthInRange(r.bufferWidth, 2560));
	CHECK(IsEvenInRange(r.bufferHeight, 1440));
	CHECK(r.bufferHeight > 720u && r.bufferHeight < 1080u);
	CHECK(WithinBudget(in, r));
	// Above the floor the RAM reduction must leave room for the full delay.
	CHECK(r.fitsFullDuration);
	CHECK_EQ(r.actualFrames, r.desiredFrames);
}

void TestFloorActive()
{
	g_currentTest = "8 floor active, duration trimmed";
	Input in = MakeInput(2560, 1440, 1920, 1080, 720);
	in.budgetBytes = 100 * kMiB;
	Result r = Compute(in);
	CHECK_EQ(r.bufferWidth, 1280u);
	CHECK_EQ(r.bufferHeight, 720u);
	CHECK(!r.fitsFullDuration);
	CHECK(r.actualFrames < r.desiredFrames);
	CHECK(r.actualFrames >= 1u);
	CHECK(WithinBudget(in, r));
	// floor(100 MiB / (1280*720*1.5/5))
	CHECK_EQ(r.actualFrames, static_cast<uint64_t>((100 * kMiB) / (1280ULL * 720 * 3 / 2 / 5)));
}

void TestFloorRounding()
{
	g_currentTest = "9 rounding: llround, then even";
	Input in = MakeInput(3840, 2160, 3840, 2160, 720);
	in.budgetBytes = 100 * kMiB;
	Result r = Compute(in);
	CHECK_EQ(r.bufferWidth, 1280u);
	CHECK_EQ(r.bufferHeight, 720u);
	CHECK_EQ(EvenDim(3840, 720.0 / 2160.0), 1280u);
	CHECK_EQ(EvenDim(2160, 720.0 / 2160.0), 720u);
	// 596 * (480 / 596) is 479.99999999999994 in doubles: truncating gave 478.
	CHECK_EQ(EvenDim(596, 480.0 / 596.0), 480u);
	// Nearest, not floor: 1366 * (480 / 768) = 853.75 -> 854, not 852; then
	// the nearest multiple of 8 for the width: 856 (v0.6.0, OpenGL rows).
	Input wxga = MakeInput(1366, 768, 1366, 768, 480);
	wxga.budgetBytes = 16 * kMiB;
	Result w = Compute(wxga);
	CHECK_EQ(w.bufferWidth, 856u);
	CHECK_EQ(w.bufferHeight, 480u);
	CHECK_EQ(AlignedWidth(3840, 720.0 / 2160.0), 1280u);
	CHECK_EQ(AlignedWidth(1366, 480.0 / 768.0), 856u);
	CHECK_EQ(AlignedWidth(1920, 0.83), 1592u);  // 1593.6 -> 1594 -> 1592.
	CHECK_EQ(AlignedWidth(1920, 0.835), 1600u); // 1603.2 -> 1603 -> 1600.
	CHECK_EQ(AlignedWidth(1924, 1.0), 1920u);   // 1928 is nearer, but wider than the source.
}

void TestOddDimensions()
{
	g_currentTest = "10 odd source dimensions";
	Result r = Compute(MakeInput(1365, 767, 1365, 767));
	CHECK(IsAlignedWidthInRange(r.bufferWidth, 1365));
	CHECK(IsEvenInRange(r.bufferHeight, 767));
	// 1368 would be nearer but is wider than the source: 1360.
	CHECK_EQ(r.bufferWidth, 1360u);
	CHECK_EQ(r.bufferHeight, 766u);
	CHECK_EQ(EvenDim(1, 1.0), 2u);
	CHECK_EQ(EvenDim(1365, 0.0), 2u);
}

void TestDegenerate()
{
	g_currentTest = "11 degenerate input";
	Result zeroSrc = Compute(MakeInput(0, 1080, 1920, 1080));
	CHECK_EQ(zeroSrc.bufferWidth, 0u);
	CHECK_EQ(zeroSrc.actualFrames, 0u);

	Input zeroFps = MakeInput(1920, 1080, 1920, 1080);
	zeroFps.fpsNum = 0;
	CHECK_EQ(Compute(zeroFps).bufferWidth, 0u);
	zeroFps.fpsNum = 60;
	zeroFps.fpsDen = 0;
	CHECK_EQ(Compute(zeroFps).bufferWidth, 0u);

	// Unknown mix geometry: no cap.
	Input unknownOut = MakeInput(2560, 1440, 0, 0);
	CHECK_NEAR(OutputCapScale(unknownOut), 1.0, 1e-12);
	CHECK_EQ(Compute(unknownOut).bufferWidth, 2560u);
	Input unknownBase = MakeInput(2560, 1440, 1920, 1080);
	unknownBase.baseWidth = 0;
	unknownBase.baseHeight = 0;
	CHECK_NEAR(OutputCapScale(unknownBase), 1.0, 1e-12);
	CHECK_EQ(Compute(unknownBase).bufferHeight, 1440u);
}

void TestNonCanvasTarget()
{
	g_currentTest = "12 target not canvas-sized";
	Input in = MakeInput(1280, 720, 1920, 1080);
	in.baseWidth = 2560;
	in.baseHeight = 1440;
	Result r = Compute(in);
	CHECK_NEAR(r.capScale, 1.0, 1e-12);
	CHECK_EQ(r.bufferWidth, 1280u);
	CHECK_EQ(r.bufferHeight, 720u);
}

void TestZeroDelay()
{
	g_currentTest = "13 zero delay";
	Input in = MakeInput(1920, 1080, 1920, 1080);
	in.delaySeconds = 0;
	Result r = Compute(in);
	CHECK_EQ(r.desiredFrames, 1u);
	CHECK_EQ(r.actualFrames, 1u);
	CHECK_EQ(DesiredFrames(0, 60, 1), 1u);
}

void TestFractionalFps()
{
	g_currentTest = "14 fractional fps";
	Input in = MakeInput(2560, 1440, 1920, 1080);
	in.fpsNum = 60000;
	in.fpsDen = 1001;
	Result r = Compute(in);
	// ceil(30 * 59.94) + ceil(59.94 / 4) + 1 = 1799 + 15 + 1
	CHECK_EQ(r.desiredFrames, 1815u);
	CHECK_EQ(r.actualFrames, 1815u);
	CHECK_EQ(DesiredFrames(30, 60, 1), 1816u);
	CHECK_EQ(DesiredFrames(30, 30, 1), 909u);          // 900 + ceil(7.5) + 1
	CHECK_EQ(DesiredFrames(30, 30000, 1001), 909u);    // ceil(899.1) + ceil(7.49) + 1
	CHECK_EQ(FrameIntervalNs(60000, 1001), 16683333u); // Not 1e9 / 59 = 16949152.
	CHECK_EQ(FrameIntervalNs(60, 1), 16666666u);
	CHECK_EQ(FrameIntervalNs(0, 1), 0u);
	// The ring must span at least the requested delay with the exact interval.
	CHECK((r.actualFrames - 1) * FrameIntervalNs(60000, 1001) >= 30ULL * 1000000000ULL);
}

void TestCapDisabled()
{
	g_currentTest = "16 buffer_output_cap = false (v0.5.0 sizing)";
	// The ticket case without the cap: canvas resolution again.
	Input ticket = MakeInput(2560, 1440, 1920, 1080, 1080);
	ticket.capToOutput = false;
	CHECK_NEAR(OutputCapScale(ticket), 1.0, 1e-12);
	Result r = Compute(ticket);
	CHECK_EQ(r.bufferWidth, 2560u);
	CHECK_EQ(r.bufferHeight, 1440u);
	CHECK_NEAR(r.capScale, 1.0, 1e-12);
	CHECK(!r.cappedByOutput);
	CHECK(!r.reducedByRam);
	CHECK(r.fitsFullDuration);

	// The quality floor is no longer limited by the output: 1080p minimum
	// with a 720p output buffers at 1080p (with the cap it is 720p, test 6).
	Input floorAbove = MakeInput(1920, 1080, 1280, 720, 1080);
	floorAbove.capToOutput = false;
	Result f = Compute(floorAbove);
	CHECK_EQ(f.bufferWidth, 1920u);
	CHECK_EQ(f.bufferHeight, 1080u);
	CHECK(!f.cappedByOutput);

	// Floor active and duration trimmed, as in v0.5.0: 1440p canvas, 1080p
	// floor, 720p output, small budget. With the cap the floor would drop to
	// the 720p ceiling; without it the 1080p floor holds (1920x1080).
	Input tight = MakeInput(2560, 1440, 1280, 720, 1080);
	tight.capToOutput = false;
	tight.budgetBytes = 100 * kMiB;
	Result t = Compute(tight);
	CHECK_EQ(t.bufferWidth, 1920u);
	CHECK_EQ(t.bufferHeight, 1080u);
	CHECK(t.reducedByRam);
	CHECK(!t.fitsFullDuration);
	CHECK(WithinBudget(tight, t));

	// When RAM, not the output, sets the size below the cap, the escape hatch
	// changes nothing (same ideal scale, clamped to a wider range).
	Input ram = MakeInput(2560, 1440, 1920, 1080, 720);
	ram.budgetBytes = 800 * kMiB;
	Result capped = Compute(ram);
	ram.capToOutput = false;
	Result uncapped = Compute(ram);
	CHECK_EQ(uncapped.bufferWidth, capped.bufferWidth);
	CHECK_EQ(uncapped.bufferHeight, capped.bufferHeight);
	CHECK_EQ(uncapped.actualFrames, capped.actualFrames);
	CHECK(!uncapped.cappedByOutput);

	// The output resolution plays no part at all without the cap.
	for (const uint32_t out : {2160u, 1080u, 720u, 480u}) {
		Input in = MakeInput(3840, 2160, out * 16 / 9, out, 720);
		in.capToOutput = false;
		Result o = Compute(in);
		CHECK_EQ(o.bufferWidth, 3840u);
		CHECK_EQ(o.bufferHeight, 2160u);
	}
}

void TestTinySource()
{
	g_currentTest = "17 EvenDim and AlignedWidth floors win over the source size";
	// NV12 needs at least one chroma sample: 2, even when that is larger than
	// the source (never 0, which Compute() would report as invalid input).
	CHECK_EQ(EvenDim(1, 1.0), 2u);
	CHECK_EQ(EvenDim(1, 0.25), 2u);
	CHECK_EQ(EvenDim(0, 1.0), 2u);
	CHECK_EQ(EvenDim(2, 0.1), 2u);
	CHECK_EQ(EvenDim(3, 1.0), 2u);
	// Widths: 8 at least (one 4-byte U/V row).
	CHECK_EQ(AlignedWidth(1, 1.0), 8u);
	CHECK_EQ(AlignedWidth(0, 1.0), 8u);
	CHECK_EQ(AlignedWidth(7, 1.0), 8u);
	CHECK_EQ(AlignedWidth(15, 1.0), 8u);
	CHECK_EQ(AlignedWidth(1365, 0.0), 8u);
	Result r = Compute(MakeInput(1, 1, 1, 1));
	CHECK_EQ(r.bufferWidth, 8u);
	CHECK_EQ(r.bufferHeight, 2u);
	CHECK_EQ(r.actualFrames, r.desiredFrames);
	Result thin = Compute(MakeInput(1, 1080, 1, 1080));
	CHECK_EQ(thin.bufferWidth, 8u);
	CHECK_EQ(thin.bufferHeight, 1080u);
}

void TestProperties()
{
	g_currentTest = "15 property sweep";
	const uint32_t sources[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160},
				       {1365, 767},  {1280, 720},  {2560, 1080}};
	const uint32_t outputs[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1600, 900}, {1280, 720}, {852, 480}};
	const uint32_t minResolutions[] = {480, 720, 1080, 1440, 2160};
	const uint32_t delays[] = {5, 30, 60};
	int violations = 0;

	for (const auto &src : sources) {
		for (uint32_t minRes : minResolutions) {
			for (uint32_t delay : delays) {
				// More budget never shrinks the buffer or the ring.
				Result prev;
				bool havePrev = false;
				for (uint64_t budget = 16 * kMiB; budget <= 24 * kGiB; budget += 48 * kMiB) {
					Input in = MakeInput(src[0], src[1], 1920, 1080, minRes);
					in.delaySeconds = delay;
					in.budgetBytes = budget;
					Result r = Compute(in);
					bool ok = IsAlignedWidthInRange(r.bufferWidth, src[0]) &&
						  IsEvenInRange(r.bufferHeight, src[1]) && r.actualFrames >= 1 &&
						  r.actualFrames <= r.desiredFrames &&
						  (r.actualFrames == 1 || WithinBudget(in, r));
					if (havePrev) {
						ok = ok && r.bufferWidth >= prev.bufferWidth &&
						     r.bufferHeight >= prev.bufferHeight &&
						     r.actualFrames >= prev.actualFrames;
					}
					if (!ok && violations++ < 5) {
						std::printf(
							"  budget sweep violation: src %ux%u min %u delay %u budget %lluMiB "
							"-> %ux%u x%llu\n",
							src[0], src[1], minRes, delay,
							static_cast<unsigned long long>(budget / kMiB), r.bufferWidth,
							r.bufferHeight,
							static_cast<unsigned long long>(r.actualFrames));
					}
					prev = r;
					havePrev = true;
				}

				// A smaller output never grows the buffer, at any budget.
				for (uint64_t budget : {64 * kMiB, 700 * kMiB, 2 * kGiB, 16 * kGiB}) {
					Result prevOut;
					bool havePrevOut = false;
					for (const auto &out : outputs) {
						Input in = MakeInput(src[0], src[1], out[0], out[1], minRes);
						in.delaySeconds = delay;
						in.budgetBytes = budget;
						Result r = Compute(in);
						bool ok = IsAlignedWidthInRange(r.bufferWidth, src[0]) &&
							  IsEvenInRange(r.bufferHeight, src[1]);
						if (havePrevOut)
							ok = ok && r.bufferWidth <= prevOut.bufferWidth &&
							     r.bufferHeight <= prevOut.bufferHeight;
						if (!ok && violations++ < 5) {
							std::printf(
								"  output sweep violation: src %ux%u out %ux%u min %u "
								"delay %u -> %ux%u\n",
								src[0], src[1], out[0], out[1], minRes, delay,
								r.bufferWidth, r.bufferHeight);
						}
						prevOut = r;
						havePrevOut = true;
					}
				}
			}
		}
	}
	CHECK_EQ(violations, 0);
}

// Design test 16 (A.4, v0.6.0): video and audio share one whole-second delay.
void TestEffectiveDelay()
{
	g_currentTest = "16 effective delay (A.4)";
	// Fits: exactly the requested delay.
	Result full = Compute(MakeInput(2560, 1440, 1920, 1080, 1080));
	CHECK(full.fitsFullDuration);
	CHECK_EQ(full.effectiveDelaySeconds, 30u);

	// Trimmed by RAM at the 720p floor: 379 frames -> span 378, minus 15 of
	// slack = 363 frames = 6.05 s -> 6 s (v0.5.1 video used all 6.3 s).
	Input in = MakeInput(2560, 1440, 1920, 1080, 720);
	in.budgetBytes = 100 * kMiB;
	Result r = Compute(in);
	CHECK(!r.fitsFullDuration);
	CHECK_EQ(r.actualFrames, 379u);
	CHECK_EQ(r.effectiveDelaySeconds, 6u);
	CHECK_EQ(SlackFrames(30, 60, 1), 15u);
	CHECK_EQ(SlackFrames(0, 60, 1), 0u);
	CHECK_EQ(SlackFrames(30, 60000, 1001), 15u);

	// So short the slack would leave 0 s: the slack is given up instead.
	in.budgetBytes = 70ULL * (1280 * 720 * 3 / 2 / 5); // 70 frames: span 69 < 60 + 15.
	r = Compute(in);
	CHECK_EQ(r.actualFrames, 70u);
	CHECK_EQ(r.effectiveDelaySeconds, 1u);

	// Fractional fps: never more than the span allows.
	in.fpsNum = 60000;
	in.fpsDen = 1001;
	in.budgetBytes = 100 * kMiB;
	r = Compute(in);
	CHECK(!r.fitsFullDuration);
	CHECK(static_cast<uint64_t>(r.effectiveDelaySeconds) * in.fpsNum <= (r.actualFrames - 1) * in.fpsDen);

	// Zero delay stays zero.
	in = MakeInput(1920, 1080, 1920, 1080);
	in.delaySeconds = 0;
	CHECK_EQ(Compute(in).effectiveDelaySeconds, 0u);

	// Sweep: always <= the request; when trimmed, the ring spans it (with slack when possible).
	int violations = 0;
	for (uint64_t budgetMiB : {40, 64, 100, 250, 500, 1000, 4000}) {
		for (uint32_t delay : {1u, 5u, 10u, 30u, 60u}) {
			for (uint32_t fps : {30u, 60u}) {
				Input sweep = MakeInput(2560, 1440, 1920, 1080, 720);
				sweep.delaySeconds = delay;
				sweep.fpsNum = fps;
				sweep.budgetBytes = budgetMiB * kMiB;
				const Result s = Compute(sweep);
				bool ok = s.effectiveDelaySeconds <= delay;
				if (s.fitsFullDuration)
					ok = ok && s.effectiveDelaySeconds == delay;
				else
					ok = ok &&
					     static_cast<uint64_t>(s.effectiveDelaySeconds) * fps <= s.actualFrames - 1;
				if (!ok && violations++ < 5)
					std::printf("  effective delay violation: budget %lluMiB delay %u fps %u -> %u "
						    "(%llu frames)\n",
						    static_cast<unsigned long long>(budgetMiB), delay, fps,
						    s.effectiveDelaySeconds,
						    static_cast<unsigned long long>(s.actualFrames));
			}
		}
	}
	CHECK_EQ(violations, 0);
}

// v0.6.0: on OpenGL every I420 plane row must be a whole number of 4-byte
// units (libobs-opengl maps rows of exactly `width` bytes, glReadPixels pads
// them to 4). Whatever the source, output, floor or budget: width % 8 == 0.
void TestWidthAlignment()
{
	g_currentTest = "18 widths are multiples of 8 (OpenGL rows)";
	const uint32_t sources[][2] = {{1920, 1080}, {2560, 1440}, {1366, 768},  {1365, 767},
				       {1600, 900},  {854, 480},   {3440, 1440}, {2560, 1080}};
	const uint32_t outputs[][2] = {{1920, 1080}, {1366, 768}, {1280, 720}, {852, 480}, {1600, 900}};
	int violations = 0;
	for (const auto &src : sources) {
		for (const auto &out : outputs) {
			for (uint32_t minRes : {480u, 720u, 1080u, 1440u, 2160u}) {
				for (uint64_t budget : {16 * kMiB, 100 * kMiB, 300 * kMiB, 800 * kMiB, 16 * kGiB}) {
					for (bool cap : {true, false}) {
						Input in = MakeInput(src[0], src[1], out[0], out[1], minRes);
						in.budgetBytes = budget;
						in.capToOutput = cap;
						const Result r = Compute(in);
						const bool ok = IsAlignedWidthInRange(r.bufferWidth, src[0]) &&
								((r.bufferWidth / 2) % 4) == 0 &&
								IsEvenInRange(r.bufferHeight, src[1]) &&
								(r.actualFrames == 1 || WithinBudget(in, r));
						if (!ok && violations++ < 5)
							std::printf(
								"  alignment violation: src %ux%u out %ux%u min %u budget "
								"%lluMiB cap %d -> %ux%u\n",
								src[0], src[1], out[0], out[1], minRes,
								static_cast<unsigned long long>(budget / kMiB),
								cap ? 1 : 0, r.bufferWidth, r.bufferHeight);
					}
				}
			}
		}
	}
	CHECK_EQ(violations, 0);
	// The cases QA found sheared on OpenGL: 852x480 output and the old rungs.
	CHECK_EQ(Compute(MakeInput(1920, 1080, 852, 480, 480)).bufferWidth, 856u);
	CHECK_EQ(Compute(MakeInput(852, 480, 852, 480, 480)).bufferWidth, 848u);
}

// v0.6.0: the 1440p (2K) and 2160p (4K) floors. Still only a floor for the
// RAM reduction, and still under the output ceiling: when RAM is short the
// delay gets shorter instead of the buffer dropping below the floor.
void TestHighFloors()
{
	g_currentTest = "19 1440p and 2160p floors";
	// 1440p output, 1440p floor, RAM for half of the 30 s at 1440p: the
	// buffer stays 2560x1440 and the delay is trimmed.
	Input tight = MakeInput(2560, 1440, 2560, 1440, 1440);
	tight.budgetBytes = 1 * kGiB;
	Result t = Compute(tight);
	CHECK_EQ(t.bufferWidth, 2560u);
	CHECK_EQ(t.bufferHeight, 1440u);
	CHECK(!t.cappedByOutput);
	CHECK(!t.reducedByRam);
	CHECK(!t.fitsFullDuration);
	CHECK(WithinBudget(tight, t));
	// floor(1 GiB / (2560*1440*1.5/5)) = 970 slots: span 969, minus 15 of
	// slack, is 15.9 s -> 15 s.
	CHECK_EQ(t.actualFrames, 970u);
	CHECK_EQ(t.effectiveDelaySeconds, 15u);

	// The same RAM with a 1080p floor: the buffer drops to 1080p instead.
	Input lower = tight;
	lower.minResolutionHeight = 1080;
	Result l = Compute(lower);
	CHECK_EQ(l.bufferHeight, 1080u);
	CHECK(l.reducedByRam);

	// Enough RAM for the full delay above 1080p but not at 1440p: with the
	// 1080p floor the buffer shrinks and keeps the 30 s, with the 1440p floor
	// it keeps 1440p and the delay is shorter (1456 slots: 24 s).
	Input mid = MakeInput(2560, 1440, 2560, 1440, 1080);
	mid.budgetBytes = 1536 * kMiB;
	Result m = Compute(mid);
	CHECK(m.reducedByRam);
	CHECK(m.fitsFullDuration);
	CHECK(m.bufferHeight >= 1080u && m.bufferHeight < 1440u);
	mid.minResolutionHeight = 1440;
	m = Compute(mid);
	CHECK_EQ(m.bufferHeight, 1440u);
	CHECK(!m.reducedByRam);
	CHECK(!m.fitsFullDuration);
	CHECK_EQ(m.effectiveDelaySeconds, 24u);

	// Plenty of RAM: the floor changes nothing.
	Result full = Compute(MakeInput(2560, 1440, 2560, 1440, 1440));
	CHECK_EQ(full.bufferHeight, 1440u);
	CHECK(full.fitsFullDuration);
	CHECK_EQ(full.effectiveDelaySeconds, 30u);

	// The output ceiling still wins: a 1080p output buffers at 1080p with a
	// 1440p or 2160p floor, and a 1440p output at 1440p with a 2160p one.
	for (const uint32_t floorRes : {1440u, 2160u}) {
		Result c = Compute(MakeInput(2560, 1440, 1920, 1080, floorRes));
		CHECK_EQ(c.bufferWidth, 1920u);
		CHECK_EQ(c.bufferHeight, 1080u);
		CHECK(c.cappedByOutput);
		Input small = MakeInput(2560, 1440, 1920, 1080, floorRes);
		small.budgetBytes = 100 * kMiB;
		Result s = Compute(small);
		CHECK_EQ(s.bufferHeight, 1080u);
		CHECK(!s.reducedByRam);
		CHECK(!s.fitsFullDuration);
	}
	Result k = Compute(MakeInput(3840, 2160, 2560, 1440, 2160));
	CHECK_EQ(k.bufferWidth, 2560u);
	CHECK_EQ(k.bufferHeight, 1440u);
	CHECK(k.cappedByOutput);

	// 4K output: a 2160p floor holds 3840x2160 and trims the delay, a 1440p
	// one lets RAM bring it down to 1440p first.
	Input uhd = MakeInput(3840, 2160, 3840, 2160, 2160);
	uhd.budgetBytes = 1536 * kMiB;
	Result u = Compute(uhd);
	CHECK_EQ(u.bufferWidth, 3840u);
	CHECK_EQ(u.bufferHeight, 2160u);
	CHECK(!u.fitsFullDuration);
	CHECK(WithinBudget(uhd, u));
	uhd.minResolutionHeight = 1440;
	u = Compute(uhd);
	CHECK_EQ(u.bufferWidth, 2560u);
	CHECK_EQ(u.bufferHeight, 1440u);
	CHECK(u.reducedByRam);
	CHECK(WithinBudget(uhd, u));
}

// v0.7.0: the storage class fixed per cycle (invariant I9). Inter: the assumed
// ratio 12 and a GOP of slack; intra: 5 and a quarter second, as before.
void TestStorageClass()
{
	g_currentTest = "21 storage class (v0.7.0)";
	CHECK_EQ(KeyframeIntervalFrames(60, 1), 30u);
	CHECK_EQ(KeyframeIntervalFrames(30, 1), 15u);
	CHECK_EQ(KeyframeIntervalFrames(60000, 1001), 30u);
	CHECK_EQ(KeyframeIntervalFrames(24, 1), 12u);
	CHECK_EQ(KeyframeIntervalFrames(25, 1), 13u);
	CHECK_EQ(KeyframeIntervalFrames(120, 1), 60u);
	CHECK_EQ(KeyframeIntervalFrames(0, 1), 0u);
	CHECK_EQ(SlackFrames(30, 60, 1, 30), 30u);
	CHECK_EQ(SlackFrames(30, 60, 1, 10), 15u); // Never below the quarter second.
	CHECK_EQ(SlackFrames(0, 60, 1, 30), 0u);
	CHECK_EQ(SlackFrames(30, 30, 1, 15), 15u);
	CHECK_EQ(DesiredFrames(30, 60, 1, 30), 1831u);
	CHECK_EQ(DesiredFrames(30, 60, 1), 1816u);

	Input intra = MakeInput(2560, 1440, 2560, 1440, 1440);
	Input inter = intra;
	Input intraGop = intra;
	ApplyStorageClass(intra, RingClass::Intra, 5.0);
	ApplyStorageClass(inter, RingClass::Inter, 5.0);
	ApplyStorageClass(intraGop, RingClass::IntraWithGop, 5.0);
	CHECK_NEAR(intra.compressionRatio, 5.0, 1e-9);
	CHECK_EQ(intra.keyframeIntervalFrames, 0u);
	CHECK_NEAR(inter.compressionRatio, 12.0, 1e-9);
	CHECK_EQ(inter.keyframeIntervalFrames, 30u);
	// The first cycle of a session, hardware possible but not probed yet:
	// MJPEG's ratio with the GOP margin a restarted decoder needs.
	CHECK_NEAR(intraGop.compressionRatio, 5.0, 1e-9);
	CHECK_EQ(intraGop.keyframeIntervalFrames, 30u);
	Input noFfmpeg = intra;
	ApplyStorageClass(noFfmpeg, RingClass::Intra, 1.0);
	CHECK_NEAR(noFfmpeg.compressionRatio, 1.0, 1e-9);
	// The filter's "storage_class" setting, both ways; anything else is intra.
	for (RingClass c : {RingClass::Intra, RingClass::IntraWithGop, RingClass::Inter})
		CHECK(ParseRingClass(RingClassName(c)) == c);
	CHECK(ParseRingClass("") == RingClass::Intra);
	CHECK(ParseRingClass("INTER") == RingClass::Intra);
	CHECK(ParseRingClass("intra-gop ") == RingClass::Intra);

	// Room to spare: the same buffer and delay, the GOP margin in slots.
	const Result a = Compute(intra);
	const Result b = Compute(inter);
	const Result g = Compute(intraGop);
	CHECK(a.fitsFullDuration && b.fitsFullDuration && g.fitsFullDuration);
	CHECK_EQ(a.actualFrames, 1816u);
	CHECK_EQ(b.actualFrames, 1831u);
	CHECK_EQ(g.actualFrames, 1831u); // The GOP margin, at intra's ratio.
	CHECK_EQ(g.effectiveDelaySeconds, 30u);
	CHECK_EQ(a.bufferWidth, b.bufferWidth);
	CHECK_EQ(a.bufferHeight, b.bufferHeight);
	CHECK_EQ(a.effectiveDelaySeconds, 30u);
	CHECK_EQ(b.effectiveDelaySeconds, 30u);

	// 4K60, 60 s on a 16 GB machine (8 GiB budget): reduced with intra, full
	// resolution with inter.
	Input k4 = MakeInput(3840, 2160, 3840, 2160, 720);
	k4.delaySeconds = 60;
	k4.budgetBytes = 8 * kGiB;
	Input k4Inter = k4;
	ApplyStorageClass(k4, RingClass::Intra, 5.0);
	ApplyStorageClass(k4Inter, RingClass::Inter, 5.0);
	const Result k4a = Compute(k4);
	const Result k4b = Compute(k4Inter);
	CHECK(k4a.reducedByRam);
	CHECK(!k4b.reducedByRam);
	CHECK_EQ(k4b.bufferHeight, 2160u);
	CHECK_EQ(k4b.actualFrames, 3631u);
	CHECK_EQ(k4b.effectiveDelaySeconds, 60u);

	// Trimmed at the floor: 2.4x the frames in the same budget, the GOP as slack.
	Input tight = MakeInput(2560, 1440, 1920, 1080, 720);
	tight.budgetBytes = 100 * kMiB;
	Input tightInter = tight;
	Input tightGop = tight;
	ApplyStorageClass(tight, RingClass::Intra, 5.0);
	ApplyStorageClass(tightInter, RingClass::Inter, 5.0);
	ApplyStorageClass(tightGop, RingClass::IntraWithGop, 5.0);
	const Result ta = Compute(tight);
	const Result tb = Compute(tightInter);
	const Result tg = Compute(tightGop);
	CHECK_EQ(ta.actualFrames, 379u);
	CHECK_EQ(tb.actualFrames, 910u); // 104857600 / (1280 * 720 * 1.5 / 12).
	CHECK_EQ(tg.actualFrames, 379u); // Intra's frames in the same budget...
	CHECK_EQ(ta.bufferHeight, tb.bufferHeight);
	CHECK_EQ(ta.effectiveDelaySeconds, 6u);
	CHECK_EQ(tb.effectiveDelaySeconds, 14u); // (909 - 30) / 60.
	CHECK_EQ(tg.effectiveDelaySeconds, 5u);  // ...minus the GOP: (378 - 30) / 60.
	CHECK(WithinBudget(tightGop, tg));
	CHECK(WithinBudget(tightInter, tb));

	// Sweeps: monotonic in the budget, within it, never past the request.
	int violations = 0;
	for (uint32_t delay : {5u, 30u, 60u}) {
		for (uint32_t fps : {30u, 60u}) {
			Result previous;
			for (uint64_t budgetMiB : {40, 64, 100, 250, 500, 1000, 2000, 4000, 16000}) {
				Input sweep = MakeInput(3840, 2160, 2560, 1440, 720);
				sweep.delaySeconds = delay;
				sweep.fpsNum = fps;
				sweep.budgetBytes = budgetMiB * kMiB;
				ApplyStorageClass(sweep, RingClass::Inter, 5.0);
				const Result r = Compute(sweep);
				bool ok = WithinBudget(sweep, r) && r.effectiveDelaySeconds <= delay &&
					  r.actualFrames <= r.desiredFrames;
				ok = ok &&
				     r.desiredFrames == DesiredFrames(delay, fps, 1, KeyframeIntervalFrames(fps, 1));
				if (previous.bufferHeight != 0)
					ok = ok && r.bufferHeight >= previous.bufferHeight &&
					     r.actualFrames >= previous.actualFrames &&
					     r.effectiveDelaySeconds >= previous.effectiveDelaySeconds;
				if (!r.fitsFullDuration)
					ok = ok &&
					     static_cast<uint64_t>(r.effectiveDelaySeconds) * fps <= r.actualFrames - 1;
				if (!ok && violations++ < 5)
					std::printf(
						"  inter sweep violation: budget %lluMiB delay %u fps %u -> %ux%u %llu "
						"frames, %u s\n",
						static_cast<unsigned long long>(budgetMiB), delay, fps, r.bufferWidth,
						r.bufferHeight, static_cast<unsigned long long>(r.actualFrames),
						r.effectiveDelaySeconds);
				previous = r;
			}
		}
	}
	CHECK_EQ(violations, 0);
}

} // namespace

int main()
{
	TestNoCap();
	TestTicketCase();
	Test4kTo1080();
	TestOutputAboveCanvas();
	TestAspectMismatch();
	TestFloorAboveCeiling();
	TestRamReductionInRange();
	TestFloorActive();
	TestFloorRounding();
	TestOddDimensions();
	TestDegenerate();
	TestNonCanvasTarget();
	TestZeroDelay();
	TestFractionalFps();
	TestProperties();
	TestCapDisabled();
	TestTinySource();
	TestEffectiveDelay();
	TestWidthAlignment();
	TestHighFloors();
	TestStorageClass();

	std::printf("buffer_fit_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
