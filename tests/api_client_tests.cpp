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

// Tests for src/api-client.*: how every API request is built and every
// response read, plus the value checks and the fixed pages the plugin opens.

#include "api-client.hpp"
#include "fake-transport.hpp"

#include <string>

using namespace trigglow;
using testsupport::FakeTransport;
using testsupport::HeaderValue;
using testsupport::HasHeader;
using testsupport::Json;
using testsupport::Url;

namespace {

const char *OsLabel()
{
	return api::OsName(api::CurrentOs());
}

void TestIdentity()
{
	testcheck::g_currentTest = "identity";
	CHECK_STR(api::ClientVersion("0.6.0"), "0.6.0");
	CHECK_STR(api::ClientVersion("0.6.0-rc1"), "0.6.0");
	CHECK_STR(api::ClientVersion("12.345.6.7"), "12.345.6");
	CHECK_STR(api::ClientVersion("1.2"), "0.0.0");
	CHECK_STR(api::ClientVersion("1.2.3456"), "0.0.0");
	CHECK_STR(api::ClientVersion("v1.2.3"), "0.0.0");
	CHECK_STR(api::UserAgent("0.6.0"), std::string("TrigglowDynamicDelay/0.6.0 (OBS plugin; ") + OsLabel() + ")");
	const json::Value client = api::ClientObject("0.6.0");
	CHECK_STR(json::Serialize(client),
		  std::string("{\"kind\":\"obs_dynamic_delay\",\"version\":\"0.6.0\",\"os\":\"") + OsLabel() + "\"}");
	CHECK_STR(api::ApiHost(), "api.trigglow.com");
	CHECK_STR(api::VerificationUrlPrefix(), Url("https", "www.trigglow.com/plugin/link?"));
}

void TestVersions()
{
	testcheck::g_currentTest = "versions";
	CHECK(api::CompareVersions("0.6.0", "0.6.0") == 0);
	CHECK(api::CompareVersions("0.6.0", "0.6.1") < 0);
	CHECK(api::CompareVersions("0.10.0", "0.9.9") > 0); // Numeric, not text.
	CHECK(api::CompareVersions("1.0", "1.0.0") == 0);
	CHECK(api::CompareVersions("2.0.0", "10.0.0") < 0);
	CHECK(api::CompareVersions("", "0.0.1") < 0);
}

void TestValueChecks()
{
	testcheck::g_currentTest = "value checks";
	const std::string secret43 = "abcdefghijklmnopqrstuvwxyzABCDEFGH_-0123456";
	CHECK(api::IsLinkSecret(secret43));
	CHECK(api::IsLinkSecret(std::string(32, 'a')));
	CHECK(!api::IsLinkSecret(std::string(31, 'a')));
	CHECK(!api::IsLinkSecret(std::string(129, 'a')));
	CHECK(!api::IsLinkSecret(secret43 + "="));
	CHECK(!api::IsLinkSecret(secret43 + "/"));
	CHECK(api::IsUserCode("KD7F-29QM"));
	CHECK(!api::IsUserCode("kd7f-29qm"));
	CHECK(!api::IsUserCode("KD7F29QM"));
	CHECK(!api::IsUserCode("KD7F-29Q"));
	CHECK(!api::IsUserCode("<b>F-29QM"));
	CHECK(api::IsPluginToken("tgp_abc-DEF_123"));
	CHECK(!api::IsPluginToken(""));
	CHECK(!api::IsPluginToken(std::string(257, 'a')));
	CHECK(!api::IsPluginToken("abc\r\nX-Evil: 1"));
	CHECK(!api::IsPluginToken("abc def"));
	CHECK(api::IsUuid("123e4567-e89b-12d3-a456-426614174000"));
	CHECK(!api::IsUuid("123e4567-e89b-12d3-a456-42661417400"));
	CHECK(!api::IsUuid("123e4567-e89b-12d3-a456-42661417400g"));
	CHECK(!api::IsUuid("../../api/support/x/y/z/a/bbbbbbbbbbbb"));
	CHECK(api::IsLocale("es-ES") && api::IsLocale("en") && !api::IsLocale("es_ES") && !api::IsLocale("ast-ES"));
	CHECK_STR(api::NormalizeLocale("es-ES"), "es-ES");
	CHECK_STR(api::NormalizeLocale("pt_br"), "pt-BR");
	CHECK_STR(api::NormalizeLocale("EN"), "en");
	CHECK_STR(api::NormalizeLocale("ast-ES"), "en-US");
	CHECK_STR(api::NormalizeLocale(""), "en-US");
	CHECK_STR(api::LoggableCode("client_disabled"), "client_disabled");
	CHECK_STR(api::LoggableCode("Evil\nline"), "?");
	CHECK_STR(api::LoggableCode(""), "");
	CHECK(api::LoggableCode(std::string(100, 'a')).size() == 40);
}

void TestRetryAfter()
{
	testcheck::g_currentTest = "retry-after";
	CHECK(api::RetryAfterSeconds("") == 60);
	CHECK(api::RetryAfterSeconds("120") == 120);
	CHECK(api::RetryAfterSeconds(" 5 ") == 5);
	CHECK(api::RetryAfterSeconds("0") == 1);
	CHECK(api::RetryAfterSeconds("999999") == 3600);
	CHECK(api::RetryAfterSeconds("99999999999999") == 60);
	CHECK(api::RetryAfterSeconds("Wed, 21 Oct 2026 07:28:00 GMT") == 60);
	CHECK(api::RetryAfterSeconds("-5") == 60);
}

void TestWebPages()
{
	testcheck::g_currentTest = "web pages";
	CHECK(api::IsAllowedWebUrl(api::web::kAppSupport));
	CHECK(api::IsAllowedWebUrl(api::web::kPublicSupport));
	CHECK(api::IsAllowedWebUrl(api::web::kPrivacy));
	CHECK(api::IsAllowedWebUrl(api::web::kProduct));
	CHECK(api::IsAllowedWebUrl(api::web::kLatestRelease));
	CHECK(api::IsAllowedWebUrl(api::AppSupportTicketUrl("123e4567-e89b-12d3-a456-426614174000")));
	CHECK(api::IsAllowedWebUrl(api::PublicSupportTicketUrl("123e4567-e89b-12d3-a456-426614174000", "ab12cd")));
	CHECK(api::IsAllowedWebUrl(Url("https", "www.trigglow.com/plugin/link?code=abc_DEF-123")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "www.trigglow.com/plugin/link?")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "www.trigglow.com/plugin/link?code=a\"b")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "www.trigglow.com/app/supportx")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "www.trigglow.com/app/support@x")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "www.trigglow.com/app/support/../../x")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "www.trigglow.com") + ".example.com/app/support"));
	CHECK(!api::IsAllowedWebUrl(Url("http", "www.trigglow.com/app/support")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "www.trigglow.com/")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "github.com/VirosMs/other-repo/")));
	CHECK(!api::IsAllowedWebUrl(Url("https", "example.com/")));
	CHECK(!api::IsAllowedWebUrl("javascript:alert(1)"));
	CHECK(!api::IsAllowedWebUrl(Url("file", "/C:/x")));
	CHECK(!api::IsAllowedWebUrl(std::string(api::web::kAppSupport) + "?a=<script>"));
	CHECK(api::AppSupportTicketUrl("not-a-uuid").empty());
	CHECK(api::PublicSupportTicketUrl("123e4567-e89b-12d3-a456-426614174000", "a&b").empty());
}

api::Client MakeClient(FakeTransport &t)
{
	return api::Client(t.Fn(), "api.trigglow.com", "0.6.0");
}

void TestRequests()
{
	testcheck::g_currentTest = "requests";
	FakeTransport t;
	const api::Client client = MakeClient(t);

	t.Push(Json(200, R"({"ok":true})"));
	json::Value body = json::Value::Object();
	body.Set("a", json::Value::Number(1));
	api::Result r = client.PostJson("/x", body, "tgp_token-1");
	CHECK(r.Ok());
	CHECK(r.body && r.body->GetBool("ok").value_or(false));
	http::Request req = t.Last();
	CHECK_STR(req.method, "POST");
	CHECK_STR(req.host, "api.trigglow.com");
	CHECK_STR(req.path, "/x");
	CHECK_STR(req.body, "{\"a\":1}");
	CHECK_STR(HeaderValue(req, "Content-Type"), "application/json; charset=utf-8");
	CHECK_STR(HeaderValue(req, "Accept"), "application/json");
	CHECK_STR(HeaderValue(req, "Authorization"), "Bearer tgp_token-1");
	CHECK_STR(req.userAgent, api::UserAgent("0.6.0"));
	CHECK(req.timeouts.resolveMs == 10000 && req.timeouts.connectMs == 10000 && req.timeouts.sendMs == 15000 &&
	      req.timeouts.receiveMs == 30000);
	CHECK(req.maxResponseBytes == 256 * 1024);

	// No token, no Authorization header at all.
	t.Push(Json(200, "{}"));
	client.Get("/y");
	CHECK(!HasHeader(t.Last(), "Authorization"));
	CHECK(!HasHeader(t.Last(), "Content-Type"));

	// Trigi's long receive timeout; the short one for background calls.
	t.Push(Json(200, "{}"));
	client.PostJson("/z", json::Value::Object(), "tok", api::Timeout::Conversation);
	CHECK(t.Last().timeouts.receiveMs == 90000);
	t.Push(Json(204, ""));
	r = client.PostJson("/z", json::Value::Object(), "tok", api::Timeout::Short);
	CHECK(t.Last().timeouts.receiveMs <= 5000 && t.Last().timeouts.totalMs <= 5000);
	CHECK(r.Ok() && !r.body);

	// A token that could inject a header is never sent.
	const size_t before = t.Count();
	r = client.Get("/me", "tok\r\nX-Evil: 1");
	CHECK(t.Count() == before);
	CHECK(!r.Ok() && r.transport == http::Error::Invalid);
}

void TestResponses()
{
	testcheck::g_currentTest = "responses";
	FakeTransport t;
	const api::Client client = MakeClient(t);

	// The error code, never the message.
	t.Push(Json(403, R"({"error":"client_disabled","message":"Something for humans"})"));
	api::Result r = client.Get("/a", "tok");
	CHECK(r.outcome == api::Outcome::HttpError && r.status == 403);
	CHECK_STR(r.error, "client_disabled");
	CHECK(r.Is(403, "client_disabled") && r.Is(403) && !r.Is(401));

	// {"status":"expired"} on 404.
	t.Push(Json(404, R"({"status":"expired"})"));
	CHECK_STR(client.Get("/b").error, "expired");

	// 429 with and without Retry-After.
	testsupport::Reply limited = Json(429, R"({"error":"rate_limited"})");
	limited.retryAfter = "17";
	t.Push(limited);
	r = client.Get("/c");
	CHECK(r.Is(429, "rate_limited") && r.retryAfterSeconds == 17);
	t.Push(Json(429, "{}"));
	CHECK(client.Get("/c").retryAfterSeconds == 60);

	// A redirect is an error, and nothing follows it.
	const size_t before = t.Count();
	testsupport::Reply redirect = Json(302, "");
	t.Push(redirect);
	r = client.Get("/d", "tok");
	CHECK(!r.Ok() && r.outcome == api::Outcome::HttpError && r.status == 302);
	CHECK(t.Count() == before + 1);

	// Non-JSON bodies, transport errors and a platform without HTTP.
	t.Push(Json(500, "<html>oops</html>"));
	r = client.Get("/e");
	CHECK(r.status == 500 && !r.body && r.error.empty());
	t.Push(testsupport::NetworkDown());
	r = client.Get("/f");
	CHECK(r.outcome == api::Outcome::Network && r.transport == http::Error::Connect);
	testsupport::Reply unavailable;
	unavailable.error = http::Error::Unavailable;
	t.Push(unavailable);
	CHECK(client.Get("/g").outcome == api::Outcome::Unavailable);
}

void TestMultipart()
{
	testcheck::g_currentTest = "multipart";
	FakeTransport t;
	const api::Client client = MakeClient(t);
	t.Push(Json(201, "{}"));
	const std::string log = "line 1\r\n--not-a-boundary\r\nline 3";
	api::Result r = client.PostFile("/up", "obs-log.txt", "text/plain", log, {}, {{"X-Ticket-Token", "ab12"}});
	CHECK(r.Ok());
	const http::Request req = t.Last();
	CHECK_STR(HeaderValue(req, "X-Ticket-Token"), "ab12");
	CHECK(!HasHeader(req, "Authorization"));
	const std::string type = HeaderValue(req, "Content-Type");
	CHECK(type.rfind("multipart/form-data; boundary=", 0) == 0);
	const std::string boundary = type.substr(std::string("multipart/form-data; boundary=").size());
	CHECK(boundary.size() > 20);
	CHECK(req.body.rfind("--" + boundary + "\r\n", 0) == 0);
	CHECK(req.body.find("Content-Disposition: form-data; name=\"file\"; filename=\"obs-log.txt\"\r\n") !=
	      std::string::npos);
	CHECK(req.body.find("Content-Type: text/plain\r\n\r\n" + log + "\r\n--" + boundary + "--\r\n") !=
	      std::string::npos);
	CHECK(req.timeouts.sendMs >= 60000);

	// Header values are checked like tokens; file names can't break the part.
	const size_t before = t.Count();
	client.PostFile("/up", "x.txt", "text/plain", "x", {}, {{"X-Ticket-Token", "a\r\nb"}});
	client.PostFile("/up", "x\".txt", "text/plain", "x");
	CHECK(t.Count() == before);
}

} // namespace

int main()
{
	TestIdentity();
	TestVersions();
	TestValueChecks();
	TestRetryAfter();
	TestWebPages();
	TestRequests();
	TestResponses();
	TestMultipart();
	return testcheck::Finish("api_client_tests");
}
