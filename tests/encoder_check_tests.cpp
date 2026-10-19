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

// Standalone unit tests for trigglow::encodercheck (src/encoder-check.hpp),
// the decision behind the "video encoder above the delay buffer" WARN. Same
// approach as buffer_fit_tests.cpp: no libobs, no framework, a CHECK macro
// that keeps going, non-zero exit on any failure. The buffer sizes come from
// bufferfit::Compute() with the cap on and off, exactly like
// BufferModeController::WarnAboutEncodersAboveBuffer() gets them.

#include "buffer-fit.hpp"
#include "encoder-check.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

using trigglow::encodercheck::CapLimitsBuffer;
using trigglow::encodercheck::Classify;
using trigglow::encodercheck::SameAspectRatio;
using trigglow::encodercheck::Scope;
using trigglow::encodercheck::ShouldWarn;
using trigglow::encodercheck::Size;
using trigglow::encodercheck::VideoSource;

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

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;
constexpr uint64_t kMiB = 1024ULL * 1024;

// One live scene the size of the canvas, 60fps, 30s, 5x assumed compression,
// a 16 GiB budget (RAM never limits these unless the test lowers it).
struct Setup {
	Size canvas;
	Size output;
	uint64_t budgetBytes = 16 * kGiB;
	bool capToOutput = true;
	Size capped;
	Size uncapped;
};

Size BufferFor(const Setup &s, bool capToOutput)
{
	trigglow::bufferfit::Input in;
	in.sourceWidth = s.canvas.width;
	in.sourceHeight = s.canvas.height;
	in.baseWidth = s.canvas.width;
	in.baseHeight = s.canvas.height;
	in.outputWidth = s.output.width;
	in.outputHeight = s.output.height;
	in.fpsNum = 60;
	in.fpsDen = 1;
	in.delaySeconds = 30;
	in.minResolutionHeight = 720;
	in.budgetBytes = s.budgetBytes;
	in.compressionRatio = 5.0;
	in.capToOutput = capToOutput;
	const trigglow::bufferfit::Result r = trigglow::bufferfit::Compute(in);
	return {r.bufferWidth, r.bufferHeight};
}

Setup MakeSetup(Size canvas, Size output, uint64_t budgetBytes = 16 * kGiB)
{
	Setup s;
	s.canvas = canvas;
	s.output = output;
	s.budgetBytes = budgetBytes;
	s.capped = BufferFor(s, true);
	s.uncapped = BufferFor(s, false);
	return s;
}

// What the WARN decides for one encoder, end to end: classification, then
// ShouldWarn() against the setup's buffers.
bool Warns(const Setup &s, VideoSource source, Size encoder)
{
	const Scope scope = Classify(source, encoder, s.canvas, s.output);
	return ShouldWarn(scope, encoder, s.capped, s.uncapped, s.capToOutput);
}

bool SameSize(Size a, Size b)
{
	return a.width == b.width && a.height == b.height;
}

void TestAspectRatio()
{
	g_currentTest = "aspect ratio";
	const Size hd{1920, 1080};
	CHECK(SameAspectRatio(hd, {1280, 720}));
	CHECK(SameAspectRatio(hd, {2560, 1440}));
	CHECK(SameAspectRatio(hd, {852, 480}));  // OBS's even-rounded 480p: 0.2% off.
	CHECK(SameAspectRatio(hd, {1366, 768})); // 0.05% off.
	CHECK(SameAspectRatio(hd, {698, 392}));
	CHECK(!SameAspectRatio(hd, {1920, 1200})); // 16:10.
	CHECK(!SameAspectRatio(hd, {1440, 1080})); // 4:3.
	CHECK(!SameAspectRatio(hd, {2560, 1080})); // 21:9.
	CHECK(!SameAspectRatio(hd, {1080, 1920})); // 9:16.
	CHECK(!SameAspectRatio(hd, {720, 1280}));
	CHECK(SameAspectRatio({1080, 1920}, {720, 1280}));
	CHECK(!SameAspectRatio(hd, {0, 1080}));
	CHECK(!SameAspectRatio({0, 0}, {0, 0}));
	// Symmetric, and no overflow at the extremes.
	CHECK(SameAspectRatio({852, 480}, hd));
	CHECK(SameAspectRatio({UINT32_MAX, UINT32_MAX}, {1, 1}));
	CHECK(!SameAspectRatio({UINT32_MAX, 1}, {1, UINT32_MAX}));
}

void TestClassify()
{
	g_currentTest = "classify";
	const Size canvas{1920, 1080};
	const Size output{1280, 720};
	// The main mix and listed canvases are exact, whatever the size.
	CHECK(Classify(VideoSource::MainMix, {1280, 720}, canvas, output) == Scope::MainCanvas);
	CHECK(Classify(VideoSource::MainMix, {1080, 1920}, canvas, output) == Scope::MainCanvas);
	CHECK(Classify(VideoSource::OtherCanvasMix, {1920, 1080}, canvas, output) == Scope::OtherCanvas);
	CHECK(Classify(VideoSource::OtherCanvasMix, {1080, 1920}, canvas, output) == Scope::OtherCanvas);
	// Unlisted mixes go by aspect ratio, against the canvas or the output.
	CHECK(Classify(VideoSource::UnlistedMix, {1920, 1080}, canvas, output) == Scope::MainCanvasByAspect);
	CHECK(Classify(VideoSource::UnlistedMix, {852, 480}, canvas, output) == Scope::MainCanvasByAspect);
	CHECK(Classify(VideoSource::UnlistedMix, {720, 1280}, canvas, output) == Scope::OtherCanvas);
	CHECK(Classify(VideoSource::UnlistedMix, {1080, 1920}, canvas, output) == Scope::OtherCanvas);
	// Output stretched to another aspect ratio than the canvas: either matches.
	CHECK(Classify(VideoSource::UnlistedMix, {1440, 1080}, canvas, {1024, 768}) == Scope::MainCanvasByAspect);
	CHECK(Classify(VideoSource::UnlistedMix, {1280, 720}, canvas, {1024, 768}) == Scope::MainCanvasByAspect);
	// Unknown main geometry: nothing matches by aspect.
	CHECK(Classify(VideoSource::UnlistedMix, {1920, 1080}, {0, 0}, {0, 0}) == Scope::OtherCanvas);
}

void TestCapLimitsBuffer()
{
	g_currentTest = "cap limits buffer";
	CHECK(CapLimitsBuffer({1280, 720}, {1920, 1080}));
	CHECK(!CapLimitsBuffer({1920, 1080}, {1920, 1080}));
	CHECK(!CapLimitsBuffer({1280, 720}, {1280, 720}));
	CHECK(!CapLimitsBuffer({0, 0}, {1920, 1080}));
	CHECK(CapLimitsBuffer({1280, 720}, {1280, 722})); // Either axis.
}

// Canvas 1080p, output 720p: the buffer is capped to 1280x720 and would be
// 1920x1080 without the cap.
void TestCanvasEncoderNotScaled()
{
	g_currentTest = "canvas encoder, not scaled";
	const Setup s = MakeSetup({1920, 1080}, {1280, 720});
	CHECK(SameSize(s.capped, {1280, 720}));
	CHECK(SameSize(s.uncapped, {1920, 1080}));
	// An encoder of the main mix without scaling encodes at the output size:
	// the buffer's own size, nothing to warn about.
	CHECK(!Warns(s, VideoSource::MainMix, {1280, 720}));
	// The same mix scaled on the CPU (obs_encoder_set_scaled_size, no GPU
	// scale type) up to the canvas: still the main mix, above the buffer.
	CHECK(Warns(s, VideoSource::MainMix, {1920, 1080}));
}

void TestRescaledEncoders()
{
	g_currentTest = "rescaled encoders";
	const Setup s = MakeSetup({1920, 1080}, {1280, 720});
	// Advanced output's Rescale output with a GPU filter: an encoder-only mix
	// libobs doesn't expose, attributed by aspect ratio.
	CHECK(Warns(s, VideoSource::UnlistedMix, {1920, 1080})); // Above the buffer.
	CHECK(Warns(s, VideoSource::UnlistedMix, {1600, 900}));
	CHECK(!Warns(s, VideoSource::UnlistedMix, {1280, 720})); // Equal.
	CHECK(!Warns(s, VideoSource::UnlistedMix, {852, 480}));  // Below.
	CHECK(Warns(s, VideoSource::UnlistedMix, {1366, 768}));  // Above 1280x720 on both axes.

	// Canvas 1440p, output 1080p: a recording rescaled back to the canvas.
	const Setup qhd = MakeSetup({2560, 1440}, {1920, 1080});
	CHECK(SameSize(qhd.capped, {1920, 1080}));
	CHECK(SameSize(qhd.uncapped, {2560, 1440}));
	CHECK(Warns(qhd, VideoSource::UnlistedMix, {2560, 1440}));
	CHECK(!Warns(qhd, VideoSource::UnlistedMix, {1920, 1080}));
}

// Enhanced Broadcasting: every track is GPU-rescaled with a preferred format,
// so all of them run on unlisted mixes, even one at the output size.
void TestMultitrackTracks()
{
	g_currentTest = "multitrack tracks";
	const Setup s = MakeSetup({2560, 1440}, {1280, 720});
	CHECK(SameSize(s.capped, {1280, 720}));
	CHECK(SameSize(s.uncapped, {2560, 1440}));
	CHECK(Warns(s, VideoSource::UnlistedMix, {1920, 1080}));
	CHECK(!Warns(s, VideoSource::UnlistedMix, {1280, 720}));
	CHECK(!Warns(s, VideoSource::UnlistedMix, {852, 480}));
	// A vertical track of an Enhanced Broadcasting extra canvas: GPU-rescaled
	// too, so unlisted, but 9:16 on a 16:9 main canvas.
	CHECK(!Warns(s, VideoSource::UnlistedMix, {1080, 1920}));
}

// A vertical canvas (e.g. Aitum Vertical): its own canvas mix is recognized
// exactly; its rescaled encoders only by aspect ratio.
void TestVerticalCanvas()
{
	g_currentTest = "vertical canvas";
	const Setup s = MakeSetup({1920, 1080}, {1280, 720});
	for (const Size vertical : {Size{720, 1280}, Size{1080, 1920}}) {
		CHECK(!Warns(s, VideoSource::OtherCanvasMix, vertical));
		CHECK(!Warns(s, VideoSource::UnlistedMix, vertical));
	}
	// Even a listed canvas the same shape as the main one never warns.
	CHECK(!Warns(s, VideoSource::OtherCanvasMix, {1920, 1080}));

	// A vertical MAIN canvas: its rescaled encoders do match it.
	const Setup v = MakeSetup({1080, 1920}, {720, 1280});
	CHECK(SameSize(v.capped, {720, 1280}));
	CHECK(SameSize(v.uncapped, {1080, 1920}));
	CHECK(Warns(v, VideoSource::UnlistedMix, {1080, 1920}));
	CHECK(!Warns(v, VideoSource::UnlistedMix, {720, 1280}));
	CHECK(!Warns(v, VideoSource::UnlistedMix, {1920, 1080}));
	CHECK(Warns(v, VideoSource::MainMix, {1080, 1920}));
}

void TestBufferNotCapped()
{
	g_currentTest = "buffer not capped";
	// buffer_output_cap = false: never a WARN, however big the encoder.
	Setup off = MakeSetup({1920, 1080}, {1280, 720});
	off.capToOutput = false;
	CHECK(!Warns(off, VideoSource::MainMix, {1920, 1080}));
	CHECK(!Warns(off, VideoSource::UnlistedMix, {3840, 2160}));

	// Output = canvas: no cap applies, capped and uncapped are the same.
	const Setup same = MakeSetup({1920, 1080}, {1920, 1080});
	CHECK(SameSize(same.capped, same.uncapped));
	CHECK(!Warns(same, VideoSource::UnlistedMix, {2560, 1440}));

	// RAM is what limits the buffer (400 MiB: not even 30s of 720p60 fit, so
	// both rings sit at the 720p floor): lifting the cap buys no pixels.
	const Setup ram = MakeSetup({1920, 1080}, {1280, 720}, 400 * kMiB);
	CHECK(SameSize(ram.capped, ram.uncapped));
	CHECK(!Warns(ram, VideoSource::UnlistedMix, {1920, 1080}));
}

void TestInvalidInput()
{
	g_currentTest = "invalid input";
	CHECK(!ShouldWarn(Scope::MainCanvas, {1920, 1080}, {0, 0}, {1920, 1080}, true));
	CHECK(!ShouldWarn(Scope::MainCanvas, {0, 0}, {1280, 720}, {1920, 1080}, true));
	CHECK(!ShouldWarn(Scope::OtherCanvas, {1920, 1080}, {1280, 720}, {1920, 1080}, true));
	CHECK(ShouldWarn(Scope::MainCanvasByAspect, {1920, 1080}, {1280, 720}, {1920, 1080}, true));
}

// v0.7.0: the ceiling the main canvas's encoders put on the buffer.
void TestEncoderCeiling()
{
	g_currentTest = "encoder ceiling";
	using trigglow::encodercheck::Ceiling;
	using trigglow::encodercheck::CeilingEncoder;
	using trigglow::encodercheck::CeilingWhy;
	using trigglow::encodercheck::EncoderCeiling;
	const Size canvas{2560, 1440};
	auto byAspect = [&](Size size) {
		return CeilingEncoder{VideoSource::UnlistedMix,
				      Classify(VideoSource::UnlistedMix, size, canvas, canvas), size};
	};
	auto mainMix = [](Size size) {
		return CeilingEncoder{VideoSource::MainMix, Scope::MainCanvas, size};
	};

	// The user's case: everything at the output -- nothing changes.
	Ceiling c = EncoderCeiling({mainMix({2560, 1440})}, canvas, canvas, true, false);
	CHECK(!c.applies && c.why == CeilingWhy::NotBelowOutput);
	// The stream rescaled to 1080p on the GPU: the buffer needn't go above it.
	c = EncoderCeiling({byAspect({1920, 1080})}, canvas, canvas, true, false);
	CHECK(c.applies && c.why == CeilingWhy::Applied && SameSize(c.size, {1920, 1080}));
	// Plus a recording at the canvas: the output cap stands.
	c = EncoderCeiling({byAspect({1920, 1080}), mainMix({2560, 1440})}, canvas, canvas, true, false);
	CHECK(!c.applies && c.why == CeilingWhy::NotBelowOutput);
	// A recording below the stream: the largest of them rules.
	c = EncoderCeiling({byAspect({1920, 1080}), byAspect({1280, 720})}, canvas, canvas, true, false);
	CHECK(c.applies && SameSize(c.size, {1920, 1080}));
	// An encoder above the output (output 1080p, a track at the canvas): the output.
	c = EncoderCeiling({byAspect({2560, 1440})}, canvas, {1920, 1080}, true, false);
	CHECK(!c.applies && c.why == CeilingWhy::NotBelowOutput);
	// Enhanced Broadcasting: tracks at 1080p, 720p and 480p with a 1440p output.
	c = EncoderCeiling({byAspect({1920, 1080}), byAspect({1280, 720}), byAspect({852, 480})}, canvas, canvas, true,
			   false);
	CHECK(c.applies && SameSize(c.size, {1920, 1080}));
	// ...and with a 1080p output whose largest track is 1080p: nothing changes.
	c = EncoderCeiling({byAspect({1920, 1080}), byAspect({1280, 720})}, canvas, {1920, 1080}, true, false);
	CHECK(!c.applies && c.why == CeilingWhy::NotBelowOutput);
	// A raw output on the main mix (virtual camera, NDI): it takes the output as is.
	c = EncoderCeiling({byAspect({1920, 1080})}, canvas, canvas, true, true);
	CHECK(!c.applies && c.why == CeilingWhy::RawOutput);
	// An unlisted mix of another aspect ratio (maybe the canvas, stretched): no ceiling.
	c = EncoderCeiling({byAspect({1920, 1080}), byAspect({1080, 1920})}, canvas, canvas, true, false);
	CHECK(!c.applies && c.why == CeilingWhy::UnknownMix);
	// Another canvas, listed (a vertical canvas through the canvas API): ignored.
	c = EncoderCeiling({byAspect({1920, 1080}), {VideoSource::OtherCanvasMix, Scope::OtherCanvas, {1080, 1920}}},
			   canvas, canvas, true, false);
	CHECK(c.applies && SameSize(c.size, {1920, 1080}));
	c = EncoderCeiling({{VideoSource::OtherCanvasMix, Scope::OtherCanvas, {1080, 1920}}}, canvas, canvas, true,
			   false);
	CHECK(!c.applies && c.why == CeilingWhy::NoEncoders);
	CHECK(EncoderCeiling({}, canvas, canvas, true, false).why == CeilingWhy::NoEncoders);
	// The hidden switch off: no ceiling of any kind.
	c = EncoderCeiling({byAspect({1920, 1080})}, canvas, canvas, false, false);
	CHECK(!c.applies && c.why == CeilingWhy::CapOff);
	// bufferfit's rules on the size: width a multiple of 8, height even,
	// within 4 px of the encoder (1366x768 on a 1080p canvas: 1368x768).
	const Size hd{1920, 1080};
	c = EncoderCeiling(
		{{VideoSource::UnlistedMix, Classify(VideoSource::UnlistedMix, {1366, 768}, hd, hd), {1366, 768}}}, hd,
		hd, true, false);
	CHECK(c.applies && c.size.width % 8 == 0 && c.size.height % 2 == 0);
	CHECK(SameSize(c.size, {1368, 768}));
	// Sweep: never above the output, never more than 4 px narrower than the largest encoder.
	int bad = 0;
	for (uint32_t h = 480; h <= 1440; h += 2) {
		const uint32_t w = (h * 16 / 9) & ~1u;
		const Ceiling s = EncoderCeiling({byAspect({w, h})}, canvas, canvas, true, false);
		if (s.applies &&
		    (s.size.width > canvas.width || s.size.height > canvas.height || s.size.width % 8 != 0 ||
		     s.size.height % 2 != 0 || s.size.height + 2 < h || s.size.width + 4 < w))
			++bad;
		if (!s.applies && h < 1440 && s.why != CeilingWhy::NotBelowOutput)
			++bad;
	}
	CHECK(bad == 0);
	CHECK(std::string(trigglow::encodercheck::CeilingWhyText(CeilingWhy::RawOutput)).size() > 0);
}

} // namespace

int main()
{
	TestAspectRatio();
	TestClassify();
	TestCapLimitsBuffer();
	TestCanvasEncoderNotScaled();
	TestRescaledEncoders();
	TestMultitrackTracks();
	TestVerticalCanvas();
	TestBufferNotCapped();
	TestInvalidInput();
	TestEncoderCeiling();

	std::printf("encoder_check_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
