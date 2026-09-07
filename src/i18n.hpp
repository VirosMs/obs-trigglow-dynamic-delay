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

extern "C" {
#include <obs-module.h>
}

// Str(key) -- thin alias for obs_module_text(), OBS's own per-module locale
// lookup (resolves against data/locale/<OBS's current UI locale>.ini, with
// data/locale/en-US.ini as the fallback -- OBS_MODULE_USE_DEFAULT_LOCALE in
// plugin-main.cpp declares that fallback). Standard convention across OBS's
// own built-in plugins, adopted here 2026-09-07: before this pass every
// user-facing string in this plugin (dock, dialogs, filter/hotkey names,
// status messages) was a hardcoded Spanish literal in the C++ source,
// regardless of what language OBS itself was set to -- data/locale/*.ini
// existed but were only ever used for the module's own listing name. Every
// user-facing string should now go through Str() instead of a literal; see
// data/locale/en-US.ini / es-ES.ini for the actual translated text, and
// TRIGGLOW_LOG_*() (logging.hpp) for the one category of string that
// deliberately stays English/untranslated -- diagnostic log lines, which
// exist for developers reading obs-studio's log file, not end users.
#define Str(lookup) obs_module_text(lookup)
