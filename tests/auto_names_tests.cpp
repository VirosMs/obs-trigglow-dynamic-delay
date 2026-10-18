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

// Standalone unit tests for trigglow::autonames (src/auto-names.hpp): the
// names of the objects the plugin manages in the scene collection and their
// migration from the "(no tocar)" names used up to v0.5.1. Same approach as the
// other tests: no libobs, no framework, non-zero exit on any failure.

#include "auto-names.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace trigglow::autonames;

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_currentTest = "";

void Check(bool ok, const char *expr, const char *file, int line)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("FAIL [%s] %s:%d: %s\n", g_currentTest, file, line, expr);
	}
}

#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

// A source name with Latin-1 and CJK characters ("Micr<o acute>fono <middle
// dot> <3 CJK ideographs>") as escaped UTF-8 bytes: the standalone test build
// doesn't pass /utf-8 to MSVC, so this file stays ASCII.
const char *const kUnicodeSource = "Micr\xc3\xb3"
				   "fono \xc2\xb7 \xe9\xba\xa6\xe5\x85\x8b\xe9\xa3\x8e";

bool IsAscii(const std::string &s)
{
	for (unsigned char c : s) {
		if (c >= 0x80)
			return false;
	}
	return true;
}

void TestCurrentNames()
{
	g_currentTest = "current names";
	// Fixed, English, ASCII: OBS finds them by name in any UI language.
	for (const char *name : {kWrapperScene, kBufferFilter, kOverlayBadge, kAudioFilterPrefix}) {
		CHECK(IsAscii(name));
		CHECK(std::strstr(name, "(auto)") != nullptr);
		CHECK(std::strstr(name, "no tocar") == nullptr);
	}
	CHECK(std::string(kWrapperScene) == "Trigglow Delay Buffer (auto)");
	CHECK(std::string(kBufferFilter) == "Trigglow Buffer Mode Delay (auto)");
	CHECK(std::string(kOverlayBadge) == "Trigglow Delay Badge (auto)");
	CHECK(AudioFilterName("Mic/Aux") == "Trigglow Audio Delay (auto) - Mic/Aux");
	CHECK(AudioFilterName("") == "Trigglow Audio Delay (auto) - ?");
	// Never the default instance name OBS gives a manually added filter of
	// our type (the localized type name), see obs-frontend-bridge.cpp.
	CHECK(std::string(kBufferFilter) != "Trigglow: Video delay buffer");
	CHECK(std::string(kBufferFilter) != "Trigglow: Buffer de delay de v\xc3\xad"
					    "deo");
}

void TestLookupOrder()
{
	g_currentTest = "lookup order";
	auto wrapper = LookupNames(Kind::WrapperScene);
	CHECK(wrapper.size() == 2);
	CHECK(wrapper.front() == kWrapperScene);
	CHECK(wrapper.back() == "Trigglow Delay Buffer (no tocar)");

	auto filter = LookupNames(Kind::BufferFilter);
	CHECK(filter.size() == 2);
	CHECK(filter.front() == kBufferFilter);
	CHECK(filter.back() == "Trigglow Buffer Mode Delay (auto, no tocar)");

	auto badge = LookupNames(Kind::OverlayBadge);
	CHECK(badge.size() == 2);
	CHECK(badge.front() == kOverlayBadge);
	CHECK(badge.back() == "Trigglow Delay Overlay Badge (auto, no tocar)");

	auto audio = AudioFilterLookupNames("Desktop Audio");
	CHECK(audio.size() == 2);
	CHECK(audio.front() == "Trigglow Audio Delay (auto) - Desktop Audio");
	CHECK(audio.back() == "Trigglow Audio Delay (auto, no tocar) - Desktop Audio");
	// Source names in any language/script survive byte for byte.
	auto unicode = AudioFilterLookupNames(kUnicodeSource);
	CHECK(unicode.front() == std::string("Trigglow Audio Delay (auto) - ") + kUnicodeSource);
	CHECK(unicode.back() == std::string("Trigglow Audio Delay (auto, no tocar) - ") + kUnicodeSource);
}

void TestMigratedName()
{
	g_currentTest = "migrated name";
	CHECK(MigratedName("Trigglow Delay Buffer (no tocar)") == kWrapperScene);
	CHECK(MigratedName("Trigglow Buffer Mode Delay (auto, no tocar)") == kBufferFilter);
	CHECK(MigratedName("Trigglow Delay Overlay Badge (auto, no tocar)") == kOverlayBadge);
	CHECK(MigratedName("Trigglow Audio Delay (auto, no tocar) - Mic/Aux") ==
	      "Trigglow Audio Delay (auto) - Mic/Aux");
	CHECK(MigratedName(std::string("Trigglow Audio Delay (auto, no tocar) - ") + kUnicodeSource) ==
	      std::string("Trigglow Audio Delay (auto) - ") + kUnicodeSource);

	// Already migrated, or not ours: nothing to do.
	for (const char *name :
	     {kWrapperScene, kBufferFilter, kOverlayBadge, "Trigglow Audio Delay (auto) - Mic/Aux", "Gameplay", "",
	      "Trigglow Delay Buffer (no tocar) 2", "Trigglow Buffer Mode Delay (auto, no tocar) 2",
	      "trigglow delay buffer (no tocar)", "Trigglow Audio Delay (auto, no tocar) - "})
		CHECK(MigratedName(name).empty());

	// The retired overlay sources are removed, not renamed.
	CHECK(MigratedName("Trigglow Delay Overlay Text (auto, no tocar)").empty());

	// Migration is idempotent: the migrated name never migrates again.
	for (const char *legacy :
	     {"Trigglow Delay Buffer (no tocar)", "Trigglow Buffer Mode Delay (auto, no tocar)",
	      "Trigglow Delay Overlay Badge (auto, no tocar)", "Trigglow Audio Delay (auto, no tocar) - Mic/Aux"}) {
		const std::string once = MigratedName(legacy);
		CHECK(!once.empty());
		CHECK(MigratedName(once).empty());
	}
}

void TestOverlayNames()
{
	g_currentTest = "overlay names";
	CHECK(IsRetiredOverlayName("Trigglow Delay Overlay Text (auto, no tocar)"));
	CHECK(IsRetiredOverlayName("Trigglow Delay Overlay (auto, no tocar)"));
	CHECK(IsRetiredOverlayName("Trigglow Delay Text (auto, no tocar)"));
	CHECK(IsRetiredOverlayName("Trigglow Delay Badge (auto, no tocar)"));
	// The current badge and the v0.5.x badge (renamed in place) are kept.
	CHECK(!IsRetiredOverlayName(kOverlayBadge));
	CHECK(!IsRetiredOverlayName("Trigglow Delay Overlay Badge (auto, no tocar)"));

	CHECK(IsOverlayName(kOverlayBadge));
	CHECK(IsOverlayName("Trigglow Delay Overlay Badge (auto, no tocar)"));
	CHECK(IsOverlayName("Trigglow Delay Overlay Text (auto, no tocar)"));
	// A live scene is never mistaken for an overlay item.
	CHECK(!IsOverlayName("Gameplay"));
	CHECK(!IsOverlayName(kWrapperScene));
	CHECK(!IsOverlayName(""));
}

void TestWrapperSceneNames()
{
	g_currentTest = "wrapper scene names";
	// Kept out of the dock's scene pickers, under both names.
	CHECK(IsWrapperSceneName(kWrapperScene));
	CHECK(IsWrapperSceneName("Trigglow Delay Buffer (no tocar)"));
	CHECK(!IsWrapperSceneName("Gameplay"));
	CHECK(!IsWrapperSceneName("Trigglow Delay Buffer"));
	CHECK(!IsWrapperSceneName("Trigglow Delay Buffer (auto) 2"));
	CHECK(!IsWrapperSceneName(kOverlayBadge));
	CHECK(!IsWrapperSceneName(""));
}

void TestRenamePlan()
{
	g_currentTest = "rename plan";
	CHECK(PlanRename(false, false) == RenamePlan::NothingToDo);
	CHECK(PlanRename(false, true) == RenamePlan::NothingToDo);
	CHECK(PlanRename(true, false) == RenamePlan::Rename);
	// libobs would silently dedupe the name to "... 2"; keep the old one and
	// let the lookups fall back to it instead.
	CHECK(PlanRename(true, true) == RenamePlan::KeepLegacy);
}

} // namespace

int main()
{
	TestCurrentNames();
	TestLookupOrder();
	TestMigratedName();
	TestOverlayNames();
	TestWrapperSceneNames();
	TestRenamePlan();

	std::printf("auto_names_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
