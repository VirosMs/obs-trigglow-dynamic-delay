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
#include <QUrl>

#include "support-service.hpp"
#include "trigi-page.hpp"
#include "ui-theme.hpp"

class QBoxLayout;
class QCheckBox;
class QFrame;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;

// The modal support dialog behind TrigglowDelayDock's "Report a problem"
// button. Three pages:
//
//   1. Trigi (Windows, signed in, Trigi on and this version allowed): the AI
//      support assistant (trigi-page.hpp), first-use notice included. From
//      it, "Create a ticket with this conversation" asks Trigi for a summary
//      and opens the form with it.
//   2. The ticket form (category "dynamic_delay"):
//      - signed in (plugin token): optional subject, the message (prefilled
//        from Trigi's summary when there is one) with the visible version
//        footer, the Trigi conversation linked, OBS's redacted log as a
//        checkbox with "See what is sent";
//      - not signed in: the guest form (name, email, message) plus "Sign in
//        to ask Trigi". If trigglow.com asks guests for a human check the
//        plugin can't show, it turns into "sign in, or use the web form"
//        (plus "Save log") instead of failing;
//      - Trigi turned off: the form, quietly; this version too old for Trigi:
//        the form plus "Get the update";
//      - no HTTP client on this platform (macOS/Linux today): the form stays
//        disabled with a link to the web form.
//   3. "Ticket sent", with "View ticket on trigglow.com" (guest tickets open
//      in the browser right away, as before).
//
// Every network call goes through SupportService (support-service.hpp); the
// dialog never sees tokens, IDs or HTTP. The log is redacted before it leaves
// the machine (log-redact.hpp) and never goes to Trigi.
namespace trigglow {

class AuthManager;

class ReportBugDialog : public QDialog {
	Q_OBJECT

public:
	// diagnostics: passed on to SupportService (the Trigi page's opt-in
	// technical data); the ticket form itself never sends it.
	ReportBugDialog(AuthManager &auth, SupportService::DiagnosticsProvider diagnostics, QWidget *parent = nullptr);

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	enum class Page { Trigi, Form, Done };
	// How the form sends the ticket.
	enum class Route { SignedIn, Guest, Unavailable };
	// Buttons of the info panel above the form.
	enum InfoButton : unsigned { kInfoSignIn = 1, kInfoOpenWeb = 2, kInfoSaveLog = 4, kInfoUpdate = 8 };

	void BuildUi();
	QWidget *BuildHeader();
	QWidget *BuildFormPage();
	QWidget *BuildDonePage();
	QWidget *BuildFooter();
	void ShowPage(Page page);
	void UpdateFooter();
	// Name | Email in two columns, and the footer's left part beside the
	// buttons, when `width` has room for them; stacked otherwise.
	void ApplyWidthLayout(int width);
	// Same tokens/fonts/icons as the dock (ui-theme.hpp), so the dialog looks
	// the same whether or not it inherits the dock's style sheet.
	void ApplyTheme();
	void FitInitialSize();
	// The form page still scrolls: grow by what's missing, within the screen.
	void GrowToFitForm();

	// The form.
	void SetRoute(Route route);
	void OpenForm(const TrigiPage::TicketDraft &draft, TrigiPage::Handover reason);
	// The message box: `text`, then the version footer; the cursor after `text`.
	void SetMessage(const QString &text);
	void CheckGuestForm();
	void OnSubmitClicked();
	// Validation errors sit under their field (danger border + icon + text),
	// focus jumps to the first one; network/server errors go in a banner at
	// the top. Both are announced to screen readers.
	void ShowFieldError(QWidget *field, QLabel *errorLabel, const QString &message);
	void ClearErrors();
	// busy: while a submission is in flight every input is off and Send says
	// "Sending...".
	void SetBusy(bool busy);
	void ShowError(const QString &message); // Network/server error banner.
	// The info panel above the form, with the buttons (InfoButton) that apply.
	void ShowInfo(const QString &message, unsigned buttons);
	void HideInfo();
	// No form: only the info panel's buttons (and Cancel) remain.
	void DisableForm();
	void OnTicketDone(const SupportService::TicketOutcome &outcome);
	void ShowLogPreview();
	// The attachment row's hint and box: the box can only be ticked once the
	// user has seen the log in the preview.
	void UpdateAttachRow();
	void SaveLog();
	void SignIn();
	// The message without the version footer the form prefills.
	QString UserMessage() const;

	AuthManager &auth_;
	SupportService *service_ = nullptr;
	SupportService::TrigiGate gate_ = SupportService::TrigiGate::Unknown;
	Route route_ = Route::Guest;
	Page page_ = Page::Form;
	QString footer_;
	bool logLocatable_ = false;
	// The preview showed the redacted log at least once: only then can the
	// "Attach OBS's log" box (unticked at first) be ticked.
	bool logPreviewed_ = false;
	bool busy_ = false;
	bool trigiAvailable_ = false; // The Trigi page can still be used ("Back to Trigi").
	bool guestChecked_ = false;
	bool formDisabled_ = false;
	bool applyingTheme_ = false;
	bool fitted_ = false; // The first show's grow-to-fit is done.
	QUrl ticketUrl_;

	ui::Theme theme_;

	// Header.
	QLabel *eyebrow_ = nullptr;
	QLabel *headerAvatar_ = nullptr;
	QLabel *titleLabel_ = nullptr;
	QWidget *chips_ = nullptr;
	QLabel *introLabel_ = nullptr;
	QWidget *header_ = nullptr;

	QStackedWidget *pages_ = nullptr;
	TrigiPage *trigiPage_ = nullptr;
	QScrollArea *formScroll_ = nullptr;
	QWidget *formContent_ = nullptr;
	QWidget *donePage_ = nullptr;

	// Form.
	QFrame *teaser_ = nullptr; // "Sign in to ask Trigi" (signed out).
	QLabel *teaserAvatar_ = nullptr;
	QPushButton *teaserSignIn_ = nullptr;
	QFrame *errorBanner_ = nullptr;
	QLabel *errorBannerIcon_ = nullptr;
	QLabel *statusLabel_ = nullptr; // Text of errorBanner_.
	QFrame *infoPanel_ = nullptr;
	QLabel *infoIcon_ = nullptr;
	QLabel *infoLabel_ = nullptr;
	QPushButton *signInButton_ = nullptr;
	QPushButton *openWebButton_ = nullptr;
	QPushButton *saveLogButton_ = nullptr;
	QPushButton *updateButton_ = nullptr;
	QBoxLayout *infoButtons_ = nullptr; // Side by side, or stacked when they don't fit.
	QWidget *draftNote_ = nullptr;      // "Trigi drafted this..."
	QLabel *draftNoteIcon_ = nullptr;
	QLabel *draftNoteText_ = nullptr;
	QWidget *identityBox_ = nullptr; // Name and email: guests only.
	QBoxLayout *identityRow_ = nullptr;
	QLineEdit *nameEdit_ = nullptr;
	QLineEdit *emailEdit_ = nullptr;
	QLabel *nameError_ = nullptr;
	QLabel *emailError_ = nullptr;
	QLabel *emailHelp_ = nullptr;   // Under the email field; the email's error takes its place.
	QWidget *subjectBox_ = nullptr; // Signed in only.
	QLineEdit *subjectEdit_ = nullptr;
	QLabel *subjectError_ = nullptr;
	QPlainTextEdit *messageEdit_ = nullptr;
	QLabel *messageError_ = nullptr;
	QWidget *linkedNote_ = nullptr; // "Your conversation with Trigi goes with the ticket".
	QLabel *linkedIcon_ = nullptr;
	QLabel *logIcon_ = nullptr;
	QCheckBox *attachCheck_ = nullptr;
	QLabel *logStatusLabel_ = nullptr;
	QPushButton *previewButton_ = nullptr;

	// Done.
	QLabel *doneIcon_ = nullptr;
	QLabel *doneText_ = nullptr;
	QWidget *doneAttachRow_ = nullptr;
	QLabel *doneAttachIcon_ = nullptr;
	QPushButton *doneSaveLogButton_ = nullptr;
	QPushButton *viewTicketButton_ = nullptr;

	// Footer.
	QBoxLayout *footerRow_ = nullptr;
	QBoxLayout *footerLinks_ = nullptr;      // The footer's links, stacked when narrow.
	QPushButton *backButton_ = nullptr;      // Form: "Back to Trigi".
	QPushButton *writeTeamButton_ = nullptr; // Trigi: "I'd rather write to the team".
	QPushButton *clearButton_ = nullptr;     // Trigi: "Clear conversation".
	QWidget *opensBrowserNote_ = nullptr;    // Guest form: "We open your ticket in the browser".
	QLabel *opensBrowserIcon_ = nullptr;
	QPushButton *cancelButton_ = nullptr;
	QPushButton *submitButton_ = nullptr;
};

} // namespace trigglow
