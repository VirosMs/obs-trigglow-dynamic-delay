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
#include "log-redact.hpp"
#include "logging.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace trigglow {

namespace {

#ifdef _WIN32
// Only referenced by the Windows implementation below -- the non-Windows
// stubs don't log at all, so declaring this unconditionally would be an
// unused-variable warning-as-error there.
constexpr const char *kComponent = "bug-report";

// Logs up to this size are read whole; past it, only the start and the end
// (redaction and the upload cap only keep 4.5 MB anyway).
constexpr std::uintmax_t kReadWholeBytes = 16ull * 1024 * 1024;
constexpr std::uintmax_t kReadHeadBytes = 1024 * 1024;
constexpr std::uintmax_t kReadTailBytes = 8ull * 1024 * 1024;

bool ReadRange(std::ifstream &file, std::uintmax_t offset, std::uintmax_t length, std::string &out)
{
	out.assign(static_cast<size_t>(length), '\0');
	file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
	if (!file.read(out.data(), static_cast<std::streamsize>(length))) {
		out.resize(static_cast<size_t>(file.gcount()));
		file.clear();
	}
	return !out.empty();
}

bool AppDataDir(std::filesystem::path &out)
{
	wchar_t appData[MAX_PATH] = {};
	const DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
		return false;
	out = std::filesystem::path(appData);
	return true;
}

// OBS names each session's log by timestamp, not a fixed "current.log"
// filename -- the most-recently-modified .txt file in its log folder IS the
// current session's log.
bool FindCurrentLog(std::filesystem::path &out)
{
	std::filesystem::path appData;
	if (!AppDataDir(appData)) {
		TRIGGLOW_LOG_WARN(kComponent, "could not resolve %%APPDATA%% to find OBS's log folder");
		return false;
	}
	const std::filesystem::path logsDir = appData / L"obs-studio" / L"logs";
	std::error_code ec;
	if (!std::filesystem::exists(logsDir, ec) || ec) {
		TRIGGLOW_LOG_WARN(kComponent, "OBS log folder not found at the expected path");
		return false;
	}
	std::filesystem::file_time_type newestTime{};
	bool any = false;
	for (const auto &entry : std::filesystem::directory_iterator(logsDir, ec)) {
		std::error_code entryEc;
		if (!entry.is_regular_file(entryEc) || entryEc || entry.path().extension() != L".txt")
			continue;
		const auto modified = entry.last_write_time(entryEc);
		if (entryEc)
			continue;
		if (!any || modified > newestTime) {
			newestTime = modified;
			out = entry.path();
			any = true;
		}
	}
	if (!any)
		TRIGGLOW_LOG_WARN(kComponent, "OBS log folder exists but has no .txt log files in it");
	return any;
}
#endif

} // namespace

#ifdef _WIN32

bool CanLocateObsLog()
{
	return true;
}

RedactedObsLog ReadCurrentObsLogRedacted()
{
	RedactedObsLog result;
	std::filesystem::path path;
	if (!FindCurrentLog(path))
		return result;

	std::error_code ec;
	const std::uintmax_t size = std::filesystem::file_size(path, ec);
	// OBS keeps writing to it: shared read, and whatever is there right now.
	std::ifstream file(path, std::ios::binary);
	if (ec || !file) {
		TRIGGLOW_LOG_WARN(kComponent, "found OBS's current log but could not open it");
		return result;
	}
	std::string raw;
	if (size <= kReadWholeBytes) {
		ReadRange(file, 0, size, raw);
	} else {
		std::string head;
		std::string tail;
		ReadRange(file, 0, kReadHeadBytes, head);
		ReadRange(file, size - kReadTailBytes, kReadTailBytes, tail);
		raw = head + logredact::kCutMarker + tail;
	}
	result.found = true;
	result.text = logredact::CapForUpload(logredact::RedactText(raw), &result.capped);
	result.capped = result.capped || size > kReadWholeBytes;
	TRIGGLOW_LOG_INFO(kComponent, "prepared OBS's current log for support (%zu KB redacted%s)",
			  result.text.size() / 1024, result.capped ? ", shortened" : "");
	return result;
}

void DeleteLeftoverSupportLogCopies()
{
	std::error_code ec;
	const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / L"Trigglow";
	if (ec || !std::filesystem::exists(dir, ec))
		return;
	size_t removed = 0;
	for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		const std::wstring name = entry.path().filename().wstring();
		if (name.rfind(L"trigglow-obs-log-", 0) == 0 && entry.path().extension() == L".txt") {
			std::error_code removeEc;
			if (std::filesystem::remove(entry.path(), removeEc))
				++removed;
		}
	}
	std::filesystem::remove(dir, ec); // Only if it is empty now.
	if (removed > 0)
		TRIGGLOW_LOG_INFO(kComponent, "deleted %zu old log copies from %%TEMP%%\\Trigglow", removed);
}

#else

bool CanLocateObsLog()
{
	return false;
}

RedactedObsLog ReadCurrentObsLogRedacted()
{
	// Not implemented yet -- same "unknown, degrade gracefully" convention
	// as hardware-info.hpp's QueryTotalSystemRamBytes() on these platforms.
	return {};
}

void DeleteLeftoverSupportLogCopies() {}

#endif

} // namespace trigglow
