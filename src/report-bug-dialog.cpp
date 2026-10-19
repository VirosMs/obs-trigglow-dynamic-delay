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
#include "chat-text.hpp"
#include "logging.hpp"
#include "plugin-support.h"
#include "trigi-states.hpp"
#include "trigi-widgets.hpp"
#include "win-http.hpp"

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
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
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace trigglow {

using supportui::T;

namespace {
constexpr const char *kComponent = "report-bug-dialog";

// Widths (logical px). The dialog opens at kDefaultWidth (kTrigiWidth with
// the Trigi page) and can shrink to kMinimumWidth; Name | Email stack below
// kTwoColumnWidth, and the footer's links or note move above the buttons
// below kFooterRowWidth.
constexpr int kMinimumWidth = 340;
constexpr int kDefaultWidth = 480;
constexpr int kTrigiWidth = 520;
constexpr int kTrigiHeight = 660;
constexpr int kTwoColumnWidth = 400;
constexpr int kFooterRowWidth = 460;
// Below this width the footer's two links stack.
constexpr int kFooterLinksWidth = 420;
// Button and link icons.
constexpr int kIconSize = 14;

// Field label above its control (OBS font 600), with the accent asterisk of a
// required field or a muted "Optional", tied to the control for screen readers.
QWidget *MakeFieldLabel(const QString &text, QWidget *buddy, QWidget *parent, bool required = true)
{
	auto *row = new QWidget(parent);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(3);
	auto *label = new QLabel(text, row);
	label->setProperty("tgRole", QStringLiteral("strong"));
	label->setBuddy(buddy);
	layout->addWidget(label);
	if (required) {
		auto *mark = new QLabel(QStringLiteral("*"), row);
		mark->setProperty("tgTone", QStringLiteral("accent"));
		layout->addWidget(mark);
	} else {
		auto *optional = new QLabel(T("ReportDialog.Optional"), row);
		optional->setProperty("tgRole", QStringLiteral("hint"));
		layout->addSpacing(5);
		layout->addWidget(optional);
	}
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

QPushButton *MakeButton(const QString &text, const char *variant, QWidget *parent)
{
	auto *button = new QPushButton(text, parent);
	button->setProperty("tgVariant", QString::fromLatin1(variant));
	button->setAutoDefault(false);
	button->setFocusPolicy(Qt::TabFocus);
	const bool link = qstrcmp(variant, "link") == 0;
	if (link)
		button->setCursor(Qt::PointingHandCursor);
	// The glyph at the left of a wider box: the extra width is its gap to
	// the text (like the dock's buttons).
	button->setIconSize(QSize(kIconSize + (link ? 5 : 6), kIconSize));
	return button;
}

// A small text row with a leading icon (hints and notes in the form).
QWidget *MakeNote(QLabel *&icon, QLabel *&text, const char *role, QWidget *parent)
{
	auto *row = new QWidget(parent);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(7);
	icon = new QLabel(row);
	icon->setFixedSize(14, 14);
	text = new QLabel(row);
	text->setWordWrap(true);
	text->setTextFormat(Qt::PlainText);
	text->setProperty("tgRole", QString::fromLatin1(role));
	layout->addWidget(icon, 0, Qt::AlignTop);
	layout->addWidget(text, 1);
	return row;
}

int CodePoints(const QString &text)
{
	return static_cast<int>(text.toUcs4().size());
}

} // namespace

ReportBugDialog::ReportBugDialog(AuthManager &auth, SupportService::DiagnosticsProvider diagnostics, QWidget *parent)
	: QDialog(parent),
	  auth_(auth)
{
	setWindowTitle(T("ReportDialog.Title"));
	setModal(true);

	service_ = new SupportService(auth_, std::move(diagnostics), this);
	footer_ = service_->TicketFooter();
	logLocatable_ = CanLocateObsLog();
	gate_ = service_->Gate();
	trigiAvailable_ = gate_ == SupportService::TrigiGate::Available;

	BuildUi();

	using Gate = SupportService::TrigiGate;
	switch (gate_) {
	case Gate::Available:
		SetRoute(Route::SignedIn);
		ShowPage(Page::Trigi);
		break;
	case Gate::NoHttpClient:
		// macOS/Linux: no Trigi and no tickets from OBS in this version.
		SetRoute(Route::Unavailable);
		ShowPage(Page::Form);
		ShowInfo(T("ReportDialog.Unavailable"), kInfoOpenWeb);
		DisableForm();
		break;
	case Gate::Outdated: {
		SetRoute(service_->CanSubmitSignedIn() ? Route::SignedIn : Route::Guest);
		ShowPage(Page::Form);
		const QString minimum = service_->TrigiMinVersion();
		ShowInfo(minimum.isEmpty() ? T("Trigi.Outdated") : T("Trigi.OutdatedVersion").arg(minimum),
			 kInfoUpdate);
		break;
	}
	case Gate::SignedOut:
		teaser_->setVisible(true);
		SetRoute(Route::Guest);
		ShowPage(Page::Form);
		break;
	case Gate::LegacySignIn:
	case Gate::AccountInactive:
	case Gate::Disabled:
	case Gate::Unknown:
		// Trigi isn't offered (quietly): the ticket form.
		SetRoute(service_->CanSubmitSignedIn() ? Route::SignedIn : Route::Guest);
		ShowPage(Page::Form);
		break;
	}
	FitInitialSize();
}

// ---- Layout -----------------------------------------------------------------------------

QWidget *ReportBugDialog::BuildHeader()
{
	// The web's SpModal: eyebrow, title (with Trigi's avatar and chips on the
	// Trigi page), one-line intro.
	auto *header = new QWidget(this);
	auto *layout = new QVBoxLayout(header);
	layout->setContentsMargins(18, 16, 18, 14);
	layout->setSpacing(4);
	eyebrow_ = new QLabel(T("ReportDialog.Eyebrow"), header);
	eyebrow_->setProperty("tgRole", QStringLiteral("eyebrow"));
	eyebrow_->setProperty("tgTone", QStringLiteral("accent"));

	auto *titleRow = new QHBoxLayout();
	titleRow->setSpacing(10);
	headerAvatar_ = new QLabel(header);
	headerAvatar_->setFixedSize(30, 30);
	headerAvatar_->setAlignment(Qt::AlignCenter);
	headerAvatar_->setProperty("tgAvatar", QStringLiteral("true"));
	titleLabel_ = new QLabel(T("ReportDialog.Title"), header);
	titleLabel_->setProperty("tgRole", QStringLiteral("title"));
	titleLabel_->setWordWrap(true);
	chips_ = new QWidget(header);
	{
		auto *chipsLayout = new QHBoxLayout(chips_);
		chipsLayout->setContentsMargins(0, 0, 0, 0);
		chipsLayout->setSpacing(6);
		// The web's assistant chips: "AI" in cyan, "Beta" in amber.
		auto *ai = new QLabel(T("Trigi.ChipAi"), chips_);
		ai->setProperty("tgRole", QStringLiteral("tag"));
		ai->setProperty("tgTone", QStringLiteral("accent"));
		ai->setAccessibleName(T("Trigi.ChipAiName"));
		auto *beta = new QLabel(T("Trigi.ChipBeta"), chips_);
		beta->setProperty("tgRole", QStringLiteral("tag"));
		beta->setProperty("tgTone", QStringLiteral("warning"));
		chipsLayout->addWidget(ai);
		chipsLayout->addWidget(beta);
	}
	titleRow->addWidget(headerAvatar_, 0, Qt::AlignVCenter);
	titleRow->addWidget(titleLabel_, 0, Qt::AlignVCenter);
	titleRow->addWidget(chips_, 0, Qt::AlignVCenter);
	titleRow->addStretch(1);

	introLabel_ = new QLabel(T("ReportDialog.Intro"), header);
	introLabel_->setProperty("tgRole", QStringLiteral("muted"));
	introLabel_->setWordWrap(true);
	introLabel_->setTextFormat(Qt::PlainText);
	layout->addWidget(eyebrow_);
	layout->addLayout(titleRow);
	layout->addWidget(introLabel_);
	return header;
}

QWidget *ReportBugDialog::BuildFormPage()
{
	formScroll_ = new QScrollArea(this);
	formScroll_->setProperty("tgPlain", QStringLiteral("true"));
	formScroll_->setFrameShape(QFrame::NoFrame);
	formScroll_->setWidgetResizable(true);
	formScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	formScroll_->setFocusPolicy(Qt::NoFocus);
	formContent_ = new QWidget(formScroll_);
	formContent_->setObjectName(QStringLiteral("tgScrollContent"));
	auto *body = new QVBoxLayout(formContent_);
	body->setContentsMargins(18, 14, 18, 14);
	body->setSpacing(6);

	// Signed out: what Trigi is and how to get it (contract A3.9), above the
	// guest form ("send a report without Trigi").
	teaser_ = new QFrame(formContent_);
	teaser_->setProperty("tgCard", QStringLiteral("true"));
	{
		auto *layout = new QHBoxLayout(teaser_);
		layout->setContentsMargins(12, 10, 12, 12);
		layout->setSpacing(10);
		teaserAvatar_ = new QLabel(teaser_);
		teaserAvatar_->setFixedSize(28, 28);
		teaserAvatar_->setAlignment(Qt::AlignCenter);
		teaserAvatar_->setProperty("tgAvatar", QStringLiteral("true"));
		auto *texts = new QVBoxLayout();
		texts->setSpacing(4);
		auto *title = new QLabel(T("Trigi.SignedOut.Title"), teaser_);
		title->setProperty("tgRole", QStringLiteral("strong"));
		title->setWordWrap(true);
		auto *text = new QLabel(T("Trigi.SignedOut.Body"), teaser_);
		text->setProperty("tgRole", QStringLiteral("muted"));
		text->setWordWrap(true);
		text->setTextFormat(Qt::PlainText);
		teaserSignIn_ = MakeButton(T("Trigi.SignedOut.Button"), "secondary", teaser_);
		auto *buttonRow = new QHBoxLayout();
		buttonRow->setContentsMargins(0, 4, 0, 0);
		buttonRow->addWidget(teaserSignIn_);
		buttonRow->addStretch(1);
		texts->addWidget(title);
		texts->addWidget(text);
		texts->addLayout(buttonRow);
		layout->addWidget(teaserAvatar_, 0, Qt::AlignTop);
		layout->addLayout(texts, 1);
	}
	teaser_->setVisible(false);
	body->addWidget(teaser_);

	// Network/server errors: a banner above the fields, hidden until needed.
	errorBanner_ = new QFrame(formContent_);
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

	// Info panel: the web-only guest form, no HTTP client, Trigi turned off
	// or outdated. Text plus the buttons that apply.
	infoPanel_ = new QFrame(formContent_);
	infoPanel_->setProperty("tgBanner", QStringLiteral("info"));
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
		auto *buttons = new QBoxLayout(QBoxLayout::LeftToRight);
		infoButtons_ = buttons;
		buttons->setContentsMargins(24, 0, 0, 0);
		buttons->setSpacing(8);
		signInButton_ = MakeButton(T("ReportDialog.SignIn"), "secondary", infoPanel_);
		openWebButton_ = MakeButton(T("ReportDialog.OpenWebSupport"), "secondary", infoPanel_);
		openWebButton_->setAccessibleName(T("ReportDialog.OpenWebSupportName"));
		saveLogButton_ = MakeButton(T("ReportDialog.SaveLog"), "secondary", infoPanel_);
		updateButton_ = MakeButton(T("Trigi.GetUpdate"), "secondary", infoPanel_);
		updateButton_->setAccessibleName(T("Trigi.GetUpdateName"));
		buttons->addWidget(signInButton_, 0, Qt::AlignLeft);
		buttons->addWidget(openWebButton_, 0, Qt::AlignLeft);
		buttons->addWidget(saveLogButton_, 0, Qt::AlignLeft);
		buttons->addWidget(updateButton_, 0, Qt::AlignLeft);
		buttons->addStretch(1);
		layout->addLayout(buttons);
	}
	infoPanel_->setVisible(false);
	body->addWidget(infoPanel_);

	// "Trigi drafted this from your conversation": the form came prefilled.
	draftNote_ = MakeNote(draftNoteIcon_, draftNoteText_, "muted", formContent_);
	draftNote_->setVisible(false);
	body->addWidget(draftNote_);
	body->addSpacing(2);

	nameEdit_ = new QLineEdit(formContent_);
	nameEdit_->setAccessibleName(T("ReportDialog.NameLabel"));
	nameEdit_->setMaxLength(static_cast<int>(support::kGuestNameMaxChars));
	emailEdit_ = new QLineEdit(formContent_);
	emailEdit_->setPlaceholderText(T("ReportDialog.EmailPlaceholder"));
	emailEdit_->setAccessibleName(T("ReportDialog.EmailLabel"));
	emailEdit_->setAccessibleDescription(T("ReportDialog.EmailHelp"));
	nameError_ = MakeFieldError(formContent_);
	emailError_ = MakeFieldError(formContent_);
	emailHelp_ = new QLabel(T("ReportDialog.EmailHelp"), formContent_);
	emailHelp_->setProperty("tgRole", QStringLiteral("hint"));
	emailHelp_->setWordWrap(true);

	// Name | Email in two columns from kTwoColumnWidth, stacked below
	// (resizeEvent). Guests only: a signed-in ticket comes from the account.
	identityBox_ = new QWidget(formContent_);
	identityRow_ = new QBoxLayout(QBoxLayout::LeftToRight, identityBox_);
	identityRow_->setContentsMargins(0, 0, 0, 8);
	identityRow_->setSpacing(12);
	{
		auto *nameColumn = new QVBoxLayout();
		nameColumn->setSpacing(5);
		nameColumn->addWidget(MakeFieldLabel(T("ReportDialog.NameLabel"), nameEdit_, formContent_));
		nameColumn->addWidget(nameEdit_);
		nameColumn->addWidget(nameError_->parentWidget());
		nameColumn->addStretch(1);
		auto *emailColumn = new QVBoxLayout();
		emailColumn->setSpacing(5);
		emailColumn->addWidget(MakeFieldLabel(T("ReportDialog.EmailLabel"), emailEdit_, formContent_));
		emailColumn->addWidget(emailEdit_);
		emailColumn->addWidget(emailError_->parentWidget());
		emailColumn->addWidget(emailHelp_);
		emailColumn->addStretch(1);
		identityRow_->addLayout(nameColumn, 1);
		identityRow_->addLayout(emailColumn, 1);
	}
	body->addWidget(identityBox_);

	// Subject: signed in only, optional (a default one names the plugin).
	subjectBox_ = new QWidget(formContent_);
	{
		auto *layout = new QVBoxLayout(subjectBox_);
		layout->setContentsMargins(0, 0, 0, 8);
		layout->setSpacing(5);
		subjectEdit_ = new QLineEdit(subjectBox_);
		subjectEdit_->setPlaceholderText(T("ReportDialog.SubjectPlaceholder"));
		subjectEdit_->setAccessibleName(T("ReportDialog.SubjectLabel"));
		subjectEdit_->setMaxLength(static_cast<int>(support::kSubjectMaxChars) + 20);
		subjectError_ = MakeFieldError(subjectBox_);
		layout->addWidget(MakeFieldLabel(T("ReportDialog.SubjectLabel"), subjectEdit_, subjectBox_, false));
		layout->addWidget(subjectEdit_);
		layout->addWidget(subjectError_->parentWidget());
	}
	body->addWidget(subjectBox_);

	messageEdit_ = new QPlainTextEdit(formContent_);
	messageEdit_->setPlaceholderText(T("ReportDialog.MessagePlaceholder"));
	messageEdit_->setAccessibleName(T("ReportDialog.MessageLabel"));
	messageEdit_->setMinimumHeight(110);
	messageEdit_->setTabChangesFocus(true);
	messageError_ = MakeFieldError(formContent_);
	body->addWidget(MakeFieldLabel(T("ReportDialog.MessageLabel"), messageEdit_, formContent_));
	body->addWidget(messageEdit_, 1);
	body->addWidget(messageError_->parentWidget());
	SetMessage(QString());

	// The Trigi conversation goes with a signed-in ticket: say so.
	QLabel *linkedText = nullptr;
	linkedNote_ = MakeNote(linkedIcon_, linkedText, "hint", formContent_);
	linkedText->setText(T("ReportDialog.ConversationLinked"));
	linkedNote_->setVisible(false);
	body->addWidget(linkedNote_);

	// Attachment row: unticked, and it can only be ticked once the user has
	// looked at the redacted log in "See what is sent" (the log goes only if
	// they choose to, knowing what it holds). What is hidden in it, and what
	// may still show.
	body->addSpacing(6);
	auto *attachment = new QFrame(formContent_);
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
		attachCheck_->setChecked(false);
		attachCheck_->setFocusPolicy(Qt::TabFocus);
		logStatusLabel_ = new QLabel(attachment);
		logStatusLabel_->setWordWrap(true);
		logStatusLabel_->setTextFormat(Qt::PlainText);
		previewButton_ = MakeButton(T("ReportDialog.SeeWhatIsSent"), "link", attachment);
		previewButton_->setAccessibleName(T("ReportDialog.SeeWhatIsSentName"));
		previewButton_->setVisible(logLocatable_);
		texts->addWidget(attachCheck_);
		texts->addWidget(logStatusLabel_);
		texts->addWidget(previewButton_, 0, Qt::AlignLeft);
		layout->addWidget(logIcon_, 0, Qt::AlignTop);
		layout->addLayout(texts, 1);
	}
	body->addWidget(attachment);
	UpdateAttachRow();
	formScroll_->setWidget(formContent_);

	connect(teaserSignIn_, &QPushButton::clicked, this, [this] { SignIn(); });
	connect(signInButton_, &QPushButton::clicked, this, [this] { SignIn(); });
	connect(previewButton_, &QPushButton::clicked, this, [this] { ShowLogPreview(); });
	connect(saveLogButton_, &QPushButton::clicked, this, [this] { SaveLog(); });
	connect(openWebButton_, &QPushButton::clicked, this, [] { supportui::OpenWebPage(api::web::kPublicSupport); });
	connect(updateButton_, &QPushButton::clicked, this, [] { supportui::OpenWebPage(api::web::kLatestRelease); });
	connect(nameEdit_, &QLineEdit::textChanged, this,
		[this](const QString &text) { TrackEmpty(nameEdit_, text.isEmpty()); });
	connect(emailEdit_, &QLineEdit::textChanged, this,
		[this](const QString &text) { TrackEmpty(emailEdit_, text.isEmpty()); });
	connect(subjectEdit_, &QLineEdit::textChanged, this,
		[this](const QString &text) { TrackEmpty(subjectEdit_, text.isEmpty()); });
	connect(messageEdit_, &QPlainTextEdit::textChanged, this,
		[this] { TrackEmpty(messageEdit_, messageEdit_->document()->isEmpty()); });
	TrackEmpty(nameEdit_, true);
	TrackEmpty(emailEdit_, true);
	TrackEmpty(subjectEdit_, true);
	TrackEmpty(messageEdit_, messageEdit_->document()->isEmpty());
	return formScroll_;
}

QWidget *ReportBugDialog::BuildDonePage()
{
	// One centered column in the middle of the page: the check, what happens
	// next, the warning if the log didn't make it, "View ticket".
	auto *page = new QWidget(this);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(36, 20, 36, 20);
	layout->setSpacing(14);
	layout->addStretch(1);

	doneIcon_ = new QLabel(page);
	doneIcon_->setFixedSize(48, 48);
	doneIcon_->setAlignment(Qt::AlignCenter);
	doneIcon_->setProperty("tgAvatar", QStringLiteral("true"));
	doneText_ = new QLabel(page);
	doneText_->setWordWrap(true);
	doneText_->setTextFormat(Qt::PlainText);
	doneText_->setAlignment(Qt::AlignHCenter);
	layout->addWidget(doneIcon_, 0, Qt::AlignHCenter);
	layout->addWidget(doneText_);

	// The ticket exists but its log didn't make it: say so, offer to save it.
	doneAttachRow_ = new QFrame(page);
	doneAttachRow_->setProperty("tgBanner", QStringLiteral("warning"));
	{
		auto *rowLayout = new QVBoxLayout(doneAttachRow_);
		rowLayout->setContentsMargins(10, 8, 10, 8);
		rowLayout->setSpacing(8);
		auto *textRow = new QHBoxLayout();
		textRow->setSpacing(8);
		doneAttachIcon_ = new QLabel(doneAttachRow_);
		doneAttachIcon_->setFixedSize(16, 16);
		auto *text = new QLabel(T("ReportDialog.AttachmentFailed"), doneAttachRow_);
		text->setWordWrap(true);
		text->setTextFormat(Qt::PlainText);
		textRow->addWidget(doneAttachIcon_, 0, Qt::AlignTop);
		textRow->addWidget(text, 1);
		rowLayout->addLayout(textRow);
		doneSaveLogButton_ = MakeButton(T("ReportDialog.SaveLog"), "secondary", doneAttachRow_);
		auto *buttonRow = new QHBoxLayout();
		buttonRow->setContentsMargins(24, 0, 0, 0);
		buttonRow->addWidget(doneSaveLogButton_);
		buttonRow->addStretch(1);
		rowLayout->addLayout(buttonRow);
	}
	doneAttachRow_->setVisible(false);
	layout->addWidget(doneAttachRow_);

	viewTicketButton_ = MakeButton(T("ReportDialog.ViewTicket"), "secondary", page);
	viewTicketButton_->setProperty("tgSize", QStringLiteral("large"));
	viewTicketButton_->setAccessibleName(T("ReportDialog.ViewTicketName"));
	layout->addWidget(viewTicketButton_, 0, Qt::AlignHCenter);
	layout->addStretch(1);

	connect(viewTicketButton_, &QPushButton::clicked, this, [this] { supportui::OpenWebPage(ticketUrl_); });
	connect(doneSaveLogButton_, &QPushButton::clicked, this, [this] { SaveLog(); });
	return page;
}

QWidget *ReportBugDialog::BuildFooter()
{
	auto *footer = new QWidget(this);
	// Links (or the browser note) and the buttons side by side from
	// kFooterRowWidth; below it the left part goes on its own line.
	footerRow_ = new QBoxLayout(QBoxLayout::LeftToRight, footer);
	footerRow_->setContentsMargins(18, 12, 18, 14);
	footerRow_->setSpacing(8);

	auto *left = new QBoxLayout(QBoxLayout::LeftToRight);
	footerLinks_ = left;
	left->setSpacing(14);
	backButton_ = MakeButton(T("ReportDialog.BackToTrigi"), "link", footer);
	writeTeamButton_ = MakeButton(T("Trigi.RatherWriteToTeam"), "link", footer);
	clearButton_ = MakeButton(T("Trigi.Clear"), "link", footer);
	opensBrowserNote_ = new QWidget(footer);
	{
		auto *noteRow = new QHBoxLayout(opensBrowserNote_);
		noteRow->setContentsMargins(0, 0, 0, 0);
		noteRow->setSpacing(8);
		opensBrowserIcon_ = new QLabel(opensBrowserNote_);
		opensBrowserIcon_->setFixedSize(14, 14);
		auto *opensBrowser = new QLabel(T("ReportDialog.OpensBrowser"), opensBrowserNote_);
		opensBrowser->setProperty("tgRole", QStringLiteral("hint"));
		opensBrowser->setWordWrap(true);
		noteRow->addWidget(opensBrowserIcon_, 0, Qt::AlignVCenter);
		noteRow->addWidget(opensBrowser, 1);
	}
	left->addWidget(backButton_, 0, Qt::AlignLeft);
	left->addWidget(writeTeamButton_, 0, Qt::AlignLeft);
	left->addWidget(clearButton_, 0, Qt::AlignLeft);
	left->addWidget(opensBrowserNote_, 1);

	cancelButton_ = new QPushButton(T("ReportDialog.Cancel"), footer);
	cancelButton_->setProperty("tgVariant", QStringLiteral("secondary"));
	cancelButton_->setProperty("tgSize", QStringLiteral("large"));
	cancelButton_->setAutoDefault(false);
	submitButton_ = new QPushButton(T("ReportDialog.Submit"), footer);
	submitButton_->setProperty("tgVariant", QStringLiteral("primary"));
	submitButton_->setAutoDefault(false);
	auto *buttons = new QHBoxLayout();
	buttons->setSpacing(8);
	buttons->addStretch(1);
	buttons->addWidget(cancelButton_);
	buttons->addWidget(submitButton_);
	footerRow_->addLayout(left, 1);
	footerRow_->addLayout(buttons, 0);

	connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
	connect(submitButton_, &QPushButton::clicked, this, [this] {
		if (page_ == Page::Done)
			accept();
		else if (page_ == Page::Form)
			OnSubmitClicked();
	});
	connect(backButton_, &QPushButton::clicked, this, [this] {
		ClearErrors();
		ShowPage(Page::Trigi);
	});
	connect(writeTeamButton_, &QPushButton::clicked, this, [this] { trigiPage_->WriteToTeam(); });
	connect(clearButton_, &QPushButton::clicked, this, [this] { trigiPage_->ClearConversation(); });
	return footer;
}

void ReportBugDialog::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	header_ = BuildHeader();
	root->addWidget(header_);
	auto *headerLine = new QFrame(this);
	headerLine->setProperty("tgDivider", QStringLiteral("true"));
	headerLine->setFixedHeight(1);
	root->addWidget(headerLine);

	pages_ = new QStackedWidget(this);
	if (trigiAvailable_) {
		trigiPage_ = new TrigiPage(*service_, this);
		trigiPage_->onOpenForm = [this](const TrigiPage::TicketDraft &draft, TrigiPage::Handover reason) {
			OpenForm(draft, reason);
		};
		trigiPage_->onSignIn = [this] {
			SignIn();
		};
		trigiPage_->onStateChanged = [this] {
			UpdateFooter();
		};
		pages_->addWidget(trigiPage_);
	}
	pages_->addWidget(BuildFormPage());
	donePage_ = BuildDonePage();
	pages_->addWidget(donePage_);
	root->addWidget(pages_, 1);

	auto *footerLine = new QFrame(this);
	footerLine->setProperty("tgDivider", QStringLiteral("true"));
	footerLine->setFixedHeight(1);
	root->addWidget(footerLine);
	root->addWidget(BuildFooter());

	// Narrow enough for large OBS font sizes and small screens, where the
	// rows reflow (resizeEvent); it still opens at a comfortable width.
	setMinimumWidth(kMinimumWidth);
	ApplyTheme();
	ApplyWidthLayout(trigiAvailable_ ? kTrigiWidth : kDefaultWidth);
}

void ReportBugDialog::FitInitialSize()
{
	const QRect screen = this->screen() ? this->screen()->availableGeometry() : QRect(0, 0, 1280, 720);
	const int maxHeight = std::max(420, static_cast<int>(screen.height() * 0.9));
	if (page_ == Page::Trigi) {
		resize(kTrigiWidth, std::min(kTrigiHeight, maxHeight));
		return;
	}
	// The form opens at its natural height (it scrolls if the screen is short).
	ensurePolished();
	const int width = kDefaultWidth;
	auto heightOf = [width](QWidget *w) {
		QLayout *l = w->layout();
		return l && l->hasHeightForWidth() ? l->totalHeightForWidth(width) : w->sizeHint().height();
	};
	const int natural = heightOf(header_) + heightOf(formContent_) + heightOf(footerRow_->parentWidget()) + 2;
	resize(width, std::min(natural, maxHeight));
}

void ReportBugDialog::ShowPage(Page page)
{
	page_ = page;
	switch (page) {
	case Page::Trigi:
		pages_->setCurrentWidget(trigiPage_);
		titleLabel_->setText(T("Trigi.Name"));
		introLabel_->setText(T("Trigi.Intro"));
		break;
	case Page::Form:
		pages_->setCurrentWidget(formScroll_);
		titleLabel_->setText(T("ReportDialog.Title"));
		introLabel_->setText(T("ReportDialog.Intro"));
		break;
	case Page::Done:
		pages_->setCurrentWidget(donePage_);
		titleLabel_->setText(T("ReportDialog.Done.Title"));
		break;
	}
	headerAvatar_->setVisible(page == Page::Trigi);
	chips_->setVisible(page == Page::Trigi);
	introLabel_->setVisible(page == Page::Trigi || (page == Page::Form && draftNote_->isHidden()));
	UpdateFooter();
	switch (page) {
	case Page::Trigi:
		trigiPage_->FocusFirst();
		break;
	case Page::Form:
		if (!formDisabled_)
			messageEdit_->setFocus(Qt::OtherFocusReason);
		break;
	case Page::Done:
		submitButton_->setFocus(Qt::OtherFocusReason);
		break;
	}
}

void ReportBugDialog::UpdateFooter()
{
	if (!submitButton_)
		return;
	const bool trigi = page_ == Page::Trigi && trigiPage_;
	const bool notice = trigi && trigiPage_->NoticeShown();
	backButton_->setVisible(page_ == Page::Form && trigiAvailable_ && trigiPage_);
	writeTeamButton_->setVisible(trigi && !notice);
	clearButton_->setVisible(trigi && !notice && trigiPage_->HasMessages());
	if (trigi) {
		const bool busy = trigiPage_->Busy();
		writeTeamButton_->setEnabled(!busy);
		clearButton_->setEnabled(!busy);
	}
	opensBrowserNote_->setVisible(page_ == Page::Form && route_ == Route::Guest && !formDisabled_);
	cancelButton_->setVisible(page_ != Page::Done);
	cancelButton_->setText(page_ == Page::Form ? T("ReportDialog.Cancel") : T("ReportDialog.Close"));
	submitButton_->setVisible(page_ != Page::Trigi);
	submitButton_->setDefault(page_ != Page::Trigi);
	submitButton_->setText(page_ == Page::Done ? T("ReportDialog.Close") : T("ReportDialog.Submit"));
	if (page_ == Page::Done)
		submitButton_->setEnabled(true);
}

void ReportBugDialog::ApplyWidthLayout(int width)
{
	if (identityRow_)
		identityRow_->setDirection(width >= kTwoColumnWidth ? QBoxLayout::LeftToRight
								    : QBoxLayout::TopToBottom);
	if (footerRow_)
		footerRow_->setDirection(width >= kFooterRowWidth ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
	if (footerLinks_)
		footerLinks_->setDirection(width >= kFooterLinksWidth ? QBoxLayout::LeftToRight
								      : QBoxLayout::TopToBottom);
	// The info panel's buttons: the body's and the panel's margins, and room
	// for the form's scroll bar.
	supportui::FitButtonRow(infoButtons_, width - 36 - 20 - 14);
}

void ReportBugDialog::ApplyTheme()
{
	if (applyingTheme_)
		return;
	applyingTheme_ = true;
	const double basePt = ui::BaseFontPointSize(this);
	ui::Theme theme = ui::BuildTheme(QApplication::palette(), basePt);
	const bool changed = theme.styleSheet != theme_.styleSheet;
	theme_ = std::move(theme);
	if (changed)
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
	// Buttons in uppercase (the brand's button type); links stay as written.
	for (QPushButton *button : findChildren<QPushButton *>()) {
		const bool link = button->property("tgVariant").toString() == QLatin1String("link");
		ui::ApplyFont(button, link ? ui::FontRole::Strong : ui::FontRole::Button, basePt);
	}

	const uitokens::Tokens &t = theme_.tokens;
	const qreal dpr = devicePixelRatioF();
	const QColor accent = ui::ToQColor(t.accentText);
	const QColor text = ui::ToQColor(t.text);
	const QColor muted = ui::ToQColor(t.textMuted);
	const QColor disabled = ui::ToQColor(t.disabledFg);
	errorBannerIcon_->setPixmap(ui::IconPixmap(ui::IconId::AlertTriangle, ui::ToQColor(t.dangerText), 16, dpr));
	infoIcon_->setPixmap(ui::IconPixmap(ui::IconId::Info, accent, 16, dpr));
	logIcon_->setPixmap(ui::IconPixmap(logLocatable_ ? ui::IconId::Paperclip : ui::IconId::AlertTriangle,
					   ui::ToQColor(logLocatable_ ? t.accentText : t.warningText), 16, dpr));
	opensBrowserIcon_->setPixmap(ui::IconPixmap(ui::IconId::ExternalLink, muted, 14, dpr));
	headerAvatar_->setPixmap(ui::IconPixmap(ui::IconId::Bolt, accent, 16, dpr));
	teaserAvatar_->setPixmap(ui::IconPixmap(ui::IconId::Bolt, accent, 16, dpr));
	draftNoteIcon_->setPixmap(ui::IconPixmap(ui::IconId::Bolt, accent, 14, dpr));
	linkedIcon_->setPixmap(ui::IconPixmap(ui::IconId::MessageCircle, muted, 14, dpr));
	doneIcon_->setPixmap(ui::IconPixmap(ui::IconId::Check, accent, 20, dpr));
	doneAttachIcon_->setPixmap(ui::IconPixmap(ui::IconId::AlertTriangle, ui::ToQColor(t.warningText), 16, dpr));
	for (QLabel *icon : findChildren<QLabel *>(QStringLiteral("tgFieldErrorIcon")))
		icon->setPixmap(ui::IconPixmap(ui::IconId::AlertTriangle, ui::ToQColor(t.dangerText), 14, dpr));
	auto setIcon = [&](QPushButton *button, ui::IconId id) {
		button->setIcon(ui::Icon(id, text, disabled, Qt::AlignLeft));
	};
	auto setLinkIcon = [&](QPushButton *button, ui::IconId id) {
		button->setIcon(ui::Icon(id, accent, disabled, Qt::AlignLeft));
	};
	setIcon(openWebButton_, ui::IconId::ExternalLink);
	setIcon(updateButton_, ui::IconId::Download);
	setIcon(signInButton_, ui::IconId::LogIn);
	setIcon(teaserSignIn_, ui::IconId::LogIn);
	setIcon(saveLogButton_, ui::IconId::Download);
	setIcon(doneSaveLogButton_, ui::IconId::Download);
	setLinkIcon(previewButton_, ui::IconId::Eye);
	setIcon(viewTicketButton_, ui::IconId::ExternalLink);
	setLinkIcon(backButton_, ui::IconId::ArrowLeft);
	setLinkIcon(writeTeamButton_, ui::IconId::MessageCircle);
	if (trigiPage_)
		trigiPage_->ApplyTheme(theme_);
	applyingTheme_ = false;
}

void ReportBugDialog::showEvent(QShowEvent *event)
{
	QDialog::showEvent(event);
	if (fitted_)
		return;
	fitted_ = true;
	GrowToFitForm();
}

void ReportBugDialog::GrowToFitForm()
{
	// Once laid out: a form that still scrolls grows by what's missing,
	// within the screen (never shrinks: the user may have sized it).
	QPointer<ReportBugDialog> self(this);
	QTimer::singleShot(0, this, [self] {
		if (!self || !self->isVisible() || self->page_ != Page::Form)
			return;
		const int missing = self->formContent_->height() - self->formScroll_->viewport()->height();
		if (missing <= 0)
			return;
		const QRect screen = self->screen() ? self->screen()->availableGeometry() : QRect(0, 0, 1280, 720);
		const int maxHeight = std::max(self->height(), static_cast<int>(screen.height() * 0.9));
		self->resize(self->width(), std::min(self->height() + missing + 2, maxHeight));
	});
}

void ReportBugDialog::resizeEvent(QResizeEvent *event)
{
	QDialog::resizeEvent(event);
	ApplyWidthLayout(event->size().width());
}

void ReportBugDialog::changeEvent(QEvent *event)
{
	QDialog::changeEvent(event);
	switch (event->type()) {
	case QEvent::ApplicationPaletteChange:
	case QEvent::ApplicationFontChange:
		// OBS switched theme while the dialog is open.
		ApplyTheme();
		break;
	default:
		break;
	}
}

// ---- The form ---------------------------------------------------------------------------

void ReportBugDialog::SetRoute(Route route)
{
	route_ = route;
	identityBox_->setVisible(route != Route::SignedIn);
	subjectBox_->setVisible(route == Route::SignedIn);
	linkedNote_->setVisible(route == Route::SignedIn && service_->HasConversation());
	if (route == Route::Guest) {
		if (nameEdit_->text().isEmpty() && auth_.IsLoggedIn())
			nameEdit_->setText(QString::fromStdString(auth_.DisplayName()));
		CheckGuestForm();
	}
	UpdateFooter();
}

void ReportBugDialog::CheckGuestForm()
{
	// The public form may need a human check the plugin can't show: ask
	// first, and degrade to the web form instead of failing at the end.
	if (guestChecked_)
		return;
	guestChecked_ = true;
	QPointer<ReportBugDialog> self(this);
	service_->CheckGuestForm([self](SupportService::GuestForm form) {
		if (!self || form != SupportService::GuestForm::WebOnly || self->route_ != Route::Guest)
			return;
		self->teaser_->setVisible(false);
		self->ShowInfo(T("ReportDialog.WebOnly"),
			       kInfoSignIn | kInfoOpenWeb | (self->logLocatable_ ? kInfoSaveLog : 0u));
		self->DisableForm();
	});
}

void ReportBugDialog::OpenForm(const TrigiPage::TicketDraft &draft, TrigiPage::Handover reason)
{
	using Handover = TrigiPage::Handover;
	ClearErrors();
	HideInfo();
	switch (reason) {
	case Handover::UserChoice:
		break;
	case Handover::Disabled:
		// Trigi was turned off: no drama, the message goes to the team.
		trigiAvailable_ = false;
		ShowInfo(T("Trigi.DisabledQuiet"), 0);
		break;
	case Handover::Outdated: {
		trigiAvailable_ = false;
		const QString minimum = service_->TrigiMinVersion();
		ShowInfo(minimum.isEmpty() ? T("Trigi.Outdated") : T("Trigi.OutdatedVersion").arg(minimum),
			 kInfoUpdate);
		break;
	}
	case Handover::SessionEnded:
	case Handover::AccountInactive:
		trigiAvailable_ = false;
		break;
	}
	SetRoute(service_->CanSubmitSignedIn() ? Route::SignedIn : Route::Guest);
	subjectEdit_->setText(draft.subject);
	SetMessage(draft.message);

	using Origin = TrigiPage::TicketDraft::Origin;
	switch (draft.origin) {
	case Origin::SummaryAi:
		draftNoteText_->setText(T("ReportDialog.FromTrigiAi"));
		draftNote_->setVisible(true);
		break;
	case Origin::Summary:
	case Origin::OwnWords:
		draftNoteText_->setText(T("ReportDialog.FromTrigiOwn"));
		draftNote_->setVisible(true);
		break;
	case Origin::None:
		draftNote_->setVisible(false);
		break;
	}
	ShowPage(Page::Form);
	formScroll_->verticalScrollBar()->setValue(0);
	if (draftNote_->isVisible())
		supportui::Announce(this, draftNoteText_->text(), false);
}

void ReportBugDialog::SetMessage(const QString &text)
{
	const QString body = text.trimmed();
	messageEdit_->setPlainText(body.isEmpty() ? footer_ : body + footer_);
	// The version footer is visible and editable; the cursor goes where the
	// user writes (the start, or after the draft).
	QTextCursor cursor = messageEdit_->textCursor();
	cursor.setPosition(static_cast<int>(body.size()));
	messageEdit_->setTextCursor(cursor);
	TrackEmpty(messageEdit_, messageEdit_->document()->isEmpty());
}

void ReportBugDialog::ShowFieldError(QWidget *field, QLabel *errorLabel, const QString &message)
{
	ui::SetStyleProperty(field, "tgInvalid", QStringLiteral("true"));
	errorLabel->setText(message);
	errorLabel->parentWidget()->setVisible(true);
	if (errorLabel == emailError_)
		emailHelp_->setVisible(false); // The error takes the help's place.
	field->setFocus();
	formScroll_->ensureWidgetVisible(field);
	supportui::Announce(field, message, true);
}

void ReportBugDialog::ClearErrors()
{
	for (QWidget *field : std::initializer_list<QWidget *>{nameEdit_, emailEdit_, subjectEdit_, messageEdit_})
		ui::SetStyleProperty(field, "tgInvalid", QStringLiteral("false"));
	for (QLabel *label : {nameError_, emailError_, subjectError_, messageError_})
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
	if (formDisabled_)
		return;
	ClearErrors();
	const QString name = nameEdit_->text().trimmed();
	const QString email = emailEdit_->text().trimmed();
	if (route_ == Route::Guest) {
		if (CodePoints(name) < static_cast<int>(support::kGuestNameMinChars)) {
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
	QString subject = T("ReportDialog.DefaultSubject").arg(QString::fromUtf8(PLUGIN_VERSION));
	if (route_ == Route::SignedIn) {
		const QString typed = subjectEdit_->text().trimmed();
		switch (trigiui::CheckSubject(typed.toStdString())) {
		case trigiui::SubjectCheck::UseDefault:
			break;
		case trigiui::SubjectCheck::Ok:
			subject = typed;
			break;
		case trigiui::SubjectCheck::TooShort:
		case trigiui::SubjectCheck::TooLong:
			ShowFieldError(subjectEdit_, subjectError_,
				       T("ReportDialog.ErrorSubjectLength")
					       .arg(support::kSubjectMinChars)
					       .arg(support::kSubjectMaxChars));
			return;
		}
	}
	if (CodePoints(UserMessage()) < static_cast<int>(support::kGuestMessageMinChars)) {
		ShowFieldError(messageEdit_, messageError_, T("ReportDialog.ErrorMessageTooShort"));
		return;
	}
	const QString message = messageEdit_->toPlainText().trimmed();
	if (CodePoints(message) > static_cast<int>(support::kMessageMaxChars)) {
		ShowFieldError(messageEdit_, messageError_,
			       T("ReportDialog.ErrorMessageTooLong").arg(support::kMessageMaxChars));
		return;
	}

	SetBusy(true);
	const bool attachLog = attachCheck_->isChecked();
	QPointer<ReportBugDialog> self(this);
	auto done = [self](const SupportService::TicketOutcome &outcome) {
		if (self)
			self->OnTicketDone(outcome);
	};
	if (route_ == Route::SignedIn)
		service_->SubmitTicket(subject, message, service_->HasConversation(), attachLog, done);
	else
		service_->SubmitGuestTicket(name, email, subject, message, attachLog, done);
}

void ReportBugDialog::OnTicketDone(const SupportService::TicketOutcome &outcome)
{
	SetBusy(false);
	if (!outcome.created) {
		using support::Status;
		if (outcome.needsWebForm) {
			teaser_->setVisible(false);
			ShowInfo(T("ReportDialog.WebOnly"),
				 kInfoSignIn | kInfoOpenWeb | (logLocatable_ ? kInfoSaveLog : 0u));
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
		case Status::Timeout:
		case Status::Unavailable:
			ShowError(T("ReportDialog.ErrorNetwork"));
			break;
		default:
			ShowError(T("ReportDialog.ErrorInvalidResponse"));
			break;
		}
		return;
	}

	// Created. Guest tickets open in the browser right away, as before;
	// signed-in ones are in the user's support panel ("View ticket").
	ticketUrl_ = outcome.ticketUrl;
	if (route_ == Route::Guest)
		supportui::OpenWebPage(ticketUrl_);
	doneText_->setText(route_ == Route::Guest ? T("ReportDialog.Done.BodyGuest") : T("ReportDialog.Done.Body"));
	viewTicketButton_->setVisible(!ticketUrl_.isEmpty());
	const bool attachmentFailed = outcome.attachmentTried && !outcome.attachmentSent;
	doneAttachRow_->setVisible(attachmentFailed);
	ShowPage(Page::Done);
	QString said = T("ReportDialog.Done.Title") + QStringLiteral(". ") + doneText_->text();
	if (attachmentFailed)
		said += QLatin1Char(' ') + T("ReportDialog.AttachmentFailed");
	supportui::Announce(this, said, false);
}

void ReportBugDialog::ShowInfo(const QString &message, unsigned buttons)
{
	infoLabel_->setText(message);
	signInButton_->setVisible((buttons & kInfoSignIn) != 0 && http::Available());
	openWebButton_->setVisible((buttons & kInfoOpenWeb) != 0);
	saveLogButton_->setVisible((buttons & kInfoSaveLog) != 0);
	updateButton_->setVisible((buttons & kInfoUpdate) != 0);
	infoPanel_->setVisible(true);
	ApplyWidthLayout(width());
	GrowToFitForm();
	supportui::Announce(infoLabel_, message, false);
}

void ReportBugDialog::HideInfo()
{
	infoPanel_->setVisible(false);
}

void ReportBugDialog::DisableForm()
{
	formDisabled_ = true;
	for (QWidget *w : std::initializer_list<QWidget *>{nameEdit_, emailEdit_, subjectEdit_, messageEdit_,
							   attachCheck_, previewButton_})
		w->setEnabled(false);
	submitButton_->setEnabled(false);
	UpdateFooter();
}

void ReportBugDialog::ShowLogPreview()
{
	previewButton_->setEnabled(false);
	QPointer<ReportBugDialog> self(this);
	service_->PrepareLog([self](const SupportService::PreparedLog &log) {
		if (!self)
			return;
		self->previewButton_->setEnabled(!self->formDisabled_ && !self->busy_);
		if (!log.found) {
			// No log of this OBS to attach: say so where the box is.
			self->logLocatable_ = false;
			self->logPreviewed_ = false;
			self->UpdateAttachRow();
			self->ApplyTheme();
			supportui::OpenTextPreview(self, self->theme_, T("ReportDialog.PreviewTitle"),
						   T("ReportDialog.LogRedacted"), T("ReportDialog.LogNotFound"));
			return;
		}
		QString content = QString::fromUtf8(log.preview.head.c_str());
		if (log.preview.truncated)
			content += QStringLiteral("\n[...]\n\n") + QString::fromUtf8(log.preview.tail.c_str());
		const QString note =
			log.preview.truncated || log.capped
				? T("ReportDialog.LogRedacted") + QLatin1Char(' ') + T("ReportDialog.PreviewTruncated")
				: T("ReportDialog.LogRedacted");
		QDialog *preview =
			supportui::OpenTextPreview(self, self->theme_, T("ReportDialog.PreviewTitle"), note, content);
		// Seen: the box can be ticked now. Focus goes back to it, so the next
		// key press can tick it.
		QObject::connect(preview, &QDialog::finished, self, [self] {
			if (!self)
				return;
			self->logPreviewed_ = true;
			self->UpdateAttachRow();
			if (self->attachCheck_->isEnabled())
				self->attachCheck_->setFocus(Qt::OtherFocusReason);
		});
	});
}

void ReportBugDialog::UpdateAttachRow()
{
	QString hint;
	if (!logLocatable_) {
		hint = T("ReportDialog.LogNotFound");
		ui::SetStyleProperty(logStatusLabel_, "tgRole", QString());
		ui::SetStyleProperty(logStatusLabel_, "tgTone", QStringLiteral("warning"));
		attachCheck_->setChecked(false);
	} else {
		hint = T("ReportDialog.LogRedacted");
		if (!logPreviewed_)
			hint += QLatin1Char(' ') + T("ReportDialog.LogReviewFirst");
		ui::SetStyleProperty(logStatusLabel_, "tgRole", QStringLiteral("hint"));
		ui::SetStyleProperty(logStatusLabel_, "tgTone", QString());
	}
	logStatusLabel_->setText(hint);
	attachCheck_->setAccessibleDescription(hint);
	attachCheck_->setEnabled(logLocatable_ && logPreviewed_ && !busy_ && !formDisabled_);
	previewButton_->setVisible(logLocatable_);
}

void ReportBugDialog::SaveLog()
{
	saveLogButton_->setEnabled(false);
	doneSaveLogButton_->setEnabled(false);
	QPointer<ReportBugDialog> self(this);
	service_->PrepareLog([self](const SupportService::PreparedLog &log) {
		if (!self)
			return;
		self->saveLogButton_->setEnabled(true);
		self->doneSaveLogButton_->setEnabled(true);
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

void ReportBugDialog::SignIn()
{
	// The sign-in itself happens in the dock (code + browser).
	if (!auth_.IsLoggedIn())
		auth_.StartLogin();
	reject();
}

void ReportBugDialog::SetBusy(bool busy)
{
	const bool guestFields = route_ == Route::Guest;
	nameEdit_->setEnabled(!busy && guestFields);
	emailEdit_->setEnabled(!busy && guestFields);
	subjectEdit_->setEnabled(!busy);
	messageEdit_->setEnabled(!busy);
	busy_ = busy;
	attachCheck_->setEnabled(!busy && logLocatable_ && logPreviewed_ && !formDisabled_);
	previewButton_->setEnabled(!busy && !formDisabled_);
	backButton_->setEnabled(!busy);
	cancelButton_->setEnabled(!busy);
	submitButton_->setEnabled(!busy);
	submitButton_->setText(busy ? T("ReportDialog.Sending") : T("ReportDialog.Submit"));
}

void ReportBugDialog::ShowError(const QString &message)
{
	statusLabel_->setText(message);
	errorBanner_->setVisible(true);
	formScroll_->ensureWidgetVisible(errorBanner_);
	supportui::Announce(statusLabel_, message, true);
}

} // namespace trigglow
