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

#include <functional>

#include <QColor>
#include <QIcon>
#include <QPlainTextEdit>
#include <QString>
#include <QUrl>
#include <QWidget>

#include "ui-theme.hpp"

class QBoxLayout;
class QDialog;
class QTextDocument;
class QTimer;

// Small widgets of the support dialog's Trigi page (trigi-page.hpp), plus the
// helpers the page and the ticket form share.
namespace trigglow {

// One line of a Trigi reply, or one message the user wrote. Unlike QLabel it
// wraps long unbroken runs anywhere (a reply can hold a 4000-character
// "word"), and its document never loads anything: no images, no links, no
// stylesheets from outside. Rich text ONLY comes from chat::LineHtml()
// (escaped segments in <b> and <span class="code">); see chat-text.hpp.
class ChatTextView : public QWidget {
	Q_OBJECT

public:
	struct Style {
		QColor text;
		QColor codeBackground;
		QString monoFamily; // Empty: the system's monospace font.
	};

	explicit ChatTextView(QWidget *parent = nullptr);

	// html: chat::LineHtml() of one reply line, and nothing else.
	void SetLineHtml(const QString &html);
	// What the user typed, as plain text.
	void SetPlainText(const QString &text);
	void SetStyle(const Style &style);
	QString PlainText() const;
	// The document, read-only (the harness checks it holds no link or image).
	const QTextDocument *Document() const;

	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	void Rebuild();

	QTextDocument *doc_ = nullptr;
	QString source_;
	bool rich_ = false;
	Style style_;
};

// The question box: Enter sends, Ctrl+Enter or Shift+Enter add a line, Tab
// moves on. Grows from 2 to 6 lines, then scrolls.
class QuestionEdit : public QPlainTextEdit {
	Q_OBJECT

public:
	explicit QuestionEdit(QWidget *parent = nullptr);
	std::function<void()> onSubmit;

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	void FitHeight();
};

// Trigi's "thinking" mark: the loader icon turning, or still when the system
// asks for less motion (ui::PrefersReducedMotion()).
class SpinnerIcon : public QWidget {
	Q_OBJECT

public:
	explicit SpinnerIcon(int size, QWidget *parent = nullptr);
	void SetColor(const QColor &color);
	void Start();
	void Stop();
	bool Turning() const;

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QIcon icon_;
	QTimer *timer_ = nullptr;
	int angle_ = 0;
};

namespace supportui {

// Str() as a QString.
QString T(const char *key);

// Opens one of the plugin's fixed pages (api-client.hpp, namespace web) or a
// URL SupportService handed over; anything else is refused and logged.
void OpenWebPage(const QUrl &url);
void OpenWebPage(const char *url);

// A row of buttons that stacks them when they don't fit side by side in
// `width` (large OBS fonts, long translations, narrow dialogs).
void FitButtonRow(QBoxLayout *row, int width);

// Screen readers: says `message` once (Qt 6.8+ announcement; an Alert event
// on older Qt when urgent). `source` is the widget it comes from.
void Announce(QWidget *source, const QString &message, bool urgent);

// Read-only preview of what is sent (the redacted log, the technical data) in
// its own window-modal dialog, opened without blocking (open(), deleted on
// close). Mono, no wrapping, plain text only.
QDialog *OpenTextPreview(QWidget *parent, const ui::Theme &theme, const QString &title, const QString &note,
			 const QString &text);

} // namespace supportui

} // namespace trigglow
