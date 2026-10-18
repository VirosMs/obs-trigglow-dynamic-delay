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

// Standalone unit tests for trigglow::uitokens (src/ui-tokens.hpp): the color
// tokens the dock and the report dialog are styled with. The point is the
// accessibility contract, checked on the palettes of the OBS themes the dock
// has to live in: every text token reaches WCAG AA (4.5:1) on the surfaces it
// is drawn on, every border/focus ring reaches 3:1. Same approach as the other
// tests: no framework, a CHECK macro that keeps going, non-zero exit on failure.

#include "ui-tokens.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace trigglow::uitokens;

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

void CheckRatio(Rgb fg, Rgb bg, double min, const char *what, const char *theme)
{
	++g_checks;
	const double ratio = ContrastRatio(fg, bg);
	if (ratio + 1e-9 < min) {
		++g_failures;
		std::printf("FAIL [%s] %s: %s on %s = %.2f:1 (< %.1f:1) on theme %s\n", g_currentTest, what,
			    Hex(fg).c_str(), Hex(bg).c_str(), ratio, min, theme);
	}
}

struct Theme {
	const char *name;
	PaletteInput palette;
	Scheme expected;
};

PaletteInput P(const char *window, const char *base, const char *text, const char *highlight,
	       const char *highlightedText, bool systemHighContrast = false)
{
	PaletteInput p;
	p.window = FromHex(window);
	p.base = FromHex(base);
	p.text = FromHex(text);
	p.highlight = FromHex(highlight);
	p.highlightedText = FromHex(highlightedText);
	p.systemHighContrast = systemHighContrast;
	return p;
}

// Palettes of the themes the dock has to live in: OBS 31.1.1's own themes
// (.deps/obs-studio-31.1.1/frontend/data/themes: Yami.obt and its Yami_*.ovt
// variants; palette_window = bg_window, palette_base = bg_base, palette_text =
// text, palette_highlight = primary, palette_highlightedText = text), the
// theme the proposal measured in the published dock captures, other dark and
// light palettes (white cards, pure black text), and the Windows
// high-contrast schemes.
const Theme kThemes[] = {
	{"Yami (default, OBS 31)", P("#1d1f26", "#272a33", "#ffffff", "#284cb8", "#ffffff"), Scheme::Dark},
	{"Captured dark", P("#31363b", "#232629", "#eff0f1", "#3daee9", "#eff0f1"), Scheme::Dark},
	{"Yami Grey (OBS 31)", P("#212121", "#2f2f2f", "#ffffff", "#284cb8", "#ffffff"), Scheme::Dark},
	{"Yami Classic (OBS 31)", P("#3a393a", "#1f1e1f", "#ffffff", "#19344c", "#ffffff"), Scheme::Dark},
	{"Yami Acri (OBS 31)", P("#101010", "#181819", "#ffffff", "#131a30", "#ffffff"), Scheme::Dark},
	{"Yami Rachni (OBS 31)", P("#31363b", "#232629", "#ffffff", "#914c67", "#ffffff"), Scheme::Dark},
	{"Dark grey", P("#2f2f2f", "#3a3a3a", "#ffffff", "#4f4f4f", "#ffffff"), Scheme::Dark},
	{"Mid grey", P("#4a4a4a", "#3c3c3c", "#dcdcdc", "#5a8dee", "#ffffff"), Scheme::Dark},
	{"Yami Light (OBS 31)", P("#d3d3d3", "#e5e5e5", "#0a0a0a", "#8cb5ff", "#0a0a0a"), Scheme::Light},
	{"Light, white cards", P("#e8e9ec", "#ffffff", "#1a1b20", "#7aa7f4", "#000000"), Scheme::Light},
	{"Light grey", P("#f0f0f0", "#ffffff", "#000000", "#0078d4", "#ffffff"), Scheme::Light},
	{"Light tinted", P("#dcdde3", "#f4f4f6", "#2a2b30", "#3874d8", "#ffffff"), Scheme::Light},
	{"HC Black (Windows)", P("#000000", "#000000", "#ffffff", "#1aebff", "#000000", true), Scheme::HighContrast},
	{"HC White (Windows)", P("#ffffff", "#ffffff", "#000000", "#37006e", "#ffffff", true), Scheme::HighContrast},
	{"HC Aquatic (Windows)", P("#202020", "#202020", "#ffffff", "#8ee3f0", "#263b50", true), Scheme::HighContrast},
	{"HC detected without OS flag", P("#000000", "#000000", "#ffffff", "#ffff00", "#000000"), Scheme::HighContrast},
};

void TestColorMath()
{
	g_currentTest = "color math";
	CHECK(FromHex("#00d2ff") == (Rgb{0x00, 0xd2, 0xff}));
	CHECK(FromHex("c44dff") == (Rgb{0xc4, 0x4d, 0xff}));
	CHECK(FromHex("#12345") == (Rgb{0, 0, 0}));
	CHECK(FromHex("#1234567") == (Rgb{0, 0, 0}));
	CHECK(FromHex("#zz0000") == (Rgb{0, 0, 0}));
	CHECK(FromHex(nullptr) == (Rgb{0, 0, 0}));
	CHECK(Hex(Rgb{0x0f, 0x10, 0x16}) == "#0f1016");

	CHECK(std::fabs(ContrastRatio(FromHex("#000000"), FromHex("#ffffff")) - 21.0) < 1e-6);
	CHECK(std::fabs(ContrastRatio(FromHex("#777777"), FromHex("#777777")) - 1.0) < 1e-9);
	// Symmetric.
	CHECK(std::fabs(ContrastRatio(FromHex("#00d2ff"), FromHex("#232629")) -
			ContrastRatio(FromHex("#232629"), FromHex("#00d2ff"))) < 1e-12);
	// Values from the proposal's audit (white on the old Tailwind buttons).
	CHECK(std::fabs(ContrastRatio(FromHex("#ffffff"), FromHex("#22c55e")) - 2.28) < 0.01);
	CHECK(std::fabs(ContrastRatio(FromHex("#ffffff"), FromHex("#ef4444")) - 3.76) < 0.01);

	// Mix endpoints and the proposal's derived values.
	CHECK(Mix(FromHex("#ffffff"), FromHex("#000000"), 1.0) == FromHex("#ffffff"));
	CHECK(Mix(FromHex("#ffffff"), FromHex("#000000"), 0.0) == FromHex("#000000"));
	CHECK(Mix(FromHex("#ffffff"), FromHex("#000000"), 2.0) == FromHex("#ffffff")); // Clamped.
	CHECK(Mix(brand::kCyan, FromHex("#232629"), 0.12) == FromHex("#1f3b43"));
	CHECK(Mix(brand::kAmber, FromHex("#31363b"), 0.12) == FromHex("#484334"));

	// EnsureContrast leaves passing colors alone and fixes failing ones.
	CHECK(EnsureContrast(FromHex("#ffffff"), {FromHex("#000000")}, 4.5) == FromHex("#ffffff"));
	const Rgb fixed = EnsureContrast(FromHex("#777777"), {FromHex("#666666")}, 4.5);
	CHECK(ContrastRatio(fixed, FromHex("#666666")) >= 4.5);
	// Impossible targets end at the best pole instead of looping.
	const Rgb pole = EnsureContrast(FromHex("#808080"), {FromHex("#000000"), FromHex("#ffffff")}, 4.5);
	CHECK(pole == FromHex("#000000") || pole == FromHex("#ffffff"));
}

void TestSchemeDetection()
{
	g_currentTest = "scheme detection";
	for (const Theme &theme : kThemes) {
		++g_checks;
		if (DetectScheme(theme.palette) != theme.expected) {
			++g_failures;
			std::printf("FAIL [%s] theme %s detected as %d\n", g_currentTest, theme.name,
				    static_cast<int>(DetectScheme(theme.palette)));
		}
	}
}

const PaletteInput &PaletteOf(const char *name)
{
	for (const Theme &theme : kThemes) {
		if (std::strcmp(theme.name, name) == 0)
			return theme.palette;
	}
	std::printf("FAIL: no theme called %s\n", name);
	++g_failures;
	return kThemes[0].palette;
}

void TestBrandValuesOnDarkAndLight()
{
	g_currentTest = "brand values";
	const Tokens dark = Compute(PaletteOf("Captured dark"));
	// The proposal's dark tokens on the captured theme.
	CHECK(dark.accentText == brand::kCyan);
	CHECK(dark.warningText == brand::kAmber);
	CHECK(dark.accentTint == FromHex("#1f3b43"));
	CHECK(dark.warningTint == FromHex("#484334"));
	CHECK(dark.dangerTint == FromHex("#4e3d42"));
	CHECK(dark.dangerBorder == FromHex("#79494c"));
	CHECK(dark.gradientStart == brand::kCyan);
	CHECK(dark.gradientEnd == brand::kMagenta);
	CHECK(dark.gradientHoverStart == FromHex("#33dbff"));
	CHECK(dark.gradientHoverEnd == FromHex("#d071ff"));
	CHECK(dark.gradientPressedStart == FromHex("#00c1eb"));
	CHECK(dark.gradientPressedEnd == FromHex("#b447eb"));
	CHECK(dark.onGradient == brand::kOnGradient);
	CHECK(dark.useGradient);

	const Tokens light = Compute(PaletteOf("Light, white cards"));
	// Fills stay brand; text variants darken to pass AA on white.
	CHECK(light.accent == brand::kCyan);
	CHECK(light.warning == brand::kAmber);
	CHECK(light.accentText == FromHex("#006a8e"));
	CHECK(light.warningText == FromHex("#8a5a00"));
	CHECK(light.dangerText == FromHex("#b42318"));
	CHECK(light.useGradient);

	// OBS 31's Yami Light has grey cards (#e5e5e5 on a #d3d3d3 window): the
	// text variants are pushed darker than on white cards to keep AA.
	const Tokens yamiLight = Compute(PaletteOf("Yami Light (OBS 31)"));
	CHECK(yamiLight.accent == brand::kCyan);
	CHECK(yamiLight.useGradient);
	CHECK(RelativeLuminance(yamiLight.accentText) < RelativeLuminance(FromHex("#006a8e")));
	CHECK(ContrastRatio(yamiLight.accentText, yamiLight.surface) >= 4.5);

	const Tokens hc = Compute(PaletteOf("HC Black (Windows)"));
	CHECK(!hc.useGradient);
	CHECK(hc.text == FromHex("#ffffff"));
	CHECK(hc.textMuted == hc.text);
	CHECK(hc.line == hc.text);
}

void TestAccessibilityContract()
{
	g_currentTest = "WCAG contract";
	for (const Theme &theme : kThemes) {
		const Tokens t = Compute(theme.palette);
		const char *n = theme.name;

		// Body text, muted text and the colored text variants (status
		// headline, estimate, account errors, links) on every surface.
		for (Rgb bg : {t.surface, t.surfaceCard, t.field}) {
			CheckRatio(t.text, bg, 4.5, "text", n);
			CheckRatio(t.textMuted, bg, 4.5, "textMuted", n);
			CheckRatio(t.accentText, bg, 4.5, "accentText", n);
			CheckRatio(t.dangerText, bg, 4.5, "dangerText", n);
		}
		for (Rgb bg : {t.surface, t.surfaceCard})
			CheckRatio(t.warningText, bg, 4.5, "warningText", n);
		// Placeholders ("you@email.com", "Choose a scene...") are text too.
		CheckRatio(t.placeholder, t.field, 4.5, "placeholder on field", n);
		// Locked fields (scenes, seconds and quality while the delay runs)
		// draw their value in textMuted on the card's background
		// (ui-theme.cpp); textMuted also tints the lock inside them.
		CheckRatio(t.textMuted, t.surfaceCard, 4.5, "locked field value", n);

		// Chips and banners.
		CheckRatio(t.accentText, t.accentTint, 4.5, "accentText on accentTint (seconds chip)", n);
		CheckRatio(t.warningText, t.warningTint, 4.5, "warningText on warningTint (BETA chip)", n);
		CheckRatio(t.text, t.warningTint, 4.5, "text on warningTint (update banner title)", n);
		CheckRatio(t.textMuted, t.warningTint, 4.5, "textMuted on warningTint (update banner version)", n);
		CheckRatio(t.dangerButtonText, t.dangerTint, 4.5, "Disable button text", n);
		CheckRatio(t.dangerText, t.dangerTint, 4.5, "dangerText on dangerTint (error banner)", n);

		// Primary button text on every gradient stop, in every state.
		for (Rgb stop : {t.gradientStart, t.gradientEnd, t.gradientHoverStart, t.gradientHoverEnd,
				 t.gradientPressedStart, t.gradientPressedEnd})
			CheckRatio(t.onGradient, stop, 4.5, "onGradient", n);

		// Component boundaries and focus indicators (WCAG 1.4.11).
		for (Rgb bg : {t.surface, t.surfaceCard, t.field}) {
			CheckRatio(t.lineStrong, bg, 3.0, "lineStrong", n);
			CheckRatio(t.focus, bg, 3.0, "focus", n);
		}
		CheckRatio(t.focusPrimary, t.surface, 3.0, "focusPrimary", n);
		CheckRatio(t.focusPrimary, t.surfaceCard, 3.0, "focusPrimary", n);
		CheckRatio(t.progressChunk, t.surfaceCard, 3.0, "progress chunk", n);
	}
}

} // namespace

int main()
{
	TestColorMath();
	TestSchemeDetection();
	TestBrandValuesOnDarkAndLight();
	TestAccessibilityContract();

	std::printf("ui_tokens_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
