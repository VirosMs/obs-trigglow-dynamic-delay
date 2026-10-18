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

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QObject>
#include <QString>
#include <QUrl>

#include "log-redact.hpp"
#include "support-client.hpp"
#include "support-diagnostics.hpp"

// What the support dialog uses for Trigi (Trigglow's AI support assistant)
// and for support tickets, so the dialog never deals with HTTP, tokens,
// session or ticket IDs. Create one per dialog, as its child: closing the
// dialog drops every pending answer (and, if a conversation had questions
// and no ticket, tells the server it was abandoned, in the background).
//
// Threading: every call below returns at once; the network work runs on a
// background thread and `done` is called later on the UI thread (never
// after this object is destroyed). At most one Trigi request (question or
// summary) is in flight; feedback and close events wait their turn.
//
// What the dialog still owns (contract A3, A5): the first-use notice and its
// two buttons, the "Include OBS technical data" checkbox with its "See what is
// sent" preview, the safe rendering of replies (chat-text.hpp: LineHtml() only),
// "Was this helpful?", the 1000-character counter, the Retry-After countdown,
// accessibility, and opening URLs (only the ones this class or api::web give).
//
// Typical flow:
//   Gate() == Available ? Trigi page : ticket form (+ "Sign in to ask Trigi"
//   when SignedOut). NoticeAccepted() or show the notice first. Ask() ->
//   render AskResult::reply. "Create a ticket with this conversation" ->
//   RequestSummary() -> prefill the form (+ TicketFooter()) -> SubmitTicket().
//   No sign-in: CheckGuestForm() -> SubmitGuestTicket(), or the degraded
//   "use the web form" panel.
namespace trigglow {

class AuthManager;

class SupportService : public QObject {
	Q_OBJECT

public:
	using DiagnosticsProvider = std::function<diag::Snapshot()>;

	// diagnostics: takes the snapshot on the UI thread (collector:
	// diagnostics-collector.hpp). Empty = never offer technical data.
	SupportService(AuthManager &auth, DiagnosticsProvider diagnostics, QObject *parent = nullptr);
	~SupportService() override;

	// --- When to show Trigi (contract A3.1) --------------------------------------
	enum class TrigiGate : unsigned char {
		Available,       // Show the Trigi page.
		NoHttpClient,    // macOS/Linux: the classic form, disabled, + web::kPublicSupport.
		SignedOut,       // Ticket form + "Sign in to ask Trigi" (AuthManager::StartLogin()).
		LegacySignIn,    // A v0.5.x sign-in not migrated yet: ticket form (guest route).
		AccountInactive, // Ticket form; the dock explains the account state.
		Disabled,        // The server turned Trigi off (or 403 client_disabled): ticket form, quietly.
		Outdated,        // "Update the plugin to use Trigi" + web::kLatestRelease.
		Unknown,         // Signed in, but the start check hasn't answered yet: ticket form.
	};
	TrigiGate Gate() const;
	// The version Trigi needs, when Gate() is Outdated and the server said it.
	QString TrigiMinVersion() const;

	// --- First-use notice and technical data (stored locally) -------------------
	static constexpr int kNoticeVersion = support::kNoticeVersion;
	bool NoticeAccepted() const;
	void AcceptNotice();
	// Off until the user ticks it; remembered.
	bool IncludeDiagnostics() const;
	void SetIncludeDiagnostics(bool include);
	// The exact technical data that would be sent now ("See what is sent"),
	// pretty-printed JSON. Empty when there is no provider.
	QString DiagnosticsPreview() const;
	// Whether the server accepts this version of the technical data at all
	// (false: hide the checkbox).
	bool DiagnosticsSupported() const;

	// --- Conversation -------------------------------------------------------------------
	bool Busy() const { return busy_; }
	// Seconds Send must stay disabled after a 429 (0 when it can be used).
	int RetryAfterSecondsLeft() const;
	// A conversation exists and has at least one question.
	bool HasConversation() const { return !sessionId_.empty() && questions_ > 0; }

	enum class AskRefusal : unsigned char {
		None,              // Sent; `done` will be called.
		Busy,              // Another Trigi request is in flight.
		RateLimited,       // Wait RetryAfterSecondsLeft().
		Empty,             // Nothing to send after trimming.
		TooLong,           // More than support::kMaxQuestionChars characters.
		NoticeNotAccepted, // Show the first-use notice first.
		NotAvailable,      // Gate() isn't Available.
	};

	struct AskResult {
		// status Ok: reply.kind is Answer, Refused or Unavailable (the
		// conversation itself may have come back unavailable). Otherwise the
		// state to show (support::Status): SessionEnded (keep the text),
		// Disabled (open the form, keep the text), Outdated, RateLimited,
		// Network (manual retry), ConversationGone (the next Ask() starts a new
		// one, keep the text), BadResponse...
		support::ReplyResult reply;
		bool conversationStarted = false; // This question started a new conversation.
	};
	AskRefusal Ask(const QString &question, std::function<void(const AskResult &)> done);

	// Draft for "Create a ticket with this conversation": subject and message
	// to prefill (plain text the user edits). Without a conversation, or if the
	// summary fails, done gets the status and the form stays as the user left it.
	bool RequestSummary(std::function<void(const support::SummaryResult &)> done);

	// Events (contract A3.7), fire-and-forget. The dialog calls:
	void SendFeedback(bool helpful); // "Was this helpful?" Yes / No.
	void MarkResolved();             // "Yes, it helped" and then Close.
	void NotifyHandoff();            // "Write to the team" or "Open support on trigglow.com".
	// The next question starts a new conversation ("Clear conversation" only
	// clears the view and doesn't need this).
	void ForgetConversation();

	// --- Tickets ------------------------------------------------------------------------
	// Footer appended to the message box when the form opens:
	// "\n\n--\nDynamic Delay X.Y.Z . OBS X.Y.Z . Windows" (U+00B7 dots).
	QString TicketFooter() const;
	// Signed in with a plugin token on a platform with an HTTP client.
	bool CanSubmitSignedIn() const;

	struct TicketOutcome {
		support::Status status = support::Status::Network; // Of the ticket itself.
		int retryAfterSeconds = 0;
		bool created = false;
		// The log: tried (the user asked and a log was found), and whether it
		// made it. A ticket created without it is still a success: say so and
		// offer "Save log...".
		bool attachmentTried = false;
		bool attachmentSent = false;
		bool logNotFound = false;
		support::Status attachmentStatus = support::Status::Ok;
		// "View ticket on trigglow.com" (signed in), or the guest ticket page
		// to open right away. Never shown as text, never logged.
		QUrl ticketUrl;
		// Guest route only: the public form needs a human check the plugin
		// can't show; switch to the "use the web form" panel, no retry.
		bool needsWebForm = false;
	};

	// Signed-in ticket (category dynamic_delay). linkConversation: attach the
	// Trigi conversation, if there was one. attachLog: read, redact and attach
	// OBS's current log.
	void SubmitTicket(const QString &subject, const QString &message, bool linkConversation, bool attachLog,
			  std::function<void(const TicketOutcome &)> done);

	enum class GuestForm : unsigned char {
		Available, // Show the guest form.
		WebOnly,   // Show "To send it from OBS, sign in. You can also use the web form" + buttons.
		Unknown,   // No answer: show the form anyway (it fails as "no connection" if still offline).
	};
	void CheckGuestForm(std::function<void(GuestForm)> done);
	void SubmitGuestTicket(const QString &name, const QString &email, const QString &subject,
			       const QString &message, bool attachLog, std::function<void(const TicketOutcome &)> done);

	// --- The log -------------------------------------------------------------------------
	struct PreparedLog {
		bool found = false;
		bool capped = false;
		std::shared_ptr<const std::string> text; // Redacted; what is attached. UTF-8.
		logredact::Preview preview;              // First/last 200 lines, for "See what is sent".
	};
	// Reads and redacts OBS's current log in the background (also used for
	// "Save log (no personal data)..."). The result is kept and reused by the
	// next ticket submission.
	void PrepareLog(std::function<void(const PreparedLog &)> done);

private:
	struct Prefs {
		int noticeAccepted = 0;
		bool includeDiagnostics = false;
	};
	void LoadPrefs();
	void SavePrefs() const;
	// Trigi requests run one at a time: Enqueue() starts `start` now or after
	// the request in flight; each request calls Finish() when it is done.
	void Enqueue(std::function<void()> start);
	void Finish();
	void QueueEvent(support::Event event);
	void NoteStatus(const support::CallInfo &info, const char *what);
	std::string Locale() const;

	template<typename R> void RunAsync(std::function<R()> work, std::function<void(const R &)> done);

	AuthManager &auth_;
	DiagnosticsProvider diagnostics_;
	Prefs prefs_;
	std::string prefsPath_;

	std::string sessionId_; // In memory only; never shown or logged.
	bool sessionHasDiagnostics_ = false;
	int questions_ = 0;
	bool ticketCreated_ = false;
	bool resolved_ = false;
	bool disabled_ = false; // 403 client_disabled seen in this dialog.
	bool outdated_ = false; // 426 seen in this dialog.
	std::string minVersion_;
	bool busy_ = false; // A question or a summary is pending.
	bool inFlight_ = false;
	std::vector<std::function<void()>> queue_;
	std::chrono::steady_clock::time_point retryAt_{};
	// Bumped by ForgetConversation(): answers for an older conversation are dropped.
	int generation_ = 0;
	std::shared_ptr<const std::string> preparedLog_;
};

} // namespace trigglow
