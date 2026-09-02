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

namespace trigglow {

namespace {

HttpResult DoRequest(const wchar_t *method, const std::wstring &host, const std::wstring &pathAndQuery,
		     const std::wstring &bearerToken, const std::string *jsonBody)
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
	if (jsonBody)
		headers += L"Content-Type: application/json\r\n";
	if (!bearerToken.empty())
		headers += L"Authorization: Bearer " + bearerToken + L"\r\n";

	LPCWSTR headersPtr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
	DWORD headersLen = headers.empty() ? 0 : static_cast<DWORD>(headers.size());

	LPVOID bodyPtr = WINHTTP_NO_REQUEST_DATA;
	DWORD bodyLen = 0;
	if (jsonBody) {
		bodyPtr = const_cast<char *>(jsonBody->data());
		bodyLen = static_cast<DWORD>(jsonBody->size());
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
	return DoRequest(L"GET", host, pathAndQuery, bearerToken, nullptr);
}

HttpResult HttpsPostJson(const std::wstring &host, const std::wstring &pathAndQuery, const std::string &jsonBody,
			 const std::wstring &bearerToken)
{
	return DoRequest(L"POST", host, pathAndQuery, bearerToken, &jsonBody);
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

} // namespace trigglow

#endif
