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

// Tests for src/support-client.*: the Trigi conversation calls and support
// tickets, with every documented answer and error of the API.

#include "support-client.hpp"
#include "fake-transport.hpp"

#include <set>
#include <string>

using namespace trigglow;
using support::Status;
using testsupport::FakeTransport;
using testsupport::HeaderValue;
using testsupport::Json;
using testsupport::ScratchDir;

namespace {

class FakeProtector final : public SecretProtector {
public:
	bool Available() const override { return true; }
	bool Protect(const std::string &plain, std::string &sealed) const override
	{
		sealed = "F" + plain;
		return true;
	}
	bool Unprotect(const std::string &sealed, std::string &plain) const override
	{
		plain = sealed.substr(1);
		return true;
	}
};

const char *kSession = "123e4567-e89b-12d3-a456-426614174000";
const char *kTicket = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

struct Fixture {
	ScratchDir dir{"support"};
	FakeTransport transport;
	api::Client client{transport.Fn(), "api.trigglow.com", "0.6.0"};
	TokenStore store{dir.path() / "trigglow-auth.dat", {}, std::make_unique<FakeProtector>()};
	support::Context ctx{client, store};

	Fixture() { store.Save(PluginToken{"tgp_tok", "", "2999-01-01T00:00:00Z", "N"}); }
};

std::set<std::string> KeysOf(const json::Value &v)
{
	return std::set<std::string>(v.Keys().begin(), v.Keys().end());
}

void TestCreateSession()
{
	testcheck::g_currentTest = "create session";
	{
		Fixture f;
		f.transport.Push(Json(201, std::string("{\"sessionId\":\"") + kSession + "\",\"available\":true}"));
		const support::SessionResult r = support::CreateSession(f.ctx, "es-ES", std::nullopt);
		CHECK(r.Ok() && r.available);
		CHECK_STR(r.sessionId, kSession);
		const http::Request req = f.transport.Last();
		CHECK_STR(req.path, "/api/support/bot/sessions");
		CHECK_STR(HeaderValue(req, "Authorization"), "Bearer tgp_tok");
		const auto body = json::Parse(req.body);
		CHECK(body && KeysOf(*body) == std::set<std::string>({"locale", "client", "noticeVersion"}));
		CHECK_STR(body->GetString("locale").value_or(""), "es-ES");
		CHECK(body->GetNumber("noticeVersion").value_or(0) == 1);
		CHECK(KeysOf(*body->Find("client")) == std::set<std::string>({"kind", "version", "os"}));
	}
	{
		// With diagnostics, exactly one more key.
		Fixture f;
		f.transport.Push(Json(201, std::string("{\"sessionId\":\"") + kSession + "\",\"available\":false}"));
		json::Value diag = json::Value::Object();
		diag.Set("v", json::Value::Number(1));
		const support::SessionResult r = support::CreateSession(f.ctx, "pt_BR", diag);
		CHECK(r.Ok() && !r.available);
		const auto body = json::Parse(f.transport.Last().body);
		CHECK(body &&
		      KeysOf(*body) == std::set<std::string>({"locale", "client", "noticeVersion", "diagnostics"}));
		CHECK_STR(body->GetString("locale").value_or(""), "pt-BR");
	}
	struct Case {
		testsupport::Reply reply;
		Status expected;
	};
	testsupport::Reply limited = Json(429, R"({"error":"rate_limited"})");
	limited.retryAfter = "90";
	const Case cases[] = {
		{Json(201, R"({"sessionId":"not-a-uuid","available":true})"), Status::BadResponse},
		{Json(201, std::string("{\"sessionId\":\"") + kSession + "\"}"), Status::BadResponse},
		{Json(403, R"({"error":"client_disabled"})"), Status::Disabled},
		{Json(426, R"({"error":"client_outdated","minVersion":"0.6.2"})"), Status::Outdated},
		{limited, Status::RateLimited},
		{Json(400, R"({"error":"invalid_body"})"), Status::Invalid},
		{Json(500, "{}"), Status::Network},
		{testsupport::NetworkDown(), Status::Network},
		{Json(301, ""), Status::BadResponse},
	};
	for (const Case &c : cases) {
		Fixture f;
		f.transport.Push(c.reply);
		const support::SessionResult r = support::CreateSession(f.ctx, "en-US", std::nullopt);
		CHECK(r.status == c.expected);
		if (c.expected == Status::Outdated)
			CHECK_STR(r.minVersion, "0.6.2");
		if (c.expected == Status::RateLimited)
			CHECK(r.retryAfterSeconds == 90);
	}
	{
		Fixture f;
		f.transport.Push(Json(401, R"({"error":"unauthorized"})"));
		const support::SessionResult r = support::CreateSession(f.ctx, "en-US", std::nullopt);
		CHECK(r.status == Status::SessionEnded && r.authEvents.sessionEnded && !f.store.HasToken());
		// Signed out now: no request.
		const size_t before = f.transport.Count();
		CHECK(support::CreateSession(f.ctx, "en-US", std::nullopt).status == Status::SignedOut);
		CHECK(f.transport.Count() == before);
	}
	{
		Fixture f;
		f.transport.Push(Json(403, R"({"error":"account_inactive"})"));
		const support::SessionResult r = support::CreateSession(f.ctx, "en-US", std::nullopt);
		CHECK(r.status == Status::AccountInactive && r.authEvents.accountInactive);
	}
}

void TestReplies()
{
	testcheck::g_currentTest = "replies";
	support::ReplyResult r;
	const auto body1 = json::Parse(R"({"reply":"Hola **x**","action":null})");
	CHECK(support::ParseReplyBody(*body1, r));
	CHECK(r.kind == support::ReplyKind::Answer && r.next == support::NextStep::None && r.lines.size() == 1);
	CHECK(r.replyChars == 10);
	const auto body2 = json::Parse(R"({"reply":"a","action":{"type":"ticket","category":"dynamic_delay"}})");
	CHECK(support::ParseReplyBody(*body2, r));
	CHECK(r.next == support::NextStep::TicketHere);
	const auto body3 = json::Parse(R"({"reply":"a","action":{"type":"human"}})");
	CHECK(support::ParseReplyBody(*body3, r));
	CHECK(r.next == support::NextStep::TicketHere);
	for (const char *other : {"billing", "paid_alert", "overlay", "kick", "account", "bug", "stream_deck", "idea",
				  "other", "something_new"}) {
		const std::string body =
			std::string(R"({"reply":"a","action":{"type":"ticket","category":")") + other + "\"}}";
		CHECK(support::ParseReplyBody(*json::Parse(body), r));
		CHECK(r.next == support::NextStep::WebSupport);
	}
	const auto body4 = json::Parse(R"({"reply":null,"refused":"opaque","action":null})");
	CHECK(support::ParseReplyBody(*body4, r));
	CHECK(r.kind == support::ReplyKind::Refused && r.next == support::NextStep::None && r.lines.empty());
	const auto body5 = json::Parse(R"({"reply":null,"refused":"x","action":{"type":"human"}})");
	CHECK(support::ParseReplyBody(*body5, r));
	CHECK(r.kind == support::ReplyKind::Refused && r.next == support::NextStep::WriteToTeam);
	const auto body6 = json::Parse(
		R"({"reply":null,"unavailable":"unavailable","action":{"type":"human"},"intakeReady":true})");
	CHECK(support::ParseReplyBody(*body6, r));
	CHECK(r.kind == support::ReplyKind::Unavailable && r.next == support::NextStep::TicketHere);
	const auto body7 = json::Parse(R"({"reply":null})");
	CHECK(!support::ParseReplyBody(*body7, r));
	const auto body8 = json::Parse(R"({"reply":42})");
	CHECK(!support::ParseReplyBody(*body8, r));
	CHECK(!support::ParseReplyBody(*json::Parse("[]"), r));
	// Hostile replies come back sanitized and parsed, never raw.
	const std::string slash(1, '\\');
	const auto hostile = json::Parse("{\"reply\":\"<img src=x onerror=alert(1)>" + slash + "u202e" + slash +
					 "ud83d" + slash + "ude00\"}");
	CHECK(support::ParseReplyBody(*hostile, r));
	CHECK_STR(chat::LineHtml(r.lines.at(0)), "&lt;img src=x onerror=alert(1)&gt;");
}

void TestAsk()
{
	testcheck::g_currentTest = "ask";
	{
		Fixture f;
		f.transport.Push(Json(200, R"({"reply":"- Paso uno\n- Paso dos","action":null})"));
		const support::ReplyResult r = support::Ask(f.ctx, kSession, "  Why is the delay grey?  ");
		CHECK(r.Ok() && r.kind == support::ReplyKind::Answer && r.lines.size() == 2);
		const http::Request req = f.transport.Last();
		CHECK_STR(req.path, std::string("/api/support/bot/sessions/") + kSession + "/messages");
		CHECK_STR(req.body, "{\"message\":\"Why is the delay grey?\"}");
		CHECK(req.timeouts.receiveMs == 90000);
	}
	{
		// Checked before sending: empty, too long, a bad session id.
		Fixture f;
		CHECK(support::Ask(f.ctx, kSession, "   ").status == Status::Invalid);
		CHECK(support::Ask(f.ctx, kSession, std::string(1001, 'a')).status == Status::Invalid);
		std::string accents;
		for (int i = 0; i < 1000; ++i)
			accents += "\xC3\xA9";
		f.transport.Push(Json(200, R"({"reply":"ok"})"));
		CHECK(support::Ask(f.ctx, kSession, accents).Ok()); // 1000 characters, 2000 bytes.
		CHECK(support::Ask(f.ctx, "../../tickets", "hi").status == Status::Invalid);
		CHECK(f.transport.Count() == 1);
	}
	struct Case {
		testsupport::Reply reply;
		Status expected;
	};
	const Case cases[] = {
		{Json(404, R"({"error":"not_found"})"), Status::ConversationGone},
		{Json(409, R"({"error":"session_closed"})"), Status::ConversationGone},
		{Json(403, R"({"error":"client_disabled"})"), Status::Disabled},
		{Json(429, R"({"error":"rate_limited"})"), Status::RateLimited},
		{Json(200, R"({"something":"else"})"), Status::BadResponse},
		{Json(200, "not json"), Status::BadResponse},
		{Json(403, R"({"error":"insufficient_scope"})"), Status::ScopeError},
	};
	for (const Case &c : cases) {
		Fixture f;
		f.transport.Push(c.reply);
		const support::ReplyResult r = support::Ask(f.ctx, kSession, "hi");
		CHECK(r.status == c.expected);
		if (c.expected == Status::RateLimited)
			CHECK(r.retryAfterSeconds == 60);
		if (c.expected == Status::ScopeError)
			CHECK(f.store.HasToken() && r.authEvents.scopeError);
	}
}

void TestSummaryAndEvents()
{
	testcheck::g_currentTest = "summary and events";
	{
		Fixture f;
		const std::string slash(1, '\\');
		f.transport.Push(Json(200, "{\"subject\":\"Delay grey " + slash + "ud83d" + slash +
						   "ude00\",\"message\":\"Line 1" + slash + "nLine 2\",\"ai\":true}"));
		const support::SummaryResult r = support::Summarize(f.ctx, kSession);
		CHECK(r.Ok() && r.ai);
		CHECK_STR(r.subject, "Delay grey ");
		CHECK_STR(r.message, "Line 1\nLine 2");
		CHECK_STR(f.transport.Last().body, "{\"category\":\"dynamic_delay\"}");
		CHECK(f.transport.Last().timeouts.receiveMs == 90000);
		f.transport.Push(Json(200, R"({"subject":"x"})"));
		CHECK(support::Summarize(f.ctx, kSession).status == Status::BadResponse);
	}
	CHECK_STR(json::Serialize(support::EventBody(support::Event::HelpfulYes)),
		  R"({"kind":"feedback","meta":{"helpful":true}})");
	CHECK_STR(json::Serialize(support::EventBody(support::Event::HelpfulNo)),
		  R"({"kind":"feedback","meta":{"helpful":false}})");
	CHECK_STR(json::Serialize(support::EventBody(support::Event::CloseResolved)),
		  R"({"kind":"close","meta":{"status":"resolved"}})");
	CHECK_STR(json::Serialize(support::EventBody(support::Event::CloseAbandoned)),
		  R"({"kind":"close","meta":{"status":"abandoned"}})");
	CHECK_STR(json::Serialize(support::EventBody(support::Event::HandoffClicked)), R"({"kind":"handoff_clicked"})");
	{
		Fixture f;
		f.transport.Push(Json(200, R"({"ok":true})"));
		CHECK(support::SendEvent(f.ctx, kSession, support::Event::CloseAbandoned).Ok());
		CHECK_STR(f.transport.Last().path, std::string("/api/support/bot/sessions/") + kSession + "/events");
		CHECK(f.transport.Last().timeouts.totalMs > 0 && f.transport.Last().timeouts.totalMs <= 5000);
	}
}

void TestTickets()
{
	testcheck::g_currentTest = "tickets";
	CHECK_STR(support::TicketFooter("0.6.0-rc2", "31.0.2"),
		  std::string("\n\n--\nDynamic Delay 0.6.0 \xC2\xB7 OBS 31.0.2 \xC2\xB7 ") +
			  (api::CurrentOs() == api::Os::Windows ? "Windows"
			   : api::CurrentOs() == api::Os::Macos ? "macOS"
								: "Linux"));
	{
		Fixture f;
		f.transport.Push(
			Json(201, std::string("{\"id\":\"") + kTicket + "\",\"subject\":\"x\",\"status\":\"open\"}"));
		const support::TicketResult r = support::CreateTicket(f.ctx, " Subject ", "Message", kSession);
		CHECK(r.Ok());
		CHECK_STR(r.ticketId, kTicket);
		const auto body = json::Parse(f.transport.Last().body);
		CHECK(body &&
		      KeysOf(*body) == std::set<std::string>({"subject", "category", "message", "botSessionId"}));
		CHECK_STR(body->GetString("category").value_or(""), "dynamic_delay");
		CHECK_STR(body->GetString("subject").value_or(""), "Subject");
		CHECK_STR(f.transport.Last().path, "/api/support/tickets");
		CHECK_STR(HeaderValue(f.transport.Last(), "Authorization"), "Bearer tgp_tok");

		// Without a conversation: no botSessionId key at all.
		f.transport.Push(Json(201, std::string("{\"id\":\"") + kTicket + "\"}"));
		CHECK(support::CreateTicket(f.ctx, "Subject", "Message").Ok());
		CHECK(!json::Parse(f.transport.Last().body)->Has("botSessionId"));

		// The log attachment.
		f.transport.Push(Json(201, "{}"));
		CHECK(support::AttachLog(f.ctx, kTicket, "redacted log").Ok());
		const http::Request up = f.transport.Last();
		CHECK_STR(up.path, std::string("/api/support/tickets/") + kTicket + "/attachments");
		CHECK_STR(HeaderValue(up, "Authorization"), "Bearer tgp_tok");
		CHECK(up.body.find("filename=\"obs-log.txt\"") != std::string::npos);
		CHECK(up.body.find("redacted log") != std::string::npos);
	}
	{
		Fixture f;
		CHECK(support::CreateTicket(f.ctx, "ab", "Message").status == Status::Invalid);
		CHECK(support::CreateTicket(f.ctx, std::string(201, 's'), "Message").status == Status::Invalid);
		CHECK(support::CreateTicket(f.ctx, "Subject", "").status == Status::Invalid);
		CHECK(support::CreateTicket(f.ctx, "Subject", std::string(6001, 'm')).status == Status::Invalid);
		CHECK(support::CreateTicket(f.ctx, "Subject", "Message", "bad").status == Status::Invalid);
		CHECK(f.transport.Count() == 0);
		for (const char *code :
		     {"file_too_large", "invalid_type", "invalid_mime", "attachment_limit", "missing_file"}) {
			f.transport.Push(Json(400, std::string("{\"error\":\"") + code + "\"}"));
			CHECK(support::AttachLog(f.ctx, kTicket, "x").status == Status::AttachmentRejected);
		}
		f.transport.Push(Json(409, R"({"error":"ticket_closed"})"));
		CHECK(support::AttachLog(f.ctx, kTicket, "x").status == Status::TicketClosed);
		f.transport.Push(Json(404, R"({"error":"not_found"})"));
		CHECK(support::AttachLog(f.ctx, kTicket, "x").status == Status::NotFound);
		f.transport.Push(Json(201, R"({"id":"nope"})"));
		CHECK(support::CreateTicket(f.ctx, "Subject", "Message").status == Status::BadResponse);
	}
}

void TestPublicTickets()
{
	testcheck::g_currentTest = "public tickets";
	FakeTransport t;
	const api::Client client(t.Fn(), "api.trigglow.com", "0.6.0");

	t.Push(Json(200, R"({"challengeSiteKey":null})"));
	support::PublicConfigResult config = support::FetchPublicConfig(client);
	CHECK(config.Ok() && !config.challengeRequired);
	CHECK_STR(t.Last().path, "/api/support/public/config");
	t.Push(Json(200, R"({"challengeSiteKey":"0x4AAAA-site"})"));
	CHECK(support::FetchPublicConfig(client).challengeRequired);
	t.Push(Json(200, R"({"challengeSiteKey":""})"));
	CHECK(!support::FetchPublicConfig(client).challengeRequired);
	t.Push(Json(200, "{}"));
	CHECK(!support::FetchPublicConfig(client).challengeRequired);
	t.Push(testsupport::NetworkDown());
	CHECK(support::FetchPublicConfig(client).status == Status::Network);

	t.Push(Json(201,
		    std::string("{\"ticketId\":\"") + kTicket + "\",\"accessToken\":\"deadbeef01\",\"ticket\":{}}"));
	const support::PublicTicketResult r =
		support::CreatePublicTicket(client, "Ana", "ana@example.com", "Subject", "Something broke");
	CHECK(r.Ok());
	CHECK_STR(r.ticketId, kTicket);
	CHECK_STR(r.accessToken, "deadbeef01");
	CHECK(!testsupport::HasHeader(t.Last(), "Authorization"));
	const auto body = json::Parse(t.Last().body);
	CHECK(body &&
	      KeysOf(*body) == std::set<std::string>({"guestName", "guestEmail", "subject", "category", "message"}));

	t.Push(Json(201, "{}"));
	CHECK(support::AttachLogPublic(client, r.ticketId, r.accessToken, "log").Ok());
	const http::Request up = t.Last();
	CHECK_STR(up.path, std::string("/api/support/public/tickets/") + kTicket + "/attachments");
	CHECK(up.path.find("deadbeef01") == std::string::npos); // Header, never the URL.
	CHECK_STR(HeaderValue(up, "X-Ticket-Token"), "deadbeef01");
	CHECK(!testsupport::HasHeader(up, "Authorization"));

	t.Push(Json(400, R"({"error":"captcha_failed"})"));
	CHECK(support::CreatePublicTicket(client, "Ana", "ana@example.com", "Subject", "Something broke").status ==
	      Status::CaptchaRequired);
	const size_t before = t.Count();
	CHECK(support::CreatePublicTicket(client, "A", "ana@example.com", "Subject", "Something broke").status ==
	      Status::Invalid);
	CHECK(support::CreatePublicTicket(client, "Ana", "not-an-email", "Subject", "Something broke").status ==
	      Status::Invalid);
	CHECK(support::CreatePublicTicket(client, "Ana", "ana@example.com", "Subject", "Hi").status == Status::Invalid);
	CHECK(t.Count() == before);
}

} // namespace

int main()
{
	TestCreateSession();
	TestReplies();
	TestAsk();
	TestSummaryAndEvents();
	TestTickets();
	TestPublicTickets();
	return testcheck::Finish("support_client_tests");
}
