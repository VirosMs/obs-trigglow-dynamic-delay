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
	// The v0.5.x plate (200x58) plus the 14 px "By Trigglow" line under it,
	// on transparent pixels: the 2.5x PNG (500x180) shown at 0.4 = 200x72.
	CHECK(Near(kDesignWidth, 200.0f));
	CHECK(Near(kPlateHeight, 58.0f));
	CHECK(Near(kDesignHeight, 72.0f));
	Placement tl = Place(1920.0f, 1080.0f, 0);
	CHECK(Near(tl.scale, 0.4f));
	CHECK(Near(tl.width, 200.0f));
	CHECK(Near(tl.height, 72.0f));
	// Top corners: the plate sits at the margin, as before the signature.
	CHECK(Near(tl.x, 24.0f));
	CHECK(Near(tl.y, 24.0f));

	Placement tr = Place(1920.0f, 1080.0f, 1);
	CHECK(Near(tr.x, 1920.0f - 200.0f - 24.0f));
	CHECK(Near(tr.y, 24.0f));

	// Bottom corners: the whole image, signature included, keeps the margin.
	Placement bl = Place(1920.0f, 1080.0f, 2);
	CHECK(Near(bl.x, 24.0f));
	CHECK(Near(bl.y, 1080.0f - 72.0f - 24.0f));
	CHECK(Near(bl.y + bl.height, 1080.0f - 24.0f));

	Placement br = Place(1920.0f, 1080.0f, 3);
	CHECK(Near(br.x, 1920.0f - 200.0f - 24.0f));
	CHECK(Near(br.y, 1080.0f - 72.0f - 24.0f));

	// Out-of-range corners behave like top-left (BufferModeController clamps too).
	Placement bad = Place(1920.0f, 1080.0f, 7);
	CHECK(Near(bad.x, tl.x) && Near(bad.y, tl.y));
}

void TestPlacementOtherCanvases()
{
	g_currentTest = "other canvases";
	// Keeps its share of the canvas height: 133x48 at 720p.
	Placement p720 = Place(1280.0f, 720.0f, 3);
	CHECK(Near(p720.height / 720.0f, 72.0f / 1080.0f, 0.001f));
	CHECK(Near(p720.height, 48.0f));
	// The "By Trigglow" line is caption-sized on the stream: 11 px at 1080p,
	// 7.3 px at 720p.
	CHECK(Near(kSignaturePixelSize * kRenderScale * Place(1920.0f, 1080.0f, 0).scale, 11.0f));
	CHECK(Near(kSignaturePixelSize * kRenderScale * p720.scale, 7.33f, 0.01f));
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

	// A canvas narrower than the badge never pushes it off the left/top edge.
	Placement narrow = Place(150.0f, 1080.0f, 3);
	CHECK(narrow.x >= 0.0f);
	CHECK(narrow.y >= 0.0f);

	// Invalid canvas falls back to 1080p.
	Placement zero = Place(0.0f, 0.0f, 0);
	CHECK(Near(zero.scale, 0.4f));
	CHECK(Near(zero.x, 24.0f));
}

void TestVerticalAndUltrawide()
{
	g_currentTest = "vertical and ultrawide canvases";
	// 1080x1920 (vertical video): sized by the short side, i.e. exactly the
	// 1080p badge -- 200x72, 18.5 % of the frame's width -- not the 355 px
	// that scaling by the height gave.
	Placement vertical = Place(1080.0f, 1920.0f, 3);
	CHECK(Near(vertical.scale, 0.4f));
	CHECK(Near(vertical.width, 200.0f));
	CHECK(Near(vertical.height, 72.0f));
	CHECK(Near(vertical.margin, 24.0f));
	CHECK(Near(vertical.x, 1080.0f - 200.0f - 24.0f));
	CHECK(Near(vertical.y, 1920.0f - 72.0f - 24.0f));
	// Never wider than a fifth of the vertical frame.
	CHECK(vertical.width / 1080.0f < 0.2f);

	// 720x1280 vertical: same as 720p landscape.
	Placement vertical720 = Place(720.0f, 1280.0f, 0);
	Placement landscape720 = Place(1280.0f, 720.0f, 0);
	CHECK(Near(vertical720.scale, landscape720.scale));
	CHECK(Near(vertical720.margin, landscape720.margin));
	CHECK(Near(vertical720.x, landscape720.x) && Near(vertical720.y, landscape720.y));

	// Square and ultrawide canvases: the short side is the height, as before.
	CHECK(Near(Place(1080.0f, 1080.0f, 0).scale, 0.4f));
	Placement ultrawide = Place(2560.0f, 1080.0f, 1);
	CHECK(Near(ultrawide.scale, 0.4f));
	CHECK(Near(ultrawide.x, 2560.0f - 200.0f - 24.0f));
}

// What SetDelayOverlay() stores after placing the badge with Place().
AppliedLayout Applied(float canvasWidth, float canvasHeight, uint32_t corner)
{
	const Placement p = Place(canvasWidth, canvasHeight, corner);
	AppliedLayout a;
	a.valid = true;
	a.corner = corner;
	a.canvasWidth = canvasWidth;
	a.canvasHeight = canvasHeight;
	a.x = p.x;
	a.y = p.y;
	a.scale = p.scale;
	a.designHeight = kDesignHeight;
	return a;
}

ItemTransform Transform(float x, float y, float scale)
{
	return ItemTransform{x, y, scale, scale};
}

void TestKeepsTheUsersTransform()
{
	g_currentTest = "user transform";
	const AppliedLayout none;
	const AppliedLayout tl1080 = Applied(1920.0f, 1080.0f, 0);
	const ItemTransform asPlaced = Transform(tl1080.x, tl1080.y, tl1080.scale);
	const ItemTransform moved = Transform(900.0f, 500.0f, tl1080.scale);
	const ItemTransform scaled = Transform(tl1080.x, tl1080.y, 0.6f);

	// First show (nothing stored, e.g. a v0.5.x badge): placed.
	CHECK(ShouldPlace(none, moved, 1920.0f, 1080.0f, 0));
	CHECK(!IsUntouched(none, asPlaced));

	// Shown again with nothing changed: never re-placed, moved or not. This
	// was the bug: every appearance reset the user's position and scale.
	CHECK(!ShouldPlace(tl1080, asPlaced, 1920.0f, 1080.0f, 0));
	CHECK(!ShouldPlace(tl1080, moved, 1920.0f, 1080.0f, 0));
	CHECK(!ShouldPlace(tl1080, scaled, 1920.0f, 1080.0f, 0));

	// Another corner picked in the dock: placed there, even if it was moved.
	CHECK(ShouldPlace(tl1080, moved, 1920.0f, 1080.0f, 3));
	CHECK(ShouldPlace(tl1080, asPlaced, 1920.0f, 1080.0f, 1));
	// Out-of-range corners are top-left, the same corner.
	CHECK(!ShouldPlace(tl1080, asPlaced, 1920.0f, 1080.0f, 9));

	// Canvas size changed: follows it only while untouched.
	CHECK(ShouldPlace(tl1080, asPlaced, 2560.0f, 1440.0f, 0));
	CHECK(!ShouldPlace(tl1080, moved, 2560.0f, 1440.0f, 0));
	CHECK(!ShouldPlace(tl1080, scaled, 2560.0f, 1440.0f, 0));

	// Tolerances: sub-pixel and float noise count as untouched.
	CHECK(IsUntouched(tl1080, Transform(tl1080.x + 0.3f, tl1080.y - 0.3f, tl1080.scale * 1.0005f)));
	CHECK(!IsUntouched(tl1080, Transform(tl1080.x + 2.0f, tl1080.y, tl1080.scale)));
	CHECK(!IsUntouched(tl1080, ItemTransform{tl1080.x, tl1080.y, tl1080.scale, tl1080.scale * 1.5f}));
}

void TestTallerBadge()
{
	g_currentTest = "taller badge";
	// Placed bottom-right by an earlier 0.6 build: the 58 px image, and no
	// design height stored (0).
	AppliedLayout old = Applied(1920.0f, 1080.0f, 3);
	old.designHeight = 0.0f;
	old.y = 1080.0f - 58.0f - 24.0f;
	const ItemTransform asPlaced = Transform(old.x, old.y, old.scale);

	// Left there: placed again, so the 72 px image (plate and signature)
	// keeps its 24 px margin instead of hanging 14 px lower.
	CHECK(ShouldPlace(old, asPlaced, 1920.0f, 1080.0f, 3));
	CHECK(Near(Place(1920.0f, 1080.0f, 3).y, 1080.0f - 72.0f - 24.0f));
	// Moved or scaled by the user: stays where they put it.
	CHECK(!ShouldPlace(old, Transform(600.0f, 300.0f, old.scale), 1920.0f, 1080.0f, 3));
	CHECK(!ShouldPlace(old, Transform(old.x, old.y, 0.5f), 1920.0f, 1080.0f, 3));

	// Placed by this build, or by a pre-release one whose image was already
	// 72 px tall (the signature inside the plate): nothing to do.
	const AppliedLayout current = Applied(1920.0f, 1080.0f, 3);
	CHECK(!ShouldPlace(current, Transform(current.x, current.y, current.scale), 1920.0f, 1080.0f, 3));
}

void TestFileNames()
{
	g_currentTest = "file names";
	// The render version is part of the name: a badge drawn by an older
	// build (no signature, or the signature inside the plate) is never
	// picked up.
	CHECK(kRenderVersion == 3);
	CHECK(BadgeFileName(30) == "delay-badge-r3-30s.png");
	CHECK(BadgeFileName(5) == "delay-badge-r3-5s.png");
	CHECK(BadgeFileName(60) == "delay-badge-r3-60s.png");
	CHECK(BadgeFileName(0) == "delay-badge-r3-1s.png");
	CHECK(BadgeFileName(600) == "delay-badge-r3-60s.png");
	CHECK(ClampSeconds(0) == kMinSeconds);
	CHECK(ClampSeconds(61) == kMaxSeconds);
	CHECK(ClampSeconds(42) == 42);
}

void TestStaleFiles()
{
	g_currentTest = "stale files";
	// Older badges: deleted.
	CHECK(IsStaleBadgeFile("delay-badge-30s.png")); // Unversioned names, before "By Trigglow".
	CHECK(IsStaleBadgeFile("delay-badge-5s.png"));
	CHECK(IsStaleBadgeFile("delay-badge-r1-30s.png"));
	CHECK(IsStaleBadgeFile("delay-badge-r2-30s.png")); // The signature inside the plate.
	CHECK(IsStaleBadgeFile("delay-badge-r2-60s.png"));
	CHECK(IsStaleBadgeFile("delay-badge-r30-30s.png")); // Not "r3-".
	// Current badges: kept.
	CHECK(!IsStaleBadgeFile(BadgeFileName(30)));
	CHECK(!IsStaleBadgeFile(BadgeFileName(1)));
	CHECK(!IsStaleBadgeFile(BadgeFileName(60)));
	// Anything else in the folder: never touched.
	CHECK(!IsStaleBadgeFile("notes.txt"));
	CHECK(!IsStaleBadgeFile("delay-badge.png"));
	CHECK(!IsStaleBadgeFile("delay-badge-.png"));
	CHECK(!IsStaleBadgeFile("delay-badge-30s.png.tmp"));
	CHECK(!IsStaleBadgeFile(""));
}

} // namespace

int main()
{
	TestMargin();
	TestPlacement1080p();
	TestPlacementOtherCanvases();
	TestVerticalAndUltrawide();
	TestKeepsTheUsersTransform();
	TestTallerBadge();
	TestFileNames();
	TestStaleFiles();

	std::printf("overlay_layout_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
