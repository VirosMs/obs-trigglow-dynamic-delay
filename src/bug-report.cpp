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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace trigglow {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32

// Set once by MarkCurrentObsLog() (UI thread, at load), read by the
// background job that prepares a ticket's log.
std::mutex g_markerMutex;
std::string g_marker;
std::vector<fs::path> g_logFolders;

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

// How many of the newest logs are searched for the marker, and how far into
// each: it is written while the plugin loads, near the start of the log.
constexpr size_t kMaxCandidates = 16;
constexpr std::uintmax_t kMarkerSearchBytes = 8ull * 1024 * 1024;

bool AppDataDir(fs::path &out)
{
	wchar_t appData[MAX_PATH] = {};
	const DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
		return false;
	out = fs::path(appData);
	return true;
}

// Whether the first kMarkerSearchBytes of `path` hold `marker`, read in
// chunks (a chunk keeps the end of the one before, so a marker split between
// two is still found).
bool FileHolds(const fs::path &path, const std::string &marker)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return false;
	const size_t chunkBytes = 1024 * 1024;
	std::string window;
	std::string chunk(chunkBytes, '\0');
	std::uintmax_t read = 0;
	while (read < kMarkerSearchBytes && file) {
		file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
		const size_t got = static_cast<size_t>(file.gcount());
		if (got == 0)
			break;
		read += got;
		window.append(chunk, 0, got);
		if (window.find(marker) != std::string::npos)
			return true;
		if (window.size() > marker.size())
			window.erase(0, window.size() - marker.size());
	}
	return false;
}

// This run's log: among the .txt files of this OBS's log folders, newest
// first, the one that holds this run's marker. Never "just the newest".
bool FindCurrentLog(fs::path &out)
{
	std::string marker;
	std::vector<fs::path> folders;
	{
		std::lock_guard<std::mutex> lock(g_markerMutex);
		marker = g_marker;
		folders = g_logFolders;
	}
	if (marker.empty()) {
		TRIGGLOW_LOG_WARN(kComponent, "this OBS's log wasn't marked at load; not attaching any log");
		return false;
	}
	std::vector<std::pair<fs::file_time_type, fs::path>> candidates;
	for (const fs::path &folder : folders) {
		std::error_code ec;
		for (const auto &entry : fs::directory_iterator(folder, ec)) {
			std::error_code entryEc;
			if (!entry.is_regular_file(entryEc) || entryEc || entry.path().extension() != L".txt")
				continue;
			const auto modified = entry.last_write_time(entryEc);
			if (!entryEc)
				candidates.emplace_back(modified, entry.path());
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	if (candidates.size() > kMaxCandidates)
		candidates.resize(kMaxCandidates);
	for (const auto &candidate : candidates) {
		if (FileHolds(candidate.second, marker)) {
			out = candidate.second;
			return true;
		}
	}
	// File names and folders stay out of the log: they hold the user name.
	TRIGGLOW_LOG_WARN(kComponent,
			  "none of the %zu newest OBS logs is this OBS's current one; not attaching any log",
			  candidates.size());
	return false;
}
#endif

} // namespace

fs::path ObsLogFolderFrom(const fs::path &inside)
{
	// Walk up to "plugin_config" or "basic"; their parent is OBS's own
	// "obs-studio" folder, whose "logs" holds the logs.
	fs::path p = inside;
	for (int depth = 0; depth < 4 && p.has_relative_path(); ++depth) {
		const fs::path name = p.filename();
		if (name == "plugin_config" || name == "basic")
			return p.parent_path() / "logs";
		if (name.empty()) {
			p = p.parent_path(); // A trailing separator.
			continue;
		}
		p = p.parent_path();
	}
	return fs::path();
}

#ifdef _WIN32

bool CanLocateObsLog()
{
	return true;
}

void MarkCurrentObsLog(std::vector<fs::path> logFolders)
{
	std::random_device random;
	char marker[48];
	std::snprintf(marker, sizeof(marker), "trigglow-log-%08x%08x", static_cast<unsigned>(random()),
		      static_cast<unsigned>(random()));
	fs::path appData;
	if (AppDataDir(appData))
		logFolders.push_back(appData / L"obs-studio" / L"logs");
	// Each folder once, in order.
	std::vector<fs::path> unique;
	for (fs::path &folder : logFolders) {
		if (folder.empty())
			continue;
		folder = folder.lexically_normal();
		if (std::find(unique.begin(), unique.end(), folder) == unique.end())
			unique.push_back(folder);
	}
	{
		std::lock_guard<std::mutex> lock(g_markerMutex);
		g_marker = marker;
		g_logFolders = std::move(unique);
	}
	TRIGGLOW_LOG_INFO(kComponent, "log marker %s (\"Report a problem\" attaches only the log that holds it)",
			  marker);
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

void MarkCurrentObsLog(std::vector<fs::path> logFolders)
{
	(void)logFolders; // Nothing reads the log here yet.
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
