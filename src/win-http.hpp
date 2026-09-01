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

#include <string>

// Minimal synchronous HTTPS client for AuthManager's account-gate calls, built on WinHTTP
// (native Windows API, TLS via the OS's own Schannel) instead of Qt's QNetworkAccessManager.
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
namespace trigglow {

struct HttpResult {
	bool ok = false;   // false only for a transport-level failure (DNS, connect, TLS handshake).
	int statusCode = 0; // HTTP status code, valid whenever ok is true, regardless of 2xx/4xx/5xx.
	std::string body;
	std::string error; // Human-readable, set when ok is false.
};

// Both calls are BLOCKING -- callers must not run them on the UI thread. AuthManager runs each
// on a short-lived background std::thread and marshals the result back via
// QMetaObject::invokeMethod(..., Qt::QueuedConnection).
//
// host: bare hostname, e.g. "trigglow.virosms.com" (no scheme, no path). Always connects over
// HTTPS (port 443). pathAndQuery: e.g. "/auth/plugin/poll?code=...". bearerToken: omit for none.
HttpResult HttpsGet(const std::wstring &host, const std::wstring &pathAndQuery,
		     const std::wstring &bearerToken = L"");
HttpResult HttpsPostJson(const std::wstring &host, const std::wstring &pathAndQuery, const std::string &jsonBody,
			  const std::wstring &bearerToken = L"");

} // namespace trigglow
