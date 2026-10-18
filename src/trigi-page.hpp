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
#include <vector>

#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "support-service.hpp"
#include "trigi-states.hpp"
#include "ui-theme.hpp"

class QBoxLayout;
class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace trigglow {

class ChatTextView;
class QuestionEdit;
class SpinnerIcon;

// The Trigi page of the support dialog (ReportBugDialog): Trigglow's AI
// support assistant, shown first when Trigi is available (signed in, Windows).
//
//   - First use: the AI and privacy notice (contract A5.5, version 1) must be
//     accepted before the first question; "I'd rather write to the team"
//     skips to the ticket form. The optional "Include OBS technical data"
//     box, with "See what is sent", sits under it and stays under the
//     question box afterwards.
//   - The conversation: the user's messages on the right, Trigi's on the
//     left with its avatar; each reply is rendered ONLY through
//     chat::LineHtml() and chat::LinePrefix() (chat-text.hpp), in views
//     whose documents never load anything. Under each answer: "Was this
//     helpful?" (Yes: resolved; No: offer the ticket) and the button the
//     reply asks for (create a ticket, write to the team, open web support).
//   - Every state of contract A3.9 (thinking, no connection, timeout,
//     server error, rate limit with its countdown, session ended,
//     conversation closed, generic error) as a banner above the question box
//     with its next step; Trigi turned off or outdated hands over to the form.
//   - Screen readers hear each new reply once, and each state change once.
//
// The page never sees tokens, IDs or HTTP: everything goes through
// SupportService.
class TrigiPage : public QWidget {
	Q_OBJECT

public:
	// What the ticket form opens with.
	struct TicketDraft {
		enum class Origin {
			None,      // Empty form ("I'd rather write to the team" before any question).
			Summary,   // Trigi's summary, built from the user's own words.
			SummaryAi, // Trigi's summary, written by the AI.
			OwnWords,  // The summary failed (or there was no conversation): the user's questions.
		};
		QString subject;
		QString message; // Without the version footer.
		Origin origin = Origin::None;
	};
	// Why the page hands over to the form.
	enum class Handover {
		UserChoice,      // "I'd rather write to the team", "Create a ticket", "Write to the team".
		Disabled,        // Trigi was turned off (keep the text, no drama).
		Outdated,        // This plugin version can't use Trigi any more.
		SessionEnded,    // "Send a report without Trigi" after the sign-in ended.
		AccountInactive, // The account isn't active.
	};

	TrigiPage(SupportService &service, QWidget *parent = nullptr);
	~TrigiPage() override;

	std::function<void(const TicketDraft &, Handover)> onOpenForm;
	std::function<void()> onSignIn;
	// Something the dialog's footer shows changed (notice, busy, messages).
	std::function<void()> onStateChanged;

	void ApplyTheme(const ui::Theme &theme);
	// Where the keyboard starts: "Got it" on the notice, the question box after.
	void FocusFirst();

	bool NoticeShown() const;
	bool Busy() const;
	// The user wrote something in this dialog (the "Clear conversation" link).
	bool HasMessages() const;
	// "I'd rather write to the team" (the dialog footer's link).
	void WriteToTeam();
	// "Clear conversation": the view only; the server conversation goes on.
	void ClearConversation();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	enum class Tone { Accent, Text, Muted, Warning, Danger, OnPrimary };
	struct IconSlot {
		QPointer<QWidget> widget; // A QLabel (pixmap) or a QPushButton (icon).
		ui::IconId id;
		Tone tone;
		int size;
	};
	struct Pending {
		QString text;
		QPointer<QWidget> bubble;
	};

	void BuildUi();
	QWidget *BuildNotice();
	QWidget *BuildComposer();
	void ShowNotice(bool show);

	// Conversation.
	void Submit();
	// fromComposer: a new question (its bubble is added and the box cleared);
	// otherwise "Try again" with `bubble`, the message that didn't go through.
	void Send(const QString &text, QWidget *bubble, bool fromComposer);
	void OnReply(const SupportService::AskResult &result, long long elapsedMs);
	QWidget *AddUserMessage(const QString &text);
	QWidget *AddTrigiMessage(const std::vector<chat::Line> &lines, const QString &plain);
	QWidget *AddTrigiText(const QString &text);
	QWidget *AddTrigiBubble(const std::function<void(QFrame *, QVBoxLayout *)> &fill, const QString &plain);
	void InsertMessage(QWidget *row);
	void AddReplyActions(QWidget *message, trigiui::ReplyAction action, bool feedback);
	void SetNotSent(QWidget *bubble, bool notSent);
	void ShowThinking(const QString &text);
	void HideThinking();
	void DropUnansweredFeedback();
	void ScrollToEnd();
	void FitBubbles();

	// "Was this helpful?" and the handovers.
	void OnHelpful(QWidget *row, bool helpful);
	void StartTicket(bool handoff);
	void OnSummary(const support::SummaryResult &result, long long elapsedMs);
	TicketDraft OwnWordsDraft() const;

	// States (contract A3.9).
	void ShowProblem(trigiui::Problem problem);
	void HideProblem();
	void FitBannerButtons();
	void SetBanner(const char *tone, ui::IconId icon, const QString &text);
	void SetBannerButton(QPushButton *button, std::function<void()> &slot, const QString &text, ui::IconId icon,
			     std::function<void()> action);
	void UpdateRateLimit();
	void HandOver(trigiui::Problem problem, const QString &text);

	void UpdateControls();
	// The buttons inside the transcript (reply actions, "Was this helpful?")
	// come right after it, before the question box.
	void UpdateTabOrder();
	void UpdateCounter();
	void ShowDiagnosticsPreview();
	void Announce(const QString &message, bool urgent);

	QLabel *MakeIconLabel(ui::IconId id, Tone tone, int size, QWidget *parent);
	QPushButton *MakeButton(const QString &text, const char *variant, QWidget *parent);
	void SetButtonIcon(QPushButton *button, ui::IconId id, Tone tone);
	QLabel *MakeAvatar(QWidget *parent);
	ChatTextView *MakeTextView(QWidget *parent);
	QColor ToneColor(Tone tone) const;
	void ApplyIcons();

	SupportService &service_;
	ui::Theme theme_;
	bool themed_ = false;

	QStackedWidget *views_ = nullptr;
	QScrollArea *noticeScroll_ = nullptr;
	QScrollArea *transcript_ = nullptr;
	QWidget *transcriptContent_ = nullptr;
	QVBoxLayout *messages_ = nullptr;
	QPushButton *acceptButton_ = nullptr;
	QPushButton *declineButton_ = nullptr;
	QPushButton *privacyButton_ = nullptr;
	QWidget *noticeActions_ = nullptr;

	QFrame *banner_ = nullptr;
	QLabel *bannerIcon_ = nullptr;
	QLabel *bannerText_ = nullptr;
	QLabel *bannerCountdown_ = nullptr; // Retry-After, m:ss.
	QPushButton *bannerPrimary_ = nullptr;
	QPushButton *bannerSecondary_ = nullptr;
	QBoxLayout *bannerButtons_ = nullptr; // Side by side, or stacked when they don't fit.
	std::function<void()> bannerPrimaryAction_;
	std::function<void()> bannerSecondaryAction_;

	QWidget *composer_ = nullptr;
	QuestionEdit *question_ = nullptr;
	QPushButton *sendButton_ = nullptr;
	QLabel *counter_ = nullptr;
	QWidget *finePrint_ = nullptr;

	QWidget *diagnosticsRow_ = nullptr;
	QBoxLayout *diagnosticsLayout_ = nullptr;
	QCheckBox *diagnosticsCheck_ = nullptr;
	QPushButton *diagnosticsSee_ = nullptr;
	QLabel *diagnosticsHint_ = nullptr; // Under the box while the notice shows.

	QPointer<QWidget> thinkingRow_;
	QPointer<QLabel> thinkingText_;
	QPointer<SpinnerIcon> spinner_;
	QTimer *slowTimer_ = nullptr;
	QTimer *rateTimer_ = nullptr;

	std::vector<IconSlot> icons_;
	std::vector<QPointer<ChatTextView>> textViews_; // Every text view, for re-theming.
	std::vector<QPointer<QWidget>> bubbles_;        // Every bubble, for their maximum width.
	std::vector<QPointer<QPushButton>> replyButtons_;
	QPointer<QWidget> feedbackRow_; // The latest unanswered "Was this helpful?".

	Pending pending_;       // The question in flight.
	Pending failed_;        // The last question that didn't go through ("Try again").
	QElapsedTimer elapsed_; // Since the question in flight was sent.
	QStringList asked_;     // What the user asked in this dialog (the own-words draft).
	trigiui::Problem problem_ = trigiui::Problem::None;
	bool summarizing_ = false;
	bool userMessages_ = false;
	bool overLimit_ = false; // The question is over the character limit (announced once).
	bool stickToEnd_ = true; // Keep the transcript scrolled to the newest message.
};

} // namespace trigglow
