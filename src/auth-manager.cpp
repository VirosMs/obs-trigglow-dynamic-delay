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

#include "auth-manager.hpp"
#include "logging.hpp"

#include <thread>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QUrl>

extern "C" {
#include <obs-module.h>
}

namespace trigglow {

namespace {
constexpr const char *kComponent = "auth-manager";
constexpr const char *kAccountFile = "trigglow-account.json";
// Same origin the web app itself runs on (see docs/architecture/DEVELOP_ENVIRONMENT.md in the
// trigglow monorepo) -- /auth/* is served from here directly, no separate API host.
constexpr const wchar_t *kApiHost = L"trigglow.virosms.com";
// 3s * 100 = 5 minutes, matching the plan's cap on how long the poll timer is allowed to run.
constexpr int kPollIntervalMs = 3000;
constexpr int kMaxPollAttempts = 100;

// Only ever called on genuinely ASCII input in this file (the device code is base64url, the
// stored session token is base64url too -- see session-token.ts on the web side), so a real
// UTF-8 decode is unnecessary; kept platform-neutral (no <windows.h>) since this file, unlike
// win-http.cpp, has to compile on every OS win-http.hpp is declared for.
std::wstring Utf8ToWide(const std::string &s)
{
	return std::wstring(s.begin(), s.end());
}

} // namespace

AuthManager::AuthManager(QObject *parent) : QObject(parent) {}

AuthManager::~AuthManager()
{
	Shutdown();
}

void AuthManager::Init()
{
	// obs_current_module() is only valid to call synchronously during
	// obs_module_load() (libobs resets it to null the instant that call
	// returns) -- Init() runs directly inside that call chain (see
	// plugin-main.cpp), but PersistToken()/ClearPersistedToken() below get
	// called again later from background-thread results, well after
	// obs_module_load() has returned. Resolve and cache the path here, once,
	// while it's still safe to ask.
	char *rawPath = obs_module_get_config_path(obs_current_module(), kAccountFile);
	if (rawPath) {
		configPath_ = rawPath;
		bfree(rawPath);
	}

	LoadPersistedToken();
	if (!token_.empty())
		ValidateStoredToken();
}

void AuthManager::Shutdown()
{
	StopPolling();
	// Any RunHttp() call already in flight is running detached on its own
	// background thread; its result delivery is guarded by a QPointer to
	// this object (see RunHttp()), so it becomes a safe no-op once this
	// object is destroyed. Clear the callback now regardless, defensively,
	// so nothing can call back into a TrigglowDelayDock that may already be
	// gone by the time that happens.
	onStatusChanged_ = nullptr;
}

void AuthManager::StartLogin()
{
	if (IsLoggedIn() || IsLoggingIn())
		return;
	RequestDeviceCode();
}

void AuthManager::Logout()
{
	token_.clear();
	displayName_.clear();
	ClearPersistedToken();
	StopPolling();
	TRIGGLOW_LOG_INFO(kComponent, "logged out");
	NotifyChanged();
}

void AuthManager::RunHttp(std::function<HttpResult()> request, std::function<void(const HttpResult &)> onDone)
{
	QPointer<AuthManager> self(this);
	std::thread([request = std::move(request), onDone = std::move(onDone), self]() {
		HttpResult result = request();
		QMetaObject::invokeMethod(
			qApp,
			[onDone, result, self]() {
				if (!self)
					return; // AuthManager was destroyed before this result arrived.
				onDone(result);
			},
			Qt::QueuedConnection);
	}).detach();
}

void AuthManager::RequestDeviceCode()
{
	RunHttp([]() { return HttpsPostJson(kApiHost, L"/auth/plugin/start", "{}"); },
		[this](const HttpResult &result) {
			if (!result.ok) {
				TRIGGLOW_LOG_WARN(kComponent, "failed to start login: %s", result.error.c_str());
				return;
			}
			if (result.statusCode < 200 || result.statusCode >= 300) {
				TRIGGLOW_LOG_WARN(kComponent, "login start returned HTTP %d", result.statusCode);
				return;
			}

			obs_data_t *data = obs_data_create_from_json(result.body.c_str());
			if (!data) {
				TRIGGLOW_LOG_WARN(kComponent, "login start returned invalid JSON");
				return;
			}
			const char *deviceCode = obs_data_get_string(data, "deviceCode");
			const char *verificationUrl = obs_data_get_string(data, "verificationUrl");
			if (!deviceCode || !*deviceCode || !verificationUrl || !*verificationUrl) {
				TRIGGLOW_LOG_WARN(kComponent,
						  "login start response missing deviceCode/verificationUrl");
				obs_data_release(data);
				return;
			}

			pendingDeviceCode_ = deviceCode;
			TRIGGLOW_LOG_INFO(kComponent, "opening browser at %s", verificationUrl);
			bool opened = QDesktopServices::openUrl(QUrl(QString::fromUtf8(verificationUrl)));
			TRIGGLOW_LOG_INFO(kComponent, "QDesktopServices::openUrl returned %s",
					  opened ? "true" : "false");
			obs_data_release(data);

			pollAttemptsLeft_ = kMaxPollAttempts;
			pollTimer_ = new QTimer(this);
			connect(pollTimer_, &QTimer::timeout, this, [this] { PollOnce(); });
			pollTimer_->start(kPollIntervalMs);
			TRIGGLOW_LOG_INFO(kComponent, "login started, waiting on browser confirmation");
			NotifyChanged();
		});
}

void AuthManager::PollOnce()
{
	if (pendingDeviceCode_.empty()) {
		StopPolling();
		return;
	}
	if (--pollAttemptsLeft_ <= 0) {
		TRIGGLOW_LOG_INFO(kComponent, "login wait timed out (5 min) -- try Sign in again");
		StopPolling();
		NotifyChanged();
		return;
	}

	std::wstring path = L"/auth/plugin/poll?code=" + Utf8ToWide(pendingDeviceCode_);
	RunHttp([path]() { return HttpsGet(kApiHost, path); },
		[this](const HttpResult &result) {
			if (!result.ok)
				return; // Transient network hiccup -- just wait for the next tick.

			obs_data_t *data = obs_data_create_from_json(result.body.c_str());
			if (!data)
				return;

			const char *status = obs_data_get_string(data, "status");
			if (status && std::string(status) == "approved") {
				const char *tokenStr = obs_data_get_string(data, "token");
				if (tokenStr && *tokenStr) {
					token_ = tokenStr;
					StopPolling();
					PersistToken();
					TRIGGLOW_LOG_INFO(kComponent, "login completed");
					ValidateStoredToken(); // Fetches displayName_, persists it once known.
					NotifyChanged();
				}
			} else if (status && std::string(status) == "not_found") {
				// Code expired (10 min) or was already consumed -- stop waiting,
				// the dock's Sign in button lets the user start over.
				TRIGGLOW_LOG_WARN(kComponent, "login link code expired or already used");
				StopPolling();
				NotifyChanged();
			}
			// "pending": nothing to do, next timer tick tries again.
			obs_data_release(data);
		});
}

void AuthManager::StopPolling()
{
	if (pollTimer_) {
		pollTimer_->stop();
		pollTimer_->deleteLater();
		pollTimer_ = nullptr;
	}
	pendingDeviceCode_.clear();
}

void AuthManager::ValidateStoredToken()
{
	if (token_.empty())
		return;

	std::wstring bearer = Utf8ToWide(token_);
	RunHttp([bearer]() { return HttpsGet(kApiHost, L"/auth/me", bearer); },
		[this](const HttpResult &result) {
			if (!result.ok || result.statusCode < 200 || result.statusCode >= 300) {
				// Session expired/revoked server-side, or a transport failure --
				// only actually sign out on a real HTTP response saying so (401),
				// not on e.g. a offline/DNS hiccup that would otherwise wipe a
				// perfectly good stored login.
				if (result.ok && result.statusCode == 401 && !token_.empty()) {
					TRIGGLOW_LOG_INFO(kComponent,
							  "stored session no longer valid, signing out locally");
					token_.clear();
					displayName_.clear();
					ClearPersistedToken();
					NotifyChanged();
				}
				return;
			}

			obs_data_t *data = obs_data_create_from_json(result.body.c_str());
			if (!data)
				return;
			const char *name = obs_data_get_string(data, "displayName");
			if (name && *name && displayName_ != name) {
				displayName_ = name;
				PersistToken();
				NotifyChanged();
			}
			obs_data_release(data);
		});
}

void AuthManager::PersistToken() const
{
	if (configPath_.empty())
		return;
	QString path = QString::fromStdString(configPath_);

	QFileInfo info(path);
	QDir().mkpath(info.absolutePath());

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "token", token_.c_str());
	obs_data_set_string(data, "display_name", displayName_.c_str());
	obs_data_save_json(data, path.toUtf8().constData());
	obs_data_release(data);
}

void AuthManager::ClearPersistedToken() const
{
	if (configPath_.empty())
		return;
	QFile::remove(QString::fromStdString(configPath_));
}

void AuthManager::LoadPersistedToken()
{
	if (configPath_.empty())
		return;

	obs_data_t *data = obs_data_create_from_json_file(configPath_.c_str());
	if (!data)
		return;
	const char *tokenStr = obs_data_get_string(data, "token");
	const char *nameStr = obs_data_get_string(data, "display_name");
	if (tokenStr)
		token_ = tokenStr;
	if (nameStr)
		displayName_ = nameStr;
	obs_data_release(data);
}

void AuthManager::NotifyChanged()
{
	if (onStatusChanged_)
		onStatusChanged_();
}

} // namespace trigglow
