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

#include "chat-text.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace trigglow::chat {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Decodes one UTF-8 sequence at s[i]; returns its length (>= 1). Overlong
// forms, surrogates and values past U+10FFFF decode as U+FFFD (one byte).
size_t DecodeUtf8(std::string_view s, size_t i, char32_t &cp)
{
	const auto b0 = static_cast<unsigned char>(s[i]);
	if (b0 < 0x80) {
		cp = b0;
		return 1;
	}
	size_t len = 0;
	char32_t value = 0;
	char32_t min = 0;
	if ((b0 & 0xE0) == 0xC0) {
		len = 2;
		value = b0 & 0x1F;
		min = 0x80;
	} else if ((b0 & 0xF0) == 0xE0) {
		len = 3;
		value = b0 & 0x0F;
		min = 0x800;
	} else if ((b0 & 0xF8) == 0xF0) {
		len = 4;
		value = b0 & 0x07;
		min = 0x10000;
	} else {
		cp = kReplacement;
		return 1;
	}
	if (s.size() - i < len) {
		cp = kReplacement;
		return 1;
	}
	for (size_t k = 1; k < len; ++k) {
		const auto b = static_cast<unsigned char>(s[i + k]);
		if ((b & 0xC0) != 0x80) {
			cp = kReplacement;
			return 1;
		}
		value = (value << 6) | (b & 0x3F);
	}
	if (value < min || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
		cp = kReplacement;
		return 1;
	}
	cp = value;
	return len;
}

void AppendUtf8(std::string &out, char32_t cp)
{
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

struct Range {
	char32_t first;
	char32_t last;
};

// Extended_Pictographic (Unicode emoji-data), plus the other code points that
// only ever appear inside emoji sequences: skin tone modifiers, regional
// indicators (flags), the keycap mark and tag characters. Sorted.
constexpr Range kPictographic[] = {
	{0x00A9, 0x00A9},   {0x00AE, 0x00AE},   {0x203C, 0x203C},   {0x2049, 0x2049},   {0x20E3, 0x20E3},
	{0x2122, 0x2122},   {0x2139, 0x2139},   {0x2194, 0x2199},   {0x21A9, 0x21AA},   {0x231A, 0x231B},
	{0x2328, 0x2328},   {0x2388, 0x2388},   {0x23CF, 0x23CF},   {0x23E9, 0x23F3},   {0x23F8, 0x23FA},
	{0x24C2, 0x24C2},   {0x25AA, 0x25AB},   {0x25B6, 0x25B6},   {0x25C0, 0x25C0},   {0x25FB, 0x25FE},
	{0x2600, 0x2605},   {0x2607, 0x2612},   {0x2614, 0x2685},   {0x2690, 0x2705},   {0x2708, 0x2712},
	{0x2714, 0x2714},   {0x2716, 0x2716},   {0x271D, 0x271D},   {0x2721, 0x2721},   {0x2728, 0x2728},
	{0x2733, 0x2734},   {0x2744, 0x2744},   {0x2747, 0x2747},   {0x274C, 0x274C},   {0x274E, 0x274E},
	{0x2753, 0x2755},   {0x2757, 0x2757},   {0x2763, 0x2767},   {0x2795, 0x2797},   {0x27A1, 0x27A1},
	{0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2934, 0x2935},   {0x2B05, 0x2B07},   {0x2B1B, 0x2B1C},
	{0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x3030, 0x3030},   {0x303D, 0x303D},   {0x3297, 0x3297},
	{0x3299, 0x3299},   {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F}, {0x1F12F, 0x1F12F}, {0x1F16C, 0x1F171},
	{0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1AD, 0x1F1FF}, {0x1F201, 0x1F20F},
	{0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F}, {0x1F249, 0x1F53D},
	{0x1F546, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F}, {0x1F7D5, 0x1F7FF}, {0x1F80C, 0x1F80F},
	{0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F}, {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F93A},
	{0x1F93C, 0x1F945}, {0x1F947, 0x1FAFF}, {0x1FC00, 0x1FFFD}, {0xE0020, 0xE007F},
};

bool IsDroppedFormatChar(char32_t cp)
{
	return cp == 0x200B || cp == 0x200D || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
	       cp == 0x2060 || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFE0E || cp == 0xFE0F || cp == 0xFEFF;
}

bool IsControl(char32_t cp)
{
	return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
}

bool IsSpaceChar(char c)
{
	return c == ' ' || c == '\t';
}

std::string_view TrimSpaces(std::string_view s)
{
	while (!s.empty() && IsSpaceChar(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && IsSpaceChar(s.back()))
		s.remove_suffix(1);
	return s;
}

size_t RunLength(std::string_view s, size_t i, char c)
{
	size_t e = i;
	while (e < s.size() && s[e] == c)
		++e;
	return e - i;
}

void AddSegment(std::vector<Segment> &segments, std::string_view text, bool bold, bool code)
{
	if (text.empty())
		return;
	if (!segments.empty() && segments.back().bold == bold && segments.back().code == code) {
		segments.back().text.append(text);
		return;
	}
	segments.push_back({std::string(text), bold, code});
}

// Inline formatting of one line's content.
std::vector<Segment> ParseInline(std::string_view s)
{
	// 1. Code spans: a single '`' up to the next single '`'. Runs of 2+
	//    backticks (fences) are literal text.
	struct Span {
		size_t begin;
		size_t end; // Exclusive; for code spans, the delimiters are outside [begin, end).
		bool code;
	};
	std::vector<Span> spans;
	size_t textStart = 0;
	size_t i = 0;
	while (i < s.size()) {
		if (s[i] != '`') {
			++i;
			continue;
		}
		const size_t run = RunLength(s, i, '`');
		if (run != 1) {
			i += run;
			continue;
		}
		size_t j = i + 1;
		size_t close = std::string_view::npos;
		while (j < s.size()) {
			if (s[j] == '`') {
				const size_t r = RunLength(s, j, '`');
				if (r == 1) {
					close = j;
					break;
				}
				j += r;
			} else {
				++j;
			}
		}
		if (close == std::string_view::npos || close == i + 1) {
			i += 1; // Unpaired or empty: literal.
			continue;
		}
		spans.push_back({textStart, i, false});
		spans.push_back({i + 1, close, true});
		i = close + 1;
		textStart = i;
	}
	spans.push_back({textStart, s.size(), false});

	// 2. Bold markers: runs of exactly two '*' outside code spans, paired in
	//    order; a last unpaired one stays literal.
	std::vector<size_t> markers;
	for (const Span &span : spans) {
		if (span.code)
			continue;
		size_t k = span.begin;
		while (k < span.end) {
			if (s[k] != '*') {
				++k;
				continue;
			}
			size_t r = 0;
			while (k + r < span.end && s[k + r] == '*')
				++r;
			if (r == 2)
				markers.push_back(k);
			k += r;
		}
	}
	if (markers.size() % 2 == 1)
		markers.pop_back();

	std::vector<Segment> segments;
	bool bold = false;
	size_t m = 0;
	for (const Span &span : spans) {
		if (span.code) {
			AddSegment(segments, s.substr(span.begin, span.end - span.begin), bold, true);
			continue;
		}
		size_t pos = span.begin;
		while (m < markers.size() && markers[m] < span.end) {
			AddSegment(segments, s.substr(pos, markers[m] - pos), bold, false);
			bold = !bold;
			pos = markers[m] + 2;
			++m;
		}
		AddSegment(segments, s.substr(pos, span.end - pos), bold, false);
	}
	return segments;
}

} // namespace

bool IsPictographic(char32_t cp)
{
	if (cp < 0xA9)
		return false;
	auto it = std::upper_bound(std::begin(kPictographic), std::end(kPictographic), cp,
				   [](char32_t value, const Range &r) { return value < r.first; });
	if (it == std::begin(kPictographic))
		return false;
	--it;
	return cp >= it->first && cp <= it->last;
}

std::string Sanitize(std::string_view utf8)
{
	std::string out;
	out.reserve(std::min(utf8.size(), kMaxReplyChars * 4));
	size_t chars = 0;
	size_t i = 0;
	while (i < utf8.size() && chars < kMaxReplyChars) {
		char32_t cp = 0;
		const size_t len = DecodeUtf8(utf8, i, cp);
		i += len;
		if (cp == '\r') {
			// "\r\n" and a lone "\r" both end a line once.
			if (i < utf8.size() && utf8[i] == '\n')
				continue;
			cp = '\n';
		}
		if (cp == 0x2028 || cp == 0x2029)
			cp = '\n';
		if (cp == '\t')
			cp = ' ';
		if (cp != '\n' && (IsControl(cp) || IsDroppedFormatChar(cp) || IsPictographic(cp) ||
				   (cp >= 0x1F3FB && cp <= 0x1F3FF)))
			continue;
		AppendUtf8(out, cp);
		++chars;
	}
	return out;
}

std::vector<Line> Parse(std::string_view text)
{
	std::vector<Line> lines;
	size_t start = 0;
	while (start <= text.size()) {
		size_t nl = text.find('\n', start);
		if (nl == std::string_view::npos)
			nl = text.size();
		std::string_view raw = TrimSpaces(text.substr(start, nl - start));
		start = nl + 1;

		Line line;
		if (raw.empty()) {
			if (!lines.empty() && lines.back().kind != LineKind::Break)
				lines.push_back({LineKind::Break, 0, {}});
			if (nl >= text.size())
				break;
			continue;
		}
		if (raw.size() >= 2 && raw[0] == '-' && raw[1] == ' ') {
			line.kind = LineKind::Bullet;
			raw = TrimSpaces(raw.substr(2));
		} else if (raw.size() >= 4 && raw.substr(0, 3) == "\xE2\x80\xA2" && raw[3] == ' ') {
			line.kind = LineKind::Bullet; // U+2022 (bullet) and a space
			raw = TrimSpaces(raw.substr(4));
		} else {
			size_t d = 0;
			unsigned number = 0;
			while (d < raw.size() && d < 3 && raw[d] >= '0' && raw[d] <= '9') {
				number = number * 10 + static_cast<unsigned>(raw[d] - '0');
				++d;
			}
			if (d > 0 && number > 0 && d + 1 < raw.size() && raw[d] == '.' && raw[d + 1] == ' ') {
				line.kind = LineKind::Step;
				line.number = number;
				raw = TrimSpaces(raw.substr(d + 2));
			}
		}
		line.segments = ParseInline(raw);
		if (line.segments.empty() && line.kind == LineKind::Paragraph)
			continue;
		lines.push_back(std::move(line));
		if (nl >= text.size())
			break;
	}
	while (!lines.empty() && lines.back().kind == LineKind::Break)
		lines.pop_back();
	return lines;
}

std::vector<Line> ParseReply(std::string_view replyUtf8)
{
	return Parse(Sanitize(replyUtf8));
}

std::string EscapeHtml(std::string_view text)
{
	std::string out;
	out.reserve(text.size() + text.size() / 8);
	for (const char c : text) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '"':
			out += "&quot;";
			break;
		case '\'':
			out += "&#39;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

std::string LineHtml(const Line &line)
{
	std::string html;
	bool inBold = false;
	for (const Segment &segment : line.segments) {
		if (segment.bold != inBold) {
			html += segment.bold ? "<b>" : "</b>";
			inBold = segment.bold;
		}
		if (segment.code)
			html += "<span class=\"code\">" + EscapeHtml(segment.text) + "</span>";
		else
			html += EscapeHtml(segment.text);
	}
	if (inBold)
		html += "</b>";
	return html;
}

std::string LinePrefix(const Line &line)
{
	if (line.kind == LineKind::Bullet)
		return "- ";
	if (line.kind == LineKind::Step)
		return std::to_string(line.number) + ". ";
	return std::string();
}

std::string PlainText(const std::vector<Line> &lines)
{
	std::string out;
	for (const Line &line : lines) {
		if (!out.empty())
			out += '\n';
		if (line.kind == LineKind::Break)
			continue;
		out += LinePrefix(line);
		for (const Segment &segment : line.segments)
			out += segment.text;
	}
	return out;
}

size_t CountChars(std::string_view utf8)
{
	size_t count = 0;
	size_t i = 0;
	while (i < utf8.size()) {
		char32_t cp = 0;
		i += DecodeUtf8(utf8, i, cp);
		++count;
	}
	return count;
}

} // namespace trigglow::chat
