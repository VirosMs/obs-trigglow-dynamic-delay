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
#include "bug-report.hpp"
#include "i18n.hpp"
#include "logging.hpp"
#include "win-http.hpp"

#include <thread>

#include <QAccessible>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
}

namespace trigglow {

namespace {
constexpr const char *kComponent = "report-bug-dialog";
constexpr const wchar_t *kApiHost = L"api.trigglow.com"; // Matches auth-manager.cpp's kApiHost.
constexpr const char *kSupportWebOrigin = "https://www.trigglow.com";

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

struct SubmitResult {
	bool ticketCreated = false;
	std::string ticketId;
	std::string accessToken;
	bool attachmentUploaded = false; // Only meaningful when ticketCreated is true and a log was attempted.
	std::string error;               // Set only when ticketCreated is false.
};

// Runs on the background thread from OnSubmitClicked() below -- never call
// this from the UI thread, same BLOCKING contract win-http.hpp documents for
// every call it makes. Mirrors exactly what the web /support form itself
// does against the same API: create the ticket, then upload one attachment
// as a second call -- not atomic server-side, so a successful ticket with a
// failed attachment is a real, expected outcome this handles gracefully
// (attachmentUploaded=false, ticketCreated=true) rather than as an error.
SubmitResult SubmitTicket(const std::string &guestName, const std::string &guestEmail, const std::string &message,
			  const std::string &pluginVersion, bool hasLog, const std::string &logBytes)
{
	SubmitResult result;

	obs_data_t *body = obs_data_create();
	obs_data_set_string(body, "guestName", guestName.c_str());
	obs_data_set_string(body, "guestEmail", guestEmail.c_str());
	obs_data_set_string(body, "subject", ("Reporte desde el plugin de OBS (v" + pluginVersion + ")").c_str());
	// "dynamic_delay" is one of the support API's public ticket categories.
	obs_data_set_string(body, "category", "dynamic_delay");
	obs_data_set_string(body, "message", message.c_str());
	std::string jsonBody = obs_data_get_json(body);
	obs_data_release(body);

	HttpResult createResult = HttpsPostJson(kApiHost, L"/api/support/public/tickets", jsonBody);
	if (!createResult.ok || createResult.statusCode != 201) {
		if (!createResult.ok) {
			// The transport's own (technical, English) error goes to the
			// log; the user gets the next step instead.
			result.error = Str("ReportDialog.ErrorNetwork");
			if (!createResult.error.empty())
				TRIGGLOW_LOG_WARN(kComponent, "ticket creation transport error: %s",
						  createResult.error.c_str());
		} else {
			std::string tmpl = Str("ReportDialog.ErrorServer");
			size_t pos = tmpl.find("%1");
			if (pos != std::string::npos)
				tmpl.replace(pos, 2, std::to_string(createResult.statusCode));
			result.error = tmpl;
		}
		TRIGGLOW_LOG_WARN(kComponent, "ticket creation failed: %s", result.error.c_str());
		return result;
	}

	obs_data_t *responseData = obs_data_create_from_json(createResult.body.c_str());
	if (!responseData) {
		result.error = Str("ReportDialog.ErrorInvalidResponse");
		return result;
	}
	const char *ticketId = obs_data_get_string(responseData, "ticketId");
	const char *accessToken = obs_data_get_string(responseData, "accessToken");
	if (!ticketId || !*ticketId || !accessToken || !*accessToken) {
		TRIGGLOW_LOG_WARN(kComponent, "ticket creation response is missing ticketId/accessToken");
		result.error = Str("ReportDialog.ErrorInvalidResponse");
		obs_data_release(responseData);
		return result;
	}
	result.ticketCreated = true;
	result.ticketId = ticketId;
	result.accessToken = accessToken;
	obs_data_release(responseData);

	// The ticket ID alone grants nothing; the access token (and any URL that
	// carries it) must never reach the log.
	TRIGGLOW_LOG_INFO(kComponent, "support ticket created: %s", result.ticketId.c_str());

	if (hasLog) {
		// ticketId/accessToken are both server-generated ASCII (a UUID and a
		// hex token respectively), so this byte-for-byte widening is safe;
		// nothing here ever contains non-ASCII input.
		std::wstring ticketIdW(result.ticketId.begin(), result.ticketId.end());
		std::wstring accessTokenW(result.accessToken.begin(), result.accessToken.end());
		std::wstring path = L"/api/support/public/tickets/" + ticketIdW + L"/attachments?token=" + accessTokenW;
		HttpResult uploadResult =
			HttpsPostMultipartFile(kApiHost, path, "file", "obs-log.txt", "text/plain", logBytes);
		result.attachmentUploaded = uploadResult.ok && uploadResult.statusCode == 201;
		if (!result.attachmentUploaded) {
			TRIGGLOW_LOG_WARN(kComponent,
					  "attachment upload failed for ticket %s (ticket itself still created): "
					  "HTTP %d, %s",
					  result.ticketId.c_str(), uploadResult.statusCode, uploadResult.error.c_str());
		}
	}

	return result;
}

} // namespace

ReportBugDialog::ReportBugDialog(QString pluginVersion, QString defaultName, QString defaultEmail, QWidget *parent)
	: QDialog(parent),
	  pluginVersion_(std::move(pluginVersion))
{
	setWindowTitle(T("ReportDialog.Title"));
	setModal(true);

	// Computed once up front, not re-fetched at submit time, so the user
	// finds out right away whether a log was found -- before they've typed
	// their whole message -- rather than only at the end.
	BugReportLogResult logResult = CopyCurrentObsLogForSupport();
	logFound_ = logResult.found && !logResult.copiedPath.empty();
	logPath_ = QString::fromUtf8(logResult.copiedPath.c_str());

	BuildUi();

	if (!defaultName.isEmpty())
		nameEdit_->setText(defaultName);
	if (!defaultEmail.isEmpty())
		emailEdit_->setText(defaultEmail);
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
	body->addSpacing(2);

	nameEdit_ = new QLineEdit(this);
	nameEdit_->setAccessibleName(T("ReportDialog.NameLabel"));
	emailEdit_ = new QLineEdit(this);
	emailEdit_->setPlaceholderText(T("ReportDialog.EmailPlaceholder"));
	emailEdit_->setAccessibleName(T("ReportDialog.EmailLabel"));
	emailEdit_->setAccessibleDescription(T("ReportDialog.EmailHelp"));
	nameError_ = MakeFieldError(this);
	emailError_ = MakeFieldError(this);
	// The email's help goes under the email field (it used to sit under both
	// columns, i.e. under Name); while the email shows an error, the error
	// takes its place (ShowFieldError/ClearErrors).
	emailHelp_ = new QLabel(T("ReportDialog.EmailHelp"), this);
	emailHelp_->setProperty("tgRole", QStringLiteral("hint"));
	emailHelp_->setWordWrap(true);

	// Name | Email in two columns from kTwoColumnWidth, stacked below
	// (resizeEvent).
	identityRow_ = new QBoxLayout(QBoxLayout::LeftToRight);
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
	body->addLayout(identityRow_);

	body->addSpacing(8);
	messageEdit_ = new QPlainTextEdit(this);
	messageEdit_->setPlaceholderText(T("ReportDialog.MessagePlaceholder"));
	messageEdit_->setAccessibleName(T("ReportDialog.MessageLabel"));
	messageEdit_->setFixedHeight(92);
	messageEdit_->setTabChangesFocus(true);
	messageError_ = MakeFieldError(this);
	body->addWidget(MakeFieldLabel(T("ReportDialog.MessageLabel"), messageEdit_, this));
	body->addWidget(messageEdit_);
	body->addWidget(messageError_->parentWidget());

	// Attachment row: what goes with the ticket, known before typing.
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
		texts->setSpacing(1);
		logStatusLabel_ = new QLabel(attachment);
		logStatusLabel_->setWordWrap(true);
		if (logFound_) {
			auto *file = new QLabel(QStringLiteral("obs-log.txt"), attachment);
			file->setProperty("tgRole", QStringLiteral("data"));
			texts->addWidget(file);
			logStatusLabel_->setText(T("ReportDialog.LogFound"));
			logStatusLabel_->setProperty("tgRole", QStringLiteral("hint"));
		} else {
			logStatusLabel_->setText(T("ReportDialog.LogNotFound"));
			logStatusLabel_->setProperty("tgTone", QStringLiteral("warning"));
		}
		texts->addWidget(logStatusLabel_);
		layout->addWidget(logIcon_, 0, Qt::AlignVCenter);
		layout->addLayout(texts, 1);
		if (logFound_) {
			auto *chip = new QLabel(T("ReportDialog.LogChip"), attachment);
			chip->setProperty("tgRole", QStringLiteral("tag"));
			chip->setProperty("tgTone", QStringLiteral("accent"));
			layout->addWidget(chip, 0, Qt::AlignVCenter);
		}
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
	connect(submitButton_, &QPushButton::clicked, this, [this] { OnSubmitClicked(); });
	connect(nameEdit_, &QLineEdit::textChanged, this,
		[this](const QString &text) { TrackEmpty(nameEdit_, text.isEmpty()); });
	connect(emailEdit_, &QLineEdit::textChanged, this,
		[this](const QString &text) { TrackEmpty(emailEdit_, text.isEmpty()); });
	connect(messageEdit_, &QPlainTextEdit::textChanged, this,
		[this] { TrackEmpty(messageEdit_, messageEdit_->document()->isEmpty()); });
	TrackEmpty(nameEdit_, true);
	TrackEmpty(emailEdit_, true);
	TrackEmpty(messageEdit_, true);

	// Narrow enough for large OBS font sizes and small screens, where the
	// rows reflow (resizeEvent); it still opens at a comfortable width.
	setMinimumWidth(kMinimumWidth);
	ApplyTheme();
	ApplyWidthLayout(kDefaultWidth);
	resize(kDefaultWidth,
	       layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(kDefaultWidth) : sizeHint().height());
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
	for (QPushButton *button : {cancelButton_, submitButton_})
		ui::ApplyFont(button, ui::FontRole::Button, basePt);

	const uitokens::Tokens &t = theme_.tokens;
	const qreal dpr = devicePixelRatioF();
	errorBannerIcon_->setPixmap(ui::IconPixmap(ui::IconId::AlertTriangle, ui::ToQColor(t.dangerText), 16, dpr));
	logIcon_->setPixmap(ui::IconPixmap(logFound_ ? ui::IconId::Paperclip : ui::IconId::AlertTriangle,
					   ui::ToQColor(logFound_ ? t.accentText : t.warningText), 16, dpr));
	opensBrowserIcon_->setPixmap(ui::IconPixmap(ui::IconId::ExternalLink, ui::ToQColor(t.textMuted), 14, dpr));
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

void ReportBugDialog::OnSubmitClicked()
{
	QString name = nameEdit_->text().trimmed();
	QString email = emailEdit_->text().trimmed();
	QString message = messageEdit_->toPlainText().trimmed();

	ClearErrors();
	if (name.size() < 2) {
		ShowFieldError(nameEdit_, nameError_, T("ReportDialog.ErrorNameTooShort"));
		return;
	}
	// Deliberately simple (not RFC 5322): matches this dialog's only real
	// goal, catching an obviously-mistyped address before it hits the
	// server's own (stricter) validation, not being a full email validator.
	static const QRegularExpression kEmailPattern(QStringLiteral(R"(^[^\s@]+@[^\s@]+\.[^\s@]+$)"));
	if (!kEmailPattern.match(email).hasMatch()) {
		ShowFieldError(emailEdit_, emailError_, T("ReportDialog.ErrorInvalidEmail"));
		return;
	}
	if (message.size() < 5) {
		ShowFieldError(messageEdit_, messageError_, T("ReportDialog.ErrorMessageTooShort"));
		return;
	}

	SetBusy(true);

	std::string logBytes;
	bool haveLogBytes = logFound_ && ReadFileBytes(logPath_.toUtf8().constData(), logBytes);
	if (logFound_ && !haveLogBytes)
		TRIGGLOW_LOG_WARN(kComponent, "found a copied log (%%TEMP%%\\Trigglow\\%s) but failed to read it back",
				  QFileInfo(logPath_).fileName().toUtf8().constData());

	std::string nameUtf8 = name.toUtf8().constData();
	std::string emailUtf8 = email.toUtf8().constData();
	std::string messageUtf8 = message.toUtf8().constData();
	std::string versionUtf8 = pluginVersion_.toUtf8().constData();

	QPointer<ReportBugDialog> self(this);
	std::thread([nameUtf8, emailUtf8, messageUtf8, versionUtf8, haveLogBytes, logBytes, self]() {
		SubmitResult result =
			SubmitTicket(nameUtf8, emailUtf8, messageUtf8, versionUtf8, haveLogBytes, logBytes);
		QMetaObject::invokeMethod(
			qApp,
			[result, self]() {
				if (!self)
					return; // Dialog closed/destroyed before this result arrived.

				self->SetBusy(false);

				if (!result.ticketCreated) {
					self->ShowError(QString::fromUtf8(result.error.c_str()));
					return;
				}

				QUrl url(QString::fromUtf8(kSupportWebOrigin) + QStringLiteral("/support"));
				QUrlQuery query;
				query.addQueryItem(QStringLiteral("ticket"),
						   QString::fromUtf8(result.ticketId.c_str()));
				query.addQueryItem(QStringLiteral("token"),
						   QString::fromUtf8(result.accessToken.c_str()));
				url.setQuery(query);
				QDesktopServices::openUrl(url);

				// Ticket exists regardless of whether the attachment made
				// it -- close the dialog either way; the warning already
				// went to the OBS log for us to diagnose later if needed.
				self->accept();
			},
			Qt::QueuedConnection);
	}).detach();
}

void ReportBugDialog::SetBusy(bool busy)
{
	nameEdit_->setEnabled(!busy);
	emailEdit_->setEnabled(!busy);
	messageEdit_->setEnabled(!busy);
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
