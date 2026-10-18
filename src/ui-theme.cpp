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

#include "ui-theme.hpp"

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStyle>
#include <QSvgRenderer>
#include <QWidget>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace trigglow::ui {

namespace {

// ---- Fonts -----------------------------------------------------------------

enum class FontSlot {
	Display,
	DisplayBold,
	Mono,
};

struct FontCandidate {
	FontSlot slot;
	const char *file;
	const char *familyHint; // Preferred when a file registers several families.
};

// Static instances first (what data/fonts ships, see tools/fonts), then the
// upstream variable files in case someone drops those in instead.
constexpr FontCandidate kFontCandidates[] = {
	{FontSlot::Display, "Archivo-SemiBold.ttf", "Archivo"},
	{FontSlot::Display, "Archivo[wdth,wght].ttf", "Archivo"},
	{FontSlot::DisplayBold, "Archivo-Bold.ttf", "Archivo"},
	{FontSlot::DisplayBold, "Archivo[wdth,wght].ttf", "Archivo"},
	{FontSlot::Mono, "GeistMono-Medium.ttf", "Geist Mono"},
	{FontSlot::Mono, "GeistMono[wght].ttf", "Geist Mono"},
};

BrandFonts g_fonts;
BrandFontsReport g_fontsReport;
bool g_fontsLoaded = false;

QString &SlotFamily(FontSlot slot)
{
	switch (slot) {
	case FontSlot::Display:
		return g_fonts.display;
	case FontSlot::DisplayBold:
		return g_fonts.displayBold;
	case FontSlot::Mono:
		break;
	}
	return g_fonts.mono;
}

struct RoleSpec {
	enum Family { Obs, Display, Mono } family;
	double sizeDelta; // Points relative to OBS's base size.
	double minSize;
	int weight;
	bool uppercase;
	double spacingPercent; // QFont::PercentageSpacing; 100 = none.
};

RoleSpec SpecFor(FontRole role)
{
	switch (role) {
	case FontRole::Headline:
		return {RoleSpec::Display, 2.5, 9.0, 600, true, 100.0};
	case FontRole::Title:
		return {RoleSpec::Display, 4.5, 11.0, 600, false, 100.0};
	case FontRole::Eyebrow:
		return {RoleSpec::Mono, -1.5, 7.5, 500, true, 112.0};
	case FontRole::Chip:
		return {RoleSpec::Mono, -1.5, 7.5, 500, false, 100.0};
	case FontRole::Tag:
		return {RoleSpec::Mono, -1.5, 7.5, 500, true, 106.0};
	case FontRole::Data:
		return {RoleSpec::Mono, 0.0, 7.5, 500, false, 100.0};
	case FontRole::DataHint:
		return {RoleSpec::Mono, -0.5, 7.5, 500, false, 100.0};
	case FontRole::Hint:
		return {RoleSpec::Obs, -0.5, 7.5, 400, false, 100.0};
	case FontRole::Button:
		return {RoleSpec::Obs, 0.0, 7.5, 600, true, 106.0};
	case FontRole::Strong:
		break;
	}
	return {RoleSpec::Obs, 0.0, 7.5, 600, false, 100.0};
}

QString RoleFamily(const RoleSpec &spec)
{
	switch (spec.family) {
	case RoleSpec::Display:
		return g_fonts.display;
	case RoleSpec::Mono:
		return g_fonts.mono;
	case RoleSpec::Obs:
		break;
	}
	return {};
}

double RoleSize(const RoleSpec &spec, double basePt)
{
	// Half-point steps keep the generated style sheet stable across tiny
	// differences in the probed base size.
	return std::max(spec.minSize, std::round((basePt + spec.sizeDelta) * 2.0) / 2.0);
}

// "font-family: "Archivo SemiBold"; font-size: 11.5pt; font-weight: 600;".
// Without the brand family the OBS font stays (no font-family at all), with the
// role's size and weight: the documented fallback.
QString FontRule(FontRole role, double basePt)
{
	const RoleSpec spec = SpecFor(role);
	QString rule;
	const QString family = RoleFamily(spec);
	if (!family.isEmpty())
		rule += QStringLiteral("font-family: \"%1\"; ").arg(family);
	rule += QStringLiteral("font-size: %1pt; font-weight: %2;")
			.arg(RoleSize(spec, basePt), 0, 'f', 1)
			.arg(spec.weight);
	return rule;
}

// ---- Icons -----------------------------------------------------------------

const char *IconFile(IconId id)
{
	switch (id) {
	case IconId::AlertTriangle:
		return ":/trigglow/icons/alert-triangle.svg";
	case IconId::Check:
		return ":/trigglow/icons/check.svg";
	case IconId::Circle:
		return ":/trigglow/icons/circle.svg";
	case IconId::Clock:
		return ":/trigglow/icons/clock.svg";
	case IconId::Download:
		return ":/trigglow/icons/download.svg";
	case IconId::ExternalLink:
		return ":/trigglow/icons/external-link.svg";
	case IconId::Flag:
		return ":/trigglow/icons/flag.svg";
	case IconId::Info:
		return ":/trigglow/icons/info.svg";
	case IconId::Lock:
		return ":/trigglow/icons/lock.svg";
	case IconId::Paperclip:
		return ":/trigglow/icons/paperclip.svg";
	case IconId::Shield:
		return ":/trigglow/icons/shield.svg";
	case IconId::User:
		return ":/trigglow/icons/user.svg";
	case IconId::X:
		break;
	}
	return ":/trigglow/icons/x.svg";
}

QByteArray IconSource(IconId id)
{
	static QHash<int, QByteArray> cache;
	const int key = static_cast<int>(id);
	auto it = cache.find(key);
	if (it != cache.end())
		return it.value();
	QFile file(QString::fromLatin1(IconFile(id)));
	QByteArray svg = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
	cache.insert(key, svg);
	return svg;
}

QByteArray Tinted(const QByteArray &svg, const QColor &color)
{
	QByteArray tinted = svg;
	tinted.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
	return tinted;
}

// Renders the SVG straight into whatever QStyle asks for, so the icon stays
// sharp on every screen and scale factor without pre-rendered pixmaps.
class TintedSvgIconEngine : public QIconEngine {
public:
	TintedSvgIconEngine(const QByteArray &svg, const QColor &normal, const QColor &disabled,
			    Qt::Alignment alignment)
		: normal_(Tinted(svg, normal)),
		  disabled_(Tinted(svg, disabled.isValid() ? disabled : normal)),
		  alignment_(alignment)
	{
	}

	void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
	{
		QSvgRenderer renderer(mode == QIcon::Disabled ? disabled_ : normal_);
		const int side = std::min(rect.width(), rect.height());
		QRect square(0, 0, side, side);
		square.moveCenter(rect.center());
		if (alignment_ & Qt::AlignLeft)
			square.moveLeft(rect.left());
		else if (alignment_ & Qt::AlignRight)
			square.moveRight(rect.right());
		renderer.render(painter, QRectF(square));
	}

	QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
	{
		return scaledPixmap(size, mode, state, 1.0);
	}

	QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
	{
		const QSize device = size * std::max<qreal>(scale, 1.0);
		QImage image(device, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		{
			QPainter painter(&image);
			painter.setRenderHint(QPainter::Antialiasing);
			paint(&painter, QRect(QPoint(0, 0), device), mode, state);
		}
		QPixmap pixmap = QPixmap::fromImage(image);
		pixmap.setDevicePixelRatio(std::max<qreal>(scale, 1.0));
		return pixmap;
	}

	QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }

	QIconEngine *clone() const override { return new TintedSvgIconEngine(*this); }

	QString key() const override { return QStringLiteral("trigglow-tinted-svg"); }

private:
	TintedSvgIconEngine(const TintedSvgIconEngine &) = default;

	QByteArray normal_;
	QByteArray disabled_;
	Qt::Alignment alignment_;
};

int g_highContrastOverride = -1;

QString C(uitokens::Rgb color)
{
	return QString::fromStdString(uitokens::Hex(color));
}

QString Gradient(uitokens::Rgb start, uitokens::Rgb end, bool diagonal)
{
	return QStringLiteral("qlineargradient(x1:0, y1:0, x2:1, y2:%1, stop:0 %2, stop:1 %3)")
		.arg(diagonal ? 1 : 0)
		.arg(C(start), C(end));
}

QString BuildStyleSheet(const uitokens::Tokens &t, double basePt)
{
	using uitokens::Scheme;
	const bool hc = t.scheme == Scheme::HighContrast;
	const QString text = C(t.text);
	const QString muted = C(t.textMuted);
	const QString card = C(t.surfaceCard);
	const QString field = C(t.field);
	const QString line = C(t.line);
	const QString lineStrong = C(t.lineStrong);
	const QString accentText = C(t.accentText);
	const QString accentTint = C(t.accentTint);
	const QString warning = C(t.warning);
	const QString warningText = C(t.warningText);
	const QString warningTint = C(t.warningTint);
	const QString danger = C(t.danger);
	const QString dangerText = C(t.dangerText);
	const QString dangerTint = C(t.dangerTint);
	const QString focus = C(t.focus);
	const QString focusPrimary = C(t.focusPrimary);
	const QString disabled = C(t.disabledFg);
	const QString onGradient = C(t.onGradient);
	const QString primary = t.useGradient ? Gradient(t.gradientStart, t.gradientEnd, true) : C(t.gradientStart);
	const QString primaryHover = t.useGradient ? Gradient(t.gradientHoverStart, t.gradientHoverEnd, true)
						   : C(t.gradientHoverStart);
	const QString primaryPressed = t.useGradient ? Gradient(t.gradientPressedStart, t.gradientPressedEnd, true)
						     : C(t.gradientPressedStart);
	const QString rule = t.useGradient ? Gradient(t.gradientStart, t.gradientEnd, false) : C(t.gradientStart);
	// The checked box draws its check in the button text color.
	const QString checkImage = uitokens::RelativeLuminance(t.onGradient) < 0.5
					   ? QStringLiteral(":/trigglow/icons/check-ink.svg")
					   : QStringLiteral(":/trigglow/icons/check-light.svg");

	QString s;
	s.reserve(9000);
	auto add = [&s](const QString &chunk) {
		s += chunk;
		s += QLatin1Char('\n');
	};

	// Surfaces.
	add(QStringLiteral(
		"QScrollArea#tgDockScroll, QWidget#tgDockContent { background: transparent; border: none; }"));
	add(QStringLiteral(
		    "QFrame[tgCard=\"true\"] { background-color: %1; border: 1px solid %2; border-radius: 0px; }")
		    .arg(card, line));
	add(QStringLiteral(
		    "QFrame[tgCard=\"inset\"] { background-color: %1; border: 1px solid %2; border-radius: 0px; }")
		    .arg(field, line));
	add(QStringLiteral("QFrame[tgDivider=\"true\"] { background-color: %1; border: none; min-height: 1px; "
			   "max-height: 1px; }")
		    .arg(line));
	// Status card's top rule (a 2 px QFrame, height fixed in C++).
	add(QStringLiteral("QFrame[tgRule=\"none\"] { border: none; background: transparent; }"));
	add(QStringLiteral("QFrame[tgRule=\"warning\"] { border: none; background-color: %1; }").arg(warning));
	add(QStringLiteral("QFrame[tgRule=\"danger\"] { border: none; background-color: %1; }").arg(danger));
	add(QStringLiteral("QFrame[tgRule=\"active\"] { border: none; background: %1; }").arg(rule));
	add(QStringLiteral("QFrame[tgBanner=\"danger\"] { background-color: %1; border: 1px solid %2; border-left: 3px "
			   "solid %3; border-radius: 0px; }")
		    .arg(dangerTint, line, hc ? text : danger));

	// Text roles; tones come after so they win (same specificity).
	add(QStringLiteral("QLabel { background: transparent; color: %1; }").arg(text));
	add(QStringLiteral("QLabel[tgRole=\"eyebrow\"] { color: %1; %2 }")
		    .arg(muted, FontRule(FontRole::Eyebrow, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"headline\"] { color: %1; %2 }")
		    .arg(text, FontRule(FontRole::Headline, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"title\"] { color: %1; %2 }").arg(text, FontRule(FontRole::Title, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"muted\"] { color: %1; }").arg(muted));
	add(QStringLiteral("QLabel[tgRole=\"hint\"] { color: %1; %2 }").arg(muted, FontRule(FontRole::Hint, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"data\"] { color: %1; %2 }").arg(text, FontRule(FontRole::Data, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"dataMuted\"] { color: %1; %2 }")
		    .arg(muted, FontRule(FontRole::Data, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"dataHint\"] { color: %1; %2 }")
		    .arg(muted, FontRule(FontRole::DataHint, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"strong\"] { color: %1; %2 }").arg(text, FontRule(FontRole::Strong, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"chip\"] { color: %1; border: 1px solid %2; padding: 1px 6px; %3 }")
		    .arg(muted, lineStrong, FontRule(FontRole::Chip, basePt)));
	add(QStringLiteral("QLabel[tgRole=\"tag\"] { color: %1; border: 1px solid %2; padding: 1px 6px; %3 }")
		    .arg(muted, lineStrong, FontRule(FontRole::Tag, basePt)));
	add(QStringLiteral("QLabel[tgTone=\"text\"] { color: %1; }").arg(text));
	add(QStringLiteral("QLabel[tgTone=\"muted\"] { color: %1; }").arg(muted));
	add(QStringLiteral("QLabel[tgTone=\"accent\"] { color: %1; }").arg(accentText));
	add(QStringLiteral("QLabel[tgTone=\"warning\"] { color: %1; }").arg(warningText));
	add(QStringLiteral("QLabel[tgTone=\"danger\"] { color: %1; }").arg(dangerText));
	add(QStringLiteral("QLabel[tgRole=\"chip\"][tgTone=\"accent\"], QLabel[tgRole=\"tag\"][tgTone=\"accent\"] { "
			   "color: %1; background-color: %2; border-color: %1; }")
		    .arg(accentText, accentTint));
	add(QStringLiteral("QLabel[tgRole=\"chip\"][tgTone=\"warning\"], QLabel[tgRole=\"tag\"][tgTone=\"warning\"] { "
			   "color: %1; background-color: %2; border-color: %1; }")
		    .arg(warningText, warningTint));

	// Fields. The drop-down and spin sub-controls keep the OBS theme's arrows.
	add(QStringLiteral("QComboBox, QSpinBox, QLineEdit, QPlainTextEdit { background-color: %1; color: %2; "
			   "border: 1px solid %3; border-radius: 0px; selection-background-color: %4; "
			   "selection-color: %5; }")
		    .arg(field, text, lineStrong, C(t.accent), C(uitokens::brand::kOnGradient)));
	// Explicit content height: OBS themes pin inputs with height/max-height
	// (Yami: 20 px) and margins. 24 + 2 x 2 padding + 2 x 1 border = 30 px.
	add(QStringLiteral("QComboBox, QSpinBox, QLineEdit { height: 24px; min-height: 24px; max-height: 24px; "
			   "margin: 0px; padding: 2px 8px 2px 10px; }"));
	add(QStringLiteral("QPlainTextEdit { padding: 4px 6px; }"));
	add(QStringLiteral(
		    "QComboBox:hover, QSpinBox:hover, QLineEdit:hover, QPlainTextEdit:hover { border-color: %1; }")
		    .arg(muted));
	add(QStringLiteral(
		    "QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border: 1px solid %1; border-bottom: 2px "
		    "solid %1; padding-bottom: 1px; }")
		    .arg(focus));
	add(QStringLiteral("QPlainTextEdit:focus { border: 1px solid %1; border-bottom: 2px solid %1; }").arg(focus));
	add(QStringLiteral(
		    "QComboBox:disabled, QSpinBox:disabled, QLineEdit:disabled, QPlainTextEdit:disabled { color: "
		    "%1; border-color: %2; }")
		    .arg(disabled, line));
	add(QStringLiteral("QLineEdit[tgInvalid=\"true\"], QPlainTextEdit[tgInvalid=\"true\"] { border: 1px solid %1; "
			   "border-bottom: 2px solid %1; }")
		    .arg(dangerText));
	// Placeholders in mono, like the web's support form (Qt can't style the
	// placeholder alone, so the empty field switches font).
	add(QStringLiteral("QLineEdit[tgEmpty=\"true\"], QPlainTextEdit[tgEmpty=\"true\"] { %1 }")
		    .arg(FontRule(FontRole::DataHint, basePt)));

	// Check box: square, hairline; checked = brand gradient + ink check.
	add(QStringLiteral("QCheckBox { color: %1; spacing: 8px; background: transparent; }").arg(text));
	add(QStringLiteral("QCheckBox:disabled { color: %1; }").arg(disabled));
	add(QStringLiteral("QCheckBox::indicator { width: 14px; height: 14px; margin: 0px; border: 1px solid %1; "
			   "background-color: %2; border-radius: 0px; image: none; }")
		    .arg(lineStrong, field));
	add(QStringLiteral("QCheckBox::indicator:hover { border-color: %1; }").arg(muted));
	add(QStringLiteral(
		    "QCheckBox::indicator:checked { border: 1px solid transparent; background: %1; image: url(%2); }")
		    .arg(primary, checkImage));
	add(QStringLiteral("QCheckBox::indicator:focus { border: 2px solid %1; width: 12px; height: 12px; }")
		    .arg(focus));
	add(QStringLiteral("QCheckBox::indicator:checked:focus { border: 2px solid %1; }").arg(focusPrimary));
	add(QStringLiteral("QCheckBox::indicator:disabled { border-color: %1; }").arg(line));
	add(QStringLiteral("QCheckBox::indicator:checked:disabled { background: %1; border-color: %2; }")
		    .arg(disabled, line));

	// Buttons. Focus rings only show for keyboard focus (Qt::TabFocus).
	const QString buttonFont = FontRule(FontRole::Button, basePt);
	// Content heights are explicit for the same reason as the fields:
	// 34 + 2 x 1 border = 36 px (primary, danger, large), 26 + 2 = 28 px.
	const QString tall = QStringLiteral("height: 34px; min-height: 34px; max-height: 34px; margin: 0px;");
	const QString small = QStringLiteral("height: 26px; min-height: 26px; max-height: 26px; margin: 0px;");
	add(QStringLiteral("QPushButton[tgVariant=\"primary\"] { background: %1; color: %2; border: 1px solid "
			   "transparent; border-radius: 0px; padding: 0px 14px; %3 %4 }")
		    .arg(primary, onGradient, tall, buttonFont));
	add(QStringLiteral("QPushButton[tgVariant=\"primary\"]:hover { background: %1; }").arg(primaryHover));
	add(QStringLiteral("QPushButton[tgVariant=\"primary\"]:pressed { background: %1; padding-top: 2px; }")
		    .arg(primaryPressed));
	add(QStringLiteral("QPushButton[tgVariant=\"primary\"]:focus { border: 2px solid %1; }").arg(focusPrimary));
	add(QStringLiteral("QPushButton[tgVariant=\"primary\"]:disabled { background: %1; color: %2; border: 1px solid "
			   "%3; }")
		    .arg(field, disabled, line));

	add(QStringLiteral(
		    "QPushButton[tgVariant=\"secondary\"] { background: transparent; color: %1; border: 1px solid "
		    "%2; border-radius: 0px; padding: 0px 12px; %3 %4 }")
		    .arg(text, lineStrong, small, buttonFont));
	add(QStringLiteral("QPushButton[tgVariant=\"secondary\"][tgSize=\"large\"] { %1 }").arg(tall));
	add(QStringLiteral(
		    "QPushButton[tgVariant=\"secondary\"]:hover { color: %1; border-color: %1; background-color: "
		    "%2; }")
		    .arg(accentText, accentTint));
	add(QStringLiteral("QPushButton[tgVariant=\"secondary\"]:pressed { background-color: %1; padding-top: 1px; }")
		    .arg(accentTint));
	add(QStringLiteral("QPushButton[tgVariant=\"secondary\"]:focus { border: 2px solid %1; }").arg(focus));
	add(QStringLiteral("QPushButton[tgVariant=\"secondary\"]:disabled { color: %1; border-color: %2; background: "
			   "transparent; }")
		    .arg(disabled, line));

	add(QStringLiteral("QPushButton[tgVariant=\"danger\"] { background-color: %1; color: %2; border: 1px solid %3; "
			   "border-radius: 0px; padding: 0px 14px; %4 %5 }")
		    .arg(dangerTint, C(t.dangerButtonText), C(t.dangerBorder), tall, buttonFont));
	add(QStringLiteral("QPushButton[tgVariant=\"danger\"]:hover { border-color: %1; }").arg(hc ? text : danger));
	add(QStringLiteral("QPushButton[tgVariant=\"danger\"]:pressed { padding-top: 2px; }"));
	add(QStringLiteral("QPushButton[tgVariant=\"danger\"]:focus { border: 2px solid %1; }").arg(focus));
	add(QStringLiteral("QPushButton[tgVariant=\"danger\"]:disabled { color: %1; border-color: %2; }")
		    .arg(disabled, line));

	// Link: no box, accent text, underline on hover and keyboard focus. 20 px
	// of content + 3 px padding each side keeps the target at 26 px (WCAG
	// 2.5.8: 24); 2 px of the bottom padding becomes the focus border, so
	// nothing moves.
	add(QStringLiteral("QPushButton[tgVariant=\"link\"] { background: transparent; color: %1; border: none; "
			   "border-radius: 0px; height: 20px; min-height: 20px; max-height: 20px; margin: 0px; "
			   "padding: 3px 0px 3px 0px; text-align: left; }")
		    .arg(accentText));
	add(QStringLiteral("QPushButton[tgVariant=\"link\"]:hover { text-decoration: underline; }"));
	add(QStringLiteral(
		    "QPushButton[tgVariant=\"link\"]:focus { text-decoration: underline; border-bottom: 2px solid "
		    "%1; padding-bottom: 1px; }")
		    .arg(focus));
	add(QStringLiteral("QPushButton[tgVariant=\"link\"]:disabled { color: %1; }").arg(disabled));

	// Update banner: the whole banner is one button.
	// Its height comes from the layout inside (TrigglowDelayDock::FitUpdateBanner).
	add(QStringLiteral("QPushButton#tgUpdateBanner { background-color: %1; border: 1px solid %2; border-left: 3px "
			   "solid %3; border-radius: 0px; padding: 0px; margin: 0px; max-height: 200px; text-align: "
			   "left; }")
		    .arg(warningTint, line, hc ? text : warning));
	add(QStringLiteral("QPushButton#tgUpdateBanner:hover { border-color: %1; border-left: 3px solid %2; }")
		    .arg(warningText, hc ? text : warning));
	add(QStringLiteral("QPushButton#tgUpdateBanner:focus { border: 2px solid %1; border-left: 3px solid %2; }")
		    .arg(focus, hc ? text : warning));

	// Fill progress: 4 px, square. High contrast outlines the track instead of
	// tinting it (track and chunk would both be the text color).
	if (hc) {
		add(QStringLiteral("QProgressBar { background-color: %1; border: 1px solid %2; border-radius: 0px; "
				   "min-height: 6px; max-height: 6px; }")
			    .arg(card, text));
	} else {
		add(QStringLiteral("QProgressBar { background-color: %1; border: none; border-radius: 0px; min-height: "
				   "4px; max-height: 4px; }")
			    .arg(line));
	}
	add(QStringLiteral("QProgressBar::chunk { background-color: %1; border-radius: 0px; }").arg(C(t.progressChunk)));
	return s;
}

} // namespace

// ---- Fonts -----------------------------------------------------------------

BrandFontsReport LoadBrandFonts(const QString &directory)
{
	if (g_fontsLoaded)
		return g_fontsReport;
	g_fontsLoaded = true;
	g_fontsReport.directory = directory;

	QHash<QString, QStringList> registered; // File -> families (a variable file serves two slots).
	for (const FontCandidate &candidate : kFontCandidates) {
		QString &family = SlotFamily(candidate.slot);
		if (!family.isEmpty())
			continue; // Slot already filled by an earlier candidate.

		const QString fileName = QString::fromUtf8(candidate.file);
		const QString path = directory.isEmpty() ? QString() : QDir(directory).filePath(fileName);
		QStringList families;
		if (registered.contains(path)) {
			families = registered.value(path);
		} else if (!path.isEmpty() && QFileInfo::exists(path)) {
			const int id = QFontDatabase::addApplicationFont(path);
			families = id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
			registered.insert(path, families);
			if (families.isEmpty())
				g_fontsReport.failed << fileName;
		} else {
			continue;
		}
		if (families.isEmpty())
			continue;

		family = families.first();
		for (const QString &candidateFamily : families) {
			if (candidateFamily.startsWith(QString::fromUtf8(candidate.familyHint))) {
				family = candidateFamily;
				break;
			}
		}
		g_fontsReport.loaded << QStringLiteral("%1 -> %2").arg(fileName, family);
	}

	if (g_fonts.display.isEmpty())
		g_fontsReport.missing << QStringLiteral("Archivo-SemiBold.ttf");
	if (g_fonts.displayBold.isEmpty())
		g_fontsReport.missing << QStringLiteral("Archivo-Bold.ttf");
	if (g_fonts.mono.isEmpty())
		g_fontsReport.missing << QStringLiteral("GeistMono-Medium.ttf");
	return g_fontsReport;
}

const BrandFonts &Fonts()
{
	return g_fonts;
}

double BaseFontPointSize(QWidget *context)
{
	// A throwaway child polished with the same cascade (application style
	// sheet + ours, which sets no generic font) reports what OBS's theme
	// gives plain widgets, e.g. Yami's `QWidget { font-size: var(--font_base) }`.
	QWidget probe(context);
	probe.ensurePolished();
	const QFont font = probe.font();
	double pt = font.pointSizeF();
	if (pt <= 0.0 && font.pixelSize() > 0)
		pt = font.pixelSize() * 0.75;
	if (pt <= 0.0)
		pt = 9.0;
	return std::clamp(pt, 7.0, 20.0);
}

void ApplyFont(QWidget *widget, FontRole role, double basePt)
{
	if (!widget)
		return;
	const RoleSpec spec = SpecFor(role);
	QFont font = widget->font();
	const QString family = RoleFamily(spec);
	if (!family.isEmpty())
		font.setFamilies({family});
	font.setPointSizeF(RoleSize(spec, basePt));
	font.setWeight(static_cast<QFont::Weight>(spec.weight));
	font.setCapitalization(spec.uppercase ? QFont::AllUppercase : QFont::MixedCase);
	font.setLetterSpacing(QFont::PercentageSpacing, spec.spacingPercent);
	widget->setFont(font);
}

// ---- Icons -----------------------------------------------------------------

QIcon Icon(IconId id, const QColor &color, const QColor &disabled, Qt::Alignment alignment)
{
	return QIcon(new TintedSvgIconEngine(IconSource(id), color, disabled, alignment));
}

QPixmap IconPixmap(IconId id, const QColor &color, int logicalSize, qreal devicePixelRatio)
{
	return Icon(id, color).pixmap(QSize(logicalSize, logicalSize), devicePixelRatio);
}

// ---- Theme -----------------------------------------------------------------

bool SystemHighContrast()
{
	if (g_highContrastOverride >= 0)
		return g_highContrastOverride == 1;
#ifdef _WIN32
	HIGHCONTRASTW hc = {};
	hc.cbSize = sizeof(hc);
	if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
		return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
#endif
	return false;
}

void SetHighContrastOverride(int value)
{
	g_highContrastOverride = value;
}

uitokens::PaletteInput PaletteInputFrom(const QPalette &palette)
{
	auto rgb = [&palette](QPalette::ColorRole role) {
		const QColor c = palette.color(QPalette::Active, role);
		return uitokens::Rgb{static_cast<uint8_t>(c.red()), static_cast<uint8_t>(c.green()),
				     static_cast<uint8_t>(c.blue())};
	};
	uitokens::PaletteInput input;
	input.window = rgb(QPalette::Window);
	input.base = rgb(QPalette::Base);
	input.text = rgb(QPalette::Text);
	input.highlight = rgb(QPalette::Highlight);
	input.highlightedText = rgb(QPalette::HighlightedText);
	input.systemHighContrast = SystemHighContrast();
	return input;
}

QColor ToQColor(uitokens::Rgb color)
{
	return QColor(color.r, color.g, color.b);
}

Theme BuildTheme(const QPalette &palette, double basePt)
{
	Theme theme;
	theme.tokens = uitokens::Compute(PaletteInputFrom(palette));
	theme.basePt = basePt;
	theme.styleSheet = BuildStyleSheet(theme.tokens, basePt);
	return theme;
}

void SetStyleProperty(QWidget *widget, const char *name, const QString &value)
{
	if (!widget || widget->property(name).toString() == value)
		return;
	widget->setProperty(name, value);
	widget->style()->unpolish(widget);
	widget->style()->polish(widget);
	widget->update();
}

} // namespace trigglow::ui
