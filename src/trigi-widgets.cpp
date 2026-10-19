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

#include "trigi-widgets.hpp"
#include "api-client.hpp"
#include "i18n.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <QAbstractTextDocumentLayout>
#include <QAccessible>
#include <QBoxLayout>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>

namespace trigglow {

namespace {

constexpr const char *kComponent = "report-bug-dialog";

// Lines the question box grows between.
constexpr int kQuestionMinLines = 2;
constexpr int kQuestionMaxLines = 6;

// A document that never loads anything: an <img> or a stylesheet link can't
// reach the disk or the network even if one ever got past the escaping.
class InertDocument : public QTextDocument {
public:
	using QTextDocument::QTextDocument;

protected:
	QVariant loadResource(int, const QUrl &) override { return QVariant(); }
};

} // namespace

// ---- ChatTextView ------------------------------------------------------------------

ChatTextView::ChatTextView(QWidget *parent) : QWidget(parent), doc_(new InertDocument(this))
{
	doc_->setUndoRedoEnabled(false);
	doc_->setDocumentMargin(0);
	QTextOption option = doc_->defaultTextOption();
	option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	doc_->setDefaultTextOption(option);
	QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	policy.setHeightForWidth(true);
	setSizePolicy(policy);
}

void ChatTextView::SetLineHtml(const QString &html)
{
	source_ = html;
	rich_ = true;
	Rebuild();
}

void ChatTextView::SetPlainText(const QString &text)
{
	source_ = text;
	rich_ = false;
	Rebuild();
}

void ChatTextView::SetStyle(const Style &style)
{
	style_ = style;
	Rebuild();
}

QString ChatTextView::PlainText() const
{
	return doc_->toPlainText();
}

const QTextDocument *ChatTextView::Document() const
{
	return doc_;
}

void ChatTextView::Rebuild()
{
	doc_->setDefaultFont(font());
	// The only class LineHtml() emits. Mono family: the bundled Geist Mono, or
	// the system's.
	const QString mono = style_.monoFamily.isEmpty() ? QStringLiteral("monospace") : style_.monoFamily;
	const QString code = style_.codeBackground.isValid() ? style_.codeBackground.name()
							     : QStringLiteral("transparent");
	doc_->setDefaultStyleSheet(
		QStringLiteral(".code { font-family: '%1'; background-color: %2; }").arg(mono, code));
	if (rich_)
		doc_->setHtml(source_);
	else
		doc_->setPlainText(source_);
	updateGeometry();
	update();
}

int ChatTextView::heightForWidth(int width) const
{
	doc_->setTextWidth(std::max(1, width));
	return static_cast<int>(std::ceil(doc_->size().height()));
}

QSize ChatTextView::sizeHint() const
{
	// Natural width (no wrapping), so a short message gets a narrow bubble;
	// the bubble's maximum width wraps the long ones.
	doc_->setTextWidth(-1);
	const int natural = static_cast<int>(std::ceil(std::min<qreal>(doc_->size().width(), 4096.0))) + 1;
	return {natural, heightForWidth(natural)};
}

QSize ChatTextView::minimumSizeHint() const
{
	const QFontMetrics metrics(font());
	return {metrics.averageCharWidth() * 6, metrics.lineSpacing()};
}

void ChatTextView::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	doc_->setTextWidth(width());
	QAbstractTextDocumentLayout::PaintContext context;
	context.palette = palette();
	if (style_.text.isValid())
		context.palette.setColor(QPalette::Text, style_.text);
	context.clip = QRectF(rect());
	doc_->documentLayout()->draw(&painter, context);
}

void ChatTextView::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::FontChange)
		Rebuild();
}

// ---- QuestionEdit -------------------------------------------------------------------

QuestionEdit::QuestionEdit(QWidget *parent) : QPlainTextEdit(parent)
{
	setTabChangesFocus(true);
	setLineWrapMode(QPlainTextEdit::WidgetWidth);
	setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
		[this] { FitHeight(); });
	FitHeight();
}

void QuestionEdit::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
		const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
		if (modifiers == Qt::NoModifier) {
			event->accept();
			if (onSubmit)
				onSubmit();
			return;
		}
		if (modifiers == Qt::ControlModifier || modifiers == Qt::ShiftModifier) {
			// A real line break (Shift+Enter would otherwise insert U+2028).
			event->accept();
			textCursor().insertText(QStringLiteral("\n"));
			ensureCursorVisible();
			return;
		}
	}
	QPlainTextEdit::keyPressEvent(event);
}

void QuestionEdit::showEvent(QShowEvent *event)
{
	QPlainTextEdit::showEvent(event);
	FitHeight();
}

void QuestionEdit::changeEvent(QEvent *event)
{
	QPlainTextEdit::changeEvent(event);
	if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
		FitHeight();
}

void QuestionEdit::FitHeight()
{
	// documentSize() of a plain text layout counts lines (wrapped ones too).
	const int lines = std::clamp(static_cast<int>(document()->documentLayout()->documentSize().height()),
				     kQuestionMinLines, kQuestionMaxLines);
	const QFontMetrics metrics(font());
	// Frame, padding and focus border: whatever the style puts around the
	// text, measured once the box has been laid out.
	int chrome = height() - viewport()->height();
	if (!isVisible() || chrome <= 0)
		chrome = 2 * frameWidth() + 10;
	const int target =
		lines * metrics.lineSpacing() + 2 * static_cast<int>(std::ceil(document()->documentMargin())) + chrome;
	if (target != height() || minimumHeight() != target)
		setFixedHeight(target);
}

// ---- SpinnerIcon --------------------------------------------------------------------

SpinnerIcon::SpinnerIcon(int size, QWidget *parent) : QWidget(parent), timer_(new QTimer(this))
{
	setFixedSize(size, size);
	timer_->setInterval(80);
	connect(timer_, &QTimer::timeout, this, [this] {
		angle_ = (angle_ + 30) % 360;
		update();
	});
}

void SpinnerIcon::SetColor(const QColor &color)
{
	icon_ = ui::Icon(ui::IconId::Loader, color);
	update();
}

void SpinnerIcon::Start()
{
	angle_ = 0;
	if (!ui::PrefersReducedMotion())
		timer_->start();
	update();
}

void SpinnerIcon::Stop()
{
	timer_->stop();
}

bool SpinnerIcon::Turning() const
{
	return timer_->isActive();
}

void SpinnerIcon::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.translate(width() / 2.0, height() / 2.0);
	painter.rotate(angle_);
	painter.translate(-width() / 2.0, -height() / 2.0);
	icon_.paint(&painter, rect());
}

// ---- Helpers ------------------------------------------------------------------------

namespace supportui {

QString T(const char *key)
{
	return QString::fromUtf8(Str(key));
}

void OpenWebPage(const QUrl &url)
{
	const std::string text = url.toString(QUrl::FullyEncoded).toStdString();
	if (!text.empty() && api::IsAllowedWebUrl(text))
		QDesktopServices::openUrl(url);
	else
		TRIGGLOW_LOG_WARN(kComponent, "refused to open a page outside the plugin's fixed list");
}

void OpenWebPage(const char *url)
{
	OpenWebPage(QUrl(QString::fromUtf8(url)));
}

void FitButtonRow(QBoxLayout *row, int width)
{
	if (!row)
		return;
	const QMargins margins = row->contentsMargins();
	int needed = margins.left() + margins.right();
	int shown = 0;
	for (int i = 0; i < row->count(); ++i) {
		QWidget *w = row->itemAt(i)->widget();
		// Skip what is hidden on purpose (not what simply isn't shown yet).
		if (!w || (w->isHidden() && w->testAttribute(Qt::WA_WState_ExplicitShowHide)))
			continue;
		needed += w->sizeHint().width();
		++shown;
	}
	needed += std::max(0, shown - 1) * row->spacing();
	row->setDirection(needed <= width ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
}

void Announce(QWidget *source, const QString &message, bool urgent)
{
	if (!source || message.isEmpty() || !QAccessible::isActive())
		return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	QAccessibleAnnouncementEvent event(source, message);
	event.setPoliteness(urgent ? QAccessible::AnnouncementPoliteness::Assertive
				   : QAccessible::AnnouncementPoliteness::Polite);
	QAccessible::updateAccessibility(&event);
#else
	// No announcement event before Qt 6.8: errors still raise an alert.
	if (urgent) {
		QAccessibleEvent event(source, QAccessible::Alert);
		QAccessible::updateAccessibility(&event);
	}
#endif
}

QDialog *OpenTextPreview(QWidget *parent, const ui::Theme &theme, const QString &title, const QString &note,
			 const QString &text)
{
	auto *dialog = new QDialog(parent);
	dialog->setObjectName(QStringLiteral("tgTextPreview"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(title);
	dialog->setStyleSheet(theme.styleSheet);

	auto *layout = new QVBoxLayout(dialog);
	layout->setContentsMargins(18, 16, 18, 14);
	layout->setSpacing(10);

	auto *noteRow = new QHBoxLayout();
	noteRow->setSpacing(8);
	auto *icon = new QLabel(dialog);
	icon->setFixedSize(16, 16);
	icon->setPixmap(ui::IconPixmap(ui::IconId::Shield, ui::ToQColor(theme.tokens.accentText), 16,
				       dialog->devicePixelRatioF()));
	auto *noteLabel = new QLabel(note, dialog);
	noteLabel->setWordWrap(true);
	noteLabel->setTextFormat(Qt::PlainText);
	noteLabel->setProperty("tgRole", QStringLiteral("muted"));
	noteRow->addWidget(icon, 0, Qt::AlignTop);
	noteRow->addWidget(noteLabel, 1);
	layout->addLayout(noteRow);

	// Plain text only: what is sent is never interpreted as markup.
	auto *box = new QPlainTextEdit(dialog);
	box->setReadOnly(true);
	box->setLineWrapMode(QPlainTextEdit::NoWrap);
	box->setProperty("tgMono", QStringLiteral("true"));
	box->setTabChangesFocus(true);
	box->setAccessibleName(title);
	box->setAccessibleDescription(note);
	box->setPlainText(text);
	layout->addWidget(box, 1);

	auto *buttons = new QHBoxLayout();
	buttons->addStretch(1);
	auto *close = new QPushButton(T("ReportDialog.Close"), dialog);
	close->setProperty("tgVariant", QStringLiteral("secondary"));
	close->setProperty("tgSize", QStringLiteral("large"));
	close->setDefault(true);
	ui::ApplyFont(close, ui::FontRole::Button, theme.basePt);
	QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
	buttons->addWidget(close);
	layout->addLayout(buttons);

	dialog->resize(680, 480);
	dialog->open();
	close->setFocus();
	return dialog;
}

} // namespace supportui

} // namespace trigglow
