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

#include <cstddef>
#include <string>
#include <string_view>

#include "support-client.hpp"

// The decisions behind the support dialog's Trigi page that don't need Qt, so
// tests/trigi_states_tests.cpp can pin them down: which state a failed
// question puts the page in (contract A3.9), what each reply offers, the
// question's character counter, the Retry-After countdown and the ticket
// subject. The page itself (trigi-page.cpp) only lays these out.
namespace trigglow::trigiui {

// What the page shows after a question (or the ticket summary) failed.
enum class Problem : unsigned char {
	None,
	Offline,            // No connection to trigglow.com: banner + Try again.
	Timeout,            // No answer in time: banner + Try again.
	ServerBusy,         // trigglow.com answered with an error (5xx): banner + Try again.
	RateLimited,        // Too many questions in a row: countdown, Send off, Create a ticket.
	SessionEnded,       // The sign-in ended: Sign in, or report without Trigi.
	ConversationClosed, // The conversation is gone: the next try starts a new one.
	Disabled,           // Trigi was turned off: the ticket form, quietly, with the text.
	Outdated,           // This version can't use Trigi any more: the form + "Get the update".
	AccountInactive,    // The account isn't active: the ticket form.
	Generic,            // Anything else: banner + Try again.
};

// A transport error that took at least this long is reported as "Trigi took
// too long" rather than "no connection": the request was sent and nothing
// came back within the answer window.
inline constexpr long long kTimeoutAfterMs = 60000;

// status/httpStatus: the call's support::CallInfo. elapsedMs: how long it took.
Problem ProblemFor(support::Status status, int httpStatus, long long elapsedMs);

// The page gives way to the ticket form (keeping the user's text).
bool OpensForm(Problem problem);
// A manual "Try again" makes sense (never automatic: contract A0).
bool CanRetry(Problem problem);
// Read out at once by screen readers (errors), or politely (the rest).
bool IsUrgent(Problem problem);

// The button under a reply (contract A3.5).
enum class ReplyAction : unsigned char {
	None,
	CreateTicket,   // "Create a ticket with this conversation".
	WriteToTeam,    // "Write to the team" (refused + a person asked for).
	OpenWebSupport, // "Open support on trigglow.com" (another category).
};
ReplyAction ActionFor(support::NextStep next);
// "Was this helpful?" goes under answers only (not refusals or "can't answer").
bool OffersFeedback(support::ReplyKind kind);

// The question box: characters after trimming, as SupportService counts them.
struct QuestionCount {
	size_t chars = 0;
	bool empty = true;
	bool tooLong = false; // Over support::kMaxQuestionChars: Send stays off.
};
QuestionCount CountQuestion(std::string_view utf8);

// Remaining Retry-After time: "0:45", "1:05", "60:00".
std::string FormatCountdown(int seconds);

// The ticket subject the user typed (trimmed): empty uses the default one.
enum class SubjectCheck : unsigned char { UseDefault, Ok, TooShort, TooLong };
SubjectCheck CheckSubject(std::string_view trimmedUtf8);

} // namespace trigglow::trigiui
