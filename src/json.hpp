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
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Small JSON reader and writer for the plugin's own API calls (api-client.hpp and
// the code built on it). No libobs or Qt, so the protocol code is unit-tested on
// its own (tests/json_tests.cpp).
//
// The reader is strict and bounded: RFC 8259 grammar only, a nesting limit, and
// no recursion beyond that limit. Unknown members are kept but callers only look
// up the ones they know (the API contract: unknown fields are ignored). Numbers
// are parsed without the C locale (OBS can run with a locale whose decimal
// separator is a comma), and the writer always emits '.'.
namespace trigglow::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
public:
	Value() = default; // null

	static Value Null() { return Value(); }
	static Value Bool(bool value);
	static Value Number(double value);
	static Value String(std::string value);
	static Value Array();
	static Value Object();

	Type type() const { return type_; }
	bool IsNull() const { return type_ == Type::Null; }
	bool IsBool() const { return type_ == Type::Bool; }
	bool IsNumber() const { return type_ == Type::Number; }
	bool IsString() const { return type_ == Type::String; }
	bool IsArray() const { return type_ == Type::Array; }
	bool IsObject() const { return type_ == Type::Object; }

	// Plain accessors: false / 0 / "" when the value has another type.
	bool AsBool() const { return type_ == Type::Bool && bool_; }
	double AsNumber() const { return type_ == Type::Number ? number_ : 0.0; }
	const std::string &AsString() const;

	// Arrays: element count and elements (a null value past the end).
	// Objects: member count; Keys()/Values() are parallel.
	size_t Size() const;
	const Value &At(size_t index) const;
	const std::vector<std::string> &Keys() const { return keys_; }
	const std::vector<Value> &Values() const { return items_; }

	// Objects only: the member called `key` (the last one if the text repeated
	// it), or nullptr when missing or when this isn't an object.
	const Value *Find(std::string_view key) const;
	bool Has(std::string_view key) const { return Find(key) != nullptr; }
	// Typed member lookups: nullopt when missing or of another type.
	std::optional<std::string> GetString(std::string_view key) const;
	std::optional<double> GetNumber(std::string_view key) const;
	std::optional<bool> GetBool(std::string_view key) const;

	// Building. Set() replaces a member with the same key (objects only);
	// Push() appends (arrays only). Both return *this for chaining.
	Value &Set(std::string key, Value value);
	Value &Push(Value value);

private:
	Type type_ = Type::Null;
	bool bool_ = false;
	double number_ = 0.0;
	std::string string_;
	std::vector<std::string> keys_; // Objects only, parallel to items_.
	std::vector<Value> items_;      // Array elements or object member values.
};

inline constexpr size_t kDefaultMaxDepth = 32;

// nullopt for anything that isn't exactly one JSON value (optional whitespace
// and a UTF-8 BOM around it), nests deeper than maxDepth, or has a number out
// of double range.
std::optional<Value> Parse(std::string_view text, size_t maxDepth = kDefaultMaxDepth);

// Compact by default; pretty = two-space indentation (the "what is sent"
// previews). Same members, same order, same numbers either way.
std::string Serialize(const Value &value, bool pretty = false);

// Appends `utf8` as a JSON string literal (quotes included). Control
// characters are escaped; everything else is copied as is.
void AppendQuoted(std::string &out, std::string_view utf8);

// Integers without a fraction ("42"), anything else with up to 15 significant
// digits ("29.97"); non-finite numbers become "null". Never locale-dependent.
std::string FormatNumber(double value);

} // namespace trigglow::json
