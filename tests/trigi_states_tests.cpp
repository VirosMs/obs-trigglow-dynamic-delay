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

// Tests for src/trigi-states.*: what the support dialog's Trigi page shows
// for every documented state (contract A3.9), what each reply offers, the
// question's character limit, the Retry-After countdown and the subject check.

#include "trigi-states.hpp"
#include "test-support.hpp"

#include <string>

using namespace trigglow;
using trigiui::Problem;

namespace {

void TestProblems()
{
	testcheck::g_currentTest = "problems";
	using support::Status;
	CHECK(trigiui::ProblemFor(Status::Ok, 200) == Problem::None);

	// Transport errors: "no connection", or "took too long" when the client
	// itself says the answer didn't come in time (never deduced from the
	// time the call took).
	CHECK(trigiui::ProblemFor(Status::Network, 0) == Problem::Offline);
	CHECK(trigiui::ProblemFor(Status::Timeout, 0) == Problem::Timeout);
	// 5xx: the server answered, so never "no connection".
	CHECK(trigiui::ProblemFor(Status::Network, 502) == Problem::ServerBusy);
	CHECK(trigiui::ProblemFor(Status::Network, 503) == Problem::ServerBusy);

	CHECK(trigiui::ProblemFor(Status::RateLimited, 429) == Problem::RateLimited);
	CHECK(trigiui::ProblemFor(Status::SessionEnded, 401) == Problem::SessionEnded);
	CHECK(trigiui::ProblemFor(Status::SignedOut, 401) == Problem::SessionEnded);
	CHECK(trigiui::ProblemFor(Status::ConversationGone, 409) == Problem::ConversationClosed);
	CHECK(trigiui::ProblemFor(Status::NotFound, 404) == Problem::ConversationClosed);
	CHECK(trigiui::ProblemFor(Status::Disabled, 403) == Problem::Disabled);
	CHECK(trigiui::ProblemFor(Status::Outdated, 426) == Problem::Outdated);
	CHECK(trigiui::ProblemFor(Status::AccountInactive, 403) == Problem::AccountInactive);
	CHECK(trigiui::ProblemFor(Status::Unavailable, 0) == Problem::Disabled);
	for (Status s : {Status::Invalid, Status::ScopeError, Status::BadResponse, Status::CaptchaRequired,
			 Status::AttachmentRejected, Status::TicketClosed})
		CHECK(trigiui::ProblemFor(s, 400) == Problem::Generic);

	// Trigi turned off, too old, or the account not active: the ticket form.
	// Everything else stays on the page with a banner.
	for (Problem p : {Problem::Disabled, Problem::Outdated, Problem::AccountInactive})
		CHECK(trigiui::OpensForm(p));
	for (Problem p : {Problem::None, Problem::Offline, Problem::Timeout, Problem::ServerBusy, Problem::RateLimited,
			  Problem::SessionEnded, Problem::ConversationClosed, Problem::Generic})
		CHECK(!trigiui::OpensForm(p));

	// "Try again" only where trying again can work right away (a manual
	// retry; never automatic). Not during a rate limit, not signed out.
	for (Problem p :
	     {Problem::Offline, Problem::Timeout, Problem::ServerBusy, Problem::ConversationClosed, Problem::Generic})
		CHECK(trigiui::CanRetry(p));
	for (Problem p : {Problem::None, Problem::RateLimited, Problem::SessionEnded, Problem::Disabled,
			  Problem::Outdated, Problem::AccountInactive})
		CHECK(!trigiui::CanRetry(p));

	// Errors are read out at once; the rest politely.
	CHECK(trigiui::IsUrgent(Problem::Offline));
	CHECK(trigiui::IsUrgent(Problem::SessionEnded));
	CHECK(!trigiui::IsUrgent(Problem::RateLimited));
	CHECK(!trigiui::IsUrgent(Problem::ConversationClosed));
}

void TestReplies()
{
	testcheck::g_currentTest = "replies";
	using support::NextStep;
	CHECK(trigiui::ActionFor(NextStep::None) == trigiui::ReplyAction::None);
	CHECK(trigiui::ActionFor(NextStep::TicketHere) == trigiui::ReplyAction::CreateTicket);
	CHECK(trigiui::ActionFor(NextStep::WriteToTeam) == trigiui::ReplyAction::WriteToTeam);
	CHECK(trigiui::ActionFor(NextStep::WebSupport) == trigiui::ReplyAction::OpenWebSupport);

	CHECK(trigiui::OffersFeedback(support::ReplyKind::Answer));
	CHECK(!trigiui::OffersFeedback(support::ReplyKind::Refused));
	CHECK(!trigiui::OffersFeedback(support::ReplyKind::Unavailable));
}

void TestQuestionCount()
{
	testcheck::g_currentTest = "question count";
	CHECK(trigiui::CountQuestion("").empty);
	CHECK(trigiui::CountQuestion(" \n\t\r ").empty);
	const trigiui::QuestionCount hello = trigiui::CountQuestion("  hola  \n");
	CHECK(!hello.empty);
	CHECK(!hello.tooLong);
	CHECK_EQ(hello.chars, 4u);
	// Characters, not bytes: "ñ" and "á" are one each.
	CHECK_EQ(trigiui::CountQuestion("ñá").chars, 2u);

	const std::string atLimit(support::kMaxQuestionChars, 'a');
	CHECK(!trigiui::CountQuestion(atLimit).tooLong);
	CHECK(!trigiui::CountQuestion("  " + atLimit + "\n\n").tooLong); // Trimmed first.
	CHECK(trigiui::CountQuestion(atLimit + "b").tooLong);
	// 1000 two-byte characters are still within the limit.
	std::string accents;
	for (size_t i = 0; i < support::kMaxQuestionChars; ++i)
		accents += "\xC3\xA1";
	CHECK(!trigiui::CountQuestion(accents).tooLong);
	CHECK(trigiui::CountQuestion(accents + "\xC3\xA1").tooLong);
}

void TestCountdown()
{
	testcheck::g_currentTest = "countdown";
	CHECK_STR(trigiui::FormatCountdown(0), "0:00");
	CHECK_STR(trigiui::FormatCountdown(5), "0:05");
	CHECK_STR(trigiui::FormatCountdown(59), "0:59");
	CHECK_STR(trigiui::FormatCountdown(60), "1:00");
	CHECK_STR(trigiui::FormatCountdown(65), "1:05");
	CHECK_STR(trigiui::FormatCountdown(3600), "60:00");
	// Retry-After is capped at an hour; nothing negative.
	CHECK_STR(trigiui::FormatCountdown(7200), "60:00");
	CHECK_STR(trigiui::FormatCountdown(-3), "0:00");
}

void TestSubject()
{
	testcheck::g_currentTest = "subject";
	using trigiui::SubjectCheck;
	CHECK(trigiui::CheckSubject("") == SubjectCheck::UseDefault);
	CHECK(trigiui::CheckSubject("   ") == SubjectCheck::UseDefault);
	CHECK(trigiui::CheckSubject("ab") == SubjectCheck::TooShort);
	CHECK(trigiui::CheckSubject("abc") == SubjectCheck::Ok);
	CHECK(trigiui::CheckSubject("El delay se queda en 0 s") == SubjectCheck::Ok);
	CHECK(trigiui::CheckSubject(std::string(support::kSubjectMaxChars, 'x')) == SubjectCheck::Ok);
	CHECK(trigiui::CheckSubject(std::string(support::kSubjectMaxChars + 1, 'x')) == SubjectCheck::TooLong);
	// Characters, not bytes.
	std::string accents;
	for (size_t i = 0; i < support::kSubjectMaxChars; ++i)
		accents += "\xC3\xB1";
	CHECK(trigiui::CheckSubject(accents) == SubjectCheck::Ok);
}

} // namespace

int main()
{
	TestProblems();
	TestReplies();
	TestQuestionCount();
	TestCountdown();
	TestSubject();
	return testcheck::Finish("trigi_states_tests");
}
