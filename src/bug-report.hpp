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

#include <string>

// Finds and copies OBS's own current session log somewhere convenient for a
// user to attach to a Trigglow support ticket. The plugin has no telemetry
// of its own -- when a stream lagged live and the user has no idea how to
// pull logs themselves, OBS's own log file (the same one "Help > Log Files >
// View Current Log" opens inside OBS) is the only record of what actually
// happened. Kept deliberately separate from settings-ui.cpp (the dock,
// Qt-facing): this file does plain filesystem work only, no Qt dependency --
// same "one small file, one job" split as win-http.cpp/hardware-info.cpp.
namespace trigglow {

struct BugReportLogResult {
	bool found = false;     // False if OBS's own log directory/current log couldn't be located.
	std::string sourcePath; // OBS's original log file path (UTF-8). Empty if !found.
	std::string copiedPath; // Where it was copied for the user to attach (UTF-8). Empty if the copy failed.
	std::string copiedDir;  // Just the containing folder of copiedPath, for opening in a file browser.
};

// Windows only for now -- OBS's log directory is a fixed, well-known path
// there (%APPDATA%\obs-studio\logs\). Other platforms return found=false,
// same "unknown, degrade gracefully rather than guess" convention
// hardware-info.hpp's QueryTotalSystemRamBytes() already uses for
// non-Windows RAM detection. Callers must handle found=false by pointing the
// user at OBS's own "Help > Log Files > View Current Log" menu instead.
BugReportLogResult CopyCurrentObsLogForSupport();

// Reads `path` fully into `outBytes` (binary, no text-mode newline
// translation). Returns false (outBytes left untouched) if the file can't be
// opened. Used by ReportBugDialog to load the copied OBS log before
// attaching it to a support ticket via win-http.hpp's
// HttpsPostMultipartFile() -- kept here, not in that Qt-facing dialog file,
// same "plain filesystem work only" split as CopyCurrentObsLogForSupport()
// above.
bool ReadFileBytes(const std::string &path, std::string &outBytes);

} // namespace trigglow
