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

#include "auto-names.hpp"

namespace trigglow::autonames {

namespace {

// Names used up to v0.5.1, oldest last.
constexpr const char *kLegacyWrapperScenes[] = {"Trigglow Delay Buffer (no tocar)"};
constexpr const char *kLegacyBufferFilters[] = {"Trigglow Buffer Mode Delay (auto, no tocar)"};
// The v0.5.x badge frame (an image source): renamed and repointed to the
// generated image, so a collection keeps the same item.
constexpr const char *kLegacyOverlayBadges[] = {"Trigglow Delay Overlay Badge (auto, no tocar)"};
constexpr const char *kLegacyAudioFilterPrefixes[] = {"Trigglow Audio Delay (auto, no tocar) - "};
// Removed instead of renamed: the v0.5.x text source (Segoe UI, GDI+/FreeType)
// and the prototypes before it.
constexpr const char *kRetiredOverlays[] = {
	"Trigglow Delay Overlay Text (auto, no tocar)",
	"Trigglow Delay Overlay (auto, no tocar)",
	"Trigglow Delay Text (auto, no tocar)",
	"Trigglow Delay Badge (auto, no tocar)",
};

bool StartsWith(std::string_view text, std::string_view prefix)
{
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

template<size_t N> bool Contains(const char *const (&names)[N], std::string_view name)
{
	for (const char *candidate : names) {
		if (name == candidate)
			return true;
	}
	return false;
}

template<size_t N> void Append(std::vector<std::string> &out, const char *const (&names)[N])
{
	for (const char *name : names)
		out.emplace_back(name);
}

} // namespace

std::vector<std::string> LookupNames(Kind kind)
{
	std::vector<std::string> names;
	switch (kind) {
	case Kind::WrapperScene:
		names.emplace_back(kWrapperScene);
		Append(names, kLegacyWrapperScenes);
		break;
	case Kind::BufferFilter:
		names.emplace_back(kBufferFilter);
		Append(names, kLegacyBufferFilters);
		break;
	case Kind::OverlayBadge:
		names.emplace_back(kOverlayBadge);
		Append(names, kLegacyOverlayBadges);
		break;
	}
	return names;
}

std::string AudioFilterName(std::string_view sourceName)
{
	std::string name(kAudioFilterPrefix);
	name.append(sourceName.empty() ? std::string_view("?") : sourceName);
	return name;
}

std::vector<std::string> AudioFilterLookupNames(std::string_view sourceName)
{
	const std::string_view suffix = sourceName.empty() ? std::string_view("?") : sourceName;
	std::vector<std::string> names{AudioFilterName(sourceName)};
	for (const char *prefix : kLegacyAudioFilterPrefixes) {
		std::string legacy(prefix);
		legacy.append(suffix);
		names.push_back(std::move(legacy));
	}
	return names;
}

std::string MigratedName(std::string_view name)
{
	if (Contains(kLegacyWrapperScenes, name))
		return kWrapperScene;
	if (Contains(kLegacyBufferFilters, name))
		return kBufferFilter;
	if (Contains(kLegacyOverlayBadges, name))
		return kOverlayBadge;
	for (const char *prefix : kLegacyAudioFilterPrefixes) {
		const std::string_view legacyPrefix(prefix);
		// A bare prefix (no source name after it) is not something this
		// plugin ever created.
		if (StartsWith(name, legacyPrefix) && name.size() > legacyPrefix.size()) {
			std::string migrated(kAudioFilterPrefix);
			migrated.append(name.substr(legacyPrefix.size()));
			return migrated;
		}
	}
	return {};
}

bool IsRetiredOverlayName(std::string_view name)
{
	return Contains(kRetiredOverlays, name);
}

bool IsOverlayName(std::string_view name)
{
	return name == kOverlayBadge || Contains(kLegacyOverlayBadges, name) || IsRetiredOverlayName(name);
}

bool IsWrapperSceneName(std::string_view name)
{
	return name == kWrapperScene || Contains(kLegacyWrapperScenes, name);
}

RenamePlan PlanRename(bool legacyExists, bool currentNameTaken)
{
	if (!legacyExists)
		return RenamePlan::NothingToDo;
	return currentNameTaken ? RenamePlan::KeepLegacy : RenamePlan::Rename;
}

} // namespace trigglow::autonames
