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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api-client.hpp"
#include "auth-client.hpp"
#include "chat-text.hpp"
#include "json.hpp"
#include "token-store.hpp"

// Trigi (Trigglow's AI support assistant) and support tickets, without Qt.
// Every call is BLOCKING and logs nothing; support-service.hpp runs them on a
// background thread for the dialog and writes the allowed log lines.
//
// Trigi only ever gets what the user types and, if they opt in, the
// "diagnostics" object of support-diagnostics.hpp. Never OBS's log.
//
// Endpoints (all on the API host):
//   POST /api/support/bot/sessions                     start a conversation (Bearer)
//   POST /api/support/bot/sessions/{id}/messages       ask (Bearer, up to 90 s)
//   POST /api/support/bot/sessions/{id}/summary        ticket draft (Bearer, up to 90 s)
//   POST /api/support/bot/sessions/{id}/events         feedback / close / handoff (Bearer)
//   POST /api/support/tickets                          ticket, signed in (Bearer)
//   POST /api/support/tickets/{id}/attachments         its log (Bearer)
//   GET  /api/support/public/config                    guest form availability
//   POST /api/support/public/tickets                   ticket, guest
//   POST /api/support/public/tickets/{id}/attachments  its log (X-Ticket-Token header)
namespace trigglow::support {

// The question box limit (a UI limit: counter and disabled Send past it).
inline constexpr size_t kMaxQuestionChars = 1000;
// Version of the first-use notice the user accepted (sent with the session).
inline constexpr int kNoticeVersion = 1;
// Ticket field limits.
inline constexpr size_t kSubjectMinChars = 3;
inline constexpr size_t kSubjectMaxChars = 200;
inline constexpr size_t kMessageMaxChars = 6000;
inline constexpr size_t kGuestMessageMinChars = 5;
inline constexpr size_t kGuestNameMinChars = 2;
inline constexpr size_t kGuestNameMaxChars = 100;
// The redacted log attachment.
inline constexpr const char *kLogFileName = "obs-log.txt";

// One status for every call, so the dialog maps them to its states in one
// place (contract A3.9 / A6).
enum class Status : unsigned char {
	Ok,
	SignedOut,          // Not signed in with a plugin token. UI: "Sign in to ask Trigi".
	SessionEnded,       // 401: the sign-in was cleared ("your session expired"); keep the typed text.
	AccountInactive,    // 403 suspended / account_inactive: hide Trigi; Enable is blocked too.
	Disabled,           // 403 client_disabled: hide Trigi quietly and open the ticket form, keeping the text.
	Outdated,           // 426 client_outdated: "Update the plugin to use Trigi" (minVersion may be set).
	RateLimited,        // 429: Send disabled for retryAfterSeconds.
	ConversationGone,   // 404 not_found / 409 session_closed: the next question starts a new conversation.
	Invalid,            // 400 invalid_body (or a value rejected before sending): "Couldn't send. Check the fields".
	ScopeError,         // 403 insufficient_scope: a plugin bug; generic error, the sign-in stays.
	Network,            // Transport error or 5xx: "No connection to trigglow.com" + manual retry.
	Timeout,            // Sent, but no answer in time (http::Error::Timeout): "took too long" + manual retry.
	Unavailable,        // No HTTP client on this platform (macOS/Linux today).
	BadResponse,        // 2xx without the required fields: "Unexpected response from the server".
	CaptchaRequired,    // Guest ticket refused for lack of a human check: degrade to the web form.
	AttachmentRejected, // 400 file_too_large / invalid_type / invalid_mime / attachment_limit / missing_file.
	TicketClosed,       // 409 ticket_closed.
	NotFound,           // 404 on a ticket attachment.
};

const char *StatusName(Status status); // Short English name, for the log.

// What every call reports besides its own data.
struct CallInfo {
	Status status = Status::Network;
	int httpStatus = 0;
	std::string errorCode;       // The server's "error" code ("" if none).
	int retryAfterSeconds = 0;   // RateLimited only.
	std::string minVersion;      // Outdated only, when the server says it.
	auth::AuthEvents authEvents; // Hand these to AuthManager::ApplyAuthEvents() on the UI thread.

	bool Ok() const { return status == Status::Ok; }
};

struct Context {
	const api::Client &client;
	TokenStore &store;
};

// --- Trigi ------------------------------------------------------------------------

struct SessionResult : CallInfo {
	std::string sessionId;  // In memory only; never shown or logged.
	bool available = false; // false: "Trigi can't answer right now" + create a ticket.
};

// diagnostics: BuildV1()'s object when the user opted in, or nullopt. locale:
// OBS's ("es-ES"), normalized here.
SessionResult CreateSession(const Context &ctx, std::string_view locale, const std::optional<json::Value> &diagnostics,
			    int noticeVersion = kNoticeVersion);

enum class ReplyKind : unsigned char {
	Answer,      // `lines` holds the reply; offer "Was this helpful?".
	Refused,     // "I can only help with Trigglow and Dynamic Delay".
	Unavailable, // "Trigi can't answer right now" + create a ticket.
};

enum class NextStep : unsigned char {
	None,
	TicketHere,  // "Create a ticket with this conversation" (dynamic_delay ticket, or a human asked for).
	WriteToTeam, // Refused + human: "Write to the team" (the ticket form).
	WebSupport,  // Another category: "Open support on trigglow.com" (web::kAppSupport).
};

struct ReplyResult : CallInfo {
	ReplyKind kind = ReplyKind::Answer;
	std::vector<chat::Line> lines; // Sanitized and parsed (chat-text.hpp). Empty unless Answer.
	size_t replyChars = 0;         // Length of the sanitized reply (for the log).
	NextStep next = NextStep::None;
	const char *action = "none"; // The server's action type, for the log: "none", "ticket" or "human".
};

// question: trimmed, 1-kMaxQuestionChars characters (checked here too).
ReplyResult Ask(const Context &ctx, std::string_view sessionId, std::string_view question);

// Pure part of Ask(), exposed for tests: reads a 200 body of .../messages.
// false if it isn't one of the three documented shapes.
bool ParseReplyBody(const json::Value &body, ReplyResult &out);

struct SummaryResult : CallInfo {
	std::string subject; // Plain text drafts the user edits before sending.
	std::string message;
	bool ai = false; // false: built from the user's own words.
};
SummaryResult Summarize(const Context &ctx, std::string_view sessionId);

enum class Event : unsigned char {
	HelpfulYes,     // {"kind":"feedback","meta":{"helpful":true}}
	HelpfulNo,      // {"kind":"feedback","meta":{"helpful":false}}
	CloseResolved,  // {"kind":"close","meta":{"status":"resolved"}}
	CloseAbandoned, // {"kind":"close","meta":{"status":"abandoned"}} (short timeout)
	HandoffClicked, // {"kind":"handoff_clicked"}
};
json::Value EventBody(Event event);
// Errors are only worth an HTTP code in the log.
CallInfo SendEvent(const Context &ctx, std::string_view sessionId, Event event);

// --- Tickets ----------------------------------------------------------------------

// "\n\n--\nDynamic Delay 0.6.0 . OBS 31.0.2 . Windows" (the dots are U+00B7):
// appended to the message box when the form opens, visible and editable.
std::string TicketFooter(std::string_view pluginVersion, std::string_view obsVersion);

struct TicketResult : CallInfo {
	std::string ticketId; // Open api::AppSupportTicketUrl(ticketId); never show or log the id.
};

// Signed in. botSessionId: the Trigi conversation to link, if there was one.
TicketResult CreateTicket(const Context &ctx, std::string_view subject, std::string_view message,
			  std::string_view botSessionId = {});
CallInfo AttachLog(const Context &ctx, std::string_view ticketId, const std::string &redactedLog);

struct PublicConfigResult : CallInfo {
	// The public form asks for a human check the plugin can't show: no guest
	// form in OBS; offer sign-in, the web form and "Save log" instead.
	bool challengeRequired = false;
};
PublicConfigResult FetchPublicConfig(const api::Client &client);

struct PublicTicketResult : CallInfo {
	std::string ticketId;
	std::string accessToken; // Never shown or logged; only for the attachment and the web link.
};
PublicTicketResult CreatePublicTicket(const api::Client &client, std::string_view guestName,
				      std::string_view guestEmail, std::string_view subject, std::string_view message);
CallInfo AttachLogPublic(const api::Client &client, std::string_view ticketId, std::string_view accessToken,
			 const std::string &redactedLog);

// --- Helpers ----------------------------------------------------------------------

// Trims spaces, tabs and line breaks at both ends.
std::string Trim(std::string_view text);

// Maps an API result to a Status (the common table of contract A6), for the
// calls above and for tests.
Status StatusFromResult(const api::Result &r, const auth::AuthEvents &events);

} // namespace trigglow::support
