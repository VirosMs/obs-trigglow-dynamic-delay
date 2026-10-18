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
#include <string_view>

// Redaction of OBS's log before it leaves the machine as a support ticket
// attachment (the only place the log ever goes: it is never sent to Trigi).
// No Qt and no libobs: tests/log_redact_tests.cpp covers every rule.
//
// Line by line, these rules run in this order (patterns in ECMAScript regex
// notation; "icase" = case-insensitive):
//
//   1. \b(?:rtmps?|srt|rist|rtsp|whip)://[^\s"'<>]+ (icase)       -> <stream-url>
//   2. \bhttps?://(?:[^@/\s"'<>]+@)?([A-Za-z0-9.-]+)(?::\d+)?(?:[/?#][^\s"'<>]*)? (icase)
//                                                                    -> <url:HOST> (only the host stays)
//   3. ([A-Za-z]:[\\/]+(?:Users|Documents and Settings)[\\/]+)[^\\/\r\n"':*?<>|]+ (icase)
//                                                                    -> $1<user>
//      (one or more separators, so JSON-escaped paths with "\\" are covered too)
//   4. (/Users/|/home/)[^/\s"']+                                     -> $1<user>
//   5. /overlay/[A-Za-z0-9_-]+                                       -> /overlay/<slug>
//   6. [A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}                -> <email>
//   7. \b(token|secret|access_token|refresh_token|auth|password|passwd|sig|signature|apikey|api_key|stream_?key)
//      \s*[=:]\s*[^\s&"',;]+ (icase)                                  -> $1=<redacted>
//      and \b(bearer)\s+[^\s&"',;]+ (icase)                           -> $1 <redacted>
//   8. IPv4 (each part 0-255)                                         -> <ip>
//   9. IPv6: 5 to 8 groups of 1-4 hex digits, or a token with "::" between hex groups -> <ip>
//  10. A run of 24 or more [A-Za-z0-9_-] with at least one digit and one letter -> <token>
//
// The rules are hand-written linear scanners with exactly those semantics
// (leftmost match, then carry on after it) rather than std::regex: some
// std::regex implementations recurse once per character and can overflow the
// stack on a long log line, and running ten regexes over a 5 MB log is slow.
//
// What stays: scene and source names (they can't be told apart reliably), the
// timestamps, versions such as "OBS 31.0.2", and everything else in the log.
namespace trigglow::logredact {

// One line, without its line break.
std::string RedactLine(std::string_view line);

// A whole log: RedactLine() on every line, line breaks ("\n" or "\r\n") kept.
std::string RedactText(std::string_view text);

// Upload cap for the redacted log (a ticket attachment takes at most 5 MB).
inline constexpr size_t kUploadCapBytes = 4718592; // 4.5 MB
inline constexpr size_t kUploadHeadBytes = 262144; // 256 KB
// Put between the head and the tail of a capped log.
inline constexpr const char *kCutMarker = "\n[\xE2\x80\xA6 recortado \xE2\x80\xA6]\n";

// Text up to kUploadCapBytes is returned unchanged. Longer text becomes its
// first 256 KB (cut after the last full line) + kCutMarker + its end (from
// the start of a line), at most kUploadCapBytes in total and never split
// inside a UTF-8 sequence. `capped` (optional) says whether it was cut.
std::string CapForUpload(std::string text, bool *capped = nullptr);

// "See what is sent": the first and the last `lines` lines of the redacted
// text. When the text has at most 2 * lines lines, head holds all of it and
// tail is empty.
struct Preview {
	std::string head;
	std::string tail;
	size_t totalLines = 0;
	bool truncated = false; // Lines between head and tail are left out of the preview (not of the upload).
};
Preview MakePreview(std::string_view text, size_t lines = 200);

} // namespace trigglow::logredact
