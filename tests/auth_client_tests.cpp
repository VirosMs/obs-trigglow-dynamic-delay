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

// Tests for src/auth-client.*: linking OBS to an account, the plugin
// token's start check, renewal, sign-out and the one-time migration of a
// v0.5.x sign-in, plus the 401/403 rules of every authenticated call.

#include "auth-client.hpp"
#include "fake-transport.hpp"

#include <fstream>
#include <string>

using namespace trigglow;
using testsupport::FakeTransport;
using testsupport::HeaderValue;
using testsupport::Json;
using testsupport::ScratchDir;
using testsupport::Url;

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
		if (sealed.empty() || sealed[0] != 'F')
			return false;
		plain = sealed.substr(1);
		return true;
	}
};

struct Fixture {
	ScratchDir dir{"auth"};
	FakeTransport transport;
	api::Client client{transport.Fn(), "api.trigglow.com", "0.6.0"};
	TokenStore store{dir.path() / "trigglow-auth.dat", dir.path() / "trigglow-account.json",
			 std::make_unique<FakeProtector>()};

	void SignIn(const std::string &tok, const std::string &renewAfter = "2999-01-01T00:00:00Z")
	{
		store.Save(PluginToken{tok, "2999-02-01T00:00:00Z", renewAfter, "Name"});
	}
};

const std::string kLink = "LinkCode_0123456789abcdefghijklmnopqrstuv";
const std::string kPoll = "PollSecret_0123456789abcdefghijklmnopqrst";

std::string StartBody(const std::string &url, const std::string &userCode = "KD7F-29QM")
{
	return "{\"linkCode\":\"" + kLink + "\",\"userCode\":\"" + userCode + "\",\"pollSecret\":\"" + kPoll +
	       "\",\"verificationUrl\":\"" + url + "\",\"expiresIn\":600,\"interval\":3}";
}

void TestStart()
{
	testcheck::g_currentTest = "start";
	Fixture f;
	const std::string page = Url("https", "www.trigglow.com/plugin/link?code=" + kLink);
	f.transport.Push(Json(200, StartBody(page)));
	auth::StartResult r = auth::StartLink(f.client);
	CHECK(r.status == auth::StartStatus::Started);
	CHECK_STR(r.link.userCode, "KD7F-29QM");
	CHECK_STR(r.link.pollSecret, kPoll);
	CHECK_STR(r.link.verificationUrl, page);
	CHECK(r.link.expiresInSeconds == 600 && r.link.intervalSeconds == 3);
	const http::Request req = f.transport.Last();
	CHECK_STR(req.path, "/auth/plugin/start");
	CHECK(!testsupport::HasHeader(req, "Authorization"));
	const auto body = json::Parse(req.body);
	CHECK(body && body->GetString("app").value_or("") == "dynamic-delay");
	CHECK(body && body->Find("client") &&
	      body->Find("client")->GetString("kind").value_or("") == "obs_dynamic_delay");

	// Clamping of the numbers.
	f.transport.Push(Json(200, "{\"linkCode\":\"" + kLink + "\",\"userCode\":\"AAAA-0000\",\"pollSecret\":\"" +
					   kPoll + "\",\"verificationUrl\":\"" + page +
					   "\",\"expiresIn\":99999,\"interval\":0}"));
	r = auth::StartLink(f.client);
	CHECK(r.status == auth::StartStatus::Started && r.link.expiresInSeconds == 1800 && r.link.intervalSeconds == 2);

	// Anything off is a clean failure.
	const std::string foreign = Url("https", "www.trigglow.com.example.com/plugin/link?code=x");
	for (const std::string &bad :
	     {StartBody(foreign), StartBody(page, "kd7f-29qm"),
	      StartBody(Url("http", "www.trigglow.com/plugin/link?code=x")),
	      StartBody(Url("https", "www.trigglow.com/app/support?code=x")), std::string("{\"linkCode\":\"short\"}"),
	      std::string("[]"), std::string("not json")}) {
		f.transport.Push(Json(200, bad));
		CHECK(auth::StartLink(f.client).status == auth::StartStatus::Failed);
	}
	// A body without the numbers.
	f.transport.Push(Json(200, "{\"linkCode\":\"" + kLink + "\",\"userCode\":\"KD7F-29QM\",\"pollSecret\":\"" +
					   kPoll + "\",\"verificationUrl\":\"" + page + "\"}"));
	CHECK(auth::StartLink(f.client).status == auth::StartStatus::Failed);

	testsupport::Reply limited = Json(429, R"({"error":"rate_limited"})");
	limited.retryAfter = "30";
	f.transport.Push(limited);
	r = auth::StartLink(f.client);
	CHECK(r.status == auth::StartStatus::RateLimited && r.retryAfterSeconds == 30);
	f.transport.Push(Json(426, R"({"error":"client_outdated","minVersion":"0.7.0"})"));
	r = auth::StartLink(f.client);
	CHECK(r.status == auth::StartStatus::Outdated);
	CHECK_STR(r.minVersion, "0.7.0");
	f.transport.Push(Json(503, ""));
	CHECK(auth::StartLink(f.client).status == auth::StartStatus::Network);
	f.transport.Push(Json(400, R"({"error":"invalid_body"})"));
	CHECK(auth::StartLink(f.client).status == auth::StartStatus::Failed);
	f.transport.Push(testsupport::NetworkDown());
	CHECK(auth::StartLink(f.client).status == auth::StartStatus::Network);
}

void TestPoll()
{
	testcheck::g_currentTest = "poll";
	Fixture f;
	auth::LinkRequest link;
	link.linkCode = kLink;
	link.pollSecret = kPoll;

	f.transport.Push(Json(200, R"({"status":"pending"})"));
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::Pending);
	// The secret travels in the body, never in the URL.
	const http::Request req = f.transport.Last();
	CHECK_STR(req.path, "/auth/plugin/token");
	CHECK(req.path.find(kPoll) == std::string::npos && req.path.find(kLink) == std::string::npos);
	const auto body = json::Parse(req.body);
	CHECK(body && body->GetString("pollSecret").value_or("") == kPoll);
	CHECK(body && body->GetString("linkCode").value_or("") == kLink);

	f.transport.Push(Json(
		200,
		R"({"status":"approved","token":"tgp_new-Token","expiresAt":"2026-12-01T00:00:00Z","renewAfter":"2026-11-01T00:00:00Z"})"));
	auth::PollResult r = auth::PollLink(f.client, link);
	CHECK(r.status == auth::PollStatus::Approved);
	CHECK_STR(r.token.token, "tgp_new-Token");
	CHECK_STR(r.token.renewAfter, "2026-11-01T00:00:00Z");

	f.transport.Push(Json(200, R"({"status":"approved","token":"bad token"})"));
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::Failed);
	f.transport.Push(Json(200, R"({"status":"denied"})"));
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::Denied);
	f.transport.Push(Json(404, R"({"status":"expired"})"));
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::Expired);
	f.transport.Push(Json(429, R"({"error":"slow_down"})"));
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::SlowDown);
	f.transport.Push(Json(502, ""));
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::Network);
	f.transport.Push(testsupport::NetworkDown());
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::Network);
	f.transport.Push(Json(400, R"({"error":"invalid_body"})"));
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::Failed);
	f.transport.Push(Json(200, R"({"status":"something_new"})"));
	CHECK(auth::PollLink(f.client, link).status == auth::PollStatus::Pending);
}

api::Result CallMe(Fixture &f, auth::AuthEvents &events, std::time_t now = 1000)
{
	return auth::CallAuthorized(
		f.client, f.store, [&f](const std::string &tok) { return f.client.Get("/auth/plugin/me", tok); },
		events, now);
}

void TestAuthorizedCalls()
{
	testcheck::g_currentTest = "authorized calls";
	{
		Fixture f;
		auth::AuthEvents events;
		const api::Result r = CallMe(f, events);
		CHECK(r.Is(401, "signed_out"));
		CHECK(f.transport.Count() == 0); // Signed out: no request at all.
	}
	{
		Fixture f;
		f.SignIn("tgp_one");
		f.transport.Push(Json(200, "{}"));
		auth::AuthEvents events;
		CHECK(CallMe(f, events).Ok());
		CHECK_STR(HeaderValue(f.transport.Last(), "Authorization"), "Bearer tgp_one");
	}
	{
		// Any 401 other than token_rotated ends the session.
		Fixture f;
		f.SignIn("tgp_one");
		f.transport.Push(Json(401, R"({"error":"unauthorized"})"));
		auth::AuthEvents events;
		CHECK(CallMe(f, events).Is(401));
		CHECK(events.sessionEnded);
		CHECK(!f.store.HasToken());
		CHECK(!std::filesystem::exists(f.dir.path() / "trigglow-auth.dat"));
	}
	{
		// token_rotated, and the file already holds a newer token: one retry with it.
		Fixture f;
		f.SignIn("tgp_old");
		TokenStore other(f.dir.path() / "trigglow-auth.dat", {}, std::make_unique<FakeProtector>());
		f.transport.handler = [&](const http::Request &req) {
			if (HeaderValue(req, "Authorization") == "Bearer tgp_old") {
				other.Save(PluginToken{"tgp_newer", "", "", "Name"});
				return Json(401, R"({"error":"token_rotated"})");
			}
			return Json(200, "{}");
		};
		auth::AuthEvents events;
		CHECK(CallMe(f, events).Ok());
		CHECK(f.transport.Count() == 2);
		CHECK_STR(HeaderValue(f.transport.Last(), "Authorization"), "Bearer tgp_newer");
		CHECK(!events.sessionEnded);
	}
	{
		// token_rotated with the same token on file: renew (grace period), then one retry.
		Fixture f;
		f.SignIn("tgp_old");
		f.transport.handler = [&](const http::Request &req) {
			if (req.path == "/auth/plugin/token/renew")
				return Json(
					200,
					R"({"token":"tgp_renewed","expiresAt":"2999-01-01T00:00:00Z","renewAfter":"2998-01-01T00:00:00Z"})");
			if (HeaderValue(req, "Authorization") == "Bearer tgp_old")
				return Json(401, R"({"error":"token_rotated"})");
			return Json(200, "{}");
		};
		auth::AuthEvents events;
		CHECK(CallMe(f, events).Ok());
		CHECK(f.transport.Count() == 3);
		CHECK(events.tokenRenewed);
		CHECK_STR(f.store.CurrentToken(), "tgp_renewed");
		CHECK_STR(f.store.Current()->displayName, "Name");
		CHECK_STR(HeaderValue(f.transport.Last(), "Authorization"), "Bearer tgp_renewed");
	}
	{
		// Never a loop: token_rotated again after the retry stops there, sign-in kept.
		Fixture f;
		f.SignIn("tgp_old");
		f.transport.handler = [&](const http::Request &req) {
			if (req.path == "/auth/plugin/token/renew")
				return Json(200, R"({"token":"tgp_renewed"})");
			return Json(401, R"({"error":"token_rotated"})");
		};
		auth::AuthEvents events;
		CHECK(CallMe(f, events).Is(401, "token_rotated"));
		CHECK(f.transport.Count() == 3);
		CHECK(f.store.HasToken());
	}
	{
		// 403s: account not active / plugin bug. The sign-in stays.
		Fixture f;
		f.SignIn("tgp_one");
		f.transport.Push(Json(403, R"({"error":"suspended"})"));
		auth::AuthEvents events;
		CallMe(f, events);
		CHECK(events.accountInactive && !events.sessionEnded && f.store.HasToken());
		f.transport.Push(Json(403, R"({"error":"insufficient_scope"})"));
		auth::AuthEvents scope;
		CallMe(f, scope);
		CHECK(scope.scopeError && !scope.sessionEnded && f.store.HasToken());
	}
	{
		// A due renewal runs before the call.
		Fixture f;
		f.SignIn("tgp_due", "1970-01-01T00:00:10Z");
		f.transport.handler = [&](const http::Request &req) {
			if (req.path == "/auth/plugin/token/renew")
				return Json(
					200,
					R"({"token":"tgp_fresh","expiresAt":"x","renewAfter":"2999-01-01T00:00:00Z"})");
			return Json(200, "{}");
		};
		auth::AuthEvents events;
		CHECK(CallMe(f, events, 1000).Ok());
		CHECK(f.transport.Count() == 2);
		CHECK_STR(f.transport.Requests()[0].path, "/auth/plugin/token/renew");
		CHECK_STR(HeaderValue(f.transport.Requests()[0], "Authorization"), "Bearer tgp_due");
		CHECK_STR(HeaderValue(f.transport.Last(), "Authorization"), "Bearer tgp_fresh");
		CHECK(events.tokenRenewed);
	}
}

void TestRenew()
{
	testcheck::g_currentTest = "renew";
	{
		Fixture f;
		f.SignIn("tgp_a");
		f.transport.Push(Json(401, R"({"error":"unauthorized"})"));
		auth::AuthEvents events;
		CHECK(auth::Renew(f.client, f.store, events) == auth::RenewStatus::SessionEnded);
		CHECK(events.sessionEnded && !f.store.HasToken());
	}
	{
		Fixture f;
		f.SignIn("tgp_a");
		f.transport.Push(testsupport::NetworkDown());
		auth::AuthEvents events;
		CHECK(auth::Renew(f.client, f.store, events) == auth::RenewStatus::Failed);
		CHECK_STR(f.store.CurrentToken(), "tgp_a");
		CHECK_STR(f.transport.Last().body, "{}");
	}
	{
		// Someone else already rotated it: no request.
		Fixture f;
		f.SignIn("tgp_b");
		auth::AuthEvents events;
		CHECK(auth::Renew(f.client, f.store, events, "tgp_a") == auth::RenewStatus::Renewed);
		CHECK(f.transport.Count() == 0);
	}
	{
		Fixture f;
		f.SignIn("tgp_a", "not a date");
		auth::AuthEvents events;
		CHECK(auth::RenewIfDue(f.client, f.store, events, 5000000000) == auth::RenewStatus::NotDue);
	}
}

void TestCheckSession()
{
	testcheck::g_currentTest = "check session";
	{
		Fixture f;
		f.SignIn("tgp_a");
		f.transport.Push(Json(200, R"({"displayName":"Streamer","account":{"status":"active"},
			"session":{"expiresAt":"2999-03-01T00:00:00Z","renewAfter":"2999-02-15T00:00:00Z"},
			"trigi":{"available":true,"minVersion":"0.6.0","diagnosticsVersions":[1]}})"));
		const auth::MeResult r = auth::CheckSession(f.client, f.store, 1000);
		CHECK(r.status == auth::MeStatus::Valid);
		CHECK_STR(r.displayName, "Streamer");
		CHECK(r.trigi.known && r.trigi.available && r.trigi.AcceptsDiagnostics(1) &&
		      !r.trigi.AcceptsDiagnostics(2));
		CHECK(r.trigi.Usable("0.6.0") && r.trigi.Usable("0.6.1") && !r.trigi.Usable("0.5.9"));
		CHECK_STR(f.store.Current()->renewAfter, "2999-02-15T00:00:00Z");
		CHECK_STR(f.store.Current()->displayName, "Streamer");
		CHECK_STR(f.transport.Last().path, "/auth/plugin/me");
	}
	{
		// /me first, then the renewal (the start order).
		Fixture f;
		f.SignIn("tgp_a", "1970-01-01T00:00:00Z");
		f.transport.handler = [&](const http::Request &req) {
			if (req.path == "/auth/plugin/token/renew")
				return Json(200, R"({"token":"tgp_b","expiresAt":"","renewAfter":""})");
			return Json(200, R"({"displayName":"S","trigi":{"available":false}})");
		};
		const auth::MeResult r = auth::CheckSession(f.client, f.store, 1000);
		CHECK(r.status == auth::MeStatus::Valid && r.events.tokenRenewed);
		CHECK(f.transport.Count() == 2);
		CHECK_STR(f.transport.Requests()[0].path, "/auth/plugin/me");
		CHECK_STR(f.transport.Requests()[1].path, "/auth/plugin/token/renew");
		CHECK_STR(f.store.CurrentToken(), "tgp_b");
		CHECK(!r.trigi.Usable("0.6.0"));
	}
	struct Case {
		testsupport::Reply reply;
		auth::MeStatus expected;
		bool tokenKept;
	};
	const Case cases[] = {
		{Json(401, R"({"error":"unauthorized"})"), auth::MeStatus::SessionEnded, false},
		{Json(403, R"({"error":"account_inactive"})"), auth::MeStatus::AccountInactive, true},
		{Json(404, R"({"error":"not_found"})"), auth::MeStatus::Unreachable, true},
		{Json(500, ""), auth::MeStatus::Unreachable, true},
		{testsupport::NetworkDown(), auth::MeStatus::Unreachable, true},
		{Json(418, "{}"), auth::MeStatus::Failed, true},
	};
	for (const Case &c : cases) {
		Fixture f;
		f.SignIn("tgp_a");
		f.transport.Push(c.reply);
		const auth::MeResult r = auth::CheckSession(f.client, f.store, 1000);
		CHECK(r.status == c.expected);
		CHECK(f.store.HasToken() == c.tokenKept);
	}
	auth::TrigiInfo noMin;
	noMin.known = noMin.available = true;
	CHECK(noMin.Usable("0.6.0"));
	CHECK(!auth::TrigiInfo{}.Usable("9.9.9"));
}

void WriteLegacy(Fixture &f, const std::string &tok)
{
	std::ofstream out(f.dir.path() / "trigglow-account.json", std::ios::binary);
	out << "{\"token\": \"" << tok << "\", \"display_name\": \"Old\", \"email\": \"\"}";
}

void TestExchange()
{
	testcheck::g_currentTest = "legacy exchange";
	{
		Fixture f;
		CHECK(auth::ExchangeLegacy(f.client, f.store) == auth::ExchangeStatus::NoLegacy);
		CHECK(f.transport.Count() == 0);
	}
	{
		Fixture f;
		WriteLegacy(f, "OldSession_abc");
		f.transport.Push(Json(
			200,
			R"({"token":"tgp_migrated","expiresAt":"2999-01-01T00:00:00Z","renewAfter":"2998-01-01T00:00:00Z"})"));
		CHECK(auth::ExchangeLegacy(f.client, f.store) == auth::ExchangeStatus::Migrated);
		const http::Request req = f.transport.Last();
		CHECK_STR(req.path, "/auth/plugin/token/exchange");
		CHECK_STR(HeaderValue(req, "Authorization"), "Bearer OldSession_abc");
		const auto body = json::Parse(req.body);
		CHECK(body && body->Find("client") && body->Keys().size() == 1);
		CHECK_STR(f.store.CurrentToken(), "tgp_migrated");
		CHECK_STR(f.store.Current()->displayName, "Old");
		CHECK(!f.store.HasLegacy());
		CHECK(std::filesystem::exists(f.dir.path() / "trigglow-auth.dat"));
	}
	{
		Fixture f;
		WriteLegacy(f, "AlreadyPlugin_xyz");
		f.transport.Push(Json(409, R"({"error":"already_migrated"})"));
		CHECK(auth::ExchangeLegacy(f.client, f.store) == auth::ExchangeStatus::AlreadyMigrated);
		CHECK_STR(f.store.CurrentToken(), "AlreadyPlugin_xyz");
		CHECK(!f.store.HasLegacy());
	}
	{
		Fixture f;
		WriteLegacy(f, "Revoked_1");
		f.transport.Push(Json(401, R"({"error":"unauthorized"})"));
		CHECK(auth::ExchangeLegacy(f.client, f.store) == auth::ExchangeStatus::SessionEnded);
		CHECK(!f.store.HasLegacy() && !f.store.HasToken());
	}
	for (const testsupport::Reply &later :
	     {Json(404, "{}"), Json(500, ""), Json(429, R"({"error":"rate_limited"})"), testsupport::NetworkDown(),
	      Json(200, R"({"token":""})")}) {
		Fixture f;
		WriteLegacy(f, "Keep_1");
		f.transport.Push(later);
		CHECK(auth::ExchangeLegacy(f.client, f.store) == auth::ExchangeStatus::RetryLater);
		CHECK(f.store.HasLegacy() && !f.store.HasToken());
	}
}

void TestLogout()
{
	testcheck::g_currentTest = "logout";
	Fixture f;
	f.transport.Push(Json(204, ""));
	const api::Result r = auth::Logout(f.client, "tgp_bye");
	CHECK(r.Ok());
	const http::Request req = f.transport.Last();
	CHECK_STR(req.path, "/auth/plugin/logout");
	CHECK_STR(req.method, "POST");
	CHECK_STR(HeaderValue(req, "Authorization"), "Bearer tgp_bye");
	CHECK(req.timeouts.totalMs > 0 && req.timeouts.totalMs <= 5000);
	const size_t before = f.transport.Count();
	auth::Logout(f.client, "");
	CHECK(f.transport.Count() == before);
}

void TestIsoDates()
{
	testcheck::g_currentTest = "iso dates";
	CHECK(auth::ParseIsoUtc("1970-01-01T00:00:00Z").value_or(-1) == 0);
	CHECK(auth::ParseIsoUtc("2026-10-18T12:34:56Z").value_or(-1) == 1792326896);
	CHECK(auth::ParseIsoUtc("2026-10-18T12:34:56.789Z").value_or(-1) == 1792326896);
	CHECK(auth::ParseIsoUtc("2026-10-18T12:34:56+00:00").value_or(-1) == 1792326896);
	CHECK(auth::ParseIsoUtc("2000-02-29T00:00:00Z").value_or(-1) == 951782400);
	CHECK(!auth::ParseIsoUtc("2026-10-18T12:34:56+02:00"));
	CHECK(!auth::ParseIsoUtc("2026-13-01T00:00:00Z"));
	CHECK(!auth::ParseIsoUtc("yesterday"));
	CHECK(!auth::ParseIsoUtc(""));
}

} // namespace

int main()
{
	TestStart();
	TestPoll();
	TestAuthorizedCalls();
	TestRenew();
	TestCheckSession();
	TestExchange();
	TestLogout();
	TestIsoDates();
	return testcheck::Finish("auth_client_tests");
}
