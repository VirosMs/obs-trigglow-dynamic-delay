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

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <QObject>

#include "api-client.hpp"
#include "auth-client.hpp"
#include "background-tasks.hpp"
#include "token-store.hpp"

class QTimer;

// AuthManager owns the plugin's link to a free trigglow.com account. It is the
// only thing gating BufferModeController::Enable() (see
// BufferModeController::SetAuthorizationCheck, wired in plugin-main.cpp): the
// delay feature itself stays entirely free, this only makes sure the streamer
// has signed in at least once so they discover Trigglow's other free
// products -- see docs/ACCOUNT_GATE.md for the product reasoning.
//
// The plugin never sees a password. StartLogin() opens the user's system
// browser on a one-time confirmation page on trigglow.com and shows a short
// code in the dock that the page must show too (device link: the "browser +
// short code" pattern of GitHub CLI and the like). Once the user confirms, the
// plugin collects its own token: limited to the few calls the plugin makes,
// renewed automatically, revoked by "Sign out", and stored encrypted with
// Windows' data protection (token-store.hpp). The protocol itself is Qt-free
// (auth-client.hpp); this class schedules it, keeps the dock's state, and
// writes the few allowed log lines (never a token, code, secret or link).
//
// Network on every OBS start: one GET /auth/plugin/me (plus a renewal when the
// server says it's due, and once ever the exchange of a v0.5.x sign-in). No
// loop; polling only exists while a sign-in is waiting for the browser.
//
// macOS/Linux: no HTTP client yet (win-http.hpp), so no sign-in there.
namespace trigglow {

class AuthManager : public QObject {
	Q_OBJECT

public:
	// Why the last sign-in attempt (or the stored session) failed, so the dock
	// can say so instead of silently going back to "Sign in". Cleared by
	// StartLogin(), CancelLogin(), Logout() and a successful sign-in.
	enum class LoginError {
		None,
		Network,      // trigglow.com unreachable while starting the sign-in.
		Failed,       // The sign-in couldn't start (unexpected answer): "Couldn't sign in".
		Expired,      // The link expired or was already used, or the wait ran out.
		Denied,       // "It wasn't me" was pressed on the web page.
		Browser,      // The system browser didn't open: the dock offers the link to copy (still waiting).
		RateLimited,  // Too many attempts in a row: try again in a moment.
		Outdated,     // This plugin version can't sign in any more: update it.
		SessionEnded, // The server ended the stored session (HTTP 401).
	};

	explicit AuthManager(QObject *parent = nullptr);
	~AuthManager() override;

	// Loads the stored sign-in (or migrates a v0.5.x one) and checks it once
	// with the server, in the background. Call once from obs_module_load(),
	// after uitasks::Start() (it resolves the module's config folder, only
	// possible during load).
	void Init();
	// Stops a sign-in wait and aborts this object's calls in flight. Call
	// from obs_module_unload().
	void Shutdown();

	// Signed in: a plugin token, or a v0.5.x sign-in still waiting for its
	// one-time migration (Enable works with either, as in 0.5.x).
	bool IsLoggedIn() const { return session_ != Session::None; }
	// What Trigi and signed-in tickets need: a plugin token (not a legacy
	// sign-in). Only ever true where there is an HTTP client.
	bool HasPluginToken() const { return session_ == Session::Plugin; }
	// The server said the account is suspended or not active (HTTP 403):
	// Enable is blocked and the dock says why; the sign-in itself stays.
	bool AccountInactive() const { return accountInactive_; }
	// The gate BufferModeController::Enable() uses.
	bool CanEnable() const { return IsLoggedIn() && !accountInactive_; }

	// True from StartLogin() until the sign-in ends one way or another (so a
	// second click can't start a second one).
	bool IsLoggingIn() const { return startingLogin_ || link_.has_value(); }
	LoginError LastError() const { return lastError_; }
	// The confirmation page, while waiting (empty otherwise), for "Copy link".
	// Never log it.
	std::string PendingUrl() const;
	// The code the page must show ("KD7F-29QM"), while waiting.
	std::string PendingUserCode() const;
	std::string DisplayName() const { return displayName_; }
	// What the server said about Trigi at the last start check (unknown until
	// then).
	const auth::TrigiInfo &Trigi() const { return trigi_; }

	// Opens the confirmation page and waits for it (at most the link's own
	// lifetime, polling at the server's interval). No-op while signed in or
	// already waiting.
	void StartLogin();
	// Stops waiting (the dock's "Cancel"): the link's secrets are forgotten,
	// nothing is sent to the server.
	void CancelLogin();
	// Signs out: the local sign-in is deleted at once, whatever happens, and
	// the token is revoked on the server in the background.
	void Logout();

	// --- For the support service (support-service.hpp) -----------------------
	// The API client and the token store that background support calls share
	// with this class.
	const api::Client &ApiClient() const { return client_; }
	std::shared_ptr<TokenStore> Store() const { return store_; }
	// Who renews the token for those calls (auth::Renewer): this class, on a
	// job that only OBS closing cancels, so closing a dialog mid-renewal never
	// loses the new token. Null before Init().
	std::shared_ptr<const auth::Renewer> TokenRenewer() const { return renewer_; }
	// The module's config folder (empty before Init()).
	const std::filesystem::path &ConfigDir() const { return configDir_; }
	// What a support call did to the sign-in (session ended, account not
	// active, token renewed). UI thread.
	void ApplyAuthEvents(const auth::AuthEvents &events);

	using StatusChangedCallback = std::function<void()>;
	// Fired whenever anything above may have changed.
	void SetStatusChangedCallback(StatusChangedCallback callback) { onStatusChanged_ = std::move(callback); }

private:
	enum class Session { None, Plugin, Legacy };

	void CheckSession();
	void MigrateLegacy();
	void DeleteLeftoverLegacy();
	void ApplySessionCheck(const auth::MeResult &result);
	void EndSession(LoginError reason);
	void SchedulePoll(int seconds);
	void PollOnce();
	void StopPolling();
	void NotifyChanged();

	// Runs `work` (blocking network calls, which must use the token) on a
	// background thread of tasks_ and delivers its result to `done` on the UI
	// thread: never after this object is gone or the plugin shut down
	// (ui-tasks.hpp), with a QPointer check as a second guard.
	template<typename R>
	void RunAsync(std::function<R(const tasks::CancelToken &)> work, std::function<void(const R &)> done);

	api::Client client_;
	std::shared_ptr<TokenStore> store_;
	std::shared_ptr<auth::BackgroundRenewer> renewer_;
	std::filesystem::path configDir_;

	Session session_ = Session::None;
	bool accountInactive_ = false;
	std::string displayName_;
	auth::TrigiInfo trigi_;

	// The sign-in being waited for. Its secrets live only here, in memory.
	std::optional<auth::LinkRequest> link_;
	std::chrono::steady_clock::time_point linkDeadline_{};
	int pollIntervalSeconds_ = 3;
	bool pollInFlight_ = false;
	QTimer *pollTimer_ = nullptr;
	bool startingLogin_ = false;
	// Bumped by StartLogin()/CancelLogin()/Logout(): replies tagged with an
	// older value belong to an abandoned attempt.
	int loginAttempt_ = 0;
	LoginError lastError_ = LoginError::None;

	StatusChangedCallback onStatusChanged_;

	// This object's background calls. Last member, so it is destroyed first:
	// the calls are aborted before anything they report to goes away.
	tasks::Group tasks_;
};

} // namespace trigglow
