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

#include "token-store.hpp"
#include "json.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace trigglow {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxFileBytes = 64 * 1024;

void Wipe(std::string &s)
{
	volatile char *p = s.data();
	for (size_t i = 0; i < s.size(); ++i)
		p[i] = 0;
	s.clear();
}

bool IsTokenText(const std::string &s)
{
	if (s.empty() || s.size() > 256)
		return false;
	for (char c : s) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == '_' || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

bool ReadAll(const fs::path &path, std::string &out)
{
	std::error_code ec;
	const auto size = fs::file_size(path, ec);
	if (ec || size > kMaxFileBytes)
		return false;
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return false;
	out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	return !file.bad();
}

// Temporary file, then a rename over the old one: a crash midway leaves the
// old record or the new one, never half of one.
bool WriteAtomic(const fs::path &path, const std::string &data)
{
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	fs::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
		if (!file)
			return false;
		file.write(data.data(), static_cast<std::streamsize>(data.size()));
		file.flush();
		if (!file)
			return false;
	}
#ifdef _WIN32
	if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		fs::remove(tmp, ec);
		return false;
	}
#else
	fs::rename(tmp, path, ec);
	if (ec) {
		fs::remove(tmp, ec);
		return false;
	}
#endif
	return true;
}

// Writes zeros over every byte of `path`, in place, and flushes them to the
// disk (FlushFileBuffers on Windows: an ofstream flush only reaches the
// system's cache).
void OverwriteWithZeros(const fs::path &path)
{
	std::error_code ec;
	const std::uintmax_t size = fs::file_size(path, ec);
	if (ec || size == 0)
		return;
	const std::string zeros(64 * 1024, '\0');
#ifdef _WIN32
	HANDLE file =
		CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	std::uintmax_t left = size;
	while (left > 0) {
		const DWORD chunk = static_cast<DWORD>(left < zeros.size() ? left : zeros.size());
		DWORD written = 0;
		if (!WriteFile(file, zeros.data(), chunk, &written, nullptr) || written == 0)
			break;
		left -= written;
	}
	FlushFileBuffers(file);
	CloseHandle(file);
#else
	std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
	if (!file)
		return;
	std::uintmax_t left = size;
	while (left > 0 && file) {
		const size_t chunk = static_cast<size_t>(left < zeros.size() ? left : zeros.size());
		file.write(zeros.data(), static_cast<std::streamsize>(chunk));
		left -= chunk;
	}
	file.flush();
#endif
}

std::string Encode(const PluginToken &t)
{
	json::Value v = json::Value::Object();
	v.Set("v", json::Value::Number(1));
	v.Set("token", json::Value::String(t.token));
	v.Set("expiresAt", json::Value::String(t.expiresAt));
	v.Set("renewAfter", json::Value::String(t.renewAfter));
	v.Set("displayName", json::Value::String(t.displayName));
	return json::Serialize(v);
}

std::optional<PluginToken> Decode(const std::string &text)
{
	const auto v = json::Parse(text);
	if (!v || !v->IsObject() || v->GetNumber("v").value_or(0) != 1)
		return std::nullopt;
	PluginToken t;
	t.token = v->GetString("token").value_or("");
	if (!IsTokenText(t.token))
		return std::nullopt;
	t.expiresAt = v->GetString("expiresAt").value_or("");
	t.renewAfter = v->GetString("renewAfter").value_or("");
	t.displayName = v->GetString("displayName").value_or("");
	return t;
}

#ifdef _WIN32

// Not a secret: it only keeps this plugin's blobs apart from any other DPAPI
// data of the same Windows user.
constexpr char kEntropy[] = "trigglow-dynamic-delay/plugin-token/v1";

class DpapiProtector final : public SecretProtector {
public:
	bool Available() const override { return true; }

	bool Protect(const std::string &plain, std::string &sealed) const override
	{
		DATA_BLOB in{static_cast<DWORD>(plain.size()),
			     reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()))};
		DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1),
				  reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy))};
		DATA_BLOB out{};
		if (!CryptProtectData(&in, L"Trigglow Dynamic Delay", &entropy, nullptr, nullptr,
				      CRYPTPROTECT_UI_FORBIDDEN, &out))
			return false;
		sealed.assign(reinterpret_cast<const char *>(out.pbData), out.cbData);
		LocalFree(out.pbData);
		return true;
	}

	bool Unprotect(const std::string &sealed, std::string &plain) const override
	{
		DATA_BLOB in{static_cast<DWORD>(sealed.size()),
			     reinterpret_cast<BYTE *>(const_cast<char *>(sealed.data()))};
		DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1),
				  reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy))};
		DATA_BLOB out{};
		if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
			return false;
		plain.assign(reinterpret_cast<const char *>(out.pbData), out.cbData);
		SecureZeroMemory(out.pbData, out.cbData);
		LocalFree(out.pbData);
		return true;
	}
};

#endif

class UnavailableProtector final : public SecretProtector {
public:
	bool Available() const override { return false; }
	bool Protect(const std::string &, std::string &) const override { return false; }
	bool Unprotect(const std::string &, std::string &) const override { return false; }
};

} // namespace

std::unique_ptr<SecretProtector> MakePlatformProtector()
{
#ifdef _WIN32
	return std::make_unique<DpapiProtector>();
#else
	return std::make_unique<UnavailableProtector>();
#endif
}

TokenStore::TokenStore(fs::path authFile, fs::path legacyFile, std::unique_ptr<SecretProtector> protector)
	: authFile_(std::move(authFile)),
	  legacyFile_(std::move(legacyFile)),
	  protector_(protector ? std::move(protector) : std::make_unique<UnavailableProtector>())
{
}

bool TokenStore::SecureStorageAvailable() const
{
	return protector_->Available();
}

std::optional<PluginToken> TokenStore::ReadFileLocked(bool &unreadable) const
{
	unreadable = false;
	std::error_code ec;
	if (authFile_.empty() || !fs::exists(authFile_, ec))
		return std::nullopt;
	std::string sealed;
	std::string plain;
	std::optional<PluginToken> token;
	if (ReadAll(authFile_, sealed) && protector_->Unprotect(sealed, plain))
		token = Decode(plain);
	Wipe(plain);
	if (!token)
		unreadable = true;
	return token;
}

TokenStore::LoadResult TokenStore::Load()
{
	std::lock_guard<std::mutex> lock(mutex_);
	bool unreadable = false;
	std::optional<PluginToken> token = ReadFileLocked(unreadable);
	if (token) {
		current_ = std::move(token);
		return LoadResult::Loaded;
	}
	if (unreadable) {
		std::error_code ec;
		fs::remove(authFile_, ec);
		current_.reset();
		return LoadResult::Discarded;
	}
	return LoadResult::Missing;
}

std::optional<PluginToken> TokenStore::Current() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return current_;
}

std::string TokenStore::CurrentToken() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return current_ ? current_->token : std::string();
}

bool TokenStore::HasToken() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return current_.has_value();
}

uint64_t TokenStore::Epoch() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return epoch_;
}

std::optional<PluginToken> TokenStore::Snapshot(uint64_t &epoch) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	epoch = epoch_;
	return current_;
}

bool TokenStore::WriteLocked(const PluginToken &token)
{
	if (authFile_.empty() || !protector_->Available())
		return false;
	std::string plain = Encode(token);
	std::string sealed;
	const bool ok = protector_->Protect(plain, sealed) && WriteAtomic(authFile_, sealed);
	Wipe(plain);
	return ok;
}

bool TokenStore::Save(const PluginToken &token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	current_ = token;
	return WriteLocked(token);
}

TokenStore::SaveResult TokenStore::SaveIfEpoch(uint64_t epoch, const PluginToken &token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (epoch != epoch_)
		return SaveResult::SignedOutMeanwhile;
	current_ = token;
	return WriteLocked(token) ? SaveResult::Saved : SaveResult::MemoryOnly;
}

bool TokenStore::UpdateDetails(const PluginToken &updated)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!current_ || current_->token != updated.token)
		return false;
	current_ = updated;
	WriteLocked(updated);
	return true;
}

void TokenStore::SetDisplayName(const std::string &displayName)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!current_ || current_->displayName == displayName)
		return;
	current_->displayName = displayName;
	WriteLocked(*current_);
}

std::string TokenStore::Reload()
{
	std::lock_guard<std::mutex> lock(mutex_);
	bool unreadable = false;
	std::optional<PluginToken> token = ReadFileLocked(unreadable);
	if (token)
		current_ = std::move(token);
	return current_ ? current_->token : std::string();
}

void TokenStore::ClearLocked()
{
	++epoch_;
	current_.reset();
	std::error_code ec;
	if (!authFile_.empty()) {
		fs::remove(authFile_, ec);
		fs::path tmp = authFile_;
		tmp += ".tmp";
		fs::remove(tmp, ec);
	}
}

void TokenStore::Clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	ClearLocked();
}

bool TokenStore::ClearIfToken(const std::string &token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!current_ || current_->token != token)
		return false;
	ClearLocked();
	return true;
}

bool TokenStore::HasLegacy() const
{
	std::error_code ec;
	return !legacyFile_.empty() && fs::exists(legacyFile_, ec);
}

PluginToken TokenStore::ReadLegacy() const
{
	PluginToken legacy;
	std::string text;
	if (legacyFile_.empty() || !ReadAll(legacyFile_, text))
		return legacy;
	if (const auto v = json::Parse(text); v && v->IsObject()) {
		const std::string token = v->GetString("token").value_or("");
		if (IsTokenText(token)) {
			legacy.token = token;
			legacy.displayName = v->GetString("display_name").value_or("");
		}
	}
	Wipe(text);
	return legacy;
}

void TokenStore::DeleteLegacy()
{
	if (legacyFile_.empty())
		return;
	std::error_code ec;
	if (!fs::exists(legacyFile_, ec))
		return;
	// Zeros over the whole old plain text, flushed to the disk, before the
	// file goes, so the token doesn't linger in the freed disk blocks.
	OverwriteWithZeros(legacyFile_);
	fs::remove(legacyFile_, ec);
}

} // namespace trigglow
