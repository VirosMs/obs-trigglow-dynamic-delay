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

#include "ui-tokens.hpp"

#include <QColor>
#include <QIcon>
#include <QString>
#include <QStringList>

class QPalette;
class QWidget;

// The single place the plugin's widgets (dock, report dialog) get their look
// from: color tokens (ui-tokens.hpp) turned into one style sheet, the bundled
// brand fonts and the tinted SVG icons. No libobs here, so the UI harness can
// render it without OBS.
//
// Rules this file keeps:
// - Never touches OBS's global look: no QApplication::setFont/setStyleSheet,
//   fonts are registered for the process but only used by our widgets.
// - Radius 0 everywhere (the web's ArtCraft shapes), hairline borders, the
//   brand gradient only on the primary action, the checked box and the Active
//   rule.
// - Fonts: OBS themes (Yami) set `QWidget { font-family; font-size }` in the
//   application style sheet, which would override QWidget::setFont(). So the
//   family, size and weight of each role are also written into our widget-level
//   style sheet (which wins over the application one), while capitalization
//   and letter spacing -- which QSS can't express -- are set with setFont() and
//   survive re-polishing (QSS only resolves the properties it declares).
namespace trigglow::ui {

// ---- Fonts -----------------------------------------------------------------

// Families of the bundled fonts as Qt registered them
// (QFontDatabase::applicationFontFamilies: DirectWrite may expose the static
// SemiBold as its own "Archivo SemiBold" family, GDI/FreeType as "Archivo").
// Empty when the file is missing or couldn't be registered: callers then fall
// back to OBS's own UI font.
struct BrandFonts {
	QString display;     // Archivo SemiBold (600): headlines, dialog title, overlay label.
	QString displayBold; // Archivo Bold (700): overlay value.
	QString mono;        // Geist Mono Medium (500): eyebrows, chips, data.
};

struct BrandFontsReport {
	QString directory;
	QStringList loaded;  // "Archivo-SemiBold.ttf -> Archivo SemiBold"
	QStringList missing; // Files not found in `directory`.
	QStringList failed;  // Files found but rejected by QFontDatabase.
};

// Registers the bundled fonts in `directory` (the plugin's data/fonts) once per
// process; later calls return the first report. Accepts the static instances
// the plugin ships (Archivo-SemiBold.ttf, Archivo-Bold.ttf,
// GeistMono-Medium.ttf) or the upstream variable files (Archivo[wdth,wght].ttf,
// GeistMono[wght].ttf). The caller logs the report (one line).
BrandFontsReport LoadBrandFonts(const QString &directory);
const BrandFonts &Fonts();

enum class FontRole {
	Headline, // Dock status headline: display 600, uppercase, base + 2.5 pt.
	Title,    // Dialog title: display 600, base + 4.5 pt.
	Eyebrow,  // Section labels: mono 500, uppercase, +12 %, base - 1.5 pt.
	Chip,     // Values in a chip ("30 s"): mono 500, base - 1.5 pt (units keep their case).
	Tag,      // Word chips ("BETA", "ATTACHED"): like Chip, uppercase, +6 %.
	Data,     // Numbers and the version: mono 500, base.
	DataHint, // Countdown and placeholders: mono 500, base - 0.5 pt.
	Hint,     // OBS font, base - 0.5 pt.
	Button,   // OBS font 600, uppercase, +6 %, base.
	Strong,   // OBS font 600, base.
};

// Base point size of OBS's UI font as the theme applies it to `context`
// (the theme's QWidget font-size rule included).
double BaseFontPointSize(QWidget *context);
// setFont() with the role's family/size/weight plus capitalization and letter
// spacing. Pair it with the style sheet from StyleSheet() for the same base.
void ApplyFont(QWidget *widget, FontRole role, double basePt);

// ---- Icons -----------------------------------------------------------------

// Stroke icons from the Trigglow web app's icon set (Lucide paths, ISC), bundled in
// src/resources/trigglow-ui.qrc and recolored at runtime.
enum class IconId {
	AlertTriangle,
	Check,
	Circle,
	Clock,
	Copy,
	Download,
	ExternalLink,
	Flag,
	Info,
	Lock,
	Paperclip,
	Shield,
	Sliders, // Something was adjusted for the user: the degradation notice.
	User,
	X,
};

// A vector icon (sharp at any device pixel ratio) in `color`; `disabled`
// colors the QIcon::Disabled mode (defaults to `color`). The glyph stays
// square: in a wider icon size (a button's icon plus its gap to the text) it
// sits at `alignment`.
QIcon Icon(IconId id, const QColor &color, const QColor &disabled = QColor(),
	   Qt::Alignment alignment = Qt::AlignCenter);
QPixmap IconPixmap(IconId id, const QColor &color, int logicalSize, qreal devicePixelRatio);

// ---- Theme -----------------------------------------------------------------

// Windows: SPI_GETHIGHCONTRAST. Elsewhere false (ui-tokens.hpp then detects it
// from the palette). The harness can force it with SetHighContrastOverride().
bool SystemHighContrast();
void SetHighContrastOverride(int value); // -1: none, 0: off, 1: on.

uitokens::PaletteInput PaletteInputFrom(const QPalette &palette);
QColor ToQColor(uitokens::Rgb color);

struct Theme {
	uitokens::Tokens tokens;
	double basePt = 9.0;
	QString styleSheet;
};

// Tokens from `palette` (normally the application palette, which OBS sets
// from the active theme) and the style sheet for the dock and the report
// dialog. Only colors, borders, paddings and the font family/size/weight of
// each role; no box shadows, no transitions (QSS has neither).
Theme BuildTheme(const QPalette &palette, double basePt);

// setProperty + re-polish, only when the value actually changes (re-polishing
// on every 250 ms tick of the fill countdown would be wasted work).
void SetStyleProperty(QWidget *widget, const char *name, const QString &value);

} // namespace trigglow::ui
