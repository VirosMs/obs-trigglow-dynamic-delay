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

#include "report-bug-dialog.hpp"
#include "api-client.hpp"
#include "auth-manager.hpp"
#include "bug-report.hpp"
#include "i18n.hpp"
#include "logging.hpp"
#include "plugin-support.h"
#include "win-http.hpp"

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>

namespace trigglow {

namespace {
constexpr const char *kComponent = "report-bug-dialog";

// Widths (logical px). The dialog opens at kDefaultWidth and can shrink to
// kMinimumWidth; Name | Email stack below kTwoColumnWidth, and the browser
// note moves above the buttons below kFooterRowWidth (it would wrap word by
// word next to them).
constexpr int kMinimumWidth = 340;
constexpr int kDefaultWidth = 460;
constexpr int kTwoColumnWidth = 400;
constexpr int kFooterRowWidth = 440;

// Str() (i18n.hpp) returns const char*/UTF-8 -- every user-facing string in
// this file goes through this tiny wrapper instead of a bare
// QString::fromUtf8(Str(...)) at each call site.
QString T(const char *key)
{
	return QString::fromUtf8(Str(key));
}

// Only the plugin's fixed pages (api-client.hpp) are ever opened.
void OpenAllowed(const QUrl &url)
{
	const std::string text = url.toString(QUrl::FullyEncoded).toStdString();
	if (api::IsAllowedWebUrl(text))
		QDesktopServices::openUrl(url);
	else
		TRIGGLOW_LOG_WARN(kComponent, "refused to open a page outside the plugin's fixed list");
}

} // namespace

ReportBugDialog::ReportBugDialog(AuthManager &auth, SupportService::DiagnosticsProvider diagnostics, QWidget *parent)
	: QDialog(parent),
	  auth_(auth)
{
	setWindowTitle(T("ReportDialog.Title"));
	setModal(true);

	service_ = new SupportService(auth_, std::move(diagnostics), this);
	if (!http::Available())
		mode_ = Mode::Unavailable;
	else if (service_->CanSubmitSignedIn())
		mode_ = Mode::SignedIn;
	else
		mode_ = Mode::Guest;
	footer_ = service_->TicketFooter();
	logLocatable_ = CanLocateObsLog();

	BuildUi();

	if (mode_ == Mode::Guest && auth_.IsLoggedIn())
		nameEdit_->setText(QString::fromStdString(auth_.DisplayName()));

	if (mode_ == Mode::Unavailable) {
		ShowInfo(T("ReportDialog.Unavailable"), false, true, false);
		DisableForm();
	} else if (mode_ == Mode::Guest) {
		// The public form may need a human check the plugin can't show: ask
		// first, and degrade to the web form instead of failing at the end.
		QPointer<ReportBugDialog> self(this);
		service_->CheckGuestForm([self](SupportService::GuestForm form) {
			if (!self || form != SupportService::GuestForm::WebOnly)
				return;
			self->ShowInfo(T("ReportDialog.WebOnly"), true, true, self->logLocatable_);
			self->DisableForm();
		});
	}
}

namespace {

// Field label above its control (OBS font 600), with the accent asterisk of a
// required field, tied to the control for screen readers.
QWidget *MakeFieldLabel(const QString &text, QWidget *buddy, QWidget *parent)
{
	auto *row = new QWidget(parent);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(3);
	auto *label = new QLabel(text, row);
	label->setProperty("tgRole", QStringLiteral("strong"));
	label->setBuddy(buddy);
	auto *required = new QLabel(QStringLiteral("*"), row);
	required->setProperty("tgTone", QStringLiteral("accent"));
	layout->addWidget(label);
	layout->addWidget(required);
	layout->addStretch(1);
	return row;
}

// Hidden error line under a field: alert icon + text, both in the danger tone.
// Returns the text label; its parent row is what gets shown and hidden.
QLabel *MakeFieldError(QWidget *parent)
{
	auto *row = new QWidget(parent);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);
	auto *icon = new QLabel(row);
	icon->setObjectName(QStringLiteral("tgFieldErrorIcon"));
	icon->setFixedSize(14, 14);
	auto *label = new QLabel(row);
	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	label->setProperty("tgTone", QStringLiteral("danger"));
	layout->addWidget(icon, 0, Qt::AlignTop);
	layout->addWidget(label, 1);
	row->setVisible(false);
	return label;
}

// Mono placeholders, like the web's support form: the empty field switches
// font, since Qt has no way to style the placeholder alone.
void TrackEmpty(QWidget *field, bool empty)
{
	ui::SetStyleProperty(field, "tgEmpty", empty ? QStringLiteral("true") : QStringLiteral("false"));
}

QPushButton *MakeSecondaryButton(const QString &text, QWidget *parent)
{
	auto *button = new QPushButton(text, parent);
	button->setProperty("tgVariant", QStringLiteral("secondary"));
	button->setAutoDefault(false);
	return button;
}

} // namespace

void ReportBugDialog::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	// --- Header (the web's SpModal): eyebrow, title, one-line intro. ---
	auto *header = new QWidget(this);
	{
		auto *layout = new QVBoxLayout(header);
		layout->setContentsMargins(18, 16, 18, 14);
		layout->setSpacing(4);
		auto *eyebrow = new QLabel(T("ReportDialog.Eyebrow"), header);
		eyebrow->setProperty("tgRole", QStringLiteral("eyebrow"));
		eyebrow->setProperty("tgTone", QStringLiteral("accent"));
		auto *title = new QLabel(T("ReportDialog.Title"), header);
		title->setProperty("tgRole", QStringLiteral("title"));
		title->setWordWrap(true);
		auto *intro = new QLabel(T("ReportDialog.Intro"), header);
		intro->setProperty("tgRole", QStringLiteral("muted"));
		intro->setWordWrap(true);
		layout->addWidget(eyebrow);
		layout->addWidget(title);
		layout->addWidget(intro);
	}
	root->addWidget(header);
	auto *headerLine = new QFrame(this);
	headerLine->setProperty("tgDivider", QStringLiteral("true"));
	headerLine->setFixedHeight(1);
	root->addWidget(headerLine);

	// --- Body. ---
	auto *body = new QVBoxLayout();
	body->setContentsMargins(18, 14, 18, 14);
	body->setSpacing(6);

	// Network/server errors: a banner above the fields, hidden until needed.
	errorBanner_ = new QFrame(this);
	errorBanner_->setProperty("tgBanner", QStringLiteral("danger"));
	{
		auto *layout = new QHBoxLayout(errorBanner_);
		layout->setContentsMargins(10, 8, 10, 8);
		layout->setSpacing(8);
		errorBannerIcon_ = new QLabel(errorBanner_);
		errorBannerIcon_->setFixedSize(16, 16);
		statusLabel_ = new QLabel(errorBanner_);
		statusLabel_->setWordWrap(true);
		statusLabel_->setTextFormat(Qt::PlainText);
		statusLabel_->setProperty("tgTone", QStringLiteral("danger"));
		layout->addWidget(errorBannerIcon_, 0, Qt::AlignTop);
		layout->addWidget(statusLabel_, 1);
	}
	errorBanner_->setVisible(false);
	body->addWidget(errorBanner_);

	// Info panel: the web-only guest form, no HTTP client, or a ticket
	// created without its log. Text plus the buttons that apply.
	infoPanel_ = new QFrame(this);
	infoPanel_->setProperty("tgCard", QStringLiteral("inset"));
	{
		auto *layout = new QVBoxLayout(infoPanel_);
		layout->setContentsMargins(10, 8, 10, 8);
		layout->setSpacing(8);
		auto *textRow = new QHBoxLayout();
		textRow->setSpacing(8);
		infoIcon_ = new QLabel(infoPanel_);
		infoIcon_->setFixedSize(16, 16);
		infoLabel_ = new QLabel(infoPanel_);
		infoLabel_->setWordWrap(true);
		infoLabel_->setTextFormat(Qt::PlainText);
		textRow->addWidget(infoIcon_, 0, Qt::AlignTop);
		textRow->addWidget(infoLabel_, 1);
		layout->addLayout(textRow);
		auto *buttons = new QHBoxLayout();
		buttons->setSpacing(8);
		signInButton_ = MakeSecondaryButton(T("ReportDialog.SignIn"), infoPanel_);
		openWebButton_ = MakeSecondaryButton(T("ReportDialog.OpenWebSupport"), infoPanel_);
		saveLogButton_ = MakeSecondaryButton(T("ReportDialog.SaveLog"), infoPanel_);
		buttons->addWidget(signInButton_);
		buttons->addWidget(openWebButton_);
		buttons->addWidget(saveLogButton_);
		buttons->addStretch(1);
		layout->addLayout(buttons);
	}
	infoPanel_->setVisible(false);
	body->addWidget(infoPanel_);
	body->addSpacing(2);

	nameEdit_ = new QLineEdit(this);
	nameEdit_->setAccessibleName(T("ReportDialog.NameLabel"));
	emailEdit_ = new QLineEdit(this);
	emailEdit_->setPlaceholderText(T("ReportDialog.EmailPlaceholder"));
	emailEdit_->setAccessibleName(T("ReportDialog.EmailLabel"));
	emailEdit_->setAccessibleDescription(T("ReportDialog.EmailHelp"));
	nameError_ = MakeFieldError(this);
	emailError_ = MakeFieldError(this);
	// The email's help goes under the email field; while the email shows an
	// error, the error takes its place (ShowFieldError/ClearErrors).
	emailHelp_ = new QLabel(T("ReportDialog.EmailHelp"), this);
	emailHelp_->setProperty("tgRole", QStringLiteral("hint"));
	emailHelp_->setWordWrap(true);

	// Name | Email in two columns from kTwoColumnWidth, stacked below
	// (resizeEvent). Only for guests: a signed-in ticket comes from the
	// account itself.
	identityBox_ = new QWidget(this);
	identityRow_ = new QBoxLayout(QBoxLayout::LeftToRight, identityBox_);
	identityRow_->setContentsMargins(0, 0, 0, 8);
	identityRow_->setSpacing(12);
	{
		auto *nameColumn = new QVBoxLayout();
		nameColumn->setSpacing(5);
		nameColumn->addWidget(MakeFieldLabel(T("ReportDialog.NameLabel"), nameEdit_, this));
		nameColumn->addWidget(nameEdit_);
		nameColumn->addWidget(nameError_->parentWidget());
		nameColumn->addStretch(1);
		auto *emailColumn = new QVBoxLayout();
		emailColumn->setSpacing(5);
		emailColumn->addWidget(MakeFieldLabel(T("ReportDialog.EmailLabel"), emailEdit_, this));
		emailColumn->addWidget(emailEdit_);
		emailColumn->addWidget(emailError_->parentWidget());
		emailColumn->addWidget(emailHelp_);
		emailColumn->addStretch(1);
		identityRow_->addLayout(nameColumn, 1);
		identityRow_->addLayout(emailColumn, 1);
	}
	identityBox_->setVisible(mode_ != Mode::SignedIn);
	body->addWidget(identityBox_);

	messageEdit_ = new QPlainTextEdit(this);
	messageEdit_->setPlaceholderText(T("ReportDialog.MessagePlaceholder"));
	messageEdit_->setAccessibleName(T("ReportDialog.MessageLabel"));
	messageEdit_->setFixedHeight(110);
	messageEdit_->setTabChangesFocus(true);
	// The version footer goes in the message, visible and editable; the
	// cursor starts above it.
	messageEdit_->setPlainText(footer_);
	{
		QTextCursor cursor = messageEdit_->textCursor();
		cursor.movePosition(QTextCursor::Start);
		messageEdit_->setTextCursor(cursor);
	}
	messageError_ = MakeFieldError(this);
	body->addWidget(MakeFieldLabel(T("ReportDialog.MessageLabel"), messageEdit_, this));
	body->addWidget(messageEdit_);
	body->addWidget(messageError_->parentWidget());

	// Attachment row: ticked when a log can be found; what is hidden in it,
	// and "See what is sent" with the redacted text.
	body->addSpacing(8);
	auto *attachment = new QFrame(this);
	attachment->setProperty("tgCard", QStringLiteral("inset"));
	{
		auto *layout = new QHBoxLayout(attachment);
		layout->setContentsMargins(10, 8, 10, 8);
		layout->setSpacing(10);
		logIcon_ = new QLabel(attachment);
		logIcon_->setFixedSize(16, 16);
		auto *texts = new QVBoxLayout();
		texts->setSpacing(3);
		attachCheck_ = new QCheckBox(T("ReportDialog.AttachLog"), attachment);
		attachCheck_->setChecked(logLocatable_);
		attachCheck_->setEnabled(logLocatable_);
		logStatusLabel_ = new QLabel(attachment);
		logStatusLabel_->setWordWrap(true);
		logStatusLabel_->setTextFormat(Qt::PlainText);
		if (logLocatable_) {
			logStatusLabel_->setText(T("ReportDialog.LogRedacted"));
			logStatusLabel_->setProperty("tgRole", QStringLiteral("hint"));
		} else {
			logStatusLabel_->setText(T("ReportDialog.LogNotFound"));
			logStatusLabel_->setProperty("tgTone", QStringLiteral("warning"));
		}
		texts->addWidget(attachCheck_);
		texts->addWidget(logStatusLabel_);
		layout->addWidget(logIcon_, 0, Qt::AlignTop);
		layout->addLayout(texts, 1);
		previewButton_ = MakeSecondaryButton(T("ReportDialog.SeeWhatIsSent"), attachment);
		previewButton_->setVisible(logLocatable_);
		layout->addWidget(previewButton_, 0, Qt::AlignTop);
	}
	body->addWidget(attachment);
	root->addLayout(body);

	// --- Footer: "opens the browser" note, Cancel, Send report. ---
	auto *footerLine = new QFrame(this);
	footerLine->setProperty("tgDivider", QStringLiteral("true"));
	footerLine->setFixedHeight(1);
	root->addWidget(footerLine);
	// The note and the buttons side by side from kFooterRowWidth; below it the
	// note goes on its own line above the buttons (resizeEvent).
	footerRow_ = new QBoxLayout(QBoxLayout::LeftToRight);
	footerRow_->setContentsMargins(18, 12, 18, 14);
	footerRow_->setSpacing(8);
	opensBrowserIcon_ = new QLabel(this);
	opensBrowserIcon_->setFixedSize(14, 14);
	auto *opensBrowser = new QLabel(T("ReportDialog.OpensBrowser"), this);
	opensBrowser->setProperty("tgRole", QStringLiteral("hint"));
	opensBrowser->setWordWrap(true);
	cancelButton_ = new QPushButton(T("ReportDialog.Cancel"), this);
	cancelButton_->setProperty("tgVariant", QStringLiteral("secondary"));
	cancelButton_->setProperty("tgSize", QStringLiteral("large"));
	cancelButton_->setAutoDefault(false);
	submitButton_ = new QPushButton(T("ReportDialog.Submit"), this);
	submitButton_->setProperty("tgVariant", QStringLiteral("primary"));
	submitButton_->setDefault(true);
	auto *noteRow = new QHBoxLayout();
	noteRow->setSpacing(8);
	noteRow->addWidget(opensBrowserIcon_, 0, Qt::AlignVCenter);
	noteRow->addWidget(opensBrowser, 1);
	auto *buttons = new QHBoxLayout();
	buttons->setSpacing(8);
	buttons->addStretch(1);
	buttons->addWidget(cancelButton_);
	buttons->addWidget(submitButton_);
	footerRow_->addLayout(noteRow, 1);
	footerRow_->addLayout(buttons, 0);
	root->addLayout(footerRow_);

	connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
	connect(submitButton_, &QPushButton::clicked, this, [this] {
		if (finished_)
			accept();
		else
			OnSubmitClicked();
	});
	connect(previewButton_, &QPushButton::clicked, this, [this] { ShowLogPreview(); });
	connect(saveLogButton_, &QPushButton::clicked, this, [this] { SaveLog(); });
	connect(openWebButton_, &QPushButton::clicked, this,
		[] { OpenAllowed(QUrl(QString::fromUtf8(api::web::kPublicSupport))); });
	connect(signInButton_, &QPushButton::clicked, this, [this] {
		// The sign-in itself happens in the dock (code + browser).
		if (!auth_.IsLoggedIn())
			auth_.StartLogin();
		reject();
	});
	connect(nameEdit_, &QLineEdit::textChanged, this,
		[this](const QString &text) { TrackEmpty(nameEdit_, text.isEmpty()); });
	connect(emailEdit_, &QLineEdit::textChanged, this,
		[this](const QString &text) { TrackEmpty(emailEdit_, text.isEmpty()); });
	connect(messageEdit_, &QPlainTextEdit::textChanged, this,
		[this] { TrackEmpty(messageEdit_, messageEdit_->document()->isEmpty()); });
	TrackEmpty(nameEdit_, true);
	TrackEmpty(emailEdit_, true);
	TrackEmpty(messageEdit_, messageEdit_->document()->isEmpty());

	// Narrow enough for large OBS font sizes and small screens, where the
	// rows reflow (resizeEvent); it still opens at a comfortable width.
	setMinimumWidth(kMinimumWidth);
	ApplyTheme();
	ApplyWidthLayout(kDefaultWidth);
	resize(kDefaultWidth,
	       layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(kDefaultWidth) : sizeHint().height());
	messageEdit_->setFocus();
}

void ReportBugDialog::ApplyWidthLayout(int width)
{
	if (identityRow_)
		identityRow_->setDirection(width >= kTwoColumnWidth ? QBoxLayout::LeftToRight
								    : QBoxLayout::TopToBottom);
	if (footerRow_)
		footerRow_->setDirection(width >= kFooterRowWidth ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
}

void ReportBugDialog::ApplyTheme()
{
	const double basePt = ui::BaseFontPointSize(this);
	theme_ = ui::BuildTheme(QApplication::palette(), basePt);
	setStyleSheet(theme_.styleSheet);
	// Capitalization and letter spacing (which QSS can't set), on top of the
	// family/size/weight the style sheet gives each role.
	for (QLabel *label : findChildren<QLabel *>()) {
		const QString role = label->property("tgRole").toString();
		if (role == QLatin1String("eyebrow"))
			ui::ApplyFont(label, ui::FontRole::Eyebrow, basePt);
		else if (role == QLatin1String("tag"))
			ui::ApplyFont(label, ui::FontRole::Tag, basePt);
		else if (role == QLatin1String("title"))
			ui::ApplyFont(label, ui::FontRole::Title, basePt);
	}
	for (QPushButton *button : findChildren<QPushButton *>())
		ui::ApplyFont(button, ui::FontRole::Button, basePt);

	const uitokens::Tokens &t = theme_.tokens;
	const qreal dpr = devicePixelRatioF();
	errorBannerIcon_->setPixmap(ui::IconPixmap(ui::IconId::AlertTriangle, ui::ToQColor(t.dangerText), 16, dpr));
	infoIcon_->setPixmap(ui::IconPixmap(ui::IconId::Info, ui::ToQColor(t.accentText), 16, dpr));
	logIcon_->setPixmap(ui::IconPixmap(logLocatable_ ? ui::IconId::Paperclip : ui::IconId::AlertTriangle,
					   ui::ToQColor(logLocatable_ ? t.accentText : t.warningText), 16, dpr));
	opensBrowserIcon_->setPixmap(ui::IconPixmap(ui::IconId::ExternalLink, ui::ToQColor(t.textMuted), 14, dpr));
	openWebButton_->setIcon(ui::Icon(ui::IconId::ExternalLink, ui::ToQColor(t.text)));
	for (QLabel *icon : findChildren<QLabel *>(QStringLiteral("tgFieldErrorIcon")))
		icon->setPixmap(ui::IconPixmap(ui::IconId::AlertTriangle, ui::ToQColor(t.dangerText), 14, dpr));
}

void ReportBugDialog::resizeEvent(QResizeEvent *event)
{
	QDialog::resizeEvent(event);
	ApplyWidthLayout(event->size().width());
}

void ReportBugDialog::ShowFieldError(QWidget *field, QLabel *errorLabel, const QString &message)
{
	ui::SetStyleProperty(field, "tgInvalid", QStringLiteral("true"));
	errorLabel->setText(message);
	errorLabel->parentWidget()->setVisible(true);
	if (errorLabel == emailError_)
		emailHelp_->setVisible(false); // The error takes the help's place.
	field->setFocus();
	if (QAccessible::isActive()) {
		QAccessibleEvent event(errorLabel, QAccessible::Alert);
		QAccessible::updateAccessibility(&event);
	}
}

void ReportBugDialog::ClearErrors()
{
	for (QWidget *field : std::initializer_list<QWidget *>{nameEdit_, emailEdit_, messageEdit_})
		ui::SetStyleProperty(field, "tgInvalid", QStringLiteral("false"));
	for (QLabel *label : {nameError_, emailError_, messageError_})
		label->parentWidget()->setVisible(false);
	emailHelp_->setVisible(true);
	errorBanner_->setVisible(false);
}

QString ReportBugDialog::UserMessage() const
{
	QString message = messageEdit_->toPlainText();
	if (!footer_.isEmpty() && message.endsWith(footer_))
		message.chop(footer_.size());
	return message.trimmed();
}

void ReportBugDialog::OnSubmitClicked()
{
	ClearErrors();
	const QString name = nameEdit_->text().trimmed();
	const QString email = emailEdit_->text().trimmed();
	if (mode_ == Mode::Guest) {
		if (name.size() < 2) {
			ShowFieldError(nameEdit_, nameError_, T("ReportDialog.ErrorNameTooShort"));
			return;
		}
		// Deliberately simple (not RFC 5322): catches an obviously mistyped
		// address before the server's own (stricter) validation.
		static const QRegularExpression kEmailPattern(QStringLiteral(R"(^[^\s@]+@[^\s@]+\.[^\s@]+$)"));
		if (!kEmailPattern.match(email).hasMatch()) {
			ShowFieldError(emailEdit_, emailError_, T("ReportDialog.ErrorInvalidEmail"));
			return;
		}
	}
	if (UserMessage().size() < 5) {
		ShowFieldError(messageEdit_, messageError_, T("ReportDialog.ErrorMessageTooShort"));
		return;
	}

	SetBusy(true);
	const QString subject =
		QStringLiteral("Reporte desde el plugin de OBS (v%1)").arg(QString::fromUtf8(PLUGIN_VERSION));
	const QString message = messageEdit_->toPlainText().trimmed();
	const bool attachLog = attachCheck_->isChecked();
	QPointer<ReportBugDialog> self(this);
	auto done = [self](const SupportService::TicketOutcome &outcome) {
		if (self)
			self->OnTicketDone(outcome);
	};
	if (mode_ == Mode::SignedIn)
		service_->SubmitTicket(subject, message, true, attachLog, done);
	else
		service_->SubmitGuestTicket(name, email, subject, message, attachLog, done);
}

void ReportBugDialog::OnTicketDone(const SupportService::TicketOutcome &outcome)
{
	SetBusy(false);
	if (!outcome.created) {
		using support::Status;
		if (outcome.needsWebForm) {
			ShowInfo(T("ReportDialog.WebOnly"), true, true, logLocatable_);
			DisableForm();
			return;
		}
		switch (outcome.status) {
		case Status::SessionEnded:
		case Status::SignedOut:
			ShowError(T("ReportDialog.ErrorSessionEnded"));
			break;
		case Status::RateLimited:
			ShowError(T("ReportDialog.ErrorRateLimited"));
			break;
		case Status::Invalid:
			ShowError(T("ReportDialog.ErrorInvalid"));
			break;
		case Status::Network:
		case Status::Unavailable:
			ShowError(T("ReportDialog.ErrorNetwork"));
			break;
		default:
			ShowError(T("ReportDialog.ErrorInvalidResponse"));
			break;
		}
		return;
	}

	OpenAllowed(outcome.ticketUrl);
	if (outcome.attachmentTried && !outcome.attachmentSent) {
		// The ticket exists: say the log didn't make it and offer to save it.
		finished_ = true;
		DisableForm();
		ShowInfo(T("ReportDialog.AttachmentFailed"), false, false, true);
		submitButton_->setText(T("ReportDialog.Close"));
		submitButton_->setEnabled(true);
		return;
	}
	accept();
}

void ReportBugDialog::ShowInfo(const QString &message, bool signIn, bool openWeb, bool saveLog)
{
	infoLabel_->setText(message);
	signInButton_->setVisible(signIn && http::Available());
	openWebButton_->setVisible(openWeb);
	saveLogButton_->setVisible(saveLog);
	infoPanel_->setVisible(true);
	if (QAccessible::isActive()) {
		QAccessibleEvent event(infoLabel_, QAccessible::Alert);
		QAccessible::updateAccessibility(&event);
	}
}

void ReportBugDialog::DisableForm()
{
	for (QWidget *w : std::initializer_list<QWidget *>{nameEdit_, emailEdit_, messageEdit_, attachCheck_})
		w->setEnabled(false);
	submitButton_->setEnabled(finished_);
}

void ReportBugDialog::ShowLogPreview()
{
	previewButton_->setEnabled(false);
	QPointer<ReportBugDialog> self(this);
	service_->PrepareLog([self](const SupportService::PreparedLog &log) {
		if (!self)
			return;
		self->previewButton_->setEnabled(true);
		QDialog preview(self);
		preview.setWindowTitle(T("ReportDialog.PreviewTitle"));
		preview.setStyleSheet(self->theme_.styleSheet);
		auto *layout = new QVBoxLayout(&preview);
		auto *note = new QLabel(log.preview.truncated || log.capped ? T("ReportDialog.PreviewTruncated")
									    : T("ReportDialog.LogRedacted"),
					&preview);
		note->setWordWrap(true);
		note->setTextFormat(Qt::PlainText);
		note->setProperty("tgRole", QStringLiteral("hint"));
		// Plain text only: the log is never interpreted as markup.
		auto *text = new QPlainTextEdit(&preview);
		text->setReadOnly(true);
		text->setLineWrapMode(QPlainTextEdit::NoWrap);
		QString content = QString::fromUtf8(log.preview.head.c_str());
		if (log.preview.truncated)
			content += QStringLiteral("\n[...]\n\n") + QString::fromUtf8(log.preview.tail.c_str());
		text->setPlainText(log.found ? content : T("ReportDialog.LogNotFound"));
		auto *close = new QPushButton(T("ReportDialog.Close"), &preview);
		close->setProperty("tgVariant", QStringLiteral("secondary"));
		QObject::connect(close, &QPushButton::clicked, &preview, &QDialog::accept);
		layout->addWidget(note);
		layout->addWidget(text, 1);
		layout->addWidget(close, 0, Qt::AlignRight);
		preview.resize(680, 480);
		preview.exec();
	});
}

void ReportBugDialog::SaveLog()
{
	saveLogButton_->setEnabled(false);
	QPointer<ReportBugDialog> self(this);
	service_->PrepareLog([self](const SupportService::PreparedLog &log) {
		if (!self)
			return;
		self->saveLogButton_->setEnabled(true);
		if (!log.found || !log.text) {
			self->ShowError(T("ReportDialog.LogNotFound"));
			return;
		}
		const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
		const QString path = QFileDialog::getSaveFileName(self, T("ReportDialog.SaveLogTitle"),
								  dir + QStringLiteral("/obs-log.txt"),
								  QStringLiteral("*.txt"));
		if (path.isEmpty())
			return;
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
		    file.write(log.text->data(), static_cast<qint64>(log.text->size())) !=
			    static_cast<qint64>(log.text->size())) {
			self->ShowError(T("ReportDialog.SaveLogFailed"));
			return;
		}
		TRIGGLOW_LOG_INFO(kComponent, "saved the redacted OBS log where the user chose");
	});
}

void ReportBugDialog::SetBusy(bool busy)
{
	const bool guestFields = mode_ == Mode::Guest;
	nameEdit_->setEnabled(!busy && guestFields);
	emailEdit_->setEnabled(!busy && guestFields);
	messageEdit_->setEnabled(!busy);
	attachCheck_->setEnabled(!busy && logLocatable_);
	previewButton_->setEnabled(!busy);
	cancelButton_->setEnabled(!busy);
	submitButton_->setEnabled(!busy);
	submitButton_->setText(busy ? T("ReportDialog.Sending") : T("ReportDialog.Submit"));
}

void ReportBugDialog::ShowError(const QString &message)
{
	statusLabel_->setText(message);
	errorBanner_->setVisible(true);
	if (QAccessible::isActive()) {
		QAccessibleEvent event(statusLabel_, QAccessible::Alert);
		QAccessible::updateAccessibility(&event);
	}
}

} // namespace trigglow
