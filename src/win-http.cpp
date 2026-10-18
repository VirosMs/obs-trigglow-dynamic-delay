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

#include "win-http.hpp"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <chrono>

namespace trigglow::http {

namespace {

class Handle {
public:
	explicit Handle(HINTERNET h = nullptr) : h_(h) {}
	~Handle()
	{
		if (h_)
			WinHttpCloseHandle(h_);
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	HINTERNET get() const { return h_; }
	void reset(HINTERNET h)
	{
		if (h_)
			WinHttpCloseHandle(h_);
		h_ = h;
	}

private:
	HINTERNET h_;
};

std::wstring Widen(const std::string &utf8)
{
	if (utf8.empty())
		return std::wstring();
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (len <= 0)
		return std::wstring();
	std::wstring out(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
	return out;
}

bool HasLineBreakOrNul(const std::string &s)
{
	for (char c : s) {
		if (c == '\r' || c == '\n' || c == '\0')
			return true;
	}
	return false;
}

bool IsValidHeaderName(const std::string &name)
{
	if (name.empty())
		return false;
	for (char c : name) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

bool IsValidTarget(const std::string &s)
{
	if (s.empty())
		return false;
	for (char c : s) {
		if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F)
			return false;
	}
	return true;
}

Error MapError(DWORD code)
{
	switch (code) {
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
		return Error::Resolve;
	case ERROR_WINHTTP_CANNOT_CONNECT:
	case ERROR_WINHTTP_CONNECTION_ERROR:
		return Error::Connect;
	case ERROR_WINHTTP_TIMEOUT:
		return Error::Timeout;
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
	case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
	case ERROR_WINHTTP_SECURE_INVALID_CA:
	case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
	case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
	case ERROR_WINHTTP_SECURE_INVALID_CERT:
	case ERROR_WINHTTP_SECURE_CERT_REVOKED:
	case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
	case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
		return Error::Tls;
	default:
		return Error::Other;
	}
}

Response Fail(Error error, DWORD code = 0)
{
	Response r;
	r.error = error;
	r.systemError = code;
	return r;
}

Response FailLast()
{
	const DWORD code = GetLastError();
	return Fail(MapError(code), code);
}

void SetDwordOption(HINTERNET handle, DWORD option, DWORD value)
{
	WinHttpSetOption(handle, option, &value, sizeof(value));
}

} // namespace

bool Available()
{
	return true;
}

Response Send(const Request &req)
{
	using Clock = std::chrono::steady_clock;
	const Clock::time_point start = Clock::now();

	if (!IsValidTarget(req.host) || !IsValidTarget(req.path) || req.path[0] != '/' || !IsValidTarget(req.method) ||
	    HasLineBreakOrNul(req.userAgent))
		return Fail(Error::Invalid);
	for (const Header &h : req.headers) {
		if (!IsValidHeaderName(h.name) || HasLineBreakOrNul(h.value))
			return Fail(Error::Invalid);
	}

	const std::wstring agent = Widen(req.userAgent);
	// The system's proxy configuration (WPAD/PAC included), falling back to
	// the static WinHTTP proxy settings on systems without it.
	Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				   WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.get())
		session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
					  WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.get())
		return FailLast();

	WinHttpSetTimeouts(session.get(), req.timeouts.resolveMs, req.timeouts.connectMs, req.timeouts.sendMs,
			   req.timeouts.receiveMs);
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
		SetDwordOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2);
#else
	SetDwordOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2);
#endif
	SetDwordOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, WINHTTP_OPTION_REDIRECT_POLICY_NEVER);

	std::wstring host = Widen(req.host);
	INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
	DWORD openFlags = WINHTTP_FLAG_SECURE;
#ifdef TRIGGLOW_HTTP_LOOPBACK_TESTS
	// Local test harnesses only, never defined in a plugin build: a host of
	// the form "127.0.0.1:<port>" talks plain HTTP to a fake server on this
	// machine, so the redirect, timeout and size rules above can be exercised
	// against canned responses.
	if (req.host.rfind("127.0.0.1:", 0) == 0) {
		host = L"127.0.0.1";
		port = static_cast<INTERNET_PORT>(std::stoi(req.host.substr(10)));
		openFlags = 0;
	}
#endif

	Handle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
	if (!connection.get())
		return FailLast();

	const std::wstring method = Widen(req.method);
	const std::wstring path = Widen(req.path);
	Handle request(WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
					  WINHTTP_DEFAULT_ACCEPT_TYPES, openFlags));
	if (!request.get())
		return FailLast();

	// Per request as well: never follow a redirect, keep no cookies, and never
	// answer an authentication challenge with the Windows account.
	SetDwordOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, WINHTTP_OPTION_REDIRECT_POLICY_NEVER);
	SetDwordOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
		       WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS);
	SetDwordOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH);
	// The wait for the response headers has its own timeout; keep it the
	// same as the receive timeout.
	SetDwordOption(request.get(), WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
		       static_cast<DWORD>(req.timeouts.receiveMs));

	std::wstring headers;
	for (const Header &h : req.headers)
		headers += Widen(h.name) + L": " + Widen(h.value) + L"\r\n";
	LPCWSTR headersPtr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
	const DWORD headersLen = headers.empty() ? 0 : static_cast<DWORD>(headers.size());

	LPVOID bodyPtr = WINHTTP_NO_REQUEST_DATA;
	const DWORD bodyLen = static_cast<DWORD>(req.body.size());
	if (!req.body.empty())
		bodyPtr = const_cast<char *>(req.body.data());

	if (!WinHttpSendRequest(request.get(), headersPtr, headersLen, bodyPtr, bodyLen, bodyLen, 0))
		return FailLast();
	if (!WinHttpReceiveResponse(request.get(), nullptr))
		return FailLast();

	Response result;
	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				 WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX))
		return FailLast();
	result.status = static_cast<int>(statusCode);

	wchar_t retryAfter[64] = {};
	DWORD retryAfterSize = sizeof(retryAfter) - sizeof(wchar_t);
	if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, retryAfter,
				&retryAfterSize, WINHTTP_NO_HEADER_INDEX)) {
		for (const wchar_t *p = retryAfter; *p; ++p) {
			if (*p >= 0x20 && *p < 0x7F)
				result.retryAfter += static_cast<char>(*p);
		}
	}

	const auto deadline = start + std::chrono::milliseconds(req.timeouts.totalMs);
	for (;;) {
		if (req.timeouts.totalMs > 0 && Clock::now() > deadline)
			return Fail(Error::Timeout);
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request.get(), &available))
			return FailLast();
		if (available == 0)
			break;
		if (result.body.size() + available > req.maxResponseBytes)
			return Fail(Error::TooLarge);
		const size_t offset = result.body.size();
		result.body.resize(offset + available);
		DWORD read = 0;
		if (!WinHttpReadData(request.get(), result.body.data() + offset, available, &read))
			return FailLast();
		result.body.resize(offset + read);
	}

	result.error = Error::None;
	return result;
}

} // namespace trigglow::http

#else // !_WIN32

namespace trigglow::http {

bool Available()
{
	return false;
}

Response Send(const Request &)
{
	Response r;
	r.error = Error::Unavailable;
	return r;
}

} // namespace trigglow::http

#endif

namespace trigglow::http {

const char *ErrorName(Error error)
{
	switch (error) {
	case Error::None:
		return "none";
	case Error::Unavailable:
		return "unavailable";
	case Error::Invalid:
		return "invalid request";
	case Error::Resolve:
		return "dns";
	case Error::Connect:
		return "connect";
	case Error::Timeout:
		return "timeout";
	case Error::Tls:
		return "tls";
	case Error::TooLarge:
		return "response too large";
	default:
		return "other";
	}
}

} // namespace trigglow::http

namespace trigglow {

namespace {

// ASCII only (hosts, paths and tokens of the old helpers).
std::string Narrow(const std::wstring &w)
{
	std::string out;
	out.reserve(w.size());
	for (wchar_t c : w)
		out += static_cast<char>(c & 0x7F);
	return out;
}

HttpResult FromResponse(const http::Response &r)
{
	HttpResult result;
	result.ok = r.Ok();
	result.statusCode = r.status;
	result.body = r.body;
	if (!r.Ok())
		result.error = http::ErrorName(r.error);
	return result;
}

http::Request PostRequest(const std::wstring &host, const std::wstring &path, const std::wstring &bearer)
{
	http::Request req;
	req.method = "POST";
	req.host = Narrow(host);
	req.path = Narrow(path);
	if (!bearer.empty())
		req.headers.push_back({"Authorization", "Bearer " + Narrow(bearer)});
	return req;
}

} // namespace

HttpResult HttpsPostJson(const std::wstring &host, const std::wstring &pathAndQuery, const std::string &jsonBody,
			 const std::wstring &bearerToken)
{
	http::Request req = PostRequest(host, pathAndQuery, bearerToken);
	req.headers.push_back({"Content-Type", "application/json"});
	req.body = jsonBody;
	return FromResponse(http::Send(req));
}

HttpResult HttpsPostMultipartFile(const std::wstring &host, const std::wstring &pathAndQuery,
				  const std::string &fieldName, const std::string &fileName,
				  const std::string &fileContentType, const std::string &fileBytes,
				  const std::wstring &bearerToken)
{
	const std::string boundary = "----TrigglowBoundaryTransitional7f3a9c";
	http::Request req = PostRequest(host, pathAndQuery, bearerToken);
	req.headers.push_back({"Content-Type", "multipart/form-data; boundary=" + boundary});
	req.body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + fieldName + "\"; filename=\"" +
		   fileName + "\"\r\nContent-Type: " + fileContentType + "\r\n\r\n" + fileBytes + "\r\n--" + boundary +
		   "--\r\n";
	req.timeouts.sendMs = 120000;
	return FromResponse(http::Send(req));
}

} // namespace trigglow
