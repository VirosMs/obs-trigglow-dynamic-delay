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

#include <functional>
#include <string>

#include <QObject>

#include "win-http.hpp"

class QTimer;

// AuthManager owns the plugin's link to a free trigglow.com account. It is the
// only thing gating BufferModeController::Enable() (see
// BufferModeController::SetAuthorizationCheck, wired in plugin-main.cpp): the
// delay feature itself stays entirely free, this only makes sure the streamer
// has signed in at least once so they discover Trigglow's other free
// products -- see docs/ACCOUNT_GATE.md for the product reasoning.
//
// The plugin never sees a password. StartLogin() opens the user's system
// browser to a one-time device-link page on trigglow.com -- the SAME login
// the web app itself already uses (email/password or Kick OAuth) -- and polls
// a short-lived endpoint for the resulting session token once the user
// confirms there. This mirrors the CLI/device "browser + short code" login
// pattern (GitHub CLI, `docker login`, etc.), not a native login form.
//
// Qt-based (unlike BufferModeController/ObsFrontendBridge, which are
// deliberately Qt-free): needs QTimer for the login poll loop and
// QDesktopServices::openUrl for the system browser, so there was no Qt-free
// place to put this. HTTPS itself goes through win-http.hpp (WinHTTP, not
// Qt's QNetworkAccessManager) -- see that header's comment for why.
namespace trigglow {

class AuthManager : public QObject {
	Q_OBJECT

public:
	explicit AuthManager(QObject *parent = nullptr);
	~AuthManager() override;

	// Loads any previously saved session token from the plugin's local config
	// file (see PersistToken) and, if present, fires one fire-and-forget
	// GET /auth/me to confirm it still works and roll its rolling TTL
	// forward. This is the ONLY network activity that happens on every OBS
	// start -- one request, not a loop. Call once from obs_module_load().
	void Init();
	// Cancels any in-flight login poll timer. Call from obs_module_unload().
	void Shutdown();

	bool IsLoggedIn() const { return !token_.empty(); }
	// True only while StartLogin() is actively waiting on the browser/poll
	// loop -- the dock uses this to show "waiting for browser..." instead of
	// the plain "sign in" button.
	bool IsLoggingIn() const { return pollTimer_ != nullptr; }
	std::string DisplayName() const { return displayName_; }

	// Opens the system browser to a one-time device-link page and starts
	// polling for the result, capped at a 5-minute timeout (see PollOnce). A
	// timer only exists while this wait is active -- no idle background
	// network use once it's done, one way or another. No-op if a login is
	// already in progress or the user is already logged in (call Logout()
	// first to switch accounts).
	void StartLogin();

	void Logout();

	using StatusChangedCallback = std::function<void()>;
	// Fired whenever IsLoggedIn()/IsLoggingIn()/DisplayName() may have
	// changed -- settings-ui.cpp wires this the same way it already wires
	// BufferModeController's own status-changed callback.
	void SetStatusChangedCallback(StatusChangedCallback callback) { onStatusChanged_ = std::move(callback); }

private:
	void PersistToken() const;
	void ClearPersistedToken() const;
	void LoadPersistedToken();
	void RequestDeviceCode();
	void PollOnce();
	void StopPolling();
	void ValidateStoredToken();
	void NotifyChanged();

	// Runs `request` (a blocking win-http.hpp call) on a short-lived background
	// thread, then delivers its result back on this object's own thread (the
	// OBS/Qt UI thread) via a queued call to `onDone` -- guarded by a QPointer
	// so a result arriving after this AuthManager has been destroyed is
	// silently dropped instead of touching freed memory. Every network call
	// in this class goes through this, so none of them ever block the UI.
	void RunHttp(std::function<HttpResult()> request, std::function<void(const HttpResult &)> onDone);

	// Exists only between StartLogin() and either success, Shutdown(), or the
	// 5-minute timeout -- see StartLogin()'s comment.
	QTimer *pollTimer_ = nullptr;

	// Resolved once in Init() (obs_current_module() is only valid to call
	// synchronously during obs_module_load() -- see PersistToken()'s
	// comment) and reused by every later PersistToken()/ClearPersistedToken()
	// call, which happen asynchronously from network replies long after
	// obs_module_load() has returned.
	std::string configPath_;

	std::string token_;
	std::string displayName_;
	std::string pendingDeviceCode_;
	int pollAttemptsLeft_ = 0;

	StatusChangedCallback onStatusChanged_;
};

} // namespace trigglow
