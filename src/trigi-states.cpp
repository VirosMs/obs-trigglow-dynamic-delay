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

#include "trigi-states.hpp"

#include <algorithm>
#include <cstdio>

#include "chat-text.hpp"

namespace trigglow::trigiui {

namespace {

std::string_view TrimView(std::string_view text)
{
	auto space = [](char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r';
	};
	while (!text.empty() && space(text.front()))
		text.remove_prefix(1);
	while (!text.empty() && space(text.back()))
		text.remove_suffix(1);
	return text;
}

} // namespace

Problem ProblemFor(support::Status status, int httpStatus)
{
	using support::Status;
	switch (status) {
	case Status::Ok:
		return Problem::None;
	case Status::Network:
		return httpStatus >= 500 ? Problem::ServerBusy : Problem::Offline;
	case Status::Timeout:
		return Problem::Timeout;
	case Status::RateLimited:
		return Problem::RateLimited;
	case Status::SignedOut:
	case Status::SessionEnded:
		return Problem::SessionEnded;
	case Status::ConversationGone:
	case Status::NotFound:
		return Problem::ConversationClosed;
	case Status::Disabled:
		return Problem::Disabled;
	case Status::Outdated:
		return Problem::Outdated;
	case Status::AccountInactive:
		return Problem::AccountInactive;
	case Status::Unavailable:
		// No HTTP client: the dialog never shows Trigi there, but if it did,
		// the form (with the web link) is the way out.
		return Problem::Disabled;
	case Status::Invalid:
	case Status::ScopeError:
	case Status::BadResponse:
	case Status::CaptchaRequired:
	case Status::AttachmentRejected:
	case Status::TicketClosed:
		break;
	}
	return Problem::Generic;
}

bool OpensForm(Problem problem)
{
	return problem == Problem::Disabled || problem == Problem::Outdated || problem == Problem::AccountInactive;
}

bool CanRetry(Problem problem)
{
	switch (problem) {
	case Problem::Offline:
	case Problem::Timeout:
	case Problem::ServerBusy:
	case Problem::ConversationClosed:
	case Problem::Generic:
		return true;
	default:
		return false;
	}
}

bool IsUrgent(Problem problem)
{
	switch (problem) {
	case Problem::Offline:
	case Problem::Timeout:
	case Problem::ServerBusy:
	case Problem::SessionEnded:
	case Problem::Generic:
		return true;
	default:
		return false;
	}
}

ReplyAction ActionFor(support::NextStep next)
{
	switch (next) {
	case support::NextStep::TicketHere:
		return ReplyAction::CreateTicket;
	case support::NextStep::WriteToTeam:
		return ReplyAction::WriteToTeam;
	case support::NextStep::WebSupport:
		return ReplyAction::OpenWebSupport;
	case support::NextStep::None:
		break;
	}
	return ReplyAction::None;
}

bool OffersFeedback(support::ReplyKind kind)
{
	return kind == support::ReplyKind::Answer;
}

QuestionCount CountQuestion(std::string_view utf8)
{
	QuestionCount count;
	count.chars = chat::CountChars(TrimView(utf8));
	count.empty = count.chars == 0;
	count.tooLong = count.chars > support::kMaxQuestionChars;
	return count;
}

std::string FormatCountdown(int seconds)
{
	seconds = std::clamp(seconds, 0, 3600);
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "%d:%02d", seconds / 60, seconds % 60);
	return buffer;
}

SubjectCheck CheckSubject(std::string_view trimmedUtf8)
{
	const size_t chars = chat::CountChars(TrimView(trimmedUtf8));
	if (chars == 0)
		return SubjectCheck::UseDefault;
	if (chars < support::kSubjectMinChars)
		return SubjectCheck::TooShort;
	if (chars > support::kSubjectMaxChars)
		return SubjectCheck::TooLong;
	return SubjectCheck::Ok;
}

} // namespace trigglow::trigiui
