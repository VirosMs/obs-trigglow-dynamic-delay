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

// Tests for src/token-store.*: the encrypted sign-in file and the one-time
// read of the plain-text file of v0.5.x and earlier. A stand-in protector
// runs everywhere; on Windows the real DPAPI one is checked too.

#include "token-store.hpp"
#include "test-support.hpp"

#include <fstream>
#include <iterator>
#include <string>

using namespace trigglow;
using testsupport::ScratchDir;

namespace {

// Reversible scrambling with a header, standing in for DPAPI.
class FakeProtector final : public SecretProtector {
public:
	bool Available() const override { return true; }
	bool Protect(const std::string &plain, std::string &sealed) const override
	{
		sealed = "FAKE1";
		for (char c : plain)
			sealed += static_cast<char>(c ^ 0x5A);
		return true;
	}
	bool Unprotect(const std::string &sealed, std::string &plain) const override
	{
		if (sealed.rfind("FAKE1", 0) != 0)
			return false;
		plain.clear();
		for (size_t i = 5; i < sealed.size(); ++i)
			plain += static_cast<char>(sealed[i] ^ 0x5A);
		return true;
	}
};

std::string ReadFile(const std::filesystem::path &path)
{
	std::ifstream f(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path &path, const std::string &data)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

PluginToken Sample(const std::string &tok = "tgp_sample-Token_0123456789")
{
	return PluginToken{tok, "2026-12-01T00:00:00Z", "2026-11-20T00:00:00Z", "Streamer Name"};
}

void TestRoundTrip()
{
	testcheck::g_currentTest = "round trip";
	ScratchDir dir("store");
	const auto file = dir.path() / "trigglow-auth.dat";
	{
		TokenStore store(file, dir.path() / "legacy.json", std::make_unique<FakeProtector>());
		CHECK(store.Load() == TokenStore::LoadResult::Missing);
		CHECK(!store.HasToken());
		CHECK(store.Save(Sample()));
		CHECK(store.HasToken());
		CHECK_STR(store.CurrentToken(), "tgp_sample-Token_0123456789");
	}
	// Not plain text on disk, no temporary file left behind.
	const std::string raw = ReadFile(file);
	CHECK(raw.rfind("FAKE1", 0) == 0);
	CHECK(raw.find("tgp_sample") == std::string::npos);
	CHECK(!std::filesystem::exists(dir.path() / "trigglow-auth.dat.tmp"));

	TokenStore again(file, {}, std::make_unique<FakeProtector>());
	CHECK(again.Load() == TokenStore::LoadResult::Loaded);
	const auto t = again.Current();
	CHECK(t.has_value());
	CHECK_STR(t->token, "tgp_sample-Token_0123456789");
	CHECK_STR(t->expiresAt, "2026-12-01T00:00:00Z");
	CHECK_STR(t->renewAfter, "2026-11-20T00:00:00Z");
	CHECK_STR(t->displayName, "Streamer Name");

	again.SetDisplayName("New Name");
	TokenStore third(file, {}, std::make_unique<FakeProtector>());
	third.Load();
	CHECK_STR(third.Current()->displayName, "New Name");

	again.Clear();
	CHECK(!again.HasToken());
	CHECK(!std::filesystem::exists(file));
}

void TestUnreadable()
{
	testcheck::g_currentTest = "unreadable file";
	ScratchDir dir("store-bad");
	const auto file = dir.path() / "trigglow-auth.dat";
	// Another user's blob, a damaged file, or a valid blob with a bad token:
	// all deleted, all "signed out".
	for (const std::string &content :
	     {std::string("garbage"), std::string("FAKE1") + std::string("\x21\x7f", 2), std::string()}) {
		WriteFile(file, content);
		TokenStore store(file, {}, std::make_unique<FakeProtector>());
		CHECK(store.Load() == TokenStore::LoadResult::Discarded);
		CHECK(!store.HasToken());
		CHECK(!std::filesystem::exists(file));
	}
	{
		FakeProtector p;
		std::string sealed;
		p.Protect(R"({"v":1,"token":"has space","expiresAt":"","renewAfter":"","displayName":""})", sealed);
		WriteFile(file, sealed);
		TokenStore store(file, {}, std::make_unique<FakeProtector>());
		CHECK(store.Load() == TokenStore::LoadResult::Discarded);
	}
	{
		FakeProtector p;
		std::string sealed;
		p.Protect(R"({"v":2,"token":"tgp_x"})", sealed);
		WriteFile(file, sealed);
		TokenStore store(file, {}, std::make_unique<FakeProtector>());
		CHECK(store.Load() == TokenStore::LoadResult::Discarded);
	}
}

void TestReload()
{
	testcheck::g_currentTest = "reload";
	ScratchDir dir("store-reload");
	const auto file = dir.path() / "trigglow-auth.dat";
	TokenStore a(file, {}, std::make_unique<FakeProtector>());
	TokenStore b(file, {}, std::make_unique<FakeProtector>());
	a.Save(Sample("tgp_first"));
	b.Load();
	a.Save(Sample("tgp_second"));
	CHECK_STR(b.CurrentToken(), "tgp_first");
	CHECK_STR(b.Reload(), "tgp_second");
	CHECK_STR(b.CurrentToken(), "tgp_second");
	// A missing file keeps what is in memory.
	std::filesystem::remove(file);
	CHECK_STR(b.Reload(), "tgp_second");
}

void TestNoSecureStorage()
{
	testcheck::g_currentTest = "no secure storage";
	ScratchDir dir("store-none");
	const auto file = dir.path() / "trigglow-auth.dat";
	TokenStore store(file, {}, nullptr);
	CHECK(!store.SecureStorageAvailable());
	CHECK(!store.Save(Sample())); // Nothing written...
	CHECK(store.HasToken());      // ...but this session keeps working.
	CHECK(!std::filesystem::exists(file));
}

void TestLegacy()
{
	testcheck::g_currentTest = "legacy file";
	ScratchDir dir("store-legacy");
	const auto legacy = dir.path() / "trigglow-account.json";
	const std::string content =
		"{\n    \"token\": \"LegacySession_abc-123\",\n    \"display_name\": \"Old Name\",\n"
		"    \"email\": \"someone@example.com\"\n}";
	WriteFile(legacy, content);
	TokenStore store(dir.path() / "trigglow-auth.dat", legacy, std::make_unique<FakeProtector>());
	CHECK(store.HasLegacy());
	const PluginToken old = store.ReadLegacy();
	CHECK_STR(old.token, "LegacySession_abc-123");
	CHECK_STR(old.displayName, "Old Name");
	CHECK(old.expiresAt.empty());
	store.DeleteLegacy();
	CHECK(!store.HasLegacy());
	CHECK(!std::filesystem::exists(legacy));

	WriteFile(legacy, "{\"token\": \"bad token\"}");
	CHECK(store.ReadLegacy().token.empty());
	WriteFile(legacy, "not json");
	CHECK(store.ReadLegacy().token.empty());
}

void TestPlatformProtector()
{
	testcheck::g_currentTest = "platform protector";
	auto protector = MakePlatformProtector();
#ifdef _WIN32
	CHECK(protector->Available());
	const std::string plain = R"({"v":1,"token":"tgp_dpapi-check_42"})";
	std::string sealed;
	CHECK(protector->Protect(plain, sealed));
	CHECK(!sealed.empty() && sealed.find("tgp_dpapi") == std::string::npos);
	std::string back;
	CHECK(protector->Unprotect(sealed, back));
	CHECK_STR(back, plain);
	// A tampered blob is refused.
	std::string tampered = sealed;
	tampered[tampered.size() / 2] = static_cast<char>(tampered[tampered.size() / 2] ^ 0x01);
	std::string ignored;
	CHECK(!protector->Unprotect(tampered, ignored));

	// End to end through the store.
	ScratchDir dir("store-dpapi");
	const auto file = dir.path() / "trigglow-auth.dat";
	{
		TokenStore store(file, {}, MakePlatformProtector());
		CHECK(store.Save(Sample("tgp_dpapi-store_7")));
	}
	CHECK(ReadFile(file).find("tgp_dpapi") == std::string::npos);
	TokenStore store(file, {}, MakePlatformProtector());
	CHECK(store.Load() == TokenStore::LoadResult::Loaded);
	CHECK_STR(store.CurrentToken(), "tgp_dpapi-store_7");
#else
	CHECK(!protector->Available());
	std::string out;
	CHECK(!protector->Protect("x", out));
#endif
}

} // namespace

int main()
{
	TestRoundTrip();
	TestUnreadable();
	TestReload();
	TestNoSecureStorage();
	TestLegacy();
	TestPlatformProtector();
	return testcheck::Finish("token_store_tests");
}
