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

#include "ui-theme.hpp"

class QBoxLayout;
class QFrame;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;

// Small modal dialog for TrigglowDelayDock's "Reportar un problema" button.
// Replaces the earlier "just copy the log and open the browser" flow: this
// collects a short description (name/email/message) and submits a real
// support ticket directly to trigglow.com -- category "dynamic_delay",
// which apps/api/src/routes/support.ts (main streampulse monorepo) already
// allows for public/guest tickets -- with OBS's current log attached
// automatically, no manual attach step left for the user. Mirrors exactly
// what the web /support form itself does (create ticket, then upload one
// attachment), just from inside OBS.
//
// Both HTTP calls (ticket create, then attachment upload) run together on
// one background std::thread, same RunHttp pattern AuthManager::RunHttp
// uses elsewhere in this plugin -- the dialog stays responsive, and the
// result is marshaled back via QMetaObject::invokeMethod(qApp, ...) guarded
// by a QPointer, so a dialog the user already closed is a safe no-op.
namespace trigglow {

class ReportBugDialog : public QDialog {
	Q_OBJECT

public:
	// defaultName/defaultEmail: prefilled from AuthManager::DisplayName()/
	// Email() when the user is logged in (Enable() already requires it --
	// see docs/ACCOUNT_GATE.md -- so this is the common case), empty
	// otherwise. Either can still come back empty even when logged in (no
	// display name set, or an OAuth-only account with no email on file) --
	// the user can always edit both fields regardless.
	explicit ReportBugDialog(QString pluginVersion, QString defaultName, QString defaultEmail,
				 QWidget *parent = nullptr);

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
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

	QString pluginVersion_;

	// Populated once in the constructor from bug-report.hpp's
	// CopyCurrentObsLogForSupport() -- computed up front (not re-fetched at
	// submit time) so the dialog can tell the user right away whether a log
	// was actually found, instead of only finding out after they've already
	// typed their whole message and pressed Enviar.
	QString logPath_;
	bool logFound_ = false;

	ui::Theme theme_;

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
	QLabel *logIcon_ = nullptr;
	QLabel *logStatusLabel_ = nullptr;
	QLabel *opensBrowserIcon_ = nullptr;
	QPushButton *submitButton_ = nullptr;
	QPushButton *cancelButton_ = nullptr;
};

} // namespace trigglow
