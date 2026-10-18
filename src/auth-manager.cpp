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
#include "win-http.hpp"

#include <thread>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QPointer>
#include <QTimer>
#include <QUrl>

extern "C" {
#include <obs-module.h>
}

// After obs-module.h: its declarations of the logging functions come first.
#include "plugin-support.h"

namespace trigglow {

namespace {
constexpr const char *kComponent = "auth-manager";
constexpr const char *kAuthFile = "trigglow-auth.dat";
constexpr const char *kLegacyFile = "trigglow-account.json";

std::filesystem::path ModuleConfigPath(const char *file)
{
	char *raw = obs_module_get_config_path(obs_current_module(), file);
	if (!raw)
		return std::filesystem::path();
	std::filesystem::path path = std::filesystem::u8path(raw);
	bfree(raw);
	return path;
}

void Wipe(std::string &s)
{
	volatile char *p = s.data();
	for (size_t i = 0; i < s.size(); ++i)
		p[i] = 0;
	s.clear();
}

} // namespace

template<typename R> void AuthManager::RunAsync(std::function<R()> work, std::function<void(const R &)> done)
{
	QPointer<AuthManager> self(this);
	std::thread([work = std::move(work), done = std::move(done), self]() {
		// R's default is the "no answer" outcome: an exception in the work
		// (out of memory) ends up as that instead of terminating OBS.
		R result{};
		try {
			result = work();
		} catch (...) {
		}
		QMetaObject::invokeMethod(
			qApp,
			[done, result = std::move(result), self]() {
				if (!self)
					return; // AuthManager was destroyed before this result arrived.
				done(result);
			},
			Qt::QueuedConnection);
	}).detach();
}

AuthManager::AuthManager(QObject *parent) : QObject(parent), client_(api::Client::Default(PLUGIN_VERSION)) {}

AuthManager::~AuthManager()
{
	Shutdown();
}

void AuthManager::Init()
{
	// obs_current_module() is only valid during obs_module_load(), which is
	// where Init() runs: resolve every path now.
	const std::filesystem::path authFile = ModuleConfigPath(kAuthFile);
	configDir_ = authFile.parent_path();
	store_ = std::make_shared<TokenStore>(authFile, ModuleConfigPath(kLegacyFile), MakePlatformProtector());

	if (!http::Available()) {
		TRIGGLOW_LOG_INFO(kComponent, "sign-in isn't available on this platform yet");
		return;
	}

	switch (store_->Load()) {
	case TokenStore::LoadResult::Loaded:
		session_ = Session::Plugin;
		if (const std::optional<PluginToken> stored = store_->Current())
			displayName_ = stored->displayName;
		CheckSession();
		break;
	case TokenStore::LoadResult::Discarded:
		// Another Windows user's file, a restored profile, a damaged file.
		TRIGGLOW_LOG_INFO(kComponent, "the stored sign-in can't be read on this Windows account; signed out");
		break;
	case TokenStore::LoadResult::Missing:
		if (store_->HasLegacy()) {
			PluginToken legacy = store_->ReadLegacy();
			if (legacy.token.empty()) {
				store_->DeleteLegacy();
			} else {
				session_ = Session::Legacy;
				displayName_ = legacy.displayName;
				MigrateLegacy();
			}
			Wipe(legacy.token);
		}
		break;
	}
}

void AuthManager::Shutdown()
{
	StopPolling();
	// Any call already running in the background delivers to a QPointer, so
	// it becomes a no-op once this object is gone. Clear the callback anyway,
	// so nothing can call back into a dock that may already be destroyed.
	onStatusChanged_ = nullptr;
}

std::string AuthManager::PendingUrl() const
{
	return link_ ? link_->verificationUrl : std::string();
}

std::string AuthManager::PendingUserCode() const
{
	return link_ ? link_->userCode : std::string();
}

void AuthManager::CheckSession()
{
	const api::Client client = client_;
	std::shared_ptr<TokenStore> store = store_;
	RunAsync<auth::MeResult>([client, store]() { return auth::CheckSession(client, *store); },
				 [this](const auth::MeResult &r) { ApplySessionCheck(r); });
}

void AuthManager::ApplySessionCheck(const auth::MeResult &r)
{
	if (session_ != Session::Plugin)
		return; // Signed out meanwhile.
	if (r.events.tokenRenewed)
		TRIGGLOW_LOG_INFO(kComponent, "token renewed");
	switch (r.status) {
	case auth::MeStatus::Valid:
		if (!r.displayName.empty())
			displayName_ = r.displayName;
		trigi_ = r.trigi;
		accountInactive_ = false;
		break;
	case auth::MeStatus::SessionEnded:
		EndSession(LoginError::SessionEnded);
		return;
	case auth::MeStatus::AccountInactive:
		accountInactive_ = true;
		TRIGGLOW_LOG_WARN(kComponent, "the Trigglow account isn't active (HTTP 403 %s)",
				  api::LoggableCode(r.errorCode).c_str());
		break;
	case auth::MeStatus::Unreachable:
		// Offline, or an API without this check: stay signed in, Enable
		// keeps working; the server decides next time it answers.
		TRIGGLOW_LOG_INFO(kComponent, "couldn't check the sign-in now (HTTP %d); staying signed in",
				  r.httpStatus);
		break;
	case auth::MeStatus::Failed:
		TRIGGLOW_LOG_WARN(kComponent, "sign-in check: HTTP %d %s", r.httpStatus,
				  api::LoggableCode(r.errorCode).c_str());
		if (r.events.scopeError)
			TRIGGLOW_LOG_WARN(kComponent, "HTTP 403 insufficient_scope");
		break;
	}
	NotifyChanged();
}

void AuthManager::MigrateLegacy()
{
	struct Outcome {
		auth::ExchangeStatus exchange = auth::ExchangeStatus::RetryLater;
		std::optional<auth::MeResult> me;
	};
	const api::Client client = client_;
	std::shared_ptr<TokenStore> store = store_;
	RunAsync<Outcome>(
		[client, store]() {
			Outcome o;
			o.exchange = auth::ExchangeLegacy(client, *store);
			if (o.exchange == auth::ExchangeStatus::Migrated ||
			    o.exchange == auth::ExchangeStatus::AlreadyMigrated)
				o.me = auth::CheckSession(client, *store);
			return o;
		},
		[this](const Outcome &o) {
			if (session_ != Session::Legacy)
				return; // Signed out meanwhile.
			switch (o.exchange) {
			case auth::ExchangeStatus::Migrated:
			case auth::ExchangeStatus::AlreadyMigrated:
				session_ = Session::Plugin;
				TRIGGLOW_LOG_INFO(kComponent, "migrated legacy login");
				if (o.me)
					ApplySessionCheck(*o.me);
				break;
			case auth::ExchangeStatus::SessionEnded:
			case auth::ExchangeStatus::NoLegacy:
				EndSession(LoginError::SessionEnded);
				return;
			case auth::ExchangeStatus::RetryLater:
				// Keep the old sign-in (Enable works as in 0.5.x); Trigi and
				// signed-in tickets wait for the migration.
				TRIGGLOW_LOG_INFO(kComponent, "legacy login migration postponed to the next start");
				break;
			}
			NotifyChanged();
		});
}

void AuthManager::EndSession(LoginError reason)
{
	if (store_)
		store_->Clear();
	session_ = Session::None;
	accountInactive_ = false;
	displayName_.clear();
	trigi_ = auth::TrigiInfo{};
	lastError_ = reason;
	TRIGGLOW_LOG_INFO(kComponent, "session ended");
	NotifyChanged();
}

void AuthManager::ApplyAuthEvents(const auth::AuthEvents &events)
{
	if (events.tokenRenewed)
		TRIGGLOW_LOG_INFO(kComponent, "token renewed");
	if (events.scopeError)
		TRIGGLOW_LOG_WARN(kComponent, "HTTP 403 insufficient_scope");
	if (events.sessionEnded && session_ != Session::None) {
		EndSession(LoginError::SessionEnded);
		return;
	}
	if (events.accountInactive && !accountInactive_) {
		accountInactive_ = true;
		TRIGGLOW_LOG_WARN(kComponent, "the Trigglow account isn't active (HTTP 403)");
		NotifyChanged();
	}
}

void AuthManager::StartLogin()
{
	if (!http::Available() || !store_ || IsLoggedIn() || IsLoggingIn())
		return;
	const int attempt = ++loginAttempt_;
	startingLogin_ = true;
	lastError_ = LoginError::None;
	NotifyChanged(); // The dock shows "waiting" (with Cancel) right away.

	const api::Client client = client_;
	RunAsync<auth::StartResult>(
		[client]() { return auth::StartLink(client); },
		[this, attempt](const auth::StartResult &r) {
			if (attempt != loginAttempt_)
				return; // Cancelled (or restarted) meanwhile.
			startingLogin_ = false;
			switch (r.status) {
			case auth::StartStatus::Started:
				break;
			case auth::StartStatus::RateLimited:
				lastError_ = LoginError::RateLimited;
				TRIGGLOW_LOG_WARN(kComponent, "sign-in start: rate limited");
				NotifyChanged();
				return;
			case auth::StartStatus::Outdated:
				lastError_ = LoginError::Outdated;
				TRIGGLOW_LOG_WARN(kComponent, "sign-in start: HTTP 426 client_outdated");
				NotifyChanged();
				return;
			case auth::StartStatus::Failed:
				lastError_ = LoginError::Failed;
				TRIGGLOW_LOG_WARN(kComponent, "sign-in start: unexpected answer (HTTP %d %s)",
						  r.httpStatus, api::LoggableCode(r.errorCode).c_str());
				NotifyChanged();
				return;
			case auth::StartStatus::Network:
			case auth::StartStatus::Unavailable:
				lastError_ = LoginError::Network;
				TRIGGLOW_LOG_WARN(kComponent, "sign-in start: no answer (HTTP %d)", r.httpStatus);
				NotifyChanged();
				return;
			}

			link_ = r.link;
			pollIntervalSeconds_ = r.link.intervalSeconds;
			linkDeadline_ =
				std::chrono::steady_clock::now() + std::chrono::seconds(r.link.expiresInSeconds);
			// The link, the codes and the secret never go to the log:
			// OBS logs get uploaded and pasted in public channels.
			const bool opened =
				api::IsAllowedWebUrl(r.link.verificationUrl) &&
				QDesktopServices::openUrl(QUrl(QString::fromStdString(r.link.verificationUrl)));
			TRIGGLOW_LOG_INFO(kComponent, "sign-in page %s in the system browser",
					  opened ? "opened" : "could not be opened");
			lastError_ = opened ? LoginError::None : LoginError::Browser;
			SchedulePoll(pollIntervalSeconds_);
			NotifyChanged();
		});
}

void AuthManager::SchedulePoll(int seconds)
{
	if (!pollTimer_) {
		pollTimer_ = new QTimer(this);
		pollTimer_->setSingleShot(true);
		connect(pollTimer_, &QTimer::timeout, this, [this] { PollOnce(); });
	}
	pollTimer_->start(seconds * 1000);
}

void AuthManager::PollOnce()
{
	if (!link_)
		return;
	if (std::chrono::steady_clock::now() >= linkDeadline_) {
		StopPolling();
		lastError_ = LoginError::Expired;
		TRIGGLOW_LOG_INFO(kComponent, "sign-in link expired -- try Sign in again");
		NotifyChanged();
		return;
	}
	if (pollInFlight_) {
		SchedulePoll(pollIntervalSeconds_);
		return;
	}
	pollInFlight_ = true;
	const int attempt = loginAttempt_;
	const api::Client client = client_;
	std::shared_ptr<TokenStore> store = store_;
	const auth::LinkRequest link = *link_;
	RunAsync<auth::PollResult>(
		[client, store, link]() {
			auth::PollResult r = auth::PollLink(client, link);
			// The token is handed out once: keep it before anything else.
			if (r.status == auth::PollStatus::Approved)
				store->Save(r.token);
			return r;
		},
		[this, attempt](const auth::PollResult &r) {
			pollInFlight_ = false;
			if (r.status == auth::PollStatus::Approved) {
				// Even if Cancel was pressed meanwhile: the user did confirm
				// in the browser, and the token is already stored.
				StopPolling();
				startingLogin_ = false;
				session_ = Session::Plugin;
				accountInactive_ = false;
				lastError_ = LoginError::None;
				TRIGGLOW_LOG_INFO(kComponent, "signed in");
				CheckSession(); // Display name and Trigi availability.
				NotifyChanged();
				return;
			}
			if (attempt != loginAttempt_ || !link_)
				return; // Cancelled, or already resolved.
			switch (r.status) {
			case auth::PollStatus::Pending:
			case auth::PollStatus::Network:
				SchedulePoll(pollIntervalSeconds_);
				return;
			case auth::PollStatus::SlowDown:
				pollIntervalSeconds_ += 5;
				SchedulePoll(pollIntervalSeconds_);
				return;
			case auth::PollStatus::Denied:
				StopPolling();
				lastError_ = LoginError::Denied;
				TRIGGLOW_LOG_INFO(kComponent, "sign-in cancelled from the web page");
				break;
			case auth::PollStatus::Expired:
				StopPolling();
				lastError_ = LoginError::Expired;
				TRIGGLOW_LOG_INFO(kComponent, "sign-in link expired or already used");
				break;
			case auth::PollStatus::Failed:
			case auth::PollStatus::Approved:
				StopPolling();
				lastError_ = LoginError::Failed;
				TRIGGLOW_LOG_WARN(kComponent, "sign-in wait: unexpected answer (HTTP %d %s)",
						  r.httpStatus, api::LoggableCode(r.errorCode).c_str());
				break;
			}
			NotifyChanged();
		});
}

void AuthManager::StopPolling()
{
	if (pollTimer_)
		pollTimer_->stop();
	if (link_) {
		Wipe(link_->pollSecret);
		Wipe(link_->linkCode);
		link_.reset();
	}
}

void AuthManager::CancelLogin()
{
	if (!IsLoggingIn())
		return;
	++loginAttempt_;
	startingLogin_ = false;
	lastError_ = LoginError::None;
	StopPolling();
	TRIGGLOW_LOG_INFO(kComponent, "sign-in cancelled from the dock");
	NotifyChanged();
}

void AuthManager::Logout()
{
	std::string token;
	if (store_) {
		token = store_->CurrentToken();
		if (token.empty() && session_ == Session::Legacy)
			token = store_->ReadLegacy().token;
		// Local data goes first, whatever the server says.
		store_->Clear();
		store_->DeleteLegacy();
	}
	++loginAttempt_;
	StopPolling();
	startingLogin_ = false;
	session_ = Session::None;
	accountInactive_ = false;
	displayName_.clear();
	trigi_ = auth::TrigiInfo{};
	lastError_ = LoginError::None;
	TRIGGLOW_LOG_INFO(kComponent, "signed out");
	if (!token.empty() && http::Available()) {
		// Revoke it on the server (short timeouts); only the status is logged.
		const api::Client client = client_;
		std::thread([client, token]() mutable {
			const api::Result r = auth::Logout(client, token);
			Wipe(token);
			if (!r.Ok() && !r.Is(401))
				TRIGGLOW_LOG_INFO(kComponent, "sign-out: the server didn't confirm (HTTP %d)",
						  r.status);
		}).detach();
	}
	NotifyChanged();
}

void AuthManager::NotifyChanged()
{
	if (onStatusChanged_)
		onStatusChanged_();
}

} // namespace trigglow
