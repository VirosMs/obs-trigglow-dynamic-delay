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

#include <QDialog>
#include <QString>

#include "support-service.hpp"
#include "ui-theme.hpp"

class QBoxLayout;
class QCheckBox;
class QFrame;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;

// Small modal dialog for TrigglowDelayDock's "Reportar un problema" button:
// a short description (plus name and email when not signed in) submitted as
// a real support ticket on trigglow.com -- category "dynamic_delay" -- with
// OBS's current log attached if the user leaves it ticked, and the ticket
// opened in the browser afterwards.
//
// Since v0.6.0 every network call goes through SupportService
// (support-service.hpp), which picks the route:
//   - signed in (plugin token): the account's own ticket, no name or email to
//     type; it shows up in the user's support panel on trigglow.com;
//   - not signed in: the guest form. If trigglow.com asks guests for a human
//     check the plugin can't show, the form degrades to "sign in, or use the
//     web form" (plus "Save log") instead of failing;
//   - no HTTP client on this platform (macOS/Linux today): the form stays
//     disabled with a link to the web form.
// The log is redacted before it leaves the machine (log-redact.hpp), and
// "See what is sent" shows the redacted text.
namespace trigglow {

class AuthManager;

class ReportBugDialog : public QDialog {
	Q_OBJECT

public:
	// diagnostics: passed on to SupportService (the Trigi page's technical
	// data); the ticket form itself never sends it.
	ReportBugDialog(AuthManager &auth, SupportService::DiagnosticsProvider diagnostics, QWidget *parent = nullptr);

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	enum class Mode { SignedIn, Guest, Unavailable };

	void BuildUi();
	// Name | Email in two columns, and the browser note beside the buttons,
	// when `width` has room for them; stacked otherwise (see the widths in
	// report-bug-dialog.cpp).
	void ApplyWidthLayout(int width);
	// Same tokens/fonts/icons as the dock (ui-theme.hpp), so the dialog looks
	// the same whether or not it inherits the dock's style sheet.
	void ApplyTheme();
	void OnSubmitClicked();
	// Validation errors sit under their field (danger border + icon + text),
	// focus jumps to the first one; network/server errors go in a banner at
	// the top. Both are announced to screen readers.
	void ShowFieldError(QWidget *field, QLabel *errorLabel, const QString &message);
	void ClearErrors();
	// busy: true while a submission is in flight -- disables every input and
	// swaps submitButton_'s text, same spirit as accountButton_ showing
	// "Esperando..." during login in settings-ui.cpp.
	void SetBusy(bool busy);
	void ShowError(const QString &message); // Network/server error banner.
	// The info panel above the form (web-only guest form, no HTTP client, or
	// a ticket created without its log), with the buttons that apply.
	void ShowInfo(const QString &message, bool signIn, bool openWeb, bool saveLog);
	// No form: only the info panel's buttons (and Cancel/Close) remain.
	void DisableForm();
	void OnTicketDone(const SupportService::TicketOutcome &outcome);
	void ShowLogPreview();
	void SaveLog();
	// The message without the version footer the form prefills.
	QString UserMessage() const;

	AuthManager &auth_;
	SupportService *service_ = nullptr;
	Mode mode_ = Mode::Guest;
	QString footer_;
	bool logLocatable_ = false;
	bool finished_ = false; // The ticket exists: Submit now just closes.

	ui::Theme theme_;

	QWidget *identityBox_ = nullptr; // Name and email: hidden when signed in.
	QLineEdit *nameEdit_ = nullptr;
	QLineEdit *emailEdit_ = nullptr;
	QPlainTextEdit *messageEdit_ = nullptr;
	QLabel *nameError_ = nullptr;
	QLabel *emailError_ = nullptr;
	QLabel *messageError_ = nullptr;
	QLabel *emailHelp_ = nullptr;       // Under the email field; the email's error takes its place.
	QBoxLayout *identityRow_ = nullptr; // Name | Email side by side, stacked when narrow.
	QBoxLayout *footerRow_ = nullptr;   // Browser note | buttons; the note above them when narrow.
	QFrame *errorBanner_ = nullptr;
	QLabel *errorBannerIcon_ = nullptr;
	QLabel *statusLabel_ = nullptr; // Text of errorBanner_.
	QFrame *infoPanel_ = nullptr;
	QLabel *infoIcon_ = nullptr;
	QLabel *infoLabel_ = nullptr;
	QPushButton *signInButton_ = nullptr;
	QPushButton *openWebButton_ = nullptr;
	QPushButton *saveLogButton_ = nullptr;
	QLabel *logIcon_ = nullptr;
	QCheckBox *attachCheck_ = nullptr;
	QLabel *logStatusLabel_ = nullptr;
	QPushButton *previewButton_ = nullptr;
	QLabel *opensBrowserIcon_ = nullptr;
	QPushButton *submitButton_ = nullptr;
	QPushButton *cancelButton_ = nullptr;
};

} // namespace trigglow
