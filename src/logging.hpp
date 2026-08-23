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

// Thin logging wrapper around OBS's own logging (obs_log(), from
// plugin-support.h, which already prefixes every line with "[PLUGIN_NAME]").
//
// This wrapper adds a *component* tag on top (e.g. "delay-controller",
// "hotkeys", "bridge") so that a log line like:
//
//   [obs-trigglow-dynamic-delay] [delay-controller] applying 12s delay
//
// is easy to filter for when reading OBS's own log file while debugging a
// live stream. Every log call funnels through here instead of calling
// obs_log() directly, so this is the single place that would need to change
// if we ever wanted structured logging (e.g. JSON lines) in a later version.

namespace trigglow::log {

enum class Level {
	Debug,
	Info,
	Warning,
	Error,
};

// component: short, stable tag identifying the subsystem (e.g. "delay-controller").
// fmt: printf-style format string, followed by the matching varargs.
void Log(Level level, const char *component, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 3, 4)))
#endif
	;

} // namespace trigglow::log

// Convenience macros so call sites read like: LOG_INFO("delay-controller", "enabled (%us)", secs);
#define TRIGGLOW_LOG_DEBUG(component, ...) ::trigglow::log::Log(::trigglow::log::Level::Debug, component, __VA_ARGS__)
#define TRIGGLOW_LOG_INFO(component, ...) ::trigglow::log::Log(::trigglow::log::Level::Info, component, __VA_ARGS__)
#define TRIGGLOW_LOG_WARN(component, ...) ::trigglow::log::Log(::trigglow::log::Level::Warning, component, __VA_ARGS__)
#define TRIGGLOW_LOG_ERROR(component, ...) ::trigglow::log::Log(::trigglow::log::Level::Error, component, __VA_ARGS__)
