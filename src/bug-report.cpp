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

#include "bug-report.hpp"
#include "logging.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace trigglow {

namespace {
constexpr const char *kComponent = "bug-report";
} // namespace

bool ReadFileBytes(const std::string &path, std::string &outBytes)
{
	std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
	if (!file)
		return false;

	std::streamsize size = file.tellg();
	if (size < 0)
		return false;
	file.seekg(0, std::ios::beg);

	std::string bytes(static_cast<size_t>(size), '\0');
	if (size > 0 && !file.read(bytes.data(), size))
		return false;

	outBytes = std::move(bytes);
	return true;
}

#ifdef _WIN32

BugReportLogResult CopyCurrentObsLogForSupport()
{
	BugReportLogResult result;

	wchar_t appData[MAX_PATH] = {};
	DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		TRIGGLOW_LOG_WARN(kComponent, "could not resolve %%APPDATA%% to find OBS's log folder");
		return result;
	}

	std::filesystem::path logsDir = std::filesystem::path(appData) / L"obs-studio" / L"logs";

	std::error_code ec;
	if (!std::filesystem::exists(logsDir, ec) || ec) {
		TRIGGLOW_LOG_WARN(kComponent, "OBS log folder not found at the expected path");
		return result;
	}

	// OBS names each session's log by timestamp, not a fixed "current.log"
	// filename -- the most-recently-modified .txt file in this folder IS
	// the current session's log, the same file "Help > Log Files > View
	// Current Log" opens inside OBS itself.
	std::filesystem::path newestPath;
	std::filesystem::file_time_type newestTime{};
	bool any = false;
	for (const auto &entry : std::filesystem::directory_iterator(logsDir, ec)) {
		if (ec)
			break;
		std::error_code entryEc;
		if (!entry.is_regular_file(entryEc) || entryEc || entry.path().extension() != L".txt")
			continue;
		std::error_code timeEc;
		auto modified = entry.last_write_time(timeEc);
		if (timeEc)
			continue;
		if (!any || modified > newestTime) {
			newestTime = modified;
			newestPath = entry.path();
			any = true;
		}
	}

	if (!any) {
		TRIGGLOW_LOG_WARN(kComponent, "OBS log folder exists but has no .txt log files in it");
		return result;
	}

	result.found = true;
	result.sourcePath = newestPath.u8string();

	std::filesystem::path destDir = std::filesystem::temp_directory_path(ec);
	if (ec) {
		TRIGGLOW_LOG_WARN(kComponent, "could not resolve a temp folder to copy the log into");
		return result;
	}
	destDir /= L"Trigglow";
	std::filesystem::create_directories(destDir, ec); // Fine if it already exists; ec checked via destPath copy below.

	// Timestamped so reporting a second issue the same day doesn't silently
	// overwrite the first copy still sitting in that folder.
	auto now = std::chrono::system_clock::now();
	std::time_t nowT = std::chrono::system_clock::to_time_t(now);
	std::tm tmBuf{};
	localtime_s(&tmBuf, &nowT);
	char stamp[32] = {};
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmBuf);

	std::filesystem::path destPath = destDir / (std::string("trigglow-obs-log-") + stamp + ".txt");

	std::filesystem::copy_file(newestPath, destPath, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		TRIGGLOW_LOG_WARN(kComponent, "failed to copy the OBS log for the support ticket: %s",
				  ec.message().c_str());
		return result;
	}

	result.copiedPath = destPath.u8string();
	result.copiedDir = destDir.u8string();
	TRIGGLOW_LOG_INFO(kComponent, "copied OBS log to %s", result.copiedPath.c_str());
	return result;
}

#else

BugReportLogResult CopyCurrentObsLogForSupport()
{
	// Not implemented yet -- same "unknown, degrade gracefully" convention
	// as hardware-info.hpp's QueryTotalSystemRamBytes() on these platforms.
	// The dock's button still opens the support page; it just can't
	// pre-copy the log for the user on macOS/Linux yet.
	return {};
}

#endif

} // namespace trigglow
