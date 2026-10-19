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

#include "json.hpp"

#include <cmath>
#include <cstdio>

namespace trigglow::json {

namespace {

const std::string kEmptyString;
const Value kNullValue;

void AppendUtf8(std::string &out, uint32_t cp)
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

class Parser {
public:
	Parser(std::string_view text, size_t maxDepth) : text_(text), maxDepth_(maxDepth) {}

	std::optional<Value> Run()
	{
		// A UTF-8 byte order mark is tolerated (and ignored).
		if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
		    static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF)
			pos_ = 3;
		Value value;
		SkipWhitespace();
		if (!ParseValue(value, 0))
			return std::nullopt;
		SkipWhitespace();
		if (pos_ != text_.size())
			return std::nullopt;
		return value;
	}

private:
	bool AtEnd() const { return pos_ >= text_.size(); }
	char Peek() const { return AtEnd() ? '\0' : text_[pos_]; }

	void SkipWhitespace()
	{
		while (!AtEnd()) {
			const char c = text_[pos_];
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
				break;
			++pos_;
		}
	}

	bool Consume(std::string_view literal)
	{
		if (text_.substr(pos_, literal.size()) != literal)
			return false;
		pos_ += literal.size();
		return true;
	}

	bool ParseValue(Value &out, size_t depth)
	{
		switch (Peek()) {
		case '{':
			return ParseObject(out, depth + 1);
		case '[':
			return ParseArray(out, depth + 1);
		case '"': {
			std::string s;
			if (!ParseString(s))
				return false;
			out = Value::String(std::move(s));
			return true;
		}
		case 't':
			if (!Consume("true"))
				return false;
			out = Value::Bool(true);
			return true;
		case 'f':
			if (!Consume("false"))
				return false;
			out = Value::Bool(false);
			return true;
		case 'n':
			if (!Consume("null"))
				return false;
			out = Value::Null();
			return true;
		default:
			return ParseNumber(out);
		}
	}

	bool ParseObject(Value &out, size_t depth)
	{
		if (depth > maxDepth_)
			return false;
		++pos_; // '{'
		out = Value::Object();
		SkipWhitespace();
		if (Peek() == '}') {
			++pos_;
			return true;
		}
		for (;;) {
			SkipWhitespace();
			if (Peek() != '"')
				return false;
			std::string key;
			if (!ParseString(key))
				return false;
			SkipWhitespace();
			if (Peek() != ':')
				return false;
			++pos_;
			SkipWhitespace();
			Value member;
			if (!ParseValue(member, depth))
				return false;
			out.Set(std::move(key), std::move(member));
			SkipWhitespace();
			const char c = Peek();
			++pos_;
			if (c == '}')
				return true;
			if (c != ',')
				return false;
		}
	}

	bool ParseArray(Value &out, size_t depth)
	{
		if (depth > maxDepth_)
			return false;
		++pos_; // '['
		out = Value::Array();
		SkipWhitespace();
		if (Peek() == ']') {
			++pos_;
			return true;
		}
		for (;;) {
			SkipWhitespace();
			Value item;
			if (!ParseValue(item, depth))
				return false;
			out.Push(std::move(item));
			SkipWhitespace();
			const char c = Peek();
			++pos_;
			if (c == ']')
				return true;
			if (c != ',')
				return false;
		}
	}

	bool ParseHex4(uint32_t &out)
	{
		if (text_.size() - pos_ < 4)
			return false;
		out = 0;
		for (int i = 0; i < 4; ++i) {
			const char c = text_[pos_++];
			out <<= 4;
			if (c >= '0' && c <= '9')
				out |= static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f')
				out |= static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				out |= static_cast<uint32_t>(c - 'A' + 10);
			else
				return false;
		}
		return true;
	}

	bool ParseString(std::string &out)
	{
		++pos_; // '"'
		for (;;) {
			if (AtEnd())
				return false;
			const char c = text_[pos_++];
			if (c == '"')
				return true;
			if (static_cast<unsigned char>(c) < 0x20)
				return false; // Raw control characters aren't allowed inside strings.
			if (c != '\\') {
				out += c;
				continue;
			}
			if (AtEnd())
				return false;
			const char e = text_[pos_++];
			switch (e) {
			case '"':
				out += '"';
				break;
			case '\\':
				out += '\\';
				break;
			case '/':
				out += '/';
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				uint32_t cp = 0;
				if (!ParseHex4(cp))
					return false;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					// A high surrogate needs its low half right after it.
					uint32_t low = 0;
					const size_t save = pos_;
					if (Consume("\\u") && ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					} else {
						pos_ = save;
						cp = 0xFFFD;
					}
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					cp = 0xFFFD; // A lone low surrogate.
				}
				AppendUtf8(out, cp);
				break;
			}
			default:
				return false;
			}
		}
	}

	bool ParseNumber(Value &out)
	{
		// -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?, converted by hand:
		// strtod() would follow the process locale.
		bool negative = false;
		if (Peek() == '-') {
			negative = true;
			++pos_;
		}
		auto isDigit = [](char c) {
			return c >= '0' && c <= '9';
		};
		if (!isDigit(Peek()))
			return false;
		double mantissa = 0.0;
		int exponent = 0;
		int significant = 0;
		auto addDigit = [&](char c, bool fraction) {
			// Beyond 17 significant digits a double can't tell the difference:
			// keep the magnitude (exponent) and drop the digit.
			if (significant < 17) {
				mantissa = mantissa * 10.0 + (c - '0');
				if (mantissa != 0.0)
					++significant;
				if (fraction)
					--exponent;
			} else if (!fraction) {
				++exponent;
			}
		};
		if (Peek() == '0') {
			++pos_;
			if (isDigit(Peek()))
				return false; // No leading zeros.
		} else {
			while (isDigit(Peek()))
				addDigit(text_[pos_++], false);
		}
		if (Peek() == '.') {
			++pos_;
			if (!isDigit(Peek()))
				return false;
			while (isDigit(Peek()))
				addDigit(text_[pos_++], true);
		}
		if (Peek() == 'e' || Peek() == 'E') {
			++pos_;
			bool expNegative = false;
			if (Peek() == '+' || Peek() == '-') {
				expNegative = Peek() == '-';
				++pos_;
			}
			if (!isDigit(Peek()))
				return false;
			int e = 0;
			while (isDigit(Peek())) {
				if (e < 100000)
					e = e * 10 + (text_[pos_] - '0');
				++pos_;
			}
			exponent += expNegative ? -e : e;
		}
		double value = mantissa;
		// Dividing by an exact power of ten (up to 1e22) rounds correctly
		// ("29.97" is 2997 / 100, not 2997 * 0.01).
		if (value != 0.0 && exponent < 0 && exponent >= -22)
			value = mantissa / std::pow(10.0, static_cast<double>(-exponent));
		else if (value != 0.0 && exponent != 0)
			value = mantissa * std::pow(10.0, static_cast<double>(exponent));
		if (!std::isfinite(value))
			return false;
		out = Value::Number(negative ? -value : value);
		return true;
	}

	std::string_view text_;
	size_t maxDepth_;
	size_t pos_ = 0;
};

void AppendIndent(std::string &out, int depth)
{
	out += '\n';
	out.append(static_cast<size_t>(depth) * 2, ' ');
}

void SerializeTo(std::string &out, const Value &value, bool pretty, int depth)
{
	switch (value.type()) {
	case Type::Null:
		out += "null";
		break;
	case Type::Bool:
		out += value.AsBool() ? "true" : "false";
		break;
	case Type::Number:
		out += FormatNumber(value.AsNumber());
		break;
	case Type::String:
		AppendQuoted(out, value.AsString());
		break;
	case Type::Array: {
		out += '[';
		const size_t n = value.Size();
		for (size_t i = 0; i < n; ++i) {
			if (i > 0)
				out += ',';
			if (pretty)
				AppendIndent(out, depth + 1);
			SerializeTo(out, value.At(i), pretty, depth + 1);
		}
		if (pretty && n > 0)
			AppendIndent(out, depth);
		out += ']';
		break;
	}
	case Type::Object: {
		out += '{';
		const auto &keys = value.Keys();
		const auto &values = value.Values();
		for (size_t i = 0; i < keys.size(); ++i) {
			if (i > 0)
				out += ',';
			if (pretty)
				AppendIndent(out, depth + 1);
			AppendQuoted(out, keys[i]);
			out += pretty ? ": " : ":";
			SerializeTo(out, values[i], pretty, depth + 1);
		}
		if (pretty && !keys.empty())
			AppendIndent(out, depth);
		out += '}';
		break;
	}
	}
}

} // namespace

Value Value::Bool(bool value)
{
	Value v;
	v.type_ = Type::Bool;
	v.bool_ = value;
	return v;
}

Value Value::Number(double value)
{
	Value v;
	v.type_ = Type::Number;
	v.number_ = value;
	return v;
}

Value Value::String(std::string value)
{
	Value v;
	v.type_ = Type::String;
	v.string_ = std::move(value);
	return v;
}

Value Value::Array()
{
	Value v;
	v.type_ = Type::Array;
	return v;
}

Value Value::Object()
{
	Value v;
	v.type_ = Type::Object;
	return v;
}

const std::string &Value::AsString() const
{
	return type_ == Type::String ? string_ : kEmptyString;
}

size_t Value::Size() const
{
	return (type_ == Type::Array || type_ == Type::Object) ? items_.size() : 0;
}

const Value &Value::At(size_t index) const
{
	if (type_ != Type::Array || index >= items_.size())
		return kNullValue;
	return items_[index];
}

const Value *Value::Find(std::string_view key) const
{
	if (type_ != Type::Object)
		return nullptr;
	for (size_t i = keys_.size(); i-- > 0;) {
		if (keys_[i] == key)
			return &items_[i];
	}
	return nullptr;
}

std::optional<std::string> Value::GetString(std::string_view key) const
{
	const Value *v = Find(key);
	if (!v || !v->IsString())
		return std::nullopt;
	return v->AsString();
}

std::optional<double> Value::GetNumber(std::string_view key) const
{
	const Value *v = Find(key);
	if (!v || !v->IsNumber())
		return std::nullopt;
	return v->AsNumber();
}

std::optional<bool> Value::GetBool(std::string_view key) const
{
	const Value *v = Find(key);
	if (!v || !v->IsBool())
		return std::nullopt;
	return v->AsBool();
}

Value &Value::Set(std::string key, Value value)
{
	if (type_ != Type::Object)
		return *this;
	for (size_t i = 0; i < keys_.size(); ++i) {
		if (keys_[i] == key) {
			items_[i] = std::move(value);
			return *this;
		}
	}
	keys_.push_back(std::move(key));
	items_.push_back(std::move(value));
	return *this;
}

Value &Value::Push(Value value)
{
	if (type_ == Type::Array)
		items_.push_back(std::move(value));
	return *this;
}

std::optional<Value> Parse(std::string_view text, size_t maxDepth)
{
	return Parser(text, maxDepth).Run();
}

std::string Serialize(const Value &value, bool pretty)
{
	std::string out;
	SerializeTo(out, value, pretty, 0);
	return out;
}

void AppendQuoted(std::string &out, std::string_view utf8)
{
	static const char kHex[] = "0123456789abcdef";
	out += '"';
	for (const char c : utf8) {
		const auto u = static_cast<unsigned char>(c);
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (u < 0x20 || u == 0x7F) {
				out += "\\u00";
				out += kHex[u >> 4];
				out += kHex[u & 0x0F];
			} else {
				out += c;
			}
		}
	}
	out += '"';
}

std::string FormatNumber(double value)
{
	if (!std::isfinite(value))
		return "null";
	char buf[64];
	if (std::fabs(value) < 1e15 && value == std::floor(value))
		std::snprintf(buf, sizeof(buf), "%.0f", value);
	else
		std::snprintf(buf, sizeof(buf), "%.15g", value);
	std::string s(buf);
	// snprintf follows the C locale, which a host application may have set to
	// one with a decimal comma.
	for (char &c : s) {
		if (c == ',')
			c = '.';
	}
	if (s == "-0")
		s = "0";
	return s;
}

} // namespace trigglow::json
