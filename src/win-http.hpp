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
#include <vector>

// Minimal synchronous HTTPS client for the plugin's few network calls (the
// Trigglow account, Trigi and support tickets through api-client.hpp, and the
// GitHub update check), built on WinHTTP (native Windows API, TLS via the OS's
// own Schannel) instead of Qt's QNetworkAccessManager.
//
// Why not Qt: QNetworkAccessManager's TLS backend on Windows loads as a Qt plugin
// (qschannelbackend.dll) whose ABI is tied EXACTLY to the Qt minor.patch version that built it
// (it's declared in a private header, QtNetwork/<version>/QtNetwork/private/qtlsbackend_p.h --
// no ABI stability promise across versions). This plugin runs inside OBS's process using
// whichever Qt6Network.dll OBS itself already loaded for its own UI, NOT the one this plugin
// was compiled against (obs-deps' vendored Qt6, which lags behind OBS's actual bundled Qt) --
// confirmed live, 2026-09-01: OBS 32.2.2 runs Qt 6.11.1, obs-deps' 2025-07-11 package is Qt
// 6.8.3, and OBS's own installation ships no TLS plugin of its own since OBS never uses Qt's
// network stack internally. Shipping a matching TLS plugin would mean re-verifying (and likely
// re-vendoring) it on every OBS Qt bump -- WinHTTP sidesteps the whole problem: it's a stable
// Win32 API, TLS is handled by the OS, and it has zero dependency on whatever Qt version happens
// to be loaded in the host process.
//
// Rules every request follows (no per-call opt-out):
//   - HTTPS on port 443 only, TLS 1.2 or later.
//   - Redirects are NEVER followed: a 3xx comes back as the response itself,
//     so a request carrying "Authorization: Bearer" can't be bounced to
//     another host. Callers treat a 3xx as an error.
//   - No cookies, no automatic Windows sign-in (NTLM/Kerberos) to any host.
//   - Timeouts (WinHttpSetTimeouts) from Request::timeouts, plus an overall
//     deadline for reading the response.
//   - The response body is capped (Request::maxResponseBytes): past it the
//     read stops and the call fails with Error::TooLarge.
//   - Header values with CR or LF are refused (no header injection).
//   - Nothing here is logged: not the URL, headers, tokens nor bodies.
//
// macOS and Linux have no HTTP client yet (planned for a later version): there
// Available() is false and Send() returns Error::Unavailable at once, so the
// account, Trigi and ticket features degrade cleanly instead of failing to
// build.
namespace trigglow::http {

// Per-step timeouts (WinHttpSetTimeouts). WinHTTP checks them with a coarse
// timer: measured on Windows 11, a step's timeout fires at the first multiple
// of 4 s past it, so a stalled call ends up to about 4 s later than asked.
struct Timeouts {
	int resolveMs = 10000;
	int connectMs = 10000;
	int sendMs = 15000;
	int receiveMs = 30000; // Per read; Trigi's answers need up to 90 s.
	int totalMs = 0;       // Overall deadline for the whole call; 0 = none beyond the above.
};

inline constexpr size_t kDefaultMaxResponseBytes = 256 * 1024;

struct Header {
	std::string name;
	std::string value;
};

struct Request {
	std::string method = "GET";
	std::string host; // Bare hostname, e.g. "api.trigglow.com".
	std::string path; // Path and query, e.g. "/auth/plugin/me". Never put a secret in it.
	std::string userAgent = "TrigglowDynamicDelay";
	std::vector<Header> headers;
	std::string body; // Sent as is (empty = no body).
	Timeouts timeouts;
	size_t maxResponseBytes = kDefaultMaxResponseBytes;
};

enum class Error : unsigned char {
	None,        // An HTTP response arrived (any status, 3xx included).
	Unavailable, // No HTTP client on this platform.
	Invalid,     // The request itself was refused (bad header, empty host).
	Resolve,     // DNS.
	Connect,     // TCP connection.
	Timeout,
	Tls,      // Certificate or TLS handshake.
	TooLarge, // Response body over Request::maxResponseBytes.
	Other,
};

struct Response {
	Error error = Error::Other;
	int status = 0;                // Valid when error == None.
	std::string body;              // Valid when error == None.
	std::string retryAfter;        // Raw Retry-After header, empty if absent.
	unsigned long systemError = 0; // WinHTTP error code (for the log), 0 if none.

	bool Ok() const { return error == Error::None; }
};

// True where Send() can actually reach the network (Windows today).
bool Available();

// BLOCKING: never call it on the UI thread or OBS's graphics thread.
Response Send(const Request &request);

// Short English name of an error, for the log ("timeout", "tls", ...).
const char *ErrorName(Error error);

} // namespace trigglow::http

// Transitional, until the guest ticket form moves to the support client: the
// two helpers it still calls, now built on Send() (same rules as above).
namespace trigglow {

struct HttpResult {
	bool ok = false;    // false only for a transport-level failure.
	int statusCode = 0; // Valid whenever ok is true.
	std::string body;
	std::string error; // Set when ok is false.
};

HttpResult HttpsPostJson(const std::wstring &host, const std::wstring &pathAndQuery, const std::string &jsonBody,
			 const std::wstring &bearerToken = L"");
HttpResult HttpsPostMultipartFile(const std::wstring &host, const std::wstring &pathAndQuery,
				  const std::string &fieldName, const std::string &fileName,
				  const std::string &fileContentType, const std::string &fileBytes,
				  const std::wstring &bearerToken = L"");

} // namespace trigglow
