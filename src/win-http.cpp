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

#include <random>
#include <sstream>

namespace trigglow {

namespace {

// contentType empty + body null: no request body at all (GET). contentType set + body non-null:
// a request body of that exact Content-Type (JSON, multipart/form-data, ...) -- generalized from
// this function's original "jsonBody or nothing" shape so HttpsPostMultipartFile() could reuse
// it below instead of duplicating the WinHTTP session/connect/request boilerplate.
HttpResult DoRequest(const wchar_t *method, const std::wstring &host, const std::wstring &pathAndQuery,
		     const std::wstring &bearerToken, const std::wstring &contentType, const std::string *body)
{
	HttpResult result;

	HINTERNET hSession = WinHttpOpen(L"TrigglowDynamicDelay/1.0 (OBS plugin)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
					 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) {
		result.error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
		return result;
	}

	HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) {
		result.error = "WinHttpConnect failed (" + std::to_string(GetLastError()) + ")";
		WinHttpCloseHandle(hSession);
		return result;
	}

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, pathAndQuery.c_str(), nullptr, WINHTTP_NO_REFERER,
						WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) {
		result.error = "WinHttpOpenRequest failed (" + std::to_string(GetLastError()) + ")";
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return result;
	}

	std::wstring headers;
	if (body && !contentType.empty())
		headers += L"Content-Type: " + contentType + L"\r\n";
	if (!bearerToken.empty())
		headers += L"Authorization: Bearer " + bearerToken + L"\r\n";

	LPCWSTR headersPtr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
	DWORD headersLen = headers.empty() ? 0 : static_cast<DWORD>(headers.size());

	LPVOID bodyPtr = WINHTTP_NO_REQUEST_DATA;
	DWORD bodyLen = 0;
	if (body) {
		bodyPtr = const_cast<char *>(body->data());
		bodyLen = static_cast<DWORD>(body->size());
	}

	BOOL sent = WinHttpSendRequest(hRequest, headersPtr, headersLen, bodyPtr, bodyLen, bodyLen, 0);
	BOOL received = sent && WinHttpReceiveResponse(hRequest, nullptr);

	if (received) {
		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
				    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
		result.statusCode = static_cast<int>(statusCode);

		std::string body;
		DWORD available = 0;
		do {
			available = 0;
			if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0)
				break;
			std::string chunk(available, '\0');
			DWORD read = 0;
			if (!WinHttpReadData(hRequest, chunk.data(), available, &read))
				break;
			chunk.resize(read);
			body += chunk;
		} while (available > 0);

		result.body = std::move(body);
		result.ok = true;
	} else {
		result.error = "Request failed (" + std::to_string(GetLastError()) + ")";
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return result;
}

} // namespace

HttpResult HttpsGet(const std::wstring &host, const std::wstring &pathAndQuery, const std::wstring &bearerToken)
{
	return DoRequest(L"GET", host, pathAndQuery, bearerToken, L"", nullptr);
}

HttpResult HttpsPostJson(const std::wstring &host, const std::wstring &pathAndQuery, const std::string &jsonBody,
			 const std::wstring &bearerToken)
{
	return DoRequest(L"POST", host, pathAndQuery, bearerToken, L"application/json", &jsonBody);
}

HttpResult HttpsPostMultipartFile(const std::wstring &host, const std::wstring &pathAndQuery,
				  const std::string &fieldName, const std::string &fileName,
				  const std::string &fileContentType, const std::string &fileBytes,
				  const std::wstring &bearerToken)
{
	// Random per-request boundary (RFC 2388) -- std::random_device rather than
	// anything content-derived, since fileBytes here is arbitrary (a whole
	// OBS log) and could theoretically contain the boundary string itself if
	// it were guessable/fixed. Built as narrow ASCII from the start (not a
	// wstring narrowed down afterward) -- MSVC's /W4 treats the reverse
	// wchar_t->char direction as a warning-as-error (C4244) even though the
	// content here is always pure hex digits and would never actually lose
	// data.
	std::mt19937_64 rng(std::random_device{}());
	std::ostringstream boundaryStream;
	boundaryStream << "----TrigglowBoundary" << std::hex << rng();
	std::string boundaryUtf8 = boundaryStream.str();

	// multipart/form-data body: WinHTTP has no built-in multipart helper (it
	// only speaks raw request bodies), so this is built by hand -- CRLF line
	// endings and the trailing "--boundary--" are both required by RFC 2388,
	// not optional formatting.
	std::string body;
	body += "--" + boundaryUtf8 + "\r\n";
	body += "Content-Disposition: form-data; name=\"" + fieldName + "\"; filename=\"" + fileName + "\"\r\n";
	body += "Content-Type: " + fileContentType + "\r\n\r\n";
	body += fileBytes;
	body += "\r\n--" + boundaryUtf8 + "--\r\n";

	// Safe widen (ASCII char -> wchar_t), the opposite direction of the
	// C4244 this function's header comment describes avoiding above.
	std::wstring boundaryW(boundaryUtf8.begin(), boundaryUtf8.end());
	std::wstring contentType = L"multipart/form-data; boundary=" + boundaryW;
	return DoRequest(L"POST", host, pathAndQuery, bearerToken, contentType, &body);
}

} // namespace trigglow

#else // !_WIN32

// macOS/Linux aren't wired up to a native HTTPS client yet -- see docs/ACCOUNT_GATE.md. AuthManager
// degrades to "login unavailable" rather than failing to build.
namespace trigglow {

HttpResult HttpsGet(const std::wstring &, const std::wstring &, const std::wstring &)
{
	return HttpResult{false, 0, "", "Not implemented on this platform yet"};
}

HttpResult HttpsPostJson(const std::wstring &, const std::wstring &, const std::string &, const std::wstring &)
{
	return HttpResult{false, 0, "", "Not implemented on this platform yet"};
}

HttpResult HttpsPostMultipartFile(const std::wstring &, const std::wstring &, const std::string &, const std::string &,
				  const std::string &, const std::string &, const std::wstring &)
{
	return HttpResult{false, 0, "", "Not implemented on this platform yet"};
}

} // namespace trigglow

#endif
