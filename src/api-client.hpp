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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "win-http.hpp"

// The plugin's side of the Trigglow API: which host it talks to, how every
// request is built (headers, timeouts, size cap) and how every response is
// read (status, the {"error": "<code>"} body, Retry-After). No Qt and no
// libobs: the transport is a plain function, so tests plug in a fake one
// (tests/api_client_tests.cpp) and the plugin plugs in http::Send().
//
// Client rules (all enforced here or in win-http.hpp):
//   - Only https://api.trigglow.com. The Authorization header is only ever
//     built by Client, which only ever targets that host.
//   - User-Agent "TrigglowDynamicDelay/<X.Y.Z> (OBS plugin; <windows|macos|linux>)".
//   - JSON requests carry "Content-Type: application/json; charset=utf-8" and
//     "Accept: application/json".
//   - Redirects are never followed; a 3xx is an error.
//   - Timeouts: resolve 10 s, connect 10 s, send 15 s, receive 30 s; receive
//     90 s for Trigi's questions and summaries.
//   - Responses over 256 KB are dropped.
//   - Unknown JSON fields are ignored; a missing required field is "unexpected
//     response". The server's "message" text is never shown to the user: the
//     "error" code is mapped to the plugin's own strings.
//   - Nothing creating something on the server (a session, a message, a
//     summary, a ticket, an attachment) is ever retried automatically.
namespace trigglow::api {

// --- Host -------------------------------------------------------------------

inline constexpr const char *kDefaultApiHost = "api.trigglow.com";

// The API host: kDefaultApiHost in every published build. Test builds made
// with the TRIGGLOW_ALLOW_API_HOST_OVERRIDE CMake option read it from the
// TRIGGLOW_API_HOST environment variable instead (a bare hostname; still
// HTTPS only).
std::string ApiHost();

// --- Web pages the plugin may open (fixed URLs, checked by prefix) -----------

namespace web {
inline constexpr const char *kAppSupport = "https://www.trigglow.com/app/support";
inline constexpr const char *kPublicSupport = "https://www.trigglow.com/support";
inline constexpr const char *kPrivacy = "https://www.trigglow.com/legal/privacy";
inline constexpr const char *kProduct = "https://www.trigglow.com/dynamic-delay";
inline constexpr const char *kRepo = "https://github.com/VirosMs/obs-trigglow-dynamic-delay/";
inline constexpr const char *kLatestRelease = "https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest";
} // namespace web

// The sign-in confirmation page prefix that /auth/plugin/start's
// verificationUrl must start with ("https://www.trigglow.com/plugin/link?",
// or the same page on the override host in test builds).
std::string VerificationUrlPrefix();

// True only for one of the fixed pages above (exact, or followed by '?', '#'
// or '/' and plain URL characters), the GitHub repo's pages, or a
// verificationUrl with the right prefix. Anything else must not be opened.
bool IsAllowedWebUrl(std::string_view url);

// "View ticket on trigglow.com": kAppSupport + "?ticket=<id>" (signed-in
// tickets). Empty if the id isn't a UUID.
std::string AppSupportTicketUrl(std::string_view ticketId);
// Guest tickets: kPublicSupport + "?ticket=<id>&token=<accessToken>". Empty
// if either value has unexpected characters. Open it; never log it.
std::string PublicSupportTicketUrl(std::string_view ticketId, std::string_view accessToken);

// --- Client identity -----------------------------------------------------------

enum class Os : unsigned char { Windows, Macos, Linux };
Os CurrentOs();
const char *OsName(Os os); // "windows", "macos", "linux"

// "X.Y.Z" from the plugin's version string ("0.6.0", "0.6.0-rc1" -> "0.6.0"),
// "0.0.0" if it doesn't start with three numbers of up to 3 digits.
std::string ClientVersion(std::string_view pluginVersion);
std::string UserAgent(std::string_view pluginVersion);
// {"kind":"obs_dynamic_delay","version":"X.Y.Z","os":"windows"}
json::Value ClientObject(std::string_view pluginVersion);

// Numeric X.Y.Z comparison (missing parts read as 0): <0, 0 or >0.
int CompareVersions(std::string_view a, std::string_view b);

// --- Value checks ----------------------------------------------------------------

bool IsLinkSecret(std::string_view s);  // ^[A-Za-z0-9_-]{32,128}$ (linkCode, pollSecret)
bool IsUserCode(std::string_view s);    // ^[A-Z0-9]{4}-[A-Z0-9]{4}$
bool IsPluginToken(std::string_view s); // 1-256 of [A-Za-z0-9_-]
bool IsUuid(std::string_view s);
bool IsLocale(std::string_view s); // ^[a-z]{2}(-[A-Z]{2})?$
// OBS's locale ("es-ES", "pt-BR", "zh-CN"...) in that form; "en-US" if it
// can't be expressed.
std::string NormalizeLocale(std::string_view obsLocale);
// An error code from the server, safe to write to the log: [a-z0-9_] only, at
// most 40 characters ("" when there is none).
std::string LoggableCode(std::string_view code);

// Retry-After of a 429 in seconds: the header's whole seconds clamped to
// 1-3600, or 60 when it is missing or not a number.
int RetryAfterSeconds(std::string_view header);

// --- Calls ---------------------------------------------------------------------

enum class Outcome : unsigned char {
	Ok,          // 2xx.
	HttpError,   // Any other status (3xx included).
	Network,     // Transport failure (DNS, connection, TLS, timeout, size cap, cancelled): see `transport`.
	Unavailable, // No HTTP client on this platform.
};

struct Result {
	Outcome outcome = Outcome::Network;
	int status = 0;
	// The "error" member of an error body (A0: {"error": "<code>"}), or the
	// "status" member when there is no "error" (the link poll's
	// {"status":"expired"}); empty if neither.
	std::string error;
	// The parsed body, when it was JSON (2xx and error bodies alike).
	std::optional<json::Value> body;
	// For 429 only: how long to wait (RetryAfterSeconds()).
	int retryAfterSeconds = 0;
	http::Error transport = http::Error::None;

	bool Ok() const { return outcome == Outcome::Ok; }
	// No answer in time: a WinHTTP step timeout or the call's deadline.
	bool TimedOut() const { return outcome == Outcome::Network && transport == http::Error::Timeout; }
	// Aborted on purpose (the job was cancelled): nobody is waiting for it.
	bool Cancelled() const { return outcome == Outcome::Network && transport == http::Error::Cancelled; }
	bool Is(int httpStatus, std::string_view code = {}) const
	{
		return outcome == Outcome::HttpError && status == httpStatus && (code.empty() || error == code);
	}
	// The body's string member `key`, if any.
	std::optional<std::string> BodyString(std::string_view key) const;
};

enum class Timeout : unsigned char {
	Standard,     // 10 / 10 / 15 / 30 s.
	Conversation, // Receive 90 s: Trigi questions and summaries.
	Short,        // About 5 s in all: sign-out and the background "abandoned" event.
	Upload,       // Send 120 s: the log attachment (up to 5 MB).
};

using Transport = std::function<http::Response(const http::Request &)>;

class Client {
public:
	// host: ApiHost() in the plugin; tests may use anything.
	Client(Transport transport, std::string host, std::string pluginVersion);

	// The plugin's client: http::Send() to ApiHost().
	static Client Default(std::string pluginVersion);

	const std::string &Host() const { return host_; }
	const std::string &PluginVersion() const { return pluginVersion_; }

	// A copy whose every request is aborted when `cancel` is cancelled (a
	// background job's token, background-tasks.hpp). The token must outlive
	// the copy's calls.
	Client WithCancel(const tasks::CancelToken *cancel) const
	{
		Client copy = *this;
		copy.cancel_ = cancel;
		return copy;
	}

	// bearer: the plugin token, or empty for none. A token with characters
	// outside [A-Za-z0-9_-] is never sent (the call fails as Network/Invalid).
	Result Get(std::string_view path, std::string_view bearer = {}, Timeout timeout = Timeout::Standard) const;
	Result PostJson(std::string_view path, const json::Value &body, std::string_view bearer = {},
			Timeout timeout = Timeout::Standard) const;
	// multipart/form-data with one part: name="file", filename=fileName.
	// extraHeaders: e.g. X-Ticket-Token (values are checked like the token).
	Result PostFile(std::string_view path, std::string_view fileName, std::string_view contentType,
			const std::string &bytes, std::string_view bearer = {},
			const std::vector<http::Header> &extraHeaders = {}) const;

private:
	Result Run(http::Request request, std::string_view bearer, Timeout timeout) const;

	Transport transport_;
	std::string host_;
	std::string pluginVersion_;
	const tasks::CancelToken *cancel_ = nullptr;
};

} // namespace trigglow::api
