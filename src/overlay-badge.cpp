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

#include "overlay-badge.hpp"
#include "logging.hpp"
#include "overlay-layout.hpp"
#include "ui-theme.hpp"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QSaveFile>
#include <QThread>

namespace trigglow::ui {

namespace {

constexpr const char *kComponent = "overlay-badge";

// Brand tokens fixed for the overlay, independent of the OBS theme
// (apps/web/src/index.css).
const QColor kInk(0x0f, 0x10, 0x16);
const QColor kMist(0xf2, 0xf1, 0xee);
const QColor kCyan(0x00, 0xd2, 0xff);
const QColor kMagenta(0xc4, 0x4d, 0xff);

QColor WithAlpha(QColor color, double alpha)
{
	color.setAlphaF(static_cast<float>(alpha));
	return color;
}

// Archivo when bundled; the system UI font otherwise. Weights are the files'
// own (600 / 700), so Qt never synthesizes a bolder face from the SemiBold.
QFont BadgeFont(const QString &family, int weight, qreal pixelSize)
{
	QFont font;
	if (!family.isEmpty()) {
		font.setFamilies({family});
	} else {
		font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
	}
	font.setWeight(static_cast<QFont::Weight>(weight));
	font.setPixelSize(std::max(1, qRound(pixelSize)));
	// Drawn under a 2.5x transform: outlines, not hinted 1x glyphs.
	font.setHintingPreference(QFont::PreferNoHinting);
	font.setStyleStrategy(QFont::PreferAntialias);
	return font;
}

} // namespace

QImage RenderDelayBadge(const QString &label, const QString &value)
{
	using namespace overlaylayout;
	const int width = qRound(kDesignWidth * kRenderScale);
	const int height = qRound(kDesignHeight * kRenderScale);
	QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);

	QPainter p(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	// Everything below is in design units (the 1x, 200x58 badge).
	p.scale(kRenderScale, kRenderScale);

	// 1 px dark ring: separates the plate from bright footage (snow, sky, menus).
	p.setPen(QPen(WithAlpha(Qt::black, 0.55), 1.0));
	p.setBrush(Qt::NoBrush);
	p.drawRect(QRectF(0.5, 0.5, 199.0, 57.0));

	// Plate: ink at 92 % keeps the mist text above 15:1 on any footage.
	p.fillRect(QRectF(1.0, 1.0, 198.0, 56.0), WithAlpha(kInk, 0.92));

	// Inner hairline (--color-pulse-line).
	p.setPen(QPen(WithAlpha(kMist, 0.18), 1.0));
	p.drawRect(QRectF(1.5, 1.5, 197.0, 55.0));

	// Brand rule (gradient-brand, 90 degrees).
	QLinearGradient brand(QPointF(1.0, 0.0), QPointF(199.0, 0.0));
	brand.setColorAt(0.0, kCyan);
	brand.setColorAt(1.0, kMagenta);
	p.fillRect(QRectF(1.0, 1.0, 198.0, 3.0), brand);

	// Dynamic Delay mark.
	const QImage mark(QStringLiteral(":/trigglow/overlay/dynamic-delay-mark.png"));
	if (!mark.isNull())
		p.drawImage(QRectF(10.0, 14.0, 32.0, 32.0), mark);

	// Separator.
	p.setPen(QPen(WithAlpha(kMist, 0.16), 1.0));
	p.drawLine(QPointF(50.5, 16.0), QPointF(50.5, 46.0));

	// "DELAY" in cyan (10.5:1 on the plate), "30 s" in mist, 7 px apart, on one
	// baseline. Scaled down together if a fallback font is wider than Archivo.
	const BrandFonts &fonts = Fonts();
	const QString labelFamily = fonts.display;
	const QString valueFamily = fonts.displayBold.isEmpty() ? fonts.display : fonts.displayBold;
	const int valueWeight = fonts.displayBold.isEmpty() && !fonts.display.isEmpty() ? 600 : 700;
	const QString labelText = label.toUpper();
	// "30 s" with a no-break space (U+00A0), which Archivo and every fallback
	// UI font have: Archivo has no narrow no-break space (U+202F), so a
	// translation using it (or a thin space) would make Qt borrow that glyph
	// from another font. Any space in the value is drawn as U+00A0.
	QString valueText = value;
	for (QChar &c : valueText) {
		if (c.isSpace())
			c = QChar(0x00A0);
	}
	constexpr qreal kTextLeft = 60.0;
	constexpr qreal kTextRight = 190.0;
	constexpr qreal kBaseline = 39.5;
	constexpr qreal kGap = 7.0;

	qreal factor = 1.0;
	QFont labelFont;
	QFont valueFont;
	qreal labelWidth = 0.0;
	qreal valueWidth = 0.0;
	for (int attempt = 0; attempt < 12; ++attempt) {
		labelFont = BadgeFont(labelFamily, 600, 18.0 * factor);
		labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0 * factor);
		valueFont = BadgeFont(valueFamily, valueWeight, 26.0 * factor);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		valueFont.setFeature(QFont::Tag("tnum"), 1); // Tabular figures: 5 s and 60 s line up.
#endif
		labelWidth = QFontMetricsF(labelFont).horizontalAdvance(labelText);
		valueWidth = QFontMetricsF(valueFont).horizontalAdvance(valueText);
		if (kTextLeft + labelWidth + kGap + valueWidth <= kTextRight)
			break;
		factor *= 0.95;
	}

	p.setFont(labelFont);
	p.setPen(kCyan);
	p.drawText(QPointF(kTextLeft, kBaseline), labelText);
	p.setFont(valueFont);
	p.setPen(kMist);
	p.drawText(QPointF(kTextLeft + labelWidth + kGap, kBaseline), valueText);
	p.end();
	return image;
}

DelayBadgeFiles::DelayBadgeFiles(QString directory) : directory_(std::move(directory)) {}

QString DelayBadgeFiles::PathFor(uint32_t seconds, const QString &label, const QString &value)
{
	seconds = overlaylayout::ClampSeconds(seconds);
	if (directory_.isEmpty())
		return {};
	const QString path = QDir(directory_).filePath(QString::fromStdString(overlaylayout::BadgeFileName(seconds)));
	// In memory only: every value is redrawn the first time it is asked for
	// in a session, before the badge is shown, so a PNG an older build left
	// on disk (other drawing, other texts) is never what OBS displays and no
	// render version is needed.
	const QString key =
		QStringLiteral("%1\x1f%2\x1f%3\x1f%4").arg(label, value, Fonts().display, Fonts().displayBold);

	if (renderedKeys_.value(seconds) == key && QFileInfo::exists(path))
		return path;

	// QPainter text on a QImage stays on the UI thread (see the header).
	if (QCoreApplication::instance() && QThread::currentThread() != QCoreApplication::instance()->thread()) {
		TRIGGLOW_LOG_WARN(kComponent, "badge for %us requested off the UI thread; not rendering it", seconds);
		return QFileInfo::exists(path) ? path : QString();
	}

	QDir().mkpath(directory_);
	const QImage image = RenderDelayBadge(label, value);
	// QSaveFile: OBS never reads a half-written PNG.
	QSaveFile file(path);
	const bool ok = file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit();
	if (!ok) {
		if (!warned_.value(seconds)) {
			warned_.insert(seconds, true);
			TRIGGLOW_LOG_WARN(kComponent, "could not write the delay badge to %s: %s",
					  path.toUtf8().constData(), file.errorString().toUtf8().constData());
		}
		return QFileInfo::exists(path) ? path : QString();
	}
	renderedKeys_.insert(seconds, key);
	return path;
}

} // namespace trigglow::ui
