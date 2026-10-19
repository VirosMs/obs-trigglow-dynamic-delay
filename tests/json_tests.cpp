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

// Tests for src/json.*: the reader the plugin uses for every API response.

#include "json.hpp"
#include "test-support.hpp"

#include <clocale>
#include <cmath>
#include <string>

using trigglow::json::Parse;
using trigglow::json::Serialize;
using trigglow::json::Value;

namespace {

void TestScalars()
{
	testcheck::g_currentTest = "scalars";
	CHECK(Parse("null")->IsNull());
	CHECK(Parse("true")->AsBool());
	CHECK(!Parse("false")->AsBool());
	CHECK(Parse(" 42 ")->AsNumber() == 42.0);
	CHECK(Parse("-0.5")->AsNumber() == -0.5);
	CHECK(Parse("29.97")->AsNumber() == 29.97);
	CHECK(Parse("1e3")->AsNumber() == 1000.0);
	CHECK(Parse("2.5E-2")->AsNumber() == 0.025);
	CHECK(Parse("0")->AsNumber() == 0.0);
	CHECK_STR(Parse("\"hi\"")->AsString(), "hi");
	// Leading zeros, bare signs, NaN, trailing garbage and lone values are invalid.
	CHECK(!Parse("01"));
	CHECK(!Parse("+1"));
	CHECK(!Parse("-"));
	CHECK(!Parse("1."));
	CHECK(!Parse(".5"));
	CHECK(!Parse("NaN"));
	CHECK(!Parse("1e999"));
	CHECK(!Parse("true false"));
	CHECK(!Parse(""));
	CHECK(!Parse("   "));
	CHECK(!Parse("nul"));
}

void TestStrings()
{
	testcheck::g_currentTest = "strings";
	// JSON texts are built from parts so that no macro argument holds a raw
	// string or a universal character name.
	const std::string bs(1, '\\');
	const std::string q(1, '"');
	auto uEsc = [&bs](const char *hex) {
		return bs + "u" + hex;
	};
	const std::string escapes = q + "a" + bs + q + "b" + bs + bs + "c" + bs + "/d" + bs + "n" + bs + "t" + q;
	const std::string eAcute = q + uEsc("00e9") + q;
	const std::string pair = q + uEsc("d83d") + uEsc("de00") + q;
	const std::string loneHigh = q + uEsc("d83d") + q;
	const std::string loneLow = q + uEsc("de00") + "x" + q;
	const std::string badEscape = q + bs + "x" + q;
	const std::string shortEscape = q + uEsc("12") + q;
	CHECK_STR(Parse(escapes)->AsString(), "a\"b\\c/d\n\t");
	CHECK_STR(Parse(eAcute)->AsString(), "\xC3\xA9");
	CHECK_STR(Parse(pair)->AsString(), "\xF0\x9F\x98\x80"); // Surrogate pair.
	CHECK_STR(Parse(loneHigh)->AsString(), "\xEF\xBF\xBD"); // Lone high surrogate.
	CHECK_STR(Parse(loneLow)->AsString(), "\xEF\xBF\xBD"
					      "x"); // Lone low surrogate.
	CHECK(!Parse("\"a\nb\""));                  // Raw control character.
	CHECK(!Parse(badEscape));
	CHECK(!Parse(shortEscape));
	CHECK(!Parse("\"unterminated"));
	// Raw UTF-8 passes through.
	CHECK_STR(Parse("\"P\xC3\xA9rez\"")->AsString(), "P\xC3\xA9rez");
}

void TestContainers()
{
	testcheck::g_currentTest = "containers";
	const auto v = Parse(R"({"a":1,"b":[true,null,"x"],"c":{"d":"e"},"a":2})");
	CHECK(v.has_value());
	CHECK(v->IsObject());
	CHECK(v->GetNumber("a").value_or(0) == 2.0); // The last duplicate wins.
	const Value *b = v->Find("b");
	CHECK(b && b->IsArray() && b->Size() == 3);
	CHECK(b && b->At(0).AsBool());
	CHECK(b && b->At(1).IsNull());
	CHECK(b && b->At(7).IsNull()); // Past the end.
	CHECK_STR(v->Find("c")->GetString("d").value_or(""), "e");
	CHECK(!v->GetString("a").has_value()); // Wrong type.
	CHECK(!v->GetBool("missing").has_value());
	CHECK(Parse("{}")->IsObject());
	CHECK(Parse("[]")->IsArray());
	CHECK(!Parse("{\"a\":1,}"));
	CHECK(!Parse("[1,]"));
	CHECK(!Parse("{\"a\" 1}"));
	CHECK(!Parse("{a:1}"));
	CHECK(!Parse("[1 2]"));
	// A BOM is tolerated.
	CHECK(Parse("\xEF\xBB\xBF{\"a\":1}").has_value());
}

void TestDepth()
{
	testcheck::g_currentTest = "depth";
	std::string deep;
	for (int i = 0; i < 32; ++i)
		deep += '[';
	for (int i = 0; i < 32; ++i)
		deep += ']';
	CHECK(Parse(deep).has_value());
	CHECK(!Parse("[" + deep + "]").has_value()); // 33 levels.
	// A hostile body can't blow the stack.
	std::string hostile(100000, '[');
	CHECK(!Parse(hostile).has_value());
}

void TestSerialize()
{
	testcheck::g_currentTest = "serialize";
	Value v = Value::Object();
	v.Set("s", Value::String("q\"\\\n\x01"));
	v.Set("n", Value::Number(1.5));
	v.Set("i", Value::Number(600));
	v.Set("neg", Value::Number(-0.0));
	v.Set("b", Value::Bool(true));
	v.Set("z", Value::Null());
	Value arr = Value::Array();
	arr.Push(Value::Number(29.97));
	arr.Push(Value::Number(1e6));
	v.Set("a", std::move(arr));
	const std::string slash(1, '\\');
	const std::string expected = std::string("{\"s\":\"q") + slash + "\"" + slash + slash + slash + "n" + slash +
				     "u0001\",\"n\":1.5,\"i\":600,\"neg\":0,\"b\":true,\"z\":null,"
				     "\"a\":[29.97,1000000]}";
	CHECK_STR(Serialize(v), expected);
	// Round trip, and pretty printing holds the same data.
	const auto back = Parse(Serialize(v, true));
	CHECK(back.has_value());
	CHECK_STR(Serialize(*back), Serialize(v));
	CHECK_STR(trigglow::json::FormatNumber(NAN), "null");
	CHECK_STR(trigglow::json::FormatNumber(0.1 + 0.2), "0.3");
	// Set() replaces, Push() on a non-array is ignored.
	v.Set("i", Value::Number(1));
	CHECK(v.GetNumber("i").value_or(0) == 1.0);
	Value notArray = Value::Object();
	notArray.Push(Value::Null());
	CHECK(notArray.Size() == 0);
}

void TestLocaleIndependent()
{
	testcheck::g_currentTest = "locale";
	// A host application may switch the C locale to one with a decimal comma.
	const char *previous = std::setlocale(LC_NUMERIC, nullptr);
	const std::string saved = previous ? previous : "C";
	const char *candidates[] = {"de_DE.UTF-8", "es_ES.UTF-8", "German_Germany.1252", "Spanish_Spain.1252", "de-DE"};
	bool switched = false;
	for (const char *name : candidates) {
		if (std::setlocale(LC_NUMERIC, name)) {
			switched = true;
			break;
		}
	}
	CHECK_STR(trigglow::json::FormatNumber(29.97), "29.97");
	CHECK(Parse("29.97")->AsNumber() == 29.97);
	std::setlocale(LC_NUMERIC, saved.c_str());
	if (!switched)
		std::printf("  (no decimal-comma locale available; checked with the default one)\n");
}

} // namespace

int main()
{
	TestScalars();
	TestStrings();
	TestContainers();
	TestDepth();
	TestSerialize();
	TestLocaleIndependent();
	return testcheck::Finish("json_tests");
}
