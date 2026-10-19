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

// The tiny check harness the v0.6.0 tests share (buffer_fit_tests.cpp and
// encoder_check_tests.cpp keep their own copy): keeps going after a failure,
// non-zero exit code if anything failed. Not assert(): RelWithDebInfo defines
// NDEBUG, which would compile every check away.

#include <cmath>
#include <cstdio>

namespace testcheck {

inline int g_checks = 0;
inline int g_failures = 0;
inline const char *g_currentTest = "";

inline void Check(bool ok, const char *expr, const char *file, int line)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("FAIL [%s] %s:%d: %s\n", g_currentTest, file, line, expr);
	}
}

inline int Finish(const char *suite)
{
	std::printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}

} // namespace testcheck

#define CHECK(expr) ::testcheck::Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                                                            \
	do {                                                                                                      \
		const auto checkA_ = (a);                                                                         \
		const auto checkB_ = (b);                                                                         \
		if (!(checkA_ == checkB_))                                                                        \
			std::printf("  %s = %llu, %s = %llu\n", #a, static_cast<unsigned long long>(checkA_), #b, \
				    static_cast<unsigned long long>(checkB_));                                    \
		::testcheck::Check(checkA_ == checkB_, #a " == " #b, __FILE__, __LINE__);                         \
	} while (0)
#define CHECK_NEAR(a, b, eps) ::testcheck::Check(std::fabs((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)
