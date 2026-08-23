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

#include "logging.hpp"

#include <cstdarg>
#include <cstdio>

extern "C" {
#include <plugin-support.h> // obs_log(), from the template's generated plugin-support.c
}

// OBS log levels (LOG_DEBUG/LOG_INFO/LOG_WARNING/LOG_ERROR) come from
// <util/base.h>, pulled in transitively through obs-module.h in other
// translation units. We avoid depending on that header here by using the
// same integer values OBS itself defines, so this file has no OBS include
// beyond plugin-support.h.
namespace {
constexpr int kObsLogError = 100;
constexpr int kObsLogWarning = 200;
constexpr int kObsLogInfo = 300;
constexpr int kObsLogDebug = 400;

int ToObsLogLevel(trigglow::log::Level level)
{
	switch (level) {
	case trigglow::log::Level::Debug:
		return kObsLogDebug;
	case trigglow::log::Level::Info:
		return kObsLogInfo;
	case trigglow::log::Level::Warning:
		return kObsLogWarning;
	case trigglow::log::Level::Error:
		return kObsLogError;
	}
	return kObsLogInfo;
}
} // namespace

namespace trigglow::log {

void Log(Level level, const char *component, const char *fmt, ...)
{
	// Build "[component] <message>" first, then hand it to obs_log(), which
	// itself prepends "[PLUGIN_NAME] " — final line looks like:
	//   [obs-trigglow-dynamic-delay] [delay-controller] applying 12s delay
	char message[1024];

	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	obs_log(ToObsLogLevel(level), "[%s] %s", component, message);
}

} // namespace trigglow::log
