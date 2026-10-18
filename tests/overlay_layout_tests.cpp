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

// Standalone unit tests for trigglow::overlaylayout (src/overlay-layout.hpp):
// where the live "Delay 30 s" badge sits on OBS's canvas and which file it is
// read from. Same approach as the other tests: no libobs, no framework.

#include "overlay-layout.hpp"

#include <cmath>
#include <cstdio>

using namespace trigglow::overlaylayout;

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

bool Near(float a, float b, float eps = 0.01f)
{
	return std::fabs(a - b) <= eps;
}

void TestMargin()
{
	g_currentTest = "safety margin";
	CHECK(Near(SafetyMargin(1080.0f), 24.0f));
	CHECK(Near(SafetyMargin(720.0f), 16.0f));
	CHECK(Near(SafetyMargin(1440.0f), 32.0f));
	CHECK(Near(SafetyMargin(2160.0f), 48.0f));
	CHECK(Near(SafetyMargin(0.0f), 24.0f)); // Unknown canvas: 1080p.
	CHECK(Near(SafetyMargin(-5.0f), 24.0f));
}

void TestPlacement1080p()
{
	g_currentTest = "1080p placement";
	// Same size as the v0.5.x badge: 2.5x PNG shown at 0.4 = 200x58.
	Placement tl = Place(1920.0f, 1080.0f, 0);
	CHECK(Near(tl.scale, 0.4f));
	CHECK(Near(tl.width, 200.0f));
	CHECK(Near(tl.height, 58.0f));
	CHECK(Near(tl.x, 24.0f));
	CHECK(Near(tl.y, 24.0f));

	Placement tr = Place(1920.0f, 1080.0f, 1);
	CHECK(Near(tr.x, 1920.0f - 200.0f - 24.0f));
	CHECK(Near(tr.y, 24.0f));

	Placement bl = Place(1920.0f, 1080.0f, 2);
	CHECK(Near(bl.x, 24.0f));
	CHECK(Near(bl.y, 1080.0f - 58.0f - 24.0f));

	Placement br = Place(1920.0f, 1080.0f, 3);
	CHECK(Near(br.x, 1920.0f - 200.0f - 24.0f));
	CHECK(Near(br.y, 1080.0f - 58.0f - 24.0f));

	// Out-of-range corners behave like top-left (BufferModeController clamps too).
	Placement bad = Place(1920.0f, 1080.0f, 7);
	CHECK(Near(bad.x, tl.x) && Near(bad.y, tl.y));
}

void TestPlacementOtherCanvases()
{
	g_currentTest = "other canvases";
	// Keeps its share of the canvas height.
	Placement p720 = Place(1280.0f, 720.0f, 3);
	CHECK(Near(p720.height / 720.0f, 58.0f / 1080.0f, 0.001f));
	CHECK(Near(p720.margin, 16.0f));
	CHECK(Near(p720.x + p720.width + p720.margin, 1280.0f));
	CHECK(Near(p720.y + p720.height + p720.margin, 720.0f));

	Placement p4k = Place(3840.0f, 2160.0f, 0);
	CHECK(Near(p4k.scale, 0.8f));
	CHECK(Near(p4k.width, 400.0f));
	CHECK(Near(p4k.margin, 48.0f));

	// Never upscales the PNG past its rendered size...
	Placement p8k = Place(7680.0f, 4320.0f, 0);
	CHECK(Near(p8k.scale, 1.0f));
	CHECK(Near(p8k.width, 500.0f));
	// ...nor shrinks below legibility.
	Placement tiny = Place(320.0f, 180.0f, 0);
	CHECK(Near(tiny.scale, 0.2f));

	// Vertical canvas (1080x1920): same height share as a 1920-tall canvas.
	Placement vertical = Place(1080.0f, 1920.0f, 1);
	CHECK(vertical.x >= 0.0f);
	CHECK(Near(vertical.x + vertical.width + vertical.margin, 1080.0f));

	// A canvas narrower than the badge never pushes it off the left/top edge.
	Placement narrow = Place(150.0f, 1080.0f, 3);
	CHECK(narrow.x >= 0.0f);
	CHECK(narrow.y >= 0.0f);

	// Invalid canvas falls back to 1080p.
	Placement zero = Place(0.0f, 0.0f, 0);
	CHECK(Near(zero.scale, 0.4f));
	CHECK(Near(zero.x, 24.0f));
}

void TestFileNames()
{
	g_currentTest = "file names";
	CHECK(BadgeFileName(30) == "delay-badge-30s.png");
	CHECK(BadgeFileName(5) == "delay-badge-5s.png");
	CHECK(BadgeFileName(60) == "delay-badge-60s.png");
	CHECK(BadgeFileName(0) == "delay-badge-1s.png");
	CHECK(BadgeFileName(600) == "delay-badge-60s.png");
	CHECK(ClampSeconds(0) == kMinSeconds);
	CHECK(ClampSeconds(61) == kMaxSeconds);
	CHECK(ClampSeconds(42) == 42);
}

} // namespace

int main()
{
	TestMargin();
	TestPlacement1080p();
	TestPlacementOtherCanvases();
	TestFileNames();

	std::printf("overlay_layout_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
