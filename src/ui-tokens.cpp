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

#include "ui-tokens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace trigglow::uitokens {

namespace {

constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kWhite{0xff, 0xff, 0xff};

// Text: WCAG 1.4.3 AA. Component borders and focus rings: WCAG 1.4.11.
constexpr double kTextContrast = 4.5;
constexpr double kComponentContrast = 3.0;

int HexDigit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

double Linearize(uint8_t channel)
{
	const double c = channel / 255.0;
	return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double MinContrast(Rgb color, std::initializer_list<Rgb> backgrounds)
{
	double lowest = 21.0;
	for (const Rgb &background : backgrounds)
		lowest = std::min(lowest, ContrastRatio(color, background));
	return lowest;
}

} // namespace

Rgb FromHex(const char *hex)
{
	if (!hex)
		return kBlack;
	if (*hex == '#')
		++hex;
	int values[6];
	for (int i = 0; i < 6; ++i) {
		if (!hex[i])
			return kBlack;
		values[i] = HexDigit(hex[i]);
		if (values[i] < 0)
			return kBlack;
	}
	if (hex[6] != '\0')
		return kBlack;
	return Rgb{static_cast<uint8_t>(values[0] * 16 + values[1]), static_cast<uint8_t>(values[2] * 16 + values[3]),
		   static_cast<uint8_t>(values[4] * 16 + values[5])};
}

std::string Hex(Rgb color)
{
	char buffer[8];
	std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", color.r, color.g, color.b);
	return buffer;
}

Rgb Mix(Rgb a, Rgb b, double t)
{
	t = std::clamp(t, 0.0, 1.0);
	auto channel = [t](uint8_t x, uint8_t y) {
		return static_cast<uint8_t>(std::lround(x * t + y * (1.0 - t)));
	};
	return Rgb{channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b)};
}

double RelativeLuminance(Rgb color)
{
	return 0.2126 * Linearize(color.r) + 0.7152 * Linearize(color.g) + 0.0722 * Linearize(color.b);
}

double ContrastRatio(Rgb a, Rgb b)
{
	const double la = RelativeLuminance(a);
	const double lb = RelativeLuminance(b);
	return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Rgb EnsureContrast(Rgb color, std::initializer_list<Rgb> backgrounds, double minRatio)
{
	if (MinContrast(color, backgrounds) >= minRatio)
		return color;
	// Push toward the pole that the backgrounds contrast with most.
	const Rgb pole = MinContrast(kWhite, backgrounds) >= MinContrast(kBlack, backgrounds) ? kWhite : kBlack;
	for (int step = 1; step <= 50; ++step) {
		const Rgb candidate = Mix(pole, color, step / 50.0);
		if (MinContrast(candidate, backgrounds) >= minRatio)
			return candidate;
	}
	return pole;
}

Scheme DetectScheme(const PaletteInput &palette)
{
	if (palette.systemHighContrast)
		return Scheme::HighContrast;
	// No OS flag to read (macOS/Linux): system high-contrast palettes put
	// near-pure black/white text on near-pure black/white surfaces. Regular
	// dark themes (Yami: 16.5:1 window/text) and light ones (#f0f0f0 with black
	// text: 18.1:1) stay below this.
	if (ContrastRatio(palette.window, palette.text) >= 19.5 && ContrastRatio(palette.base, palette.text) >= 19.5)
		return Scheme::HighContrast;
	return ContrastRatio(palette.window, kWhite) >= ContrastRatio(palette.window, kBlack) ? Scheme::Dark
											      : Scheme::Light;
}

Tokens Compute(const PaletteInput &palette)
{
	Tokens t;
	t.scheme = DetectScheme(palette);
	t.surface = palette.window;
	t.surfaceCard = palette.base;
	t.text = palette.text;

	if (t.scheme == Scheme::HighContrast) {
		// System colors only: no tints, no gradient, state shown by icon + text.
		const Rgb highlight =
			EnsureContrast(palette.highlight, {palette.base, palette.window}, kComponentContrast);
		t.field = palette.base;
		t.textMuted = palette.text;
		t.placeholder = palette.text;
		t.line = palette.text;
		t.lineStrong = palette.text;
		t.accent = highlight;
		t.accentText = EnsureContrast(palette.highlight, {palette.base, palette.window}, kTextContrast);
		t.accentTint = palette.base;
		t.warning = palette.text;
		t.warningText = palette.text;
		t.warningTint = palette.window;
		t.progressChunk = palette.text;
		t.danger = palette.text;
		t.dangerText = palette.text;
		t.dangerTint = palette.window;
		t.dangerBorder = palette.text;
		t.dangerButtonText = palette.text;
		t.focus = highlight;
		// The primary button is filled with the highlight color, so its focus
		// ring uses the text color instead (a highlight ring would vanish).
		t.focusPrimary = palette.text;
		t.disabledFg = Mix(palette.text, palette.base, 0.6);
		t.useGradient = false;
		t.gradientStart = t.gradientEnd = highlight;
		t.gradientHoverStart = t.gradientHoverEnd = highlight;
		t.gradientPressedStart = t.gradientPressedEnd = highlight;
		t.onGradient = EnsureContrast(palette.highlightedText, {highlight}, kTextContrast);
		return t;
	}

	const bool dark = t.scheme == Scheme::Dark;
	const Rgb card = palette.base;
	const Rgb window = palette.window;

	t.field = dark ? Mix(kBlack, card, 0.22) : Mix(brand::kInk, card, 0.04);
	t.line = dark ? Mix(brand::kMist, card, 0.16) : Mix(brand::kInk, card, 0.14);
	t.lineStrong = EnsureContrast(dark ? Mix(brand::kMist, card, 0.42) : Mix(brand::kInk, card, 0.50),
				      {card, window, t.field}, kComponentContrast);

	t.accent = brand::kCyan;
	t.accentTint = dark ? Mix(brand::kCyan, card, 0.12) : Mix(brand::kCyan, card, 0.10);
	t.accentText = EnsureContrast(dark ? brand::kCyan : FromHex("#006a8e"), {card, window, t.field, t.accentTint},
				      kTextContrast);

	t.warning = brand::kAmber;
	t.warningTint = dark ? Mix(brand::kAmber, window, 0.12) : Mix(brand::kAmber, card, 0.12);
	t.warningText =
		EnsureContrast(dark ? brand::kAmber : FromHex("#8a5a00"), {card, window, t.warningTint}, kTextContrast);
	// Amber on a light card is 2:1; the darker text variant keeps the bar visible.
	t.progressChunk = EnsureContrast(dark ? brand::kAmber : t.warningText, {card}, kComponentContrast);

	t.danger = brand::kDanger;
	t.dangerTint = dark ? Mix(brand::kDanger, window, 0.14) : Mix(brand::kDanger, card, 0.10);
	t.dangerBorder = dark ? Mix(brand::kDanger, window, 0.35) : Mix(brand::kDanger, card, 0.40);
	t.dangerText = EnsureContrast(dark ? FromHex("#ff8f8f") : FromHex("#b42318"), {card, window, t.dangerTint},
				      kTextContrast);
	t.dangerButtonText =
		EnsureContrast(dark ? FromHex("#ffc9c9") : FromHex("#b42318"), {t.dangerTint}, kTextContrast);

	// Labels and hints sit on cards, on the window (hint, footer) and on the
	// amber banner (its version line).
	t.textMuted = EnsureContrast(Mix(palette.text, card, dark ? 0.65 : 0.70),
				     {card, window, t.field, t.warningTint}, kTextContrast);
	t.text = EnsureContrast(palette.text, {card, window, t.field, t.warningTint, t.accentTint}, kTextContrast);
	// Without a color of its own, Qt draws placeholders in the text color at
	// half alpha: 3.3:1 on Yami Light's fields. The muted text reaches AA on
	// the field and still reads as "nothing typed yet" next to real text.
	t.placeholder = EnsureContrast(t.textMuted, {t.field}, kTextContrast);

	t.focus = EnsureContrast(dark ? brand::kCyan : FromHex("#006a8e"), {card, window, t.field}, kComponentContrast);
	t.focusPrimary = EnsureContrast(palette.text, {window, card}, kComponentContrast);
	t.disabledFg = Mix(palette.text, card, 0.45);

	t.useGradient = true;
	t.gradientStart = brand::kCyan;
	t.gradientEnd = brand::kMagenta;
	// Hover +12 % light, pressed -8 %, like the web's .sp-btn-primary.
	t.gradientHoverStart = Mix(kWhite, brand::kCyan, 0.20);
	t.gradientHoverEnd = Mix(kWhite, brand::kMagenta, 0.20);
	t.gradientPressedStart = Mix(kBlack, brand::kCyan, 0.08);
	t.gradientPressedEnd = Mix(kBlack, brand::kMagenta, 0.08);
	t.onGradient = EnsureContrast(brand::kOnGradient,
				      {t.gradientStart, t.gradientEnd, t.gradientHoverStart, t.gradientHoverEnd,
				       t.gradientPressedStart, t.gradientPressedEnd},
				      kTextContrast);
	return t;
}

} // namespace trigglow::uitokens
