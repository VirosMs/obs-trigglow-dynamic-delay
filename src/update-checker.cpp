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

#include "update-checker.hpp"
#include "logging.hpp"
#include "win-http.hpp"

extern "C" {
#include <obs-module.h>
}

#include <array>
#include <cstdlib>

namespace trigglow {

namespace {
constexpr const char *kComponent = "update-checker";
constexpr const wchar_t *kGithubApiHost = L"api.github.com";
constexpr const wchar_t *kGithubReleasesPath = L"/repos/VirosMs/obs-trigglow-dynamic-delay/releases/latest";

// Parses "X.Y.Z" (leading 'v' already stripped by the caller) into up to 3
// numeric components; a missing or non-numeric component reads as 0 --
// lenient on purpose, a malformed tag should just compare as "not newer"
// rather than throw/crash this background thread.
std::array<int, 3> ParseVersion(const std::string &version)
{
	std::array<int, 3> parts{0, 0, 0};
	size_t start = 0;
	for (int i = 0; i < 3 && start <= version.size(); ++i) {
		size_t dot = version.find('.', start);
		std::string piece = version.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
		parts[static_cast<size_t>(i)] = std::atoi(piece.c_str());
		if (dot == std::string::npos)
			break;
		start = dot + 1;
	}
	return parts;
}

// std::array<int, 3>'s operator< is already lexicographic (major, then
// minor, then patch), which is exactly semver ordering for this plugin's
// plain X.Y.Z tags (no pre-release/build-metadata suffixes to worry about).
bool IsNewer(const std::string &latest, const std::string &current)
{
	return ParseVersion(latest) > ParseVersion(current);
}

} // namespace

UpdateCheckResult CheckForUpdate(const std::string &currentVersion)
{
	UpdateCheckResult result;

	// GitHub's REST API requires a User-Agent header on every request (403
	// otherwise) -- win-http.cpp's WinHttpOpen() already sets one for the
	// whole session (its own agent string), so nothing extra is needed here.
	HttpResult httpResult = HttpsGet(kGithubApiHost, kGithubReleasesPath);
	if (!httpResult.ok || httpResult.statusCode != 200) {
		TRIGGLOW_LOG_INFO(kComponent,
				  "update check failed (HTTP %d, %s) -- treating as unknown, not an error",
				  httpResult.statusCode, httpResult.error.c_str());
		return result;
	}

	obs_data_t *data = obs_data_create_from_json(httpResult.body.c_str());
	if (!data) {
		TRIGGLOW_LOG_WARN(kComponent, "update check: invalid JSON from GitHub");
		return result;
	}

	const char *tagName = obs_data_get_string(data, "tag_name");
	const char *htmlUrl = obs_data_get_string(data, "html_url");
	if (!tagName || !*tagName) {
		TRIGGLOW_LOG_WARN(kComponent, "update check: response missing tag_name");
		obs_data_release(data);
		return result;
	}

	std::string latest = tagName;
	if (!latest.empty() && (latest.front() == 'v' || latest.front() == 'V'))
		latest.erase(0, 1);

	result.checked = true;
	result.latestVersion = latest;
	result.releaseUrl = htmlUrl ? htmlUrl : "";
	result.updateAvailable = IsNewer(latest, currentVersion);

	obs_data_release(data);

	TRIGGLOW_LOG_INFO(kComponent, "update check: running v%s, latest published is v%s (%s)",
			  currentVersion.c_str(), latest.c_str(),
			  result.updateAvailable ? "update available" : "up to date");

	return result;
}

} // namespace trigglow
