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

// OBS's own current session log, prepared for a support ticket. The plugin
// has no telemetry of its own -- when a stream lagged live and the user has no
// idea how to pull logs themselves, OBS's own log file (the same one "Help >
// Log Files > View Current Log" opens inside OBS) is the only record of what
// actually happened. Plain filesystem work, no Qt -- same "one small file, one
// job" split as win-http.cpp/hardware-info.cpp.
//
// Since v0.6.0 the log is read into memory and redacted there
// (log-redact.hpp: user names in paths, URLs, keys, emails, IPs, tokens)
// before anything else sees it; no copy is written to disk. It is only ever
// sent as a ticket attachment, never to Trigi.
namespace trigglow {

// True where the current log can be located (Windows today: OBS's log folder
// is a fixed, well-known path there, %APPDATA%\obs-studio\logs\). Elsewhere
// callers point the user at OBS's own "Help > Log Files" menu instead.
bool CanLocateObsLog();

struct RedactedObsLog {
	bool found = false;  // OBS's current log was located and read.
	std::string text;    // Redacted and capped for upload (logredact::CapForUpload). UTF-8.
	bool capped = false; // The middle of a very long log was left out.
};

// BLOCKING: reads (up to a few MB of the start and end of a huge log) and
// redacts. Run it off the UI thread.
RedactedObsLog ReadCurrentObsLogRedacted();

// Deletes the %TEMP%\Trigglow\trigglow-obs-log-*.txt copies that versions
// before 0.6.0 left behind (they were never cleaned up). Call once at load.
void DeleteLeftoverSupportLogCopies();

} // namespace trigglow
