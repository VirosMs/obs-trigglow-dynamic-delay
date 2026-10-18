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

#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api-client.hpp"
#include "token-store.hpp"

// The account calls of the plugin, without Qt: linking OBS to a Trigglow
// account (device link v2), the plugin token's lifecycle, and the rules every
// authenticated call follows. Every function here is BLOCKING (run them on a
// background thread) and logs nothing: the caller (AuthManager, the support
// service) writes the few allowed log lines.
//
// Linking, in short:
//   1. StartLink(): POST /auth/plugin/start -> linkCode (only part of the
//      page's URL), userCode (shown in OBS and on the page), pollSecret (never
//      leaves the plugin, never logged) and the page to open.
//   2. The user confirms on trigglow.com that the page shows the same code.
//   3. PollLink() every `interval` seconds until `expiresIn`: POST
//      /auth/plugin/token {linkCode, pollSecret}. The token is handed out once.
// Then the plugin token lives in TokenStore (encrypted), is checked at start
// (GET /auth/plugin/me), renewed by rotation when the server says so
// (POST /auth/plugin/token/renew) and revoked on sign-out
// (POST /auth/plugin/logout). A plain-text sign-in from v0.5.x or earlier is
// exchanged once, silently, for a plugin token (POST /auth/plugin/token/exchange).
namespace trigglow::auth {

// --- Linking -----------------------------------------------------------------

struct LinkRequest {
	std::string linkCode;
	std::string userCode; // "KD7F-29QM": what the dock shows.
	std::string pollSecret;
	std::string verificationUrl; // The page to open (and to copy).
	int expiresInSeconds = 600;  // Clamped to 60-1800.
	int intervalSeconds = 3;     // Clamped to 2-30.
};

enum class StartStatus : unsigned char {
	Started,
	Failed,      // Unexpected or invalid answer (bad codes, foreign page...): "couldn't sign in".
	RateLimited, // 429: try again after retryAfterSeconds.
	Outdated,    // 426 client_outdated: this plugin version can't sign in any more.
	Network,     // Transport error or 5xx.
	Unavailable, // No HTTP client on this platform.
};

struct StartResult {
	StartStatus status = StartStatus::Failed;
	LinkRequest link; // Valid when Started.
	int httpStatus = 0;
	std::string errorCode;
	int retryAfterSeconds = 0;
	std::string minVersion; // With Outdated, when the server says it.
};

// Validates everything the contract asks for; anything off is Failed (never a
// fallback to an older sign-in flow).
StartResult StartLink(const api::Client &client);

enum class PollStatus : unsigned char {
	Pending,  // Not confirmed yet: poll again after the interval.
	Approved, // `token` holds the new plugin token (handed out only this once).
	Denied,   // "It wasn't me" on the web page.
	Expired,  // 404: expired, already collected or unknown.
	SlowDown, // 429: add 5 s to the interval.
	Network,  // Keep polling until the link expires.
	Failed,   // Any other answer: stop.
};

struct PollResult {
	PollStatus status = PollStatus::Network;
	PluginToken token;
	int httpStatus = 0;
	std::string errorCode;
};

PollResult PollLink(const api::Client &client, const LinkRequest &link);

// --- Authenticated calls -------------------------------------------------------

// What an authenticated call did to the sign-in, for AuthManager to show.
struct AuthEvents {
	bool sessionEnded = false;    // 401: the stored sign-in was cleared.
	bool accountInactive = false; // 403 suspended / account_inactive.
	bool tokenRenewed = false;    // The token was rotated (and saved) along the way.
	bool scopeError = false;      // 403 insufficient_scope: a plugin bug, the sign-in stays.

	void Merge(const AuthEvents &other)
	{
		sessionEnded = sessionEnded || other.sessionEnded;
		accountInactive = accountInactive || other.accountInactive;
		tokenRenewed = tokenRenewed || other.tokenRenewed;
		scopeError = scopeError || other.scopeError;
	}
};

using BearerCall = std::function<api::Result(const std::string &token)>;

// Runs `call` with the stored token, following the rules for every Bearer call:
//   - renews first when the token's renewAfter has passed;
//   - 401 token_rotated: re-reads the store; a different token is retried once,
//     the same one is renewed (the server's grace period) and retried once;
//   - any other 401: the sign-in is over (store cleared, events.sessionEnded);
//   - 403 suspended / account_inactive: events.accountInactive;
//   - 403 insufficient_scope: events.scopeError (the sign-in stays).
// Without a stored token, returns an HttpError 401 "signed_out" without any
// network call. renewFirst = false skips the up-front renewal (the start check
// renews after GET /auth/plugin/me instead).
api::Result CallAuthorized(const api::Client &client, TokenStore &store, const BearerCall &call, AuthEvents &events,
			   std::time_t now = std::time(nullptr), bool renewFirst = true);

enum class RenewStatus : unsigned char {
	Renewed,      // New token saved.
	NotDue,       // RenewIfDue(): nothing to do.
	SessionEnded, // 401 (other than token_rotated): store cleared.
	Failed,       // Network, 5xx or an unexpected answer: keep the current token.
};

// POST /auth/plugin/token/renew with the current token. The new token is saved
// before it is used. Serialized with the store's renew mutex: with
// expectedToken set, a caller that finds the stored token already different
// (another thread renewed meanwhile) returns Renewed without a request.
RenewStatus Renew(const api::Client &client, TokenStore &store, AuthEvents &events,
		  std::string_view expectedToken = {});
RenewStatus RenewIfDue(const api::Client &client, TokenStore &store, AuthEvents &events,
		       std::time_t now = std::time(nullptr));

// --- Start of OBS ------------------------------------------------------------

struct TrigiInfo {
	bool known = false; // The server said something about Trigi.
	bool available = false;
	std::string minVersion;
	std::vector<int> diagnosticsVersions;

	// Trigi can be offered to this plugin version.
	bool Usable(std::string_view pluginVersion) const;
	bool AcceptsDiagnostics(int version) const;
};

enum class MeStatus : unsigned char {
	Valid,
	SessionEnded,    // 401: cleared.
	AccountInactive, // 403 suspended / account_inactive: keep the token, block Enable.
	Unreachable,     // Network, 5xx, 404 (an API without this contract): stay signed in.
	Failed,          // Anything else (no change).
};

struct MeResult {
	MeStatus status = MeStatus::Unreachable;
	std::string displayName;
	TrigiInfo trigi;
	AuthEvents events;
	int httpStatus = 0;
	std::string errorCode;
};

// GET /auth/plugin/me through CallAuthorized(), then a renewal when due.
MeResult CheckSession(const api::Client &client, TokenStore &store, std::time_t now = std::time(nullptr));

enum class ExchangeStatus : unsigned char {
	Migrated,        // New plugin token saved, legacy file wiped.
	AlreadyMigrated, // 409: the old token was already a plugin token; saved as is, legacy file wiped.
	SessionEnded,    // 401: legacy file wiped, the user signs in again.
	RetryLater,      // Network, 5xx, 404, 429: legacy file kept, try again next start.
	NoLegacy,        // Nothing to migrate.
};

// One-time migration of the plain-text sign-in of v0.5.x and earlier.
ExchangeStatus ExchangeLegacy(const api::Client &client, TokenStore &store);

// POST /auth/plugin/logout with `token` (revokes it on the server). Short
// timeouts; the result only matters for the log. The caller clears the store
// whatever happens.
api::Result Logout(const api::Client &client, const std::string &token);

// ISO-8601 UTC ("2026-10-18T12:00:00Z", fractions and "+00:00" accepted) to
// time_t; nullopt if unreadable.
std::optional<std::time_t> ParseIsoUtc(std::string_view iso);

} // namespace trigglow::auth
