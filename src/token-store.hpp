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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// Where the plugin keeps its Trigglow sign-in: the plugin token (its own,
// limited to the few calls the plugin makes), when it expires, when to renew
// it, and the account's display name. Never the email.
//
// On Windows the file is encrypted with the Windows data protection API for
// the current user (DPAPI), so another Windows user, or a copy of the file on
// another machine, can't read it. On macOS and Linux nothing is stored yet
// (there is no sign-in there until the plugin has an HTTP client).
//
// The file is separate from the plugin's settings and is never read,
// attached or mentioned by "Report a problem".
//
// Thread-safe: AuthManager (UI thread) and the background threads of the
// support calls share one TokenStore. No Qt, no libobs
// (tests/token_store_tests.cpp runs it with a stand-in protector).
namespace trigglow {

struct PluginToken {
	std::string token;
	std::string expiresAt;  // ISO-8601 UTC from the server, may be empty.
	std::string renewAfter; // Same.
	std::string displayName;
};

// Encrypts and decrypts the stored record. MakePlatformProtector() gives the
// DPAPI one on Windows and an unavailable one elsewhere.
class SecretProtector {
public:
	virtual ~SecretProtector() = default;
	virtual bool Available() const = 0;
	virtual bool Protect(const std::string &plain, std::string &sealed) const = 0;
	virtual bool Unprotect(const std::string &sealed, std::string &plain) const = 0;
};
std::unique_ptr<SecretProtector> MakePlatformProtector();

class TokenStore {
public:
	// authFile: the encrypted sign-in file. legacyFile: the plain-text file of
	// v0.5.x and earlier, only ever read once to migrate it, then wiped.
	TokenStore(std::filesystem::path authFile, std::filesystem::path legacyFile,
		   std::unique_ptr<SecretProtector> protector);

	enum class LoadResult {
		Loaded,    // A valid record is now Current().
		Missing,   // No file.
		Discarded, // The file couldn't be decrypted or read (another Windows user, a restored
			   // profile, a damaged file): it was deleted, and there is no sign-in.
	};
	LoadResult Load();

	std::optional<PluginToken> Current() const;
	std::string CurrentToken() const; // Empty when signed out.
	bool HasToken() const;

	// Counts sign-outs: every Clear() adds one. Whatever will save a token it
	// is about to get from the server (a renewal, the exchange of a v0.5.x
	// sign-in, a sign-in wait) reads it first and saves with SaveIfEpoch(), so
	// a sign-out in between always wins: the token it brings back is never
	// stored, and the sign-out can't come undone at the next start of OBS.
	uint64_t Epoch() const;
	// Current() and Epoch() as they were at the same moment.
	std::optional<PluginToken> Snapshot(uint64_t &epoch) const;

	// Replaces the record in memory and on disk (written to a temporary file,
	// then moved over the old one). Returns false if it couldn't be written
	// (the in-memory copy is replaced anyway, so this session keeps working).
	// Only for a token nothing can have signed out of meanwhile; the plugin's
	// own flows use SaveIfEpoch().
	bool Save(const PluginToken &token);

	enum class SaveResult {
		Saved,
		MemoryOnly,         // Kept for this session, but the file couldn't be written.
		SignedOutMeanwhile, // Clear() ran after `epoch` was read: nothing changed.
	};
	// Save(), only if no Clear() ran since `epoch` was read (Epoch(),
	// Snapshot()). Checked and written under one lock, so a sign-out can't
	// slip in between.
	SaveResult SaveIfEpoch(uint64_t epoch, const PluginToken &token);

	// Replaces the dates and the display name, only while the stored token is
	// still `updated.token` (not signed out, not renewed meanwhile). false:
	// nothing changed.
	bool UpdateDetails(const PluginToken &updated);
	// Keeps the token, updates the stored display name.
	void SetDisplayName(const std::string &displayName);

	// Re-reads the file: another part of the plugin, or another OBS on this
	// account, may have renewed the token. Returns the token now current.
	std::string Reload();

	// Forgets the sign-in: memory and file. The legacy file is left alone.
	void Clear();
	// Clear(), only while `token` is still the stored one: a call that failed
	// with an old token never signs out a newer sign-in. true: cleared.
	bool ClearIfToken(const std::string &token);

	// --- Plain-text sign-in of v0.5.x and earlier -------------------------------
	bool HasLegacy() const;
	// The token and display name from the legacy file (empty token if none or
	// unreadable).
	PluginToken ReadLegacy() const;
	// Overwrites the whole legacy file with zeros, flushes them to the disk,
	// then deletes it. Nothing happens when there is none.
	void DeleteLegacy();

	// Held while a token renewal runs, so two threads never renew at once.
	std::mutex &RenewMutex() { return renewMutex_; }

	bool SecureStorageAvailable() const;

private:
	bool WriteLocked(const PluginToken &token);
	void ClearLocked();
	std::optional<PluginToken> ReadFileLocked(bool &unreadable) const;

	mutable std::mutex mutex_;
	std::mutex renewMutex_;
	std::filesystem::path authFile_;
	std::filesystem::path legacyFile_;
	std::unique_ptr<SecretProtector> protector_;
	std::optional<PluginToken> current_;
	uint64_t epoch_ = 0;
};

} // namespace trigglow
