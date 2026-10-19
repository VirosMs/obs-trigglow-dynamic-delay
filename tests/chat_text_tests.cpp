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

// Tests for src/chat-text.*: hostile replies must never turn into active
// HTML, a link or an image, and the allowed formatting must survive.

#include "chat-text.hpp"
#include "test-support.hpp"

#include <string>
#include <vector>

using namespace trigglow::chat;
using testsupport::Url;

namespace {

// With the four allowed tags taken out, no '<' or '>' is left: every other
// one was escaped.
bool OnlyAllowedTags(std::string html)
{
	static const char *kAllowed[] = {"<b>", "</b>", "<span class=\"code\">", "</span>"};
	for (const char *tag : kAllowed) {
		const std::string t(tag);
		size_t pos = 0;
		while ((pos = html.find(t, pos)) != std::string::npos)
			html.erase(pos, t.size());
	}
	return html.find('<') == std::string::npos && html.find('>') == std::string::npos;
}

std::string AllHtml(const std::vector<Line> &lines)
{
	std::string html;
	for (const Line &line : lines)
		html += LineHtml(line) + "\n";
	return html;
}

bool IsSafe(const std::string &reply)
{
	const std::string html = AllHtml(ParseReply(reply));
	return OnlyAllowedTags(html) && html.find("<a") == std::string::npos &&
	       html.find("<img") == std::string::npos && html.find("<script") == std::string::npos;
}

void TestHostileInput()
{
	testcheck::g_currentTest = "hostile input";
	CHECK(IsSafe("<script>alert(1)</script>"));
	CHECK(IsSafe("<img src=x onerror=alert(1)>"));
	CHECK(IsSafe("<img src=file:///C:/x>"));
	CHECK(IsSafe("[clic](" + Url("http", "evil") + ")"));
	CHECK(IsSafe("**<b>x</b>**"));
	CHECK(IsSafe("```code```"));
	CHECK(IsSafe("`<a href=x>`"));
	CHECK(IsSafe("<style>*{}</style><iframe src=x></iframe>"));
	CHECK(IsSafe("&lt;b&gt; already escaped"));

	// The text stays visible, as text.
	const auto lines = ParseReply("<script>alert(1)</script>");
	CHECK(lines.size() == 1);
	CHECK_STR(LineHtml(lines[0]), "&lt;script&gt;alert(1)&lt;/script&gt;");
	CHECK_STR(PlainText(lines), "<script>alert(1)</script>");
	const auto bold = ParseReply("**<b>x</b>**");
	CHECK_STR(LineHtml(bold[0]), "<b>&lt;b&gt;x&lt;/b&gt;</b>");
	CHECK_STR(LineHtml(ParseReply("a & \"b\" 'c'")[0]), "a &amp; &quot;b&quot; &#39;c&#39;");

	// Links stay plain text: no anchor, nothing clickable.
	const std::string link = "[clic](" + Url("http", "evil") + ")";
	CHECK_STR(PlainText(ParseReply(link)), link);
	const std::string bare = "see " + Url("https", "www.trigglow.com/app/support");
	CHECK_STR(LineHtml(ParseReply(bare)[0]), bare);
}

void TestSanitize()
{
	testcheck::g_currentTest = "sanitize";
	// Bidi controls, zero-width characters and other controls go.
	CHECK_STR(Sanitize("abc\xE2\x80\xAE"
			   "def"),
		  "abcdef"); // U+202E
	CHECK_STR(Sanitize("a\xE2\x81\xA6"
			   "b\xE2\x81\xA9"
			   "c"),
		  "abc"); // U+2066, U+2069
	CHECK_STR(Sanitize("a\xE2\x80\x8E"
			   "b\xE2\x80\x8F"
			   "c"),
		  "abc"); // U+200E, U+200F
	CHECK_STR(Sanitize("a\xE2\x80\x8B"
			   "b\xEF\xBB\xBF"
			   "c"),
		  "abc"); // ZWSP, BOM
	CHECK_STR(Sanitize(std::string("a\0b\x07"
				       "c\x1B[31md\x7F",
				       12)),
		  "abc[31md");
	CHECK_STR(Sanitize("a\xC2\x85"
			   "b"),
		  "ab"); // C1 control (NEL).
	CHECK_STR(Sanitize("a\r\nb\rc\td"), "a\nb\nc d");
	CHECK_STR(Sanitize("a\xE2\x80\xA8"
			   "b"),
		  "a\nb"); // U+2028

	// Emojis and pictographs, including ZWJ sequences, skin tones, flags and keycaps.
	CHECK_STR(Sanitize("ok \xF0\x9F\x98\x80!"), "ok !"); // U+1F600
	CHECK_STR(Sanitize("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
			   "x"),
		  "x"); // Family (ZWJ)
	CHECK_STR(Sanitize("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"
			   "y"),
		  "y"); // Thumbs up + skin tone
	CHECK_STR(Sanitize("\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8"
			   "z"),
		  "z");                                                              // Flag ES
	CHECK_STR(Sanitize("1\xEF\xB8\x8F\xE2\x83\xA3"), "1");                       // Keycap 1
	CHECK_STR(Sanitize("\xE2\x9C\x85 done \xE2\x9D\xA4\xEF\xB8\x8F"), " done "); // U+2705, U+2764 U+FE0F
	CHECK_STR(Sanitize("\xE2\x86\x92 next"), "\xE2\x86\x92 next"); // U+2192: a plain arrow, not an emoji.
	// Ordinary letters, accents and punctuation stay.
	CHECK_STR(Sanitize("Configuraci\xC3\xB3n: \xC2\xBF"
			   "Activar?"),
		  "Configuraci\xC3\xB3n: \xC2\xBF"
		  "Activar?");
	CHECK_STR(Sanitize("\xE4\xB8\xAD\xE6\x96\x87"), "\xE4\xB8\xAD\xE6\x96\x87");

	// Invalid UTF-8 becomes U+FFFD, never raw bytes.
	CHECK_STR(Sanitize("a\xFF"
			   "b"),
		  "a\xEF\xBF\xBD"
		  "b");
	CHECK_STR(Sanitize("a\xC0\xAF"
			   "b"),
		  "a\xEF\xBF\xBD\xEF\xBF\xBD"
		  "b"); // Overlong '/'.
	CHECK_STR(Sanitize("a\xED\xA0\x80"
			   "b"),
		  "a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
		  "b");                                              // Surrogate.
	CHECK_STR(Sanitize("\xE2\x82"), "\xEF\xBF\xBD\xEF\xBF\xBD"); // Truncated.

	// 4000 characters at most.
	const std::string longReply(10000, 'x');
	CHECK(CountChars(Sanitize(longReply)) == kMaxReplyChars);
	std::string accents;
	for (int i = 0; i < 5000; ++i)
		accents += "\xC3\xA9";
	const std::string cut = Sanitize(accents);
	CHECK(CountChars(cut) == kMaxReplyChars);
	CHECK(cut.size() == 2 * kMaxReplyChars);
	CHECK(IsSafe(std::string(10000, '<')));
}

void TestPictographicTable()
{
	testcheck::g_currentTest = "pictographic table";
	CHECK(IsPictographic(0x1F600));
	CHECK(IsPictographic(0x2705));
	CHECK(IsPictographic(0x00A9));
	CHECK(IsPictographic(0x1F1EA));
	CHECK(IsPictographic(0x1FAFF));
	CHECK(!IsPictographic('A'));
	CHECK(!IsPictographic(0x00E9));
	CHECK(!IsPictographic(0x4E2D));
	CHECK(!IsPictographic(0x2190)); // Left arrow: not in the set (U+2194-U+2199 are).
}

void TestStructure()
{
	testcheck::g_currentTest = "structure";
	const auto lines =
		ParseReply("Hola.\n\n\n- Abre **Ajustes**\n- Pulsa `Enable`\n1. Primero\n12. Luego\n\n# Titulo\n");
	CHECK(lines.size() == 8);
	CHECK(lines[0].kind == LineKind::Paragraph);
	CHECK(lines[1].kind == LineKind::Break); // Three blank lines collapse into one.
	CHECK(lines[2].kind == LineKind::Bullet);
	CHECK_STR(LineHtml(lines[2]), "Abre <b>Ajustes</b>");
	CHECK(lines[3].kind == LineKind::Bullet);
	CHECK_STR(LineHtml(lines[3]), "Pulsa <span class=\"code\">Enable</span>");
	CHECK(lines[4].kind == LineKind::Step && lines[4].number == 1);
	CHECK(lines[5].kind == LineKind::Step && lines[5].number == 12);
	CHECK_STR(LinePrefix(lines[5]), "12. ");
	CHECK(lines[6].kind == LineKind::Break);
	CHECK(lines[7].kind == LineKind::Paragraph);
	CHECK_STR(LineHtml(lines[7]), "# Titulo"); // A heading is just text.
	CHECK_STR(PlainText(lines), "Hola.\n\n- Abre Ajustes\n- Pulsa Enable\n1. Primero\n12. Luego\n\n# Titulo");

	// Leading/trailing blank lines dropped; empty reply -> no lines.
	CHECK(ParseReply("\n\n").empty());
	CHECK(ParseReply("").empty());
	CHECK(ParseReply("\xF0\x9F\x98\x80").empty());
	// Not steps or bullets.
	CHECK(ParseReply("0. zero")[0].kind == LineKind::Paragraph);
	CHECK(ParseReply("1234. big")[0].kind == LineKind::Paragraph);
	CHECK(ParseReply("-no space")[0].kind == LineKind::Paragraph);
	CHECK(ParseReply("1.5 GB")[0].kind == LineKind::Paragraph);
	CHECK(ParseReply("\xE2\x80\xA2 dot")[0].kind == LineKind::Bullet);
}

void TestInline()
{
	testcheck::g_currentTest = "inline";
	auto html = [](const std::string &s) {
		return LineHtml(ParseReply(s).at(0));
	};
	CHECK_STR(html("a **b** c **d"), "a <b>b</b> c **d"); // Unpaired marker stays literal.
	CHECK_STR(html("***x***"), "***x***");                // Runs of 3 are literal.
	CHECK_STR(html("****"), "****");
	CHECK_STR(html("`a**b**`"), "<span class=\"code\">a**b**</span>"); // No bold inside code.
	CHECK_STR(html("**see `x` now**"), "<b>see <span class=\"code\">x</span> now</b>");
	CHECK_STR(html("`unclosed"), "`unclosed");
	CHECK_STR(html("``double``"), "``double``");
	CHECK_STR(html("```code```"), "```code```"); // Fences are text.
	CHECK_STR(html("a ` b ` c"), "a <span class=\"code\"> b </span> c");
	CHECK_STR(html("x * y * z"), "x * y * z");
}

} // namespace

int main()
{
	TestHostileInput();
	TestSanitize();
	TestPictographicTable();
	TestStructure();
	TestInline();
	return testcheck::Finish("chat_text_tests");
}
