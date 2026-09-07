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

#include <QCoreApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
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
// does against the same API (apps/api/src/routes/support.ts in the main
// streampulse monorepo): create the ticket, then upload one attachment as a
// second call -- not atomic server-side, so a successful ticket with a
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
	// "dynamic_delay" is one of the categories apps/api/src/routes/support.ts already allows for
	// public/guest tickets (publicCategorySchema) -- no backend change needed for this plugin to
	// use it.
	obs_data_set_string(body, "category", "dynamic_delay");
	obs_data_set_string(body, "message", message.c_str());
	std::string jsonBody = obs_data_get_json(body);
	obs_data_release(body);

	HttpResult createResult = HttpsPostJson(kApiHost, L"/api/support/public/tickets", jsonBody);
	if (!createResult.ok || createResult.statusCode != 201) {
		if (!createResult.ok) {
			result.error = createResult.error.empty() ? Str("ReportDialog.ErrorNetwork")
								  : createResult.error;
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
		result.error = Str("ReportDialog.ErrorMissingFields");
		obs_data_release(responseData);
		return result;
	}
	result.ticketCreated = true;
	result.ticketId = ticketId;
	result.accessToken = accessToken;
	obs_data_release(responseData);

	TRIGGLOW_LOG_INFO(kComponent, "support ticket created: %s", result.ticketId.c_str());

	if (hasLog) {
		// ticketId/accessToken are both server-generated ASCII (a UUID and a
		// hex token respectively -- see support.ts), so this byte-for-byte
		// widening is safe; nothing here ever contains non-ASCII input.
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

void ReportBugDialog::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(14, 14, 14, 14);
	root->setSpacing(8);

	auto *intro = new QLabel(T("ReportDialog.Intro"), this);
	intro->setWordWrap(true);
	root->addWidget(intro);

	auto *nameRow = new QHBoxLayout();
	nameRow->addWidget(new QLabel(T("ReportDialog.NameLabel"), this));
	nameEdit_ = new QLineEdit(this);
	nameRow->addWidget(nameEdit_, /*stretch=*/1);
	root->addLayout(nameRow);

	auto *emailRow = new QHBoxLayout();
	emailRow->addWidget(new QLabel(T("ReportDialog.EmailLabel"), this));
	emailEdit_ = new QLineEdit(this);
	emailEdit_->setPlaceholderText(T("ReportDialog.EmailPlaceholder"));
	emailRow->addWidget(emailEdit_, /*stretch=*/1);
	root->addLayout(emailRow);

	messageEdit_ = new QPlainTextEdit(this);
	messageEdit_->setPlaceholderText(T("ReportDialog.MessagePlaceholder"));
	messageEdit_->setFixedHeight(90);
	root->addWidget(messageEdit_);

	logStatusLabel_ = new QLabel(this);
	logStatusLabel_->setWordWrap(true);
	if (logFound_) {
		logStatusLabel_->setText(T("ReportDialog.LogFound"));
		logStatusLabel_->setStyleSheet("color: #2e9e44; font-size: 10px;");
	} else {
		logStatusLabel_->setText(T("ReportDialog.LogNotFound"));
		logStatusLabel_->setStyleSheet("color: #d8a400; font-size: 10px;");
	}
	root->addWidget(logStatusLabel_);

	// Hidden until ShowError() has something to say -- kept as its own
	// label (not reusing logStatusLabel_) so a submit error never overwrites
	// the log-found/not-found information above it.
	statusLabel_ = new QLabel(this);
	statusLabel_->setWordWrap(true);
	statusLabel_->setStyleSheet("color: #c0392b; font-size: 11px;");
	statusLabel_->setVisible(false);
	root->addWidget(statusLabel_);

	auto *buttonRow = new QHBoxLayout();
	buttonRow->addStretch(1);
	cancelButton_ = new QPushButton(T("ReportDialog.Cancel"), this);
	submitButton_ = new QPushButton(T("ReportDialog.Submit"), this);
	submitButton_->setDefault(true);
	buttonRow->addWidget(cancelButton_);
	buttonRow->addWidget(submitButton_);
	root->addLayout(buttonRow);

	connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
	connect(submitButton_, &QPushButton::clicked, this, [this] { OnSubmitClicked(); });

	setMinimumWidth(380);
}

void ReportBugDialog::OnSubmitClicked()
{
	QString name = nameEdit_->text().trimmed();
	QString email = emailEdit_->text().trimmed();
	QString message = messageEdit_->toPlainText().trimmed();

	if (name.size() < 2) {
		ShowError(T("ReportDialog.ErrorNameTooShort"));
		return;
	}
	// Deliberately simple (not RFC 5322): matches this dialog's only real
	// goal, catching an obviously-mistyped address before it hits the
	// server's own (stricter) validation, not being a full email validator.
	static const QRegularExpression kEmailPattern(QStringLiteral(R"(^[^\s@]+@[^\s@]+\.[^\s@]+$)"));
	if (!kEmailPattern.match(email).hasMatch()) {
		ShowError(T("ReportDialog.ErrorInvalidEmail"));
		return;
	}
	if (message.size() < 5) {
		ShowError(T("ReportDialog.ErrorMessageTooShort"));
		return;
	}

	statusLabel_->setVisible(false);
	SetBusy(true);

	std::string logBytes;
	bool haveLogBytes = logFound_ && ReadFileBytes(logPath_.toUtf8().constData(), logBytes);
	if (logFound_ && !haveLogBytes)
		TRIGGLOW_LOG_WARN(kComponent, "found a copied log at %s but failed to read it back",
				  logPath_.toUtf8().constData());

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
	statusLabel_->setVisible(true);
}

} // namespace trigglow
