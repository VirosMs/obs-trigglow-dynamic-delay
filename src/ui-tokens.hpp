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

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

// Color tokens of the plugin's UI (dock and report dialog), derived from the
// active OBS theme's palette. Pure C++ (no Qt, no libobs) so the contrast
// guarantees can be unit-tested: tests/ui_tokens_tests.cpp.
//
// Hybrid theming: surfaces (window, cards, fields) follow OBS's palette so the
// dock sits naturally in Yami, light themes and the system high-contrast mode;
// the Trigglow brand comes in through the fixed accents below (the web app's
// color tokens) and through square shapes, fonts and icons
// (ui-theme.hpp). Every text token is pushed until it reaches WCAG AA (4.5:1)
// against the backgrounds it is drawn on, and every component border
// (fields, secondary buttons, focus ring) reaches 3:1, whatever the theme.
namespace trigglow::uitokens {

struct Rgb {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;

	bool operator==(const Rgb &other) const { return r == other.r && g == other.g && b == other.b; }
	bool operator!=(const Rgb &other) const { return !(*this == other); }
};

// "#rrggbb" or "rrggbb"; anything else parses as black.
Rgb FromHex(const char *hex);
// Lowercase "#rrggbb".
std::string Hex(Rgb color);

// a * t + b * (1 - t), per channel in sRGB, rounded (t clamped to [0, 1]).
Rgb Mix(Rgb a, Rgb b, double t);
// WCAG 2.x relative luminance and contrast ratio (1..21).
double RelativeLuminance(Rgb color);
double ContrastRatio(Rgb a, Rgb b);

// `color` itself if it already reaches `minRatio` against every background;
// otherwise `color` mixed step by step toward black or white (whichever
// contrasts more with the backgrounds) until it does, or the pole itself.
Rgb EnsureContrast(Rgb color, std::initializer_list<Rgb> backgrounds, double minRatio);

// Brand constants (the Trigglow web palette).
namespace brand {
constexpr Rgb kInk{0x0f, 0x10, 0x16};        // --color-pulse-ink
constexpr Rgb kMist{0xf2, 0xf1, 0xee};       // --color-pulse-mist
constexpr Rgb kCyan{0x00, 0xd2, 0xff};       // --color-pulse-cyan / signal
constexpr Rgb kMagenta{0xc4, 0x4d, 0xff};    // --color-pulse-magenta
constexpr Rgb kAmber{0xf0, 0xa2, 0x02};      // --color-pulse-amber
constexpr Rgb kDanger{0xff, 0x6b, 0x6b};     // --color-pulse-danger
constexpr Rgb kOnGradient{0x0b, 0x0c, 0x12}; // .sp-btn-primary text
} // namespace brand

enum class Scheme {
	Dark,
	Light,
	HighContrast,
};

// The QPalette roles the tokens are derived from (QPalette::Active group).
struct PaletteInput {
	Rgb window;
	Rgb base;
	Rgb text;
	Rgb highlight;
	Rgb highlightedText;
	// The OS reports a high-contrast mode (Windows: SPI_GETHIGHCONTRAST).
	bool systemHighContrast = false;
};

struct Tokens {
	Scheme scheme = Scheme::Dark;

	Rgb surface;     // Dock background (the theme's window color).
	Rgb surfaceCard; // Cards (the theme's base color).
	Rgb field;       // Combo boxes, spin boxes, line edits: a step darker than the card.
	Rgb text;
	Rgb textMuted;   // Labels, details, hints.
	Rgb placeholder; // Placeholder text in fields (QSS placeholder-text-color): AA on the field.
	Rgb line;        // Decorative hairlines (card borders, dividers, progress track).
	Rgb lineStrong;  // Field and secondary-button borders (WCAG 1.4.11: 3:1).

	Rgb accent;     // Cyan fill.
	Rgb accentText; // Cyan text and icons (darkened on light themes).
	Rgb accentTint; // Chip and secondary-hover background.

	Rgb warning; // Amber fill (rules, progress chunk on dark themes).
	Rgb warningText;
	Rgb warningTint;   // Update banner and BETA chip background.
	Rgb progressChunk; // Filling progress bar.

	Rgb danger; // Red fill (Error rule).
	Rgb dangerText;
	Rgb dangerTint;       // Disable button and error banner background.
	Rgb dangerBorder;     // Disable button border.
	Rgb dangerButtonText; // Disable button text.

	Rgb focus;        // Focus ring of fields, secondary buttons, links.
	Rgb focusPrimary; // Focus ring of the gradient primary button.
	Rgb disabledFg;   // Disabled text (opaque: QSS has no opacity).

	// Primary button, checked check box and the Active rule. On high
	// contrast the gradient collapses into the system highlight.
	bool useGradient = true;
	Rgb gradientStart;
	Rgb gradientEnd;
	Rgb gradientHoverStart;
	Rgb gradientHoverEnd;
	Rgb gradientPressedStart;
	Rgb gradientPressedEnd;
	Rgb onGradient; // Text and icons on the primary button.
};

// Dark when white text reads better than black on the window color; high
// contrast when the OS says so, or (other systems) when window, base and text
// are near pure black/white extremes.
Scheme DetectScheme(const PaletteInput &palette);
Tokens Compute(const PaletteInput &palette);

} // namespace trigglow::uitokens
