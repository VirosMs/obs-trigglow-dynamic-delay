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
#include <string_view>
#include <vector>

// Names of the objects the plugin creates and manages by itself in the user's
// scene collection (wrapper scene, delay filters, overlay image). Pure C++, no
// libobs: tests/auto_names_tests.cpp.
//
// OBS looks these up BY NAME (obs_get_source_by_name,
// obs_source_get_filter_by_name), so they are fixed English ASCII constants,
// never translated: a translated name would orphan every object created while
// OBS ran in another language. Up to v0.5.1 they ended in "(no tocar)" /
// "(auto, no tocar)" ("don't touch" in Spanish) in every language; since then
// they end in "(auto)". Old collections are migrated once
// (ObsFrontendBridge::MigrateLegacyAutoNames renames them in place, keeping
// position, filters and settings), and every lookup also accepts the old names
// (LookupNames), so a collection that could not be renamed -- the new name was
// already taken -- keeps working.
namespace trigglow::autonames {

// Current names.
inline constexpr const char *kWrapperScene = "Trigglow Delay Buffer (auto)";
inline constexpr const char *kBufferFilter = "Trigglow Buffer Mode Delay (auto)";
// Single image source with the generated badge (overlay-badge.hpp).
inline constexpr const char *kOverlayBadge = "Trigglow Delay Badge (auto)";
// One audio filter per audio source of the live scene: prefix + source name.
inline constexpr const char *kAudioFilterPrefix = "Trigglow Audio Delay (auto) - ";

enum class Kind {
	WrapperScene,
	BufferFilter,
	OverlayBadge,
};

// Names to try, in order, when looking an object up: the current name first,
// then the names earlier versions created.
std::vector<std::string> LookupNames(Kind kind);
// Same for the audio filter attached to the source called `sourceName`.
std::vector<std::string> AudioFilterLookupNames(std::string_view sourceName);
std::string AudioFilterName(std::string_view sourceName);

// The current name for an object that still carries a name from an earlier
// version, or an empty string if `name` isn't one (already migrated, or not
// ours). Covers the wrapper scene, the buffer filter, the image source of the
// v0.5.x overlay and the audio filters; not the overlay sources that are
// removed instead (IsRetiredOverlayName).
std::string MigratedName(std::string_view name);

// Overlay sources of earlier versions that the single generated image
// replaces: the v0.5.x "Delay Ns" text source and earlier prototypes. They are
// removed from the wrapper scene on sight.
bool IsRetiredOverlayName(std::string_view name);
// Any overlay item the wrapper scene may hold, current or old: never the live
// scene the delay filter attaches to.
bool IsOverlayName(std::string_view name);

// What to do with an object found under an old name.
enum class RenamePlan {
	Rename,      // The current name is free: rename in place.
	KeepLegacy,  // The current name is taken by something else: leave it, lookups fall back.
	NothingToDo, // No object under the old name.
};
RenamePlan PlanRename(bool legacyExists, bool currentNameTaken);

} // namespace trigglow::autonames
