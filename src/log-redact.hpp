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
// Line by line, these rules run in this order ("icase" = any case; "\/" is
// the JSON-escaped '/', accepted wherever a URL's '/' is):
//
//   1. Streaming URLs: a scheme starting with rtmp or rtsp (rtmps, rtmpt,
//      rtmpe, rtsps...), or srt, rist, whip, whep, then "://" and everything
//      up to a space, quote or angle bracket                        -> <stream-url>
//   2. Any other URL ("scheme://" with a scheme of 2+ characters: https, wss,
//      ftp, udp, obsws...; not file://): the host stays, the credentials (up
//      to the LAST '@' of the authority), port, path, query and fragment go
//                                                                    -> <url:HOST>
//   3. Windows profiles: "<drive>:\Users\<name>", "<drive>:\Documents and Settings\<name>"
//      (icase, '\' or '/', one or more of them: JSON-escaped paths too), and
//      "\Users\<name>" after any other backslash ("\\host\c$\Users\x")
//                                                                    -> ...\<user>
//      The name ends at a separator or a character a name can't hold; an
//      apostrophe between two letters belongs to it ("O'Brien").
//      "\\host\" where a path starts                                 -> \\<host>\.
//   4. "/Users/<name>" (icase) and "/home/<name>" (exact case)      -> /Users/<user>
//      A profile moved elsewhere: the folder right before one of a profile's
//      own folders (AppData, Desktop, Documents, Downloads, Music, Pictures,
//      Videos, Movies, OneDrive...)                                  -> <user>
//      "OneDrive - <company>"                                        -> OneDrive - <org>
//   5. /overlay/[A-Za-z0-9_-]+                                       -> /overlay/<slug>
//   6. Emails: local part of letters (any script), digits, ._%+- and an
//      apostrophe between letters; '@' or "%40"; a domain whose last label
//      has 2+ letters (any script)                                   -> <email>
//   7. A value after an identifier that ends with token, secret, password,
//      passwd, passphrase, pwd, apikey, streamkey, key, auth, oauth, sig,
//      signature, credential(s), cookie, bearer or authorization, as a whole
//      part of it (after '_' or '-', at a lower-to-upper change, or all of it:
//      "client_secret", "x_api_key", "apiKey", but not "Hotkey" or "tokens"),
//      then '=' or ':'. Unquoted                                     -> name=<redacted>
//      Quoted ("...", '...' or \"...\", JSON keys included)         -> "name": "<redacted>"
//      "Authorization:", "Proxy-Authorization:", "Cookie:", "Set-Cookie:": the
//      rest of the line (the auth scheme word stays)                 -> Authorization: Bearer <redacted>
//      "Bearer <value>"                                              -> Bearer <redacted>
//   8. JSON Web Tokens: "eyJ..." "." part ("." part...)             -> <token>
//   9. Best effort: a quoted name after "device", "device name", "capturing"
//      or "window", and the rest of the line after "device:", "device name:"
//      or "title:"                                                   -> <device> / <window>
//  10. IPv4 (each part 0-255, leading zeros allowed)                 -> <ip>
//  11. IPv6: 5 to 8 groups of 1-4 hex digits, or a token with "::" between hex groups -> <ip>
//  12. A run of 24 or more [A-Za-z0-9_-] with at least one digit and one
//      letter, or five groups of four letters/digits joined by '-' (a stream
//      key's shape); standard base64 with '+' or '/' (20+ letters and digits
//      of all three kinds, switching kind at least every three characters, and
//      not part of a file name)                                      -> <token>
//
// The rules are hand-written linear scanners (leftmost match, then carry on
// after it) rather than std::regex: some std::regex implementations recurse
// once per character and can overflow the stack on a long log line, and
// running a dozen regexes over a 5 MB log is slow.
//
// What stays: scene and source names (they can't be told apart reliably),
// audio device names and window titles in any other shape than in rule 9, the
// timestamps, versions such as "OBS 31.0.2", and everything else in the log.
// That is why "Report a problem" asks the user to look at it first.
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
