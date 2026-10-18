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
#include <vector>

// Safe rendering of Trigi's replies. The server sends plain text with a small
// markdown subset (**bold**, `code`, "- " bullets and "1. " steps, one per
// line) and promises no links, HTML or emojis; the plugin doesn't rely on that
// promise. No Qt here, so tests/chat_text_tests.cpp checks it with hostile
// input.
//
// How the dialog must show a reply:
//   - ParseReply() the raw text, then one label per Line.
//   - Rich text ONLY from LineHtml(): every segment HTML-escaped, and the only
//     tags are <b> and <span class="code">. Never pass the reply itself (or
//     anything built from it some other way) to a widget that interprets
//     markup: no Qt::AutoText, Qt::MarkdownText, setMarkdown() (not even with
//     MarkdownNoHTML), setHtml(reply) or QTextBrowser::setSource(). Labels keep
//     setOpenExternalLinks(false) and nothing is connected to linkActivated. A
//     QTextBrowser, if used, needs setOpenLinks(false), setOpenExternalLinks(false)
//     and a loadResource() that returns nothing.
//   - URLs in a reply stay plain, non-clickable text. Useful links are the
//     plugin's own fixed buttons (api-client.hpp, namespace web).
//   - Long unbroken runs (a 4000-character "word") are possible: the label
//     needs a wrap mode that can break anywhere.
namespace trigglow::chat {

// Replies longer than this many characters (Unicode code points) are cut.
inline constexpr size_t kMaxReplyChars = 4000;

// Steps 1 and 2 of the safe render: removes control characters other than
// "\n" (a "\r\n" becomes "\n", U+2028/U+2029 become "\n" and a tab becomes a
// space), bidi controls
// (U+200E, U+200F, U+202A-U+202E, U+2066-U+2069), invisible characters
// (U+200B, U+2060, U+FEFF), pictographs and emojis (Extended_Pictographic,
// skin tones, regional indicators, keycaps, tags, U+FE0E/U+FE0F and U+200D),
// replaces invalid UTF-8 with U+FFFD, and cuts the result to kMaxReplyChars.
std::string Sanitize(std::string_view utf8);

// True for the code points Sanitize() drops as pictographs or emoji parts.
bool IsPictographic(char32_t cp);

enum class LineKind : unsigned char {
	Paragraph, // Plain line.
	Bullet,    // "- text" (or U+2022 "bullet" + space): show a bullet, then the segments.
	Step,      // "N. text": show "N.", then the segments.
	Break,     // An empty line between blocks (consecutive ones are merged).
};

// A run of text with its formatting. Text is plain UTF-8, never markup.
struct Segment {
	std::string text;
	bool bold = false;
	bool code = false;
};

struct Line {
	LineKind kind = LineKind::Paragraph;
	unsigned number = 0; // Step number (1-999), 0 for other kinds.
	std::vector<Segment> segments;
};

// Splits sanitized text into lines and segments. Only a single backtick opens
// and closes `code`, and only exactly two asterisks open and close **bold**,
// each with its closing pair on the same line; anything unpaired, "```"
// fences and "#" headings stay as literal text. Leading and trailing empty
// lines are dropped.
std::vector<Line> Parse(std::string_view sanitizedUtf8);

// Sanitize() + Parse(): what the dialog calls with the server's reply.
std::vector<Line> ParseReply(std::string_view replyUtf8);

// & < > " ' as entities. Same job as QString::toHtmlEscaped(), plus "'".
std::string EscapeHtml(std::string_view text);

// A line's segments as rich text for a QLabel (Qt::RichText): escaped text in
// <b>...</b> and <span class="code">...</span> only. Bullets and step numbers
// are NOT included (the dialog lays them out); see LinePrefix().
std::string LineHtml(const Line &line);

// "- " for a bullet, "N. " for a step, "" otherwise (plain text).
std::string LinePrefix(const Line &line);

// The reply as plain text (prefixes included, no formatting), for copying or
// an accessible description.
std::string PlainText(const std::vector<Line> &lines);

// Number of Unicode code points in UTF-8 text (invalid bytes count as one).
size_t CountChars(std::string_view utf8);

} // namespace trigglow::chat
