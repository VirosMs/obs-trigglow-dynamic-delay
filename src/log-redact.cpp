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

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace trigglow::logredact {

namespace {

using std::string;
using std::string_view;

bool IsAsciiLetter(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsUpper(char c)
{
	return c >= 'A' && c <= 'Z';
}

bool IsLower(char c)
{
	return c >= 'a' && c <= 'z';
}

bool IsDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool IsHexDigit(char c)
{
	return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Any byte of a multi-byte UTF-8 sequence ("é", "ñ"...).
bool IsUtf8Byte(char c)
{
	return (static_cast<unsigned char>(c) & 0x80) != 0;
}

// A letter of a name in any script: ASCII letters and digits, or UTF-8 bytes.
bool IsNameLetter(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || IsUtf8Byte(c);
}

// \w of ECMAScript regex over bytes: [A-Za-z0-9_]. UTF-8 bytes are not word
// characters, as with std::regex in the "C" locale.
bool IsWord(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '_';
}

// A character of an identifier such as "x_api_key" or "X-Api-Key".
bool IsIdentChar(char c)
{
	return IsWord(c) || c == '-';
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
	return pos <= s.size() && s.size() - pos >= literal.size() && s.compare(pos, literal.size(), literal) == 0;
}

bool StartsWithAtI(string_view s, size_t pos, string_view lowerLiteral)
{
	if (pos > s.size() || s.size() - pos < lowerLiteral.size())
		return false;
	for (size_t i = 0; i < lowerLiteral.size(); ++i) {
		if (Lower(s[pos + i]) != lowerLiteral[i])
			return false;
	}
	return true;
}

bool EqualsI(string_view s, string_view lowerLiteral)
{
	return s.size() == lowerLiteral.size() && StartsWithAtI(s, 0, lowerLiteral);
}

// \b before position i, for a match that starts with a word character.
bool WordBoundaryBefore(string_view s, size_t i)
{
	return i == 0 || !IsWord(s[i - 1]);
}

size_t SkipBlanks(string_view s, size_t p)
{
	while (p < s.size() && (s[p] == ' ' || s[p] == '\t'))
		++p;
	return p;
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

// --- 1 and 2. URLs ------------------------------------------------------------

bool IsUrlStop(char c)
{
	return IsSpace(c) || c == '"' || c == '\'' || c == '<' || c == '>';
}

bool IsSchemeChar(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '+' || c == '-' || c == '.';
}

// "scheme://" at i, or its JSON-escaped form "scheme:\/\/". Returns the index
// right after it (0 for none) and the scheme in `scheme`. The scheme starts a
// word and has at least two characters, so "C://x" (a drive) isn't one.
size_t SchemeEnd(string_view s, size_t i, string_view &scheme)
{
	if (!IsAsciiLetter(s[i]) || (i > 0 && IsSchemeChar(s[i - 1])))
		return 0;
	size_t p = i + 1;
	while (p < s.size() && p - i <= 32 && IsSchemeChar(s[p]))
		++p;
	if (p - i < 2 || p >= s.size() || s[p] != ':')
		return 0;
	scheme = s.substr(i, p - i);
	if (StartsWithAt(s, p, "://"))
		return p + 3;
	if (StartsWithAt(s, p, ":\\/\\/"))
		return p + 5;
	return 0;
}

// Streaming protocols: the whole URL can be the stream key.
bool IsStreamScheme(string_view scheme)
{
	return StartsWithAtI(scheme, 0, "rtmp") || StartsWithAtI(scheme, 0, "rtsp") || EqualsI(scheme, "srt") ||
	       EqualsI(scheme, "rist") || EqualsI(scheme, "whip") || EqualsI(scheme, "whep");
}

size_t MatchStreamUrl(string_view s, size_t i, string &replacement)
{
	string_view scheme;
	const size_t p = SchemeEnd(s, i, scheme);
	if (p == 0 || !IsStreamScheme(scheme))
		return 0;
	size_t end = p;
	while (end < s.size() && !IsUrlStop(s[end]))
		++end;
	if (end == p)
		return 0; // "rtmp://" alone.
	replacement = "<stream-url>";
	return end;
}

bool IsHostChar(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '.' || c == '-';
}

bool IsUrlHostChar(char c)
{
	return IsHostChar(c) || c == '_' || IsUtf8Byte(c);
}

// Any other scheme (https, wss, ftp, udp, obsws...): only the host stays.
size_t MatchWebUrl(string_view s, size_t i, string &replacement)
{
	string_view scheme;
	const size_t p = SchemeEnd(s, i, scheme);
	if (p == 0 || IsStreamScheme(scheme) || EqualsI(scheme, "file"))
		return 0;
	// The authority ends at the first '/', '?', '#' or '\' (as browsers read
	// it); the credentials in it end at its LAST '@' ("user:p@ss@host").
	size_t authEnd = p;
	while (authEnd < s.size() && !IsUrlStop(s[authEnd]) && s[authEnd] != '/' && s[authEnd] != '?' &&
	       s[authEnd] != '#' && s[authEnd] != '\\')
		++authEnd;
	const size_t at = s.substr(p, authEnd - p).rfind('@');
	const size_t hostStart = at == string_view::npos ? p : p + at + 1;
	size_t hostEnd = hostStart;
	if (hostStart < authEnd && s[hostStart] == '[') {
		// "[2001:db8::1]": the IPv6 rule hides it later.
		const size_t close = s.substr(hostStart, authEnd - hostStart).find(']');
		hostEnd = close == string_view::npos ? hostStart : hostStart + close + 1;
	} else {
		while (hostEnd < authEnd && IsUrlHostChar(s[hostEnd]))
			++hostEnd;
	}
	if (hostEnd == hostStart && at == string_view::npos)
		return 0; // No host: "https:// nothing", "file:///".
	// The rest of the authority (":port"), then the path, query and fragment
	// ("\/" in JSON-escaped text counts as '/').
	size_t end = authEnd;
	if (end < s.size() && (s[end] == '/' || s[end] == '?' || s[end] == '#' || s[end] == '\\')) {
		++end;
		while (end < s.size() && !IsUrlStop(s[end]))
			++end;
	}
	if (hostEnd == hostStart) {
		replacement = "<url>"; // Credentials and no readable host: nothing stays.
		return end;
	}
	replacement = "<url:";
	replacement.append(s.substr(hostStart, hostEnd - hostStart));
	replacement += '>';
	return end;
}

// --- 3 and 4. User folders ---------------------------------------------------

bool IsPathSeparator(char c)
{
	return c == '\\' || c == '/';
}

bool IsWindowsNameChar(char c)
{
	return !(c == '\\' || c == '/' || c == '\r' || c == '\n' || c == '"' || c == '\'' || c == ':' || c == '*' ||
		 c == '?' || c == '<' || c == '>' || c == '|');
}

// End of a Windows file name starting at p. An apostrophe between two letters
// belongs to the name ("O'Brien"); one before a space, a separator or the end
// of the line is a quote around the path ("'C:\...\zoe'").
size_t WindowsNameEnd(string_view s, size_t p)
{
	size_t e = p;
	while (e < s.size()) {
		if (s[e] == '\'') {
			if (e > p && e + 1 < s.size() && IsNameLetter(s[e - 1]) && IsNameLetter(s[e + 1])) {
				++e;
				continue;
			}
			break;
		}
		if (!IsWindowsNameChar(s[e]))
			break;
		++e;
	}
	return e;
}

// After `p` (just past a separator): "Users" or "Documents and Settings" (any
// case), one or more separators, and the user's folder name. Returns where the
// name starts (0 for none) and its end in `nameEnd`.
size_t ProfileNameStart(string_view s, size_t p, size_t &nameEnd)
{
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
	nameEnd = WindowsNameEnd(s, p);
	return nameEnd > p ? p : 0;
}

// 3. "<drive>:\Users\<name>", any drive and separator ("<drive>:/users/x",
// JSON-escaped "<drive>:\\Users\\x"), and "\Users\<name>" after anything else
// that uses backslashes (a network share "\\host\c$\Users\x", "\\?\<drive>:\...").
size_t MatchWindowsUserPath(string_view s, size_t i, string &replacement)
{
	size_t p = 0;
	if (IsAsciiLetter(s[i]) && i + 2 < s.size() && s[i + 1] == ':' && IsPathSeparator(s[i + 2]))
		p = i + 2;
	else if (s[i] == '\\' && (i == 0 || !IsPathSeparator(s[i - 1])))
		p = i; // Only where a run of separators starts: each run is read once.
	else
		return 0;
	while (p < s.size() && IsPathSeparator(s[p]))
		++p;
	size_t nameEnd = 0;
	const size_t name = ProfileNameStart(s, p, nameEnd);
	if (name == 0)
		return 0;
	replacement.assign(s.substr(i, name - i));
	replacement += "<user>";
	return nameEnd;
}

// "\\host\share": the computer's name (often its owner's) goes. Not "\\?\" or
// "\\.\" (Windows' own prefixes), and only where a path can start, so the "\\"
// of JSON-escaped text ("C:\\Program Files") isn't taken for one.
size_t MatchUncHost(string_view s, size_t i, string &replacement)
{
	if (s[i] != '\\' || i + 2 >= s.size() || s[i + 1] != '\\')
		return 0;
	if (i > 0) {
		const char b = s[i - 1];
		if (!(IsSpace(b) || b == '"' || b == '\'' || b == '(' || b == '[' || b == '=' || b == ','))
			return 0;
	}
	const size_t p = i + 2;
	if (!(IsAsciiLetter(s[p]) || IsDigit(s[p])))
		return 0;
	size_t e = p;
	while (e < s.size() && (IsAsciiLetter(s[e]) || IsDigit(s[e]) || s[e] == '-' || s[e] == '_' || s[e] == '.'))
		++e;
	if (e >= s.size() || s[e] != '\\')
		return 0;
	replacement = "\\\\<host>";
	return e;
}

// 4. macOS and Linux homes: "/Users/<name>" (any case: macOS disks usually
// ignore it) and "/home/<name>" (that exact case, as on Linux). The name ends
// at the next '/', space or quote.
size_t PosixHomeParentEnd(string_view s, size_t i)
{
	if (s[i] != '/')
		return 0;
	if (StartsWithAtI(s, i + 1, "users") && i + 6 < s.size() && s[i + 6] == '/')
		return i + 7;
	if (StartsWithAt(s, i + 1, "home") && i + 5 < s.size() && s[i + 5] == '/')
		return i + 6;
	return 0;
}

size_t MatchPosixHome(string_view s, size_t i, string &replacement)
{
	const size_t p = PosixHomeParentEnd(s, i);
	if (p == 0)
		return 0;
	size_t end = p;
	while (end < s.size() && s[end] != '/' && !IsSpace(s[end]) && s[end] != '"' && s[end] != '\'' && s[end] != '<')
		++end;
	if (end == p)
		return 0;
	replacement.assign(s.substr(i, p - i));
	replacement += "<user>";
	return end;
}

// A profile folder moved somewhere else ("D:\Profiles\<name>\Documents"): the
// folder right before one of a profile's own sub-folders is taken for the
// user's. Returns the end of the sub-folder's name at p, or 0.
size_t ProfileSubfolderEnd(string_view s, size_t p)
{
	static constexpr std::array<string_view, 17> kSubfolders = {
		"appdata",     "application data", "desktop", "documents",    "downloads", "favorites",
		"links",       "movies",           "music",   "my documents", "onedrive",  "pictures",
		"saved games", "searches",         "videos",  "contacts",     "3d objects"};
	for (string_view folder : kSubfolders) {
		if (!StartsWithAtI(s, p, folder))
			continue;
		const size_t e = p + folder.size();
		if (folder == "onedrive" && StartsWithAtI(s, e, " - "))
			return e; // "OneDrive - <company>"
		if (e == s.size() || IsPathSeparator(s[e]) || IsSpace(s[e]) || s[e] == '"' || s[e] == '\'' ||
		    s[e] == ')' || s[e] == ']' || s[e] == ',' || s[e] == ';')
			return e;
	}
	return 0;
}

size_t MatchProfileOwner(string_view s, size_t i, string &replacement)
{
	if (!IsPathSeparator(s[i]))
		return 0;
	const size_t p = i + 1;
	if (p >= s.size() || IsPathSeparator(s[p]) || s[p] == '<' || s[p] == '.')
		return 0;
	const size_t e = WindowsNameEnd(s, p);
	if (e == p || e >= s.size() || !IsPathSeparator(s[e]))
		return 0;
	if (ProfileSubfolderEnd(s, p) == e)
		return 0; // "\Documents\Videos": a sub-folder itself, not a user.
	size_t q = e;
	while (q < s.size() && IsPathSeparator(s[q]))
		++q;
	if (ProfileSubfolderEnd(s, q) == 0)
		return 0;
	replacement.assign(1, s[i]);
	replacement += "<user>";
	return e;
}

// "OneDrive - Company Name": OneDrive for work names its folder after the
// employer.
size_t MatchOneDriveOrg(string_view s, size_t i, string &replacement)
{
	static constexpr string_view kPrefix = "onedrive - ";
	if (!IsPathSeparator(s[i]) || !StartsWithAtI(s, i + 1, kPrefix))
		return 0;
	const size_t p = i + 1 + kPrefix.size();
	const size_t e = WindowsNameEnd(s, p);
	if (e == p)
		return 0;
	replacement.assign(s.substr(i, p - i));
	replacement += "<org>";
	return e;
}

// --- 5. Overlay links ------------------------------------------------------

bool IsSlugChar(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '_' || c == '-';
}

size_t MatchOverlaySlug(string_view s, size_t i, string &replacement)
{
	size_t p = 0;
	if (StartsWithAt(s, i, "/overlay/"))
		p = i + 9;
	else if (StartsWithAt(s, i, "\\/overlay\\/"))
		p = i + 11;
	else
		return 0;
	size_t end = p;
	while (end < s.size() && IsSlugChar(s[end]))
		++end;
	if (end == p)
		return 0;
	replacement.assign(s.substr(i, p - i));
	replacement += "<slug>";
	return end;
}

// --- 6. Emails ---------------------------------------------------------------

// "%40": an '@' in URL-encoded text ("contact=bob%40example.com").
bool IsEncodedAt(string_view s, size_t q)
{
	return q + 2 < s.size() && s[q] == '%' && s[q + 1] == '4' && s[q + 2] == '0';
}

// The local part takes letters of any script, the usual symbols, and an
// apostrophe between two letters ("o'brien@...").
bool IsEmailLocalCharAt(string_view s, size_t q)
{
	const char c = s[q];
	if (IsAsciiLetter(c) || IsDigit(c) || IsUtf8Byte(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-')
		return true;
	return c == '\'' && q > 0 && q + 1 < s.size() && IsNameLetter(s[q - 1]) && IsNameLetter(s[q + 1]);
}

bool IsEmailDomainChar(char c)
{
	return IsHostChar(c) || IsUtf8Byte(c);
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
		if (!IsEmailLocalCharAt(s, i) || IsEncodedAt(s, i)) {
			++i;
			continue;
		}
		size_t q = i;
		while (q < s.size() && IsEmailLocalCharAt(s, q) && !IsEncodedAt(s, q))
			++q;
		// [i, q) is a maximal run of local characters: a match needs '@' (or
		// "%40") at q.
		const size_t atLen = (q < s.size() && s[q] == '@') ? 1 : (IsEncodedAt(s, q) ? 3 : 0);
		if (atLen > 0) {
			const size_t d = q + atLen;
			size_t r = d;
			while (r < s.size() && IsEmailDomainChar(s[r]))
				++r;
			// Domain: the last '.' in [d, r) (with something before it)
			// followed by 2+ letters; the match ends after those letters.
			size_t matchEnd = 0;
			for (size_t j = r; j-- > d + 1;) {
				if (s[j] != '.')
					continue;
				size_t letters = j + 1;
				while (letters < r && (IsAsciiLetter(s[letters]) || IsUtf8Byte(s[letters])))
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
		i = q > i ? q : i + 1; // No match starts anywhere in this run.
	}
	if (last == 0)
		return string(s);
	out.append(s.substr(last));
	return out;
}

// --- 7. Secrets after a known key, and credential headers ----------------------

bool IsSecretValueChar(char c)
{
	return !(IsSpace(c) || c == '&' || c == '"' || c == '\'' || c == ',' || c == ';');
}

// An identifier holds a secret when it ends with one of these words as a whole
// part of it: after '_' or '-', at a lower-to-upper change ("apiKey"), or as
// all of it. So "client_secret", "x_api_key", "X-Api-Key", "apiKey",
// "my_token" and "key" do, and "Hotkey", "monkey" and "tokens" don't.
bool EndsWithSecretWord(string_view id)
{
	static constexpr std::array<string_view, 18> kWords = {"token",      "secret",       "password",    "passwd",
							       "passphrase", "pwd",          "apikey",      "streamkey",
							       "key",        "auth",         "oauth",       "sig",
							       "signature",  "credential",   "credentials", "cookie",
							       "bearer",     "authorization"};
	for (string_view word : kWords) {
		if (id.size() < word.size())
			continue;
		const size_t b = id.size() - word.size();
		if (!StartsWithAtI(id, b, word))
			continue;
		if (b == 0 || id[b - 1] == '_' || id[b - 1] == '-' ||
		    (IsUpper(id[b]) && (IsLower(id[b - 1]) || IsDigit(id[b - 1]))))
			return true;
	}
	return false;
}

// The quote right before position i: "\"" (JSON inside a string), '"' or '\''.
string_view QuoteBefore(string_view s, size_t i)
{
	if (i >= 2 && s[i - 1] == '"' && s[i - 2] == '\\')
		return s.substr(i - 2, 2);
	if (i >= 1 && (s[i - 1] == '"' || s[i - 1] == '\''))
		return s.substr(i - 1, 1);
	return {};
}

// The quote that opens a value at v, as above.
string_view QuoteAt(string_view s, size_t v)
{
	if (StartsWithAt(s, v, "\\\""))
		return s.substr(v, 2);
	if (v < s.size() && (s[v] == '"' || s[v] == '\''))
		return s.substr(v, 1);
	return {};
}

// The closing `quote` of a value that starts at p, skipping backslash escapes
// ("a\"b"); s.size() when it never closes.
size_t ClosingQuote(string_view s, size_t p, string_view quote)
{
	if (quote.size() == 2)
		return std::min(s.find(quote, p), s.size());
	for (size_t j = p; j < s.size(); ++j) {
		if (s[j] == '\\') {
			++j;
			continue;
		}
		if (s[j] == quote[0])
			return j;
	}
	return s.size();
}

// "Authorization: Bearer x", "Proxy-Authorization: Digest username=...",
// "Cookie: a=b; c=d", "Set-Cookie: ...": the value goes up to the end of the
// line (quotes included: a Digest header has them). The scheme word of an
// authorization stays.
size_t MatchCredentialHeader(string_view s, size_t i, string_view id, string &replacement)
{
	const bool authorization = EqualsI(id, "authorization") || EqualsI(id, "proxy-authorization");
	if (!authorization && !EqualsI(id, "cookie") && !EqualsI(id, "set-cookie"))
		return 0;
	size_t p = SkipBlanks(s, i + id.size());
	if (p >= s.size() || (s[p] != ':' && s[p] != '='))
		return 0;
	p = SkipBlanks(s, p + 1);
	const size_t end = s.size();
	if (end == p)
		return 0;
	replacement.assign(id);
	replacement += ": ";
	if (authorization) {
		size_t w = p;
		while (w < end && IsAsciiLetter(s[w]))
			++w;
		if (w > p && w < end && IsSpace(s[w]) && SkipBlanks(s, w) < end) {
			replacement.append(s.substr(p, w - p));
			replacement += ' ';
		}
	}
	replacement += "<redacted>";
	return end;
}

size_t MatchKeyedSecret(string_view s, size_t i, string &replacement)
{
	if (!IsAsciiLetter(s[i]) || (i > 0 && IsIdentChar(s[i - 1])))
		return 0;
	size_t idEnd = i;
	while (idEnd < s.size() && IsIdentChar(s[idEnd]))
		++idEnd;
	const string_view id = s.substr(i, idEnd - i);
	const string_view openQuote = QuoteBefore(s, i);

	if (openQuote.empty()) {
		if (const size_t end = MatchCredentialHeader(s, i, id, replacement))
			return end;
	}
	if (EqualsI(id, "bearer") && idEnd < s.size() && IsSpace(s[idEnd])) {
		// "Bearer <token>" on its own.
		size_t p = idEnd;
		while (p < s.size() && IsSpace(s[p]))
			++p;
		size_t end = p;
		while (end < s.size() && IsSecretValueChar(s[end]))
			++end;
		if (end == p)
			return 0;
		replacement.assign(id);
		replacement += " <redacted>";
		return end;
	}
	if (!EndsWithSecretWord(id))
		return 0;

	// key = value, key: value, "key": "value", 'key'='value', \"key\":\"value\".
	size_t p = idEnd;
	if (!openQuote.empty() && StartsWithAt(s, p, openQuote))
		p += openQuote.size();
	p = SkipBlanks(s, p);
	if (p >= s.size() || (s[p] != '=' && s[p] != ':'))
		return 0;
	const size_t v = SkipBlanks(s, p + 1);
	if (v >= s.size())
		return 0;
	if (const string_view quote = QuoteAt(s, v); !quote.empty()) {
		const size_t close = ClosingQuote(s, v + quote.size(), quote);
		if (close == v + quote.size())
			return 0; // An empty value: nothing to hide.
		replacement.assign(s.substr(i, v - i));
		replacement.append(quote);
		replacement += "<redacted>";
		if (close >= s.size())
			return s.size();
		replacement.append(quote);
		return close + quote.size();
	}
	// In JSON ("token": null) the value also ends at the object or array.
	size_t end = v;
	while (end < s.size() && IsSecretValueChar(s[end]) && (openQuote.empty() || (s[end] != '}' && s[end] != ']')))
		++end;
	if (end == v)
		return 0;
	if (openQuote.empty()) {
		replacement.assign(id);
		replacement += "=<redacted>";
	} else {
		replacement.assign(s.substr(i, v - i)); // "token": null
		replacement += "<redacted>";
	}
	return end;
}

// --- 8. JSON Web Tokens --------------------------------------------------------

// "eyJ..." (base64url of '{"'), a '.', a second part, then any further
// ".part" (the signature, which may be empty).
size_t MatchJwt(string_view s, size_t i, string &replacement)
{
	if (s[i] != 'e' || !StartsWithAt(s, i, "eyJ") || (i > 0 && IsSlugChar(s[i - 1])))
		return 0;
	size_t e = i;
	while (e < s.size() && IsSlugChar(s[e]))
		++e;
	if (e - i < 6 || e >= s.size() || s[e] != '.')
		return 0;
	size_t end = e + 1;
	while (end < s.size() && IsSlugChar(s[end]))
		++end;
	if (end == e + 1)
		return 0;
	while (end < s.size() && s[end] == '.') {
		++end;
		while (end < s.size() && IsSlugChar(s[end]))
			++end;
	}
	replacement = "<token>";
	return end;
}

// --- 9. Device names and window titles (best effort) ---------------------------

// The closing quote of a name opened at p - 1: the first `quote` not followed
// by a letter, so "Carlos's AirPods" stays whole; the end of the line if none.
size_t ClosingNameQuote(string_view s, size_t p, char quote)
{
	for (size_t j = p; j < s.size(); ++j) {
		if (s[j] == quote && (j + 1 >= s.size() || !IsNameLetter(s[j + 1])))
			return j;
	}
	return s.size();
}

// "Device 'Headphones (Ana's AirPods)'", "video device: Cam", "device name: X",
// "capturing 'Chat - App'", "window 'X'", "title: X". Names in other shapes stay,
// as do scene and source names.
size_t MatchDeviceOrWindow(string_view s, size_t i, string &replacement)
{
	if (!IsAsciiLetter(s[i]) || (i > 0 && IsIdentChar(s[i - 1])))
		return 0;
	size_t idEnd = i;
	while (idEnd < s.size() && IsIdentChar(s[idEnd]))
		++idEnd;
	const string_view id = s.substr(i, idEnd - i);
	const char *label = nullptr;
	bool unquotedAfterColon = false;
	size_t p = idEnd;
	if (EqualsI(id, "device")) {
		label = "<device>";
		unquotedAfterColon = true;
		if (StartsWithAtI(s, p, " name") && (p + 5 >= s.size() || !IsIdentChar(s[p + 5])))
			p += 5;
	} else if (EqualsI(id, "title")) {
		label = "<window>";
		unquotedAfterColon = true;
	} else if (EqualsI(id, "window") || EqualsI(id, "capturing")) {
		label = "<window>";
	} else {
		return 0;
	}
	size_t q = SkipBlanks(s, p);
	bool colon = false;
	if (q < s.size() && (s[q] == ':' || s[q] == '=')) {
		colon = true;
		q = SkipBlanks(s, q + 1);
	}
	if (q >= s.size())
		return 0;
	if (s[q] == '\'' || s[q] == '"') {
		const size_t close = ClosingNameQuote(s, q + 1, s[q]);
		if (close == q + 1)
			return 0;
		replacement.assign(s.substr(i, q + 1 - i));
		replacement += label;
		if (close >= s.size())
			return s.size();
		replacement += s[q];
		return close + 1;
	}
	if (!colon || !unquotedAfterColon)
		return 0;
	replacement.assign(s.substr(i, q - i));
	replacement += label;
	return s.size();
}

// --- 10. IPv4 ----------------------------------------------------------------

// One octet starting at p: the whole run of digits (1 to 3 of them, leading
// zeros allowed: "010.001.002.003"), at most 255. Returns its end, or 0.
size_t Ipv4Octet(string_view s, size_t p)
{
	size_t e = p;
	int value = 0;
	while (e < s.size() && IsDigit(s[e])) {
		if (e - p >= 3)
			return 0;
		value = value * 10 + (s[e] - '0');
		++e;
	}
	if (e == p || value > 255)
		return 0;
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

// --- 11. IPv6 ----------------------------------------------------------------

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

// --- 12. Long tokens, stream keys and base64 ---------------------------------

// "abcd-efgh-ijkl-mnop-qrst": five groups of four letters or digits, the shape
// of a YouTube stream key (even without a digit in it).
bool IsGroupedKey(string_view run)
{
	if (run.size() != 24)
		return false;
	for (size_t k = 0; k < run.size(); ++k) {
		const bool dash = k % 5 == 4;
		if (dash ? run[k] != '-' : !(IsAsciiLetter(run[k]) || IsDigit(run[k])))
			return false;
	}
	return true;
}

// A run of 24 or more [A-Za-z0-9_-] with a digit and a letter, or a grouped key.
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
		if ((e - i >= 24 && digit && letter) || IsGroupedKey(s.substr(i, e - i))) {
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

bool IsBase64Char(char c)
{
	return IsAsciiLetter(c) || IsDigit(c) || c == '+' || c == '/';
}

int CharClass(char c)
{
	return IsDigit(c) ? 0 : (IsUpper(c) ? 1 : 2);
}

// Standard base64 ("Ab3d+Ef9/Gh12..."), which the rule above misses because of
// '+' and '/': a standalone run of [A-Za-z0-9+/] (plus '=' padding) with a '+'
// or '/', 20 or more letters and digits of all three kinds, that switches
// between digits, upper and lower case at least once every three characters.
// Paths don't: their parts are words ("data/MyApp2/Config"), and a '-', '_' or
// '.' next to the run marks it as part of a file name.
string RedactBase64(string_view s)
{
	string out;
	size_t last = 0;
	size_t i = 0;
	while (i < s.size()) {
		if (!IsBase64Char(s[i])) {
			++i;
			continue;
		}
		size_t e = i;
		size_t alnum = 0, switches = 0;
		bool symbol = false, digit = false, upper = false, lower = false;
		int prevClass = -1;
		while (e < s.size() && IsBase64Char(s[e])) {
			const char c = s[e];
			if (c == '+' || c == '/') {
				symbol = true;
				prevClass = -1;
			} else {
				const int cls = CharClass(c);
				digit = digit || cls == 0;
				upper = upper || cls == 1;
				lower = lower || cls == 2;
				if (prevClass >= 0 && cls != prevClass)
					++switches;
				prevClass = cls;
				++alnum;
			}
			++e;
		}
		size_t end = e;
		while (end < s.size() && end - e < 2 && s[end] == '=')
			++end;
		const auto partOfName = [](char c) {
			return IsSlugChar(c) || c == '.' || c == '\\' || c == ':' || c == '<' || c == '>';
		};
		const bool standalone = (i == 0 || !partOfName(s[i - 1])) && (end == s.size() || !partOfName(s[end]));
		if (standalone && symbol && alnum >= 20 && digit && upper && lower && switches * 3 >= alnum) {
			if (out.empty())
				out.reserve(s.size());
			out.append(s.substr(last, i - last));
			out += "<token>";
			last = end;
		}
		i = end > i ? end : i + 1;
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

bool ContainsAnyI(string_view s, std::initializer_list<string_view> lowerNeedles)
{
	for (string_view needle : lowerNeedles) {
		if (ContainsI(s, needle))
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
	return ContainsAnyI(s,
			    {"token", "secret", "auth", "pass", "pwd", "sig", "key", "bearer", "cookie", "credential"});
}

} // namespace

string RedactLine(string_view line)
{
	string s(line);
	if (s.find("://") != string::npos || s.find(":\\/\\/") != string::npos) {
		s = ReplaceAll(s, MatchStreamUrl);
		s = ReplaceAll(s, MatchWebUrl);
	}
	const bool backslash = s.find('\\') != string::npos;
	const bool separators = backslash || s.find('/') != string::npos;
	if (backslash || s.find(":/") != string::npos)
		s = ReplaceAll(s, MatchWindowsUserPath);
	if (backslash)
		s = ReplaceAll(s, MatchUncHost);
	if (ContainsI(s, "users/") || s.find("home/") != string::npos)
		s = ReplaceAll(s, MatchPosixHome);
	if (separators) {
		s = ReplaceAll(s, MatchOneDriveOrg);
		s = ReplaceAll(s, MatchProfileOwner);
	}
	if (s.find("overlay") != string::npos)
		s = ReplaceAll(s, MatchOverlaySlug);
	if (s.find('@') != string::npos || s.find("%40") != string::npos)
		s = RedactEmails(s);
	if (MayHaveKeyedSecret(s))
		s = ReplaceAll(s, MatchKeyedSecret);
	if (s.find("eyJ") != string::npos)
		s = ReplaceAll(s, MatchJwt);
	if (ContainsAnyI(s, {"device", "window", "capturing", "title"}))
		s = ReplaceAll(s, MatchDeviceOrWindow);
	if (HasIpv4Shape(s))
		s = ReplaceAll(s, MatchIpv4);
	if (HasIpv6Shape(s))
		s = ReplaceAll(s, MatchIpv6);
	if (s.size() >= 20) {
		s = RedactLongTokens(s);
		if (s.find('+') != string::npos || s.find('/') != string::npos)
			s = RedactBase64(s);
	}
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
