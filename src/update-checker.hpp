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

// Checks this repo's latest published GitHub release against the running
// plugin's own version -- the ONLY update-awareness this plugin has today:
// distribution is otherwise fully manual (the user has to notice a new
// version themselves on trigglow.com/dynamic-delay or GitHub). Deliberately
// simple: a public, unauthenticated GET against GitHub's REST API, no new
// trigglow.com backend endpoint needed. Kept Qt-free (plain filesystem/
// networking code only) -- same "one small file, one job" split as
// bug-report.hpp/win-http.hpp; TrigglowDelayDock does its own threading
// around this, same RunHttp-style pattern ReportBugDialog uses.
namespace trigglow {

struct UpdateCheckResult {
	// False for ANY failure (no network, GitHub down, unexpected response
	// shape) -- callers must treat this as "unknown," never surface it as an
	// error to the user. This is a nice-to-have notice, not something worth
	// bothering a streamer about failing.
	bool checked = false;
	bool updateAvailable = false;
	std::string latestVersion; // e.g. "0.4.0" (leading 'v' already stripped). Empty if !checked.
	std::string releaseUrl;    // GitHub release page to open on click. Empty if !checked.
};

// BLOCKING -- callers must not run this on the UI thread, same contract as
// every call in win-http.hpp (this is built directly on top of HttpsGet).
// currentVersion: PLUGIN_VERSION (plugin-support.h), passed in rather than
// read here so this file stays free of that header's C-linkage extern.
UpdateCheckResult CheckForUpdate(const std::string &currentVersion);

} // namespace trigglow
