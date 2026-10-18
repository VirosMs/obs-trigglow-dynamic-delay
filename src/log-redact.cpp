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

#include "log-redact.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace trigglow::logredact {

namespace {

using std::string;
using std::string_view;

bool IsAsciiLetter(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool IsHexDigit(char c)
{
	return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// \w of ECMAScript regex over bytes: [A-Za-z0-9_]. UTF-8 bytes are not word
// characters, as with std::regex in the "C" locale.
bool IsWord(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '_';
}

// \s over bytes.
bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

char Lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool StartsWithAt(string_view s, size_t pos, string_view literal)
{
	return s.size() - pos >= literal.size() && s.compare(pos, literal.size(), literal) == 0;
}

bool StartsWithAtI(string_view s, size_t pos, string_view lowerLiteral)
{
	if (s.size() - pos < lowerLiteral.size())
		return false;
	for (size_t i = 0; i < lowerLiteral.size(); ++i) {
		if (Lower(s[pos + i]) != lowerLiteral[i])
			return false;
	}
	return true;
}

// \b before position i, for a match that starts with a word character.
bool WordBoundaryBefore(string_view s, size_t i)
{
	return i == 0 || !IsWord(s[i - 1]);
}

// Shared driver: at each position, `match(s, i, replacement)` returns the end
// of a match starting at i (or 0 for none) and fills `replacement`. Matches
// never overlap; scanning resumes right after each one.
template<typename MatchFn> string ReplaceAll(string_view s, MatchFn match)
{
	string out;
	size_t last = 0;
	size_t i = 0;
	string replacement;
	while (i < s.size()) {
		replacement.clear();
		const size_t end = match(s, i, replacement);
		if (end > i) {
			if (out.empty())
				out.reserve(s.size());
			out.append(s.substr(last, i - last));
			out += replacement;
			i = end;
			last = end;
		} else {
			++i;
		}
	}
	if (last == 0)
		return string(s);
	out.append(s.substr(last));
	return out;
}

// --- 1. Stream URLs ---------------------------------------------------------

bool IsUrlStop(char c)
{
	return IsSpace(c) || c == '"' || c == '\'' || c == '<' || c == '>';
}

size_t MatchStreamUrl(string_view s, size_t i, string &replacement)
{
	if (!IsAsciiLetter(s[i]) || !WordBoundaryBefore(s, i))
		return 0;
	static constexpr std::array<string_view, 6> kSchemes = {"rtmps://", "rtmp://", "srt://",
								"rist://",  "rtsp://", "whip://"};
	for (string_view scheme : kSchemes) {
		if (!StartsWithAtI(s, i, scheme))
			continue;
		size_t end = i + scheme.size();
		while (end < s.size() && !IsUrlStop(s[end]))
			++end;
		if (end == i + scheme.size())
			return 0; // "[^\s"'<>]+" needs at least one character.
		replacement = "<stream-url>";
		return end;
	}
	return 0;
}

// --- 2. Web URLs (only the host stays) -------------------------------------

bool IsHostChar(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '.' || c == '-';
}

size_t MatchWebUrl(string_view s, size_t i, string &replacement)
{
	if (Lower(s[i]) != 'h' || !WordBoundaryBefore(s, i))
		return 0;
	size_t p = 0;
	if (StartsWithAtI(s, i, "https://"))
		p = i + 8;
	else if (StartsWithAtI(s, i, "http://"))
		p = i + 7;
	else
		return 0;

	// Optional "user:password@": the longest run without @ / or a stop
	// character, taken only when an '@' ends it and a host follows.
	auto hostEnd = [&](size_t from) {
		size_t e = from;
		while (e < s.size() && IsHostChar(s[e]))
			++e;
		return e;
	};
	size_t hostStart = p;
	size_t q = p;
	while (q < s.size() && s[q] != '@' && s[q] != '/' && !IsUrlStop(s[q]))
		++q;
	if (q > p && q < s.size() && s[q] == '@' && hostEnd(q + 1) > q + 1)
		hostStart = q + 1;
	size_t end = hostEnd(hostStart);
	if (end == hostStart)
		return 0;
	const string_view host = s.substr(hostStart, end - hostStart);

	// Optional ":port".
	if (end + 1 < s.size() && s[end] == ':' && IsDigit(s[end + 1])) {
		end += 1;
		while (end < s.size() && IsDigit(s[end]))
			++end;
	}
	// Optional path, query or fragment.
	if (end < s.size() && (s[end] == '/' || s[end] == '?' || s[end] == '#')) {
		++end;
		while (end < s.size() && !IsUrlStop(s[end]))
			++end;
	}
	replacement = "<url:";
	replacement.append(host);
	replacement += '>';
	return end;
}

// --- 3. Windows user folders -------------------------------------------------

bool IsPathSeparator(char c)
{
	return c == '\\' || c == '/';
}

bool IsWindowsNameChar(char c)
{
	return !(c == '\\' || c == '/' || c == '\r' || c == '\n' || c == '"' || c == '\'' || c == ':' || c == '*' ||
		 c == '?' || c == '<' || c == '>' || c == '|');
}

size_t MatchWindowsUserPath(string_view s, size_t i, string &replacement)
{
	if (!IsAsciiLetter(s[i]) || i + 2 >= s.size() || s[i + 1] != ':' || !IsPathSeparator(s[i + 2]))
		return 0;
	size_t p = i + 2;
	while (p < s.size() && IsPathSeparator(s[p]))
		++p;
	if (StartsWithAtI(s, p, "users"))
		p += 5;
	else if (StartsWithAtI(s, p, "documents and settings"))
		p += 22;
	else
		return 0;
	if (p >= s.size() || !IsPathSeparator(s[p]))
		return 0;
	while (p < s.size() && IsPathSeparator(s[p]))
		++p;
	size_t end = p;
	while (end < s.size() && IsWindowsNameChar(s[end]))
		++end;
	if (end == p)
		return 0;
	replacement.assign(s.substr(i, p - i));
	replacement += "<user>";
	return end;
}

// --- 4. macOS and Linux home folders -----------------------------------------

// The home folders' parent: "Users" (macOS) or "home" (Linux), between two
// '/'. Returns the index right after the second '/', or 0.
size_t PosixHomeParentEnd(string_view s, size_t i)
{
	if (s[i] != '/')
		return 0;
	static constexpr std::array<string_view, 2> kParents = {"Users", "home"};
	for (string_view parent : kParents) {
		const size_t slash = i + 1 + parent.size();
		if (slash < s.size() && s[slash] == '/' && StartsWithAt(s, i + 1, parent))
			return slash + 1;
	}
	return 0;
}

size_t MatchPosixHome(string_view s, size_t i, string &replacement)
{
	const size_t p = PosixHomeParentEnd(s, i);
	if (p == 0)
		return 0;
	size_t end = p;
	while (end < s.size() && s[end] != '/' && !IsSpace(s[end]) && s[end] != '"' && s[end] != '\'')
		++end;
	if (end == p)
		return 0;
	replacement.assign(s.substr(i, p - i));
	replacement += "<user>";
	return end;
}

// --- 5. Overlay links ------------------------------------------------------

bool IsSlugChar(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '_' || c == '-';
}

size_t MatchOverlaySlug(string_view s, size_t i, string &replacement)
{
	if (s[i] != '/' || !StartsWithAt(s, i, "/overlay/"))
		return 0;
	const size_t p = i + 9;
	size_t end = p;
	while (end < s.size() && IsSlugChar(s[end]))
		++end;
	if (end == p)
		return 0;
	replacement = "/overlay/<slug>";
	return end;
}

// --- 6. Emails ---------------------------------------------------------------

bool IsEmailLocalChar(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}

// Emails go through their own loop: a local part is a whole run of local
// characters, so a failed run is skipped in one step (the regex would try,
// and fail, at every position inside it).
string RedactEmails(string_view s)
{
	string out;
	size_t last = 0;
	size_t i = 0;
	while (i < s.size()) {
		if (!IsEmailLocalChar(s[i])) {
			++i;
			continue;
		}
		size_t q = i;
		while (q < s.size() && IsEmailLocalChar(s[q]))
			++q;
		// [i, q) is a maximal run of local characters: a match needs '@' at q.
		if (q < s.size() && s[q] == '@') {
			const size_t d = q + 1;
			size_t r = d;
			while (r < s.size() && IsHostChar(s[r]))
				++r;
			// Domain: the last '.' in [d, r) (with something before it)
			// followed by 2+ letters; the match ends after those letters.
			size_t matchEnd = 0;
			for (size_t j = r; j-- > d + 1;) {
				if (s[j] != '.')
					continue;
				size_t letters = j + 1;
				while (letters < r && IsAsciiLetter(s[letters]))
					++letters;
				if (letters - (j + 1) >= 2) {
					matchEnd = letters;
					break;
				}
			}
			if (matchEnd > 0) {
				if (out.empty())
					out.reserve(s.size());
				out.append(s.substr(last, i - last));
				out += "<email>";
				i = matchEnd;
				last = matchEnd;
				continue;
			}
		}
		i = q; // No match starts anywhere in this run.
	}
	if (last == 0)
		return string(s);
	out.append(s.substr(last));
	return out;
}

// --- 7. Secrets after a known key --------------------------------------------

bool IsSecretValueChar(char c)
{
	return !(IsSpace(c) || c == '&' || c == '"' || c == '\'' || c == ',' || c == ';');
}

size_t MatchKeyedSecret(string_view s, size_t i, string &replacement)
{
	if (!IsAsciiLetter(s[i]) || !WordBoundaryBefore(s, i))
		return 0;
	// Same order as the alternation; a key that matches but isn't followed by
	// a separator falls through to the next one ("sig" -> "signature").
	static constexpr std::array<string_view, 13> kKeys = {"token",     "secret",   "access_token", "refresh_token",
							      "auth",      "password", "passwd",       "sig",
							      "signature", "apikey",   "api_key",      "stream_key",
							      "streamkey"};
	for (string_view key : kKeys) {
		if (!StartsWithAtI(s, i, key))
			continue;
		size_t p = i + key.size();
		while (p < s.size() && IsSpace(s[p]))
			++p;
		if (p >= s.size() || (s[p] != '=' && s[p] != ':'))
			continue;
		++p;
		while (p < s.size() && IsSpace(s[p]))
			++p;
		size_t end = p;
		while (end < s.size() && IsSecretValueChar(s[end]))
			++end;
		if (end == p)
			continue;
		replacement.assign(s.substr(i, key.size()));
		replacement += "=<redacted>";
		return end;
	}
	if (StartsWithAtI(s, i, "bearer")) {
		size_t p = i + 6;
		if (p >= s.size() || !IsSpace(s[p]))
			return 0;
		while (p < s.size() && IsSpace(s[p]))
			++p;
		size_t end = p;
		while (end < s.size() && IsSecretValueChar(s[end]))
			++end;
		if (end == p)
			return 0;
		replacement.assign(s.substr(i, 6));
		replacement += " <redacted>";
		return end;
	}
	return 0;
}

// --- 8. IPv4 -----------------------------------------------------------------

// One octet starting at p: the whole run of digits, as the regex can only
// end an octet before '.' or a word boundary. Returns its end, or 0.
size_t Ipv4Octet(string_view s, size_t p)
{
	size_t e = p;
	while (e < s.size() && IsDigit(s[e]))
		++e;
	const size_t len = e - p;
	if (len == 0 || len > 3)
		return 0;
	if (len == 3) {
		const char a = s[p], b = s[p + 1], c = s[p + 2];
		const bool ok = a == '1' || (a == '2' && b >= '0' && b <= '4') || (a == '2' && b == '5' && c <= '5');
		if (!ok)
			return 0;
	}
	return e;
}

size_t MatchIpv4(string_view s, size_t i, string &replacement)
{
	if (!IsDigit(s[i]) || !WordBoundaryBefore(s, i))
		return 0;
	size_t p = i;
	for (int part = 0; part < 4; ++part) {
		const size_t e = Ipv4Octet(s, p);
		if (e == 0)
			return 0;
		if (part < 3) {
			if (e >= s.size() || s[e] != '.')
				return 0;
			p = e + 1;
		} else {
			if (e < s.size() && IsWord(s[e]))
				return 0; // Trailing \b.
			p = e;
		}
	}
	replacement = "<ip>";
	return p;
}

// --- 9. IPv6 -----------------------------------------------------------------

size_t HexRunEnd(string_view s, size_t p)
{
	size_t e = p;
	while (e < s.size() && IsHexDigit(s[e]))
		++e;
	return e;
}

// \b(?:[0-9A-Fa-f]{1,4}:){4,7}[0-9A-Fa-f]{1,4}\b
size_t MatchIpv6Full(string_view s, size_t i)
{
	if (!IsHexDigit(s[i]) || !WordBoundaryBefore(s, i))
		return 0;
	// Ends of the "group:" units in a row (each group a whole hex run of
	// 1-4 digits, as a shorter one would be followed by another hex digit).
	std::array<size_t, 8> unitEnds{};
	size_t units = 0;
	size_t p = i;
	while (units < 8) {
		const size_t e = HexRunEnd(s, p);
		if (e == p || e - p > 4 || e >= s.size() || s[e] != ':')
			break;
		unitEnds[units++] = e + 1;
		p = e + 1;
	}
	const size_t maxReps = units < 7 ? units : 7;
	for (size_t reps = maxReps; reps >= 4; --reps) {
		const size_t g = unitEnds[reps - 1];
		const size_t e = HexRunEnd(s, g);
		if (e == g || e - g > 4)
			continue;
		if (e < s.size() && IsWord(s[e]))
			continue;
		return e;
	}
	return 0;
}

bool IsIpv6TokenChar(char c)
{
	return IsHexDigit(c) || c == ':';
}

// A whole run of [0-9A-Fa-f:] standing on its own with exactly one "::",
// at most 7 groups of 1-4 hex digits, and at least one group ("fe80::1",
// "::1", "2001:db8::7334").
size_t MatchIpv6Compressed(string_view s, size_t i)
{
	if (!IsIpv6TokenChar(s[i]) || (i > 0 && (IsWord(s[i - 1]) || s[i - 1] == ':')))
		return 0;
	size_t e = i;
	while (e < s.size() && IsIpv6TokenChar(s[e]))
		++e;
	if (e < s.size() && IsWord(s[e]))
		return 0;
	const string_view token = s.substr(i, e - i);
	const size_t dc = token.find("::");
	if (dc == string_view::npos || token.find("::", dc + 1) != string_view::npos ||
	    token.find(":::") != string_view::npos)
		return 0;
	size_t groups = 0;
	size_t runLen = 0;
	for (char c : token) {
		if (c == ':') {
			if (runLen > 4)
				return 0;
			if (runLen > 0)
				++groups;
			runLen = 0;
		} else {
			++runLen;
		}
	}
	if (runLen > 4)
		return 0;
	if (runLen > 0)
		++groups;
	if (groups == 0 || groups > 7)
		return 0;
	return e;
}

size_t MatchIpv6(string_view s, size_t i, string &replacement)
{
	size_t end = MatchIpv6Full(s, i);
	if (end == 0)
		end = MatchIpv6Compressed(s, i);
	if (end == 0)
		return 0;
	replacement = "<ip>";
	return end;
}

// --- 10. Long tokens -------------------------------------------------------

string RedactLongTokens(string_view s)
{
	string out;
	size_t last = 0;
	size_t i = 0;
	while (i < s.size()) {
		if (!IsSlugChar(s[i])) {
			++i;
			continue;
		}
		size_t e = i;
		bool digit = false, letter = false;
		while (e < s.size() && IsSlugChar(s[e])) {
			digit = digit || IsDigit(s[e]);
			letter = letter || IsAsciiLetter(s[e]);
			++e;
		}
		if (e - i >= 24 && digit && letter) {
			if (out.empty())
				out.reserve(s.size());
			out.append(s.substr(last, i - last));
			out += "<token>";
			last = e;
		}
		i = e;
	}
	if (last == 0)
		return string(s);
	out.append(s.substr(last));
	return out;
}

// Cheap pre-checks so the common log line (no URL, path, '@', key or IP)
// skips most passes.
bool ContainsI(string_view s, string_view lowerNeedle)
{
	if (lowerNeedle.size() > s.size())
		return false;
	for (size_t i = 0; i + lowerNeedle.size() <= s.size(); ++i) {
		if (StartsWithAtI(s, i, lowerNeedle))
			return true;
	}
	return false;
}

bool HasIpv4Shape(string_view s)
{
	int dots = 0;
	for (char c : s)
		dots += c == '.';
	return dots >= 3;
}

bool HasIpv6Shape(string_view s)
{
	if (s.find("::") != string_view::npos)
		return true;
	int colons = 0;
	for (char c : s)
		colons += c == ':';
	return colons >= 4;
}

bool MayHaveKeyedSecret(string_view s)
{
	if (s.find('=') == string_view::npos && s.find(':') == string_view::npos && !ContainsI(s, "bearer"))
		return false;
	static constexpr std::array<string_view, 7> kStems = {"token", "secret", "auth",  "pass",
							      "sig",   "key",    "bearer"};
	for (string_view stem : kStems) {
		if (ContainsI(s, stem))
			return true;
	}
	return false;
}

} // namespace

string RedactLine(string_view line)
{
	string s(line);
	if (s.find("://") != string::npos) {
		s = ReplaceAll(s, MatchStreamUrl);
		s = ReplaceAll(s, MatchWebUrl);
	}
	if (s.find(":\\") != string::npos || s.find(":/") != string::npos)
		s = ReplaceAll(s, MatchWindowsUserPath);
	if (s.find("Users/") != string::npos || s.find("home/") != string::npos)
		s = ReplaceAll(s, MatchPosixHome);
	if (s.find("/overlay/") != string::npos)
		s = ReplaceAll(s, MatchOverlaySlug);
	if (s.find('@') != string::npos)
		s = RedactEmails(s);
	if (MayHaveKeyedSecret(s))
		s = ReplaceAll(s, MatchKeyedSecret);
	if (HasIpv4Shape(s))
		s = ReplaceAll(s, MatchIpv4);
	if (HasIpv6Shape(s))
		s = ReplaceAll(s, MatchIpv6);
	if (s.size() >= 24)
		s = RedactLongTokens(s);
	return s;
}

string RedactText(string_view text)
{
	string out;
	out.reserve(text.size());
	size_t start = 0;
	while (start < text.size()) {
		size_t nl = text.find('\n', start);
		const size_t end = nl == string_view::npos ? text.size() : nl;
		// Keep a "\r" before the "\n" out of the line, so no rule sees it.
		size_t lineEnd = end;
		if (lineEnd > start && text[lineEnd - 1] == '\r')
			--lineEnd;
		out += RedactLine(text.substr(start, lineEnd - start));
		out.append(text.substr(lineEnd, end - lineEnd));
		if (nl == string_view::npos)
			break;
		out += '\n';
		start = nl + 1;
	}
	return out;
}

namespace {

// Moves `pos` back to the start of the UTF-8 sequence it may be inside of.
size_t Utf8Boundary(const string &s, size_t pos)
{
	while (pos > 0 && pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
		--pos;
	return pos;
}

} // namespace

string CapForUpload(string text, bool *capped)
{
	if (capped)
		*capped = false;
	if (text.size() <= kUploadCapBytes)
		return text;
	if (capped)
		*capped = true;

	const string marker = kCutMarker;
	// Head: the first 256 KB, cut after its last full line.
	size_t headEnd = Utf8Boundary(text, kUploadHeadBytes);
	const size_t lastNewline = text.rfind('\n', headEnd - 1);
	if (lastNewline != string::npos && lastNewline > 0)
		headEnd = lastNewline + 1;

	// Tail: whatever fits after the marker, from the start of a line.
	const size_t tailBudget = kUploadCapBytes - headEnd - marker.size();
	size_t tailStart = text.size() - tailBudget;
	const size_t nextNewline = text.find('\n', tailStart);
	if (nextNewline != string::npos && nextNewline + 1 < text.size()) {
		tailStart = nextNewline + 1;
	} else {
		// Forward, so the result never grows past the cap.
		while (tailStart < text.size() && (static_cast<unsigned char>(text[tailStart]) & 0xC0) == 0x80)
			++tailStart;
	}
	if (tailStart < headEnd)
		tailStart = headEnd;

	string out;
	out.reserve(headEnd + marker.size() + (text.size() - tailStart));
	out.append(text, 0, headEnd);
	out += marker;
	out.append(text, tailStart, string::npos);
	return out;
}

Preview MakePreview(string_view text, size_t lines)
{
	Preview preview;
	std::vector<size_t> starts;
	starts.push_back(0);
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\n' && i + 1 < text.size())
			starts.push_back(i + 1);
	}
	preview.totalLines = text.empty() ? 0 : starts.size();
	if (preview.totalLines <= 2 * lines) {
		preview.head = string(text);
		return preview;
	}
	preview.truncated = true;
	preview.head = string(text.substr(0, starts[lines]));
	preview.tail = string(text.substr(starts[preview.totalLines - lines]));
	return preview;
}

} // namespace trigglow::logredact
