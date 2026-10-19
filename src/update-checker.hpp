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

#include "cancel-token.hpp"

// Checks this repo's latest published GitHub release against the running
// plugin's own version -- the ONLY update-awareness this plugin has today:
// distribution is otherwise fully manual (the user has to notice a new
// version themselves on trigglow.com/dynamic-delay or GitHub). Deliberately
// simple: a public, unauthenticated GET against GitHub's REST API, no new
// trigglow.com backend endpoint needed. Kept Qt-free (plain filesystem/
// networking code only) -- same "one small file, one job" split as
// bug-report.hpp/win-http.hpp; TrigglowDelayDock runs it as one of its
// background jobs (ui-tasks.hpp), cancelled when the dock or OBS closes.
namespace trigglow {

struct UpdateCheckResult {
	// False for ANY failure (no network, GitHub down, unexpected response
	// shape) -- callers must treat this as "unknown," never surface it as an
	// error to the user. This is a nice-to-have notice, not something worth
	// bothering a streamer about failing.
	bool checked = false;
	bool updateAvailable = false;
	std::string latestVersion; // e.g. "0.4.0" (leading 'v' already stripped). Empty if !checked.
	// GitHub release page to open on click: always one of this repo's release
	// pages (see IsOwnReleasePageUrl). Empty if !checked.
	std::string releaseUrl;
};

// This repo's release pages, the only URLs the update notice ever opens.
inline constexpr const char *kOwnReleasePagePrefix = "https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/";
inline constexpr const char *kOwnLatestReleasePage =
	"https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest";

// True only for kOwnReleasePagePrefix followed by a plain path ([A-Za-z0-9._-]
// segments separated by single '/', no ".."): html_url comes from the
// network, so another host or repo, another scheme, a query, a fragment,
// percent-escapes, whitespace or control characters are all rejected.
inline bool IsOwnReleasePageUrl(const std::string &url)
{
	const std::string prefix = kOwnReleasePagePrefix;
	if (url.size() <= prefix.size() || url.compare(0, prefix.size(), prefix) != 0)
		return false;
	char previous = '/';
	for (size_t i = prefix.size(); i < url.size(); ++i) {
		const char c = url[i];
		const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
				   c == '.' || c == '-' || c == '_';
		if (!plain && c != '/')
			return false;
		if (c == '/' && previous == '/')
			return false;
		previous = c;
	}
	return url.find("..", prefix.size()) == std::string::npos;
}

// BLOCKING -- callers must not run this on the UI thread, same contract as
// every call in win-http.hpp (this is built directly on top of http::Send).
// currentVersion: PLUGIN_VERSION (plugin-support.h), passed in rather than
// read here so this file stays free of that header's C-linkage extern.
// cancel: the job's token; when it is cancelled the request ends at once and
// the result is "not checked", without a log line.
UpdateCheckResult CheckForUpdate(const std::string &currentVersion, const tasks::CancelToken *cancel = nullptr);

} // namespace trigglow
