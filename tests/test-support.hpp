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

// Helpers shared by the tests of the account, Trigi and ticket code (v0.6.0):
// string checks that print both sides, a scratch folder, and builders for
// test inputs. The scripted HTTP transport is in fake-transport.hpp.
//
// Test inputs that look like real machine data (a Windows profile path, a
// URL on a host other than Trigglow's) are assembled from parts at run time
// instead of being written out whole, so a scan of the published sources for
// leaked paths and hosts stays clean. The values themselves are invented.

#include "check.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace testsupport {

inline void CheckStrEq(const std::string &actual, const std::string &expected, const char *expr, const char *file,
		       int line)
{
	if (actual != expected) {
		std::printf("  actual:   \"%s\"\n  expected: \"%s\"\n", actual.c_str(), expected.c_str());
	}
	::testcheck::Check(actual == expected, expr, file, line);
}

// {"C:", "Users", "name"} joined with backslashes (or with `sep`).
inline std::string JoinPath(std::initializer_list<std::string_view> parts, std::string_view sep = "\\")
{
	std::string out;
	bool first = true;
	for (std::string_view p : parts) {
		if (!first)
			out += sep;
		out += p;
		first = false;
	}
	return out;
}

// Scheme + "://" + rest, e.g. Url("https", "example.com/a").
inline std::string Url(std::string_view scheme, std::string_view rest)
{
	return std::string(scheme) + "://" + std::string(rest);
}

// A fresh folder under the system temp folder, removed when it goes out of scope.
class ScratchDir {
public:
	explicit ScratchDir(const char *tag)
	{
		static std::atomic<int> counter{0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path_ = std::filesystem::temp_directory_path() /
			("trigglow-test-" + std::string(tag) + "-" + std::to_string(stamp) + "-" +
			 std::to_string(counter++));
		std::filesystem::create_directories(path_);
	}
	~ScratchDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(path_, ec);
	}
	ScratchDir(const ScratchDir &) = delete;
	ScratchDir &operator=(const ScratchDir &) = delete;
	const std::filesystem::path &path() const { return path_; }

private:
	std::filesystem::path path_;
};

} // namespace testsupport

#define CHECK_STR(actual, expected) \
	::testsupport::CheckStrEq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
