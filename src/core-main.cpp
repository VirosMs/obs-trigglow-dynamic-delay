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

// Entry point of the core build (TRIGGLOW_CORE_ONLY): registers the two delay
// filters and nothing else. No dock, no hotkeys, no account gate, no
// obs-frontend-api -- the host (Trigglow Studio) builds the wrapper scene,
// attaches the filters, drives them through their settings (host_* video info
// included, see video-delay-filter.cpp) and gates them on its own sign-in.
// plugin-main.cpp is the OBS build.

extern "C" {
#include <obs-module.h>
#include <plugin-support.h>
}

#include "audio-delay-filter.hpp"
#include "video-delay-filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("Trigglow (VirosMs)")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Trigglow Dynamic Delay core: the video and audio delay filters, without OBS's UI.";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "loading Trigglow Dynamic Delay core v%s", PLUGIN_VERSION);
	trigglow::VideoDelayFilter::Register();
	trigglow::AudioDelayFilter::Register();
	return true;
}
