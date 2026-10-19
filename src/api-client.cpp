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

#include "api-client.hpp"

#include <array>
#include <random>
#include <sstream>

#if defined(TRIGGLOW_ALLOW_API_HOST_OVERRIDE) && defined(_WIN32)
#include <windows.h>
#elif defined(TRIGGLOW_ALLOW_API_HOST_OVERRIDE)
#include <cstdlib>
#endif

namespace trigglow::api {

namespace {

bool IsDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool IsAlnum(char c)
{
	return IsDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsTokenChar(char c)
{
	return IsAlnum(c) || c == '_' || c == '-';
}

bool AllTokenChars(std::string_view s)
{
	for (char c : s) {
		if (!IsTokenChar(c))
			return false;
	}
	return true;
}

bool StartsWith(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

int HexValue(char c)
{
	if (IsDigit(c))
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

// A URL's tail as a browser or a server may read it: "%2e" (any case) is a
// '.', "%2f" a '/', "%5c" a '\' and "%25" a '%', decoded until nothing
// changes, so "%2e%2e", ".%2E" and "%252e%252e" all end up as "..". Other
// escapes stay as they are. false: a '%' without two hex digits after it, or
// still encoded after a few rounds.
bool NormalizeUrlTail(std::string_view in, std::string &out)
{
	std::string current(in);
	for (int round = 0; round < 4; ++round) {
		std::string next;
		next.reserve(current.size());
		bool changed = false;
		for (size_t i = 0; i < current.size(); ++i) {
			if (current[i] != '%') {
				next += current[i];
				continue;
			}
			if (i + 2 >= current.size() || HexValue(current[i + 1]) < 0 || HexValue(current[i + 2]) < 0)
				return false;
			const char decoded =
				static_cast<char>(HexValue(current[i + 1]) * 16 + HexValue(current[i + 2]));
			if (decoded == '.' || decoded == '/' || decoded == '\\' || decoded == '%') {
				next += decoded;
				changed = true;
			} else {
				next.append(current, i, 3);
			}
			i += 2;
		}
		current = std::move(next);
		if (!changed) {
			out = std::move(current);
			return true;
		}
	}
	return false;
}

// What may follow a fixed page's prefix: a plain path, query and fragment,
// checked once normalized (above). No spaces, quotes, angle brackets,
// backslashes, '@' or ':', no ".." anywhere and no "." path segment, so a
// link can never step out of the page it names (as "/support/%2e%2e/x" would).
bool IsPlainUrlTail(std::string_view raw)
{
	std::string s;
	if (!NormalizeUrlTail(raw, s))
		return false;
	for (char c : s) {
		const bool ok = IsAlnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == '?' ||
				c == '#' || c == '&' || c == '=' || c == '%' || c == '+';
		if (!ok)
			return false;
	}
	if (s.find("..") != std::string::npos)
		return false;
	const std::string_view path = std::string_view(s).substr(0, s.find_first_of("?#"));
	size_t start = 0;
	while (start <= path.size()) {
		size_t slash = path.find('/', start);
		if (slash == std::string_view::npos)
			slash = path.size();
		if (path.substr(start, slash - start) == ".")
			return false;
		start = slash + 1;
	}
	return true;
}

#ifdef TRIGGLOW_ALLOW_API_HOST_OVERRIDE
bool IsHostname(std::string_view s)
{
	if (s.empty() || s.size() > 253 || s.front() == '.' || s.front() == '-' || s.back() == '.')
		return false;
	for (char c : s) {
		const bool ok = (c >= 'a' && c <= 'z') || IsDigit(c) || c == '.' || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

std::string HostOverride()
{
	std::string value;
#ifdef _WIN32
	char buf[256] = {};
	const DWORD len = GetEnvironmentVariableA("TRIGGLOW_API_HOST", buf, sizeof(buf));
	if (len > 0 && len < sizeof(buf))
		value.assign(buf, len);
#else
	if (const char *env = std::getenv("TRIGGLOW_API_HOST"))
		value = env;
#endif
	return IsHostname(value) ? value : std::string();
}
#endif

http::Timeouts TimeoutsFor(Timeout timeout)
{
	http::Timeouts t;
	switch (timeout) {
	case Timeout::Conversation:
		t.receiveMs = 90000;
		break;
	case Timeout::Short:
		// About 5 s in all. WinHTTP enforces its timeouts with a coarse
		// timer (measured: a step's timeout fires at the first multiple of
		// 4 s past it), so 3 s per step is what keeps a stalled call near
		// 4-5 s.
		t.resolveMs = 3000;
		t.connectMs = 3000;
		t.sendMs = 3000;
		t.receiveMs = 3000;
		t.totalMs = 5000;
		break;
	case Timeout::Upload:
		t.sendMs = 120000;
		break;
	case Timeout::Standard:
		break;
	}
	return t;
}

// Parses "X.Y.Z..." into up to three numbers; returns how many were read.
// `end` (optional) gets the index right after the last digit read.
size_t ParseVersionParts(std::string_view v, std::array<long, 3> &parts, size_t maxDigits, size_t *end = nullptr)
{
	parts = {0, 0, 0};
	size_t i = 0;
	size_t lastDigitEnd = 0;
	size_t count = 0;
	while (count < 3) {
		size_t digits = 0;
		long value = 0;
		while (i < v.size() && IsDigit(v[i]) && digits < maxDigits) {
			value = value * 10 + (v[i] - '0');
			++i;
			++digits;
		}
		if (digits == 0)
			break;
		parts[count++] = value;
		lastDigitEnd = i;
		if (count < 3) {
			if (i >= v.size() || v[i] != '.')
				break;
			++i;
		}
	}
	if (end)
		*end = lastDigitEnd;
	return count;
}

} // namespace

std::string ApiHost()
{
#ifdef TRIGGLOW_ALLOW_API_HOST_OVERRIDE
	static const std::string overridden = HostOverride();
	if (!overridden.empty())
		return overridden;
#endif
	return kDefaultApiHost;
}

std::string VerificationUrlPrefix()
{
#ifdef TRIGGLOW_ALLOW_API_HOST_OVERRIDE
	const std::string host = ApiHost();
	if (host != kDefaultApiHost)
		return "https://" + host + "/plugin/link?";
#endif
	return "https://www.trigglow.com/plugin/link?";
}

bool IsAllowedWebUrl(std::string_view url)
{
	static constexpr std::array<const char *, 4> kPages = {web::kAppSupport, web::kPublicSupport, web::kPrivacy,
							       web::kProduct};
	for (const char *page : kPages) {
		const std::string_view p(page);
		if (!StartsWith(url, p))
			continue;
		const std::string_view tail = url.substr(p.size());
		if (tail.empty())
			return true;
		if ((tail[0] == '?' || tail[0] == '#' || tail[0] == '/') && IsPlainUrlTail(tail))
			return true;
	}
	const std::string_view repo(web::kRepo);
	if (StartsWith(url, repo))
		return IsPlainUrlTail(url.substr(repo.size()));
	const std::string link = VerificationUrlPrefix();
	if (StartsWith(url, link))
		return url.size() > link.size() && IsPlainUrlTail(url.substr(link.size()));
	return false;
}

std::string AppSupportTicketUrl(std::string_view ticketId)
{
	if (!IsUuid(ticketId))
		return std::string();
	return std::string(web::kAppSupport) + "?ticket=" + std::string(ticketId);
}

std::string PublicSupportTicketUrl(std::string_view ticketId, std::string_view accessToken)
{
	if (!IsUuid(ticketId) || accessToken.empty() || accessToken.size() > 256 || !AllTokenChars(accessToken))
		return std::string();
	return std::string(web::kPublicSupport) + "?ticket=" + std::string(ticketId) +
	       "&token=" + std::string(accessToken);
}

Os CurrentOs()
{
#if defined(_WIN32)
	return Os::Windows;
#elif defined(__APPLE__)
	return Os::Macos;
#else
	return Os::Linux;
#endif
}

const char *OsName(Os os)
{
	switch (os) {
	case Os::Windows:
		return "windows";
	case Os::Macos:
		return "macos";
	default:
		return "linux";
	}
}

std::string ClientVersion(std::string_view pluginVersion)
{
	std::array<long, 3> parts{};
	size_t end = 0;
	if (ParseVersionParts(pluginVersion, parts, 3, &end) != 3)
		return "0.0.0";
	// Each part has at most 3 digits: a digit right after the third part
	// means it had more (and a fourth part after a '.' is ignored, as is a
	// "-rc1" suffix).
	if (end < pluginVersion.size() && IsDigit(pluginVersion[end]))
		return "0.0.0";
	return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]);
}

std::string UserAgent(std::string_view pluginVersion)
{
	return "TrigglowDynamicDelay/" + ClientVersion(pluginVersion) + " (OBS plugin; " + OsName(CurrentOs()) + ")";
}

json::Value ClientObject(std::string_view pluginVersion)
{
	json::Value client = json::Value::Object();
	client.Set("kind", json::Value::String("obs_dynamic_delay"));
	client.Set("version", json::Value::String(ClientVersion(pluginVersion)));
	client.Set("os", json::Value::String(OsName(CurrentOs())));
	return client;
}

int CompareVersions(std::string_view a, std::string_view b)
{
	std::array<long, 3> pa{}, pb{};
	ParseVersionParts(a, pa, 9);
	ParseVersionParts(b, pb, 9);
	for (size_t i = 0; i < 3; ++i) {
		if (pa[i] != pb[i])
			return pa[i] < pb[i] ? -1 : 1;
	}
	return 0;
}

bool IsLinkSecret(std::string_view s)
{
	return s.size() >= 32 && s.size() <= 128 && AllTokenChars(s);
}

bool IsUserCode(std::string_view s)
{
	if (s.size() != 9 || s[4] != '-')
		return false;
	for (size_t i = 0; i < 9; ++i) {
		if (i == 4)
			continue;
		const char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || IsDigit(c)))
			return false;
	}
	return true;
}

bool IsPluginToken(std::string_view s)
{
	return !s.empty() && s.size() <= 256 && AllTokenChars(s);
}

bool IsUuid(std::string_view s)
{
	if (s.size() != 36)
		return false;
	for (size_t i = 0; i < s.size(); ++i) {
		const char c = s[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-')
				return false;
		} else if (!(IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	return true;
}

bool IsLocale(std::string_view s)
{
	auto lower = [](char c) {
		return c >= 'a' && c <= 'z';
	};
	auto upper = [](char c) {
		return c >= 'A' && c <= 'Z';
	};
	if (s.size() == 2)
		return lower(s[0]) && lower(s[1]);
	return s.size() == 5 && lower(s[0]) && lower(s[1]) && s[2] == '-' && upper(s[3]) && upper(s[4]);
}

std::string NormalizeLocale(std::string_view obsLocale)
{
	auto toLower = [](char c) {
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	};
	auto toUpper = [](char c) {
		return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
	};
	std::string s;
	for (char c : obsLocale)
		s += c == '_' ? '-' : c;
	if (s.size() == 2 || (s.size() == 5 && s[2] == '-')) {
		s[0] = toLower(s[0]);
		s[1] = toLower(s[1]);
		if (s.size() == 5) {
			s[3] = toUpper(s[3]);
			s[4] = toUpper(s[4]);
		}
		if (IsLocale(s))
			return s;
	}
	return "en-US";
}

std::string LoggableCode(std::string_view code)
{
	std::string out;
	for (char c : code) {
		if (out.size() >= 40)
			break;
		if ((c >= 'a' && c <= 'z') || IsDigit(c) || c == '_')
			out += c;
		else
			return "?";
	}
	return out;
}

int RetryAfterSeconds(std::string_view header)
{
	while (!header.empty() && (header.front() == ' ' || header.front() == '\t'))
		header.remove_prefix(1);
	while (!header.empty() && (header.back() == ' ' || header.back() == '\t'))
		header.remove_suffix(1);
	if (header.empty() || header.size() > 9)
		return 60;
	long value = 0;
	for (char c : header) {
		if (!IsDigit(c))
			return 60; // An HTTP date, or garbage: the default wait.
		value = value * 10 + (c - '0');
	}
	if (value < 1)
		return 1;
	return value > 3600 ? 3600 : static_cast<int>(value);
}

std::optional<std::string> Result::BodyString(std::string_view key) const
{
	if (!body)
		return std::nullopt;
	return body->GetString(key);
}

Client::Client(Transport transport, std::string host, std::string pluginVersion)
	: transport_(std::move(transport)),
	  host_(std::move(host)),
	  pluginVersion_(std::move(pluginVersion))
{
}

Client Client::Default(std::string pluginVersion)
{
	return Client(http::Send, ApiHost(), std::move(pluginVersion));
}

Result Client::Get(std::string_view path, std::string_view bearer, Timeout timeout) const
{
	http::Request request;
	request.method = "GET";
	request.path = std::string(path);
	return Run(std::move(request), bearer, timeout);
}

Result Client::PostJson(std::string_view path, const json::Value &body, std::string_view bearer, Timeout timeout) const
{
	http::Request request;
	request.method = "POST";
	request.path = std::string(path);
	request.headers.push_back({"Content-Type", "application/json; charset=utf-8"});
	request.body = json::Serialize(body);
	return Run(std::move(request), bearer, timeout);
}

Result Client::PostFile(std::string_view path, std::string_view fileName, std::string_view contentType,
			const std::string &bytes, std::string_view bearer,
			const std::vector<http::Header> &extraHeaders) const
{
	for (const http::Header &h : extraHeaders) {
		if (!IsPluginToken(h.value)) {
			Result invalid;
			invalid.transport = http::Error::Invalid;
			return invalid;
		}
	}
	for (std::string_view s : {fileName, contentType}) {
		for (char c : s) {
			if (c == '"' || c == '\r' || c == '\n' || c == '\\') {
				Result invalid;
				invalid.transport = http::Error::Invalid;
				return invalid;
			}
		}
	}
	// Random boundary: the file is a whole log and can't be allowed to contain
	// a guessable one.
	std::mt19937_64 rng(std::random_device{}());
	std::ostringstream boundaryStream;
	boundaryStream << "----TrigglowBoundary" << std::hex << rng() << rng();
	const std::string boundary = boundaryStream.str();

	http::Request request;
	request.method = "POST";
	request.path = std::string(path);
	request.headers.push_back({"Content-Type", "multipart/form-data; boundary=" + boundary});
	for (const http::Header &h : extraHeaders)
		request.headers.push_back(h);
	std::string &body = request.body;
	body.reserve(bytes.size() + 512);
	body += "--" + boundary + "\r\n";
	body += "Content-Disposition: form-data; name=\"file\"; filename=\"";
	body.append(fileName);
	body += "\"\r\nContent-Type: ";
	body.append(contentType);
	body += "\r\n\r\n";
	body += bytes;
	body += "\r\n--" + boundary + "--\r\n";
	return Run(std::move(request), bearer, Timeout::Upload);
}

Result Client::Run(http::Request request, std::string_view bearer, Timeout timeout) const
{
	Result result;
	if (!bearer.empty() && !IsPluginToken(bearer)) {
		result.transport = http::Error::Invalid;
		return result;
	}
	request.host = host_;
	request.userAgent = UserAgent(pluginVersion_);
	request.timeouts = TimeoutsFor(timeout);
	request.cancel = cancel_;
	request.headers.push_back({"Accept", "application/json"});
	if (!bearer.empty())
		request.headers.push_back({"Authorization", "Bearer " + std::string(bearer)});

	const http::Response response = transport_(request);
	if (!response.Ok()) {
		result.outcome = response.error == http::Error::Unavailable ? Outcome::Unavailable : Outcome::Network;
		result.transport = response.error;
		return result;
	}
	result.status = response.status;
	result.outcome = (response.status >= 200 && response.status < 300) ? Outcome::Ok : Outcome::HttpError;
	if (!response.body.empty())
		result.body = json::Parse(response.body);
	if (result.outcome == Outcome::HttpError) {
		if (auto code = result.BodyString("error"))
			result.error = *code;
		else if (auto status = result.BodyString("status"))
			result.error = *status;
		if (response.status == 429)
			result.retryAfterSeconds = RetryAfterSeconds(response.retryAfter);
	}
	return result;
}

} // namespace trigglow::api
