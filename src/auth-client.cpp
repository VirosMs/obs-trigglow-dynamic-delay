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

#include "auth-client.hpp"

#include <algorithm>
#include <cmath>

namespace trigglow::auth {

namespace {

bool IsServerOrTransportError(const api::Result &r)
{
	return r.outcome == api::Outcome::Network || r.outcome == api::Outcome::Unavailable ||
	       (r.outcome == api::Outcome::HttpError && r.status >= 500);
}

int ClampedSeconds(const json::Value &body, std::string_view key, int lo, int hi, bool &ok)
{
	const auto n = body.GetNumber(key);
	if (!n || !std::isfinite(*n)) {
		ok = false;
		return lo;
	}
	return static_cast<int>(std::clamp(*n, static_cast<double>(lo), static_cast<double>(hi)));
}

// The token record of an approved poll, a renewal or an exchange.
std::optional<PluginToken> TokenFromBody(const api::Result &r)
{
	if (!r.body)
		return std::nullopt;
	PluginToken t;
	t.token = r.body->GetString("token").value_or("");
	if (!api::IsPluginToken(t.token))
		return std::nullopt;
	t.expiresAt = r.body->GetString("expiresAt").value_or("");
	t.renewAfter = r.body->GetString("renewAfter").value_or("");
	return t;
}

api::Result SignedOutResult()
{
	api::Result r;
	r.outcome = api::Outcome::HttpError;
	r.status = 401;
	r.error = "signed_out";
	return r;
}

// Days since 1970-01-01 of a proleptic Gregorian date.
long long DaysFromCivil(long long y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const long long era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned mp = m > 2 ? m - 3 : m + 9;
	const unsigned doy = (153 * mp + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<long long>(doe) - 719468;
}

} // namespace

StartResult StartLink(const api::Client &client)
{
	StartResult result;
	json::Value body = json::Value::Object();
	body.Set("app", json::Value::String("dynamic-delay"));
	body.Set("client", api::ClientObject(client.PluginVersion()));
	const api::Result r = client.PostJson("/auth/plugin/start", body);
	result.httpStatus = r.status;
	result.errorCode = r.error;

	if (r.outcome == api::Outcome::Unavailable) {
		result.status = StartStatus::Unavailable;
		return result;
	}
	if (r.Is(429)) {
		result.status = StartStatus::RateLimited;
		result.retryAfterSeconds = r.retryAfterSeconds;
		return result;
	}
	if (r.Is(426)) {
		result.status = StartStatus::Outdated;
		result.minVersion = r.BodyString("minVersion").value_or("");
		return result;
	}
	if (IsServerOrTransportError(r)) {
		result.status = StartStatus::Network;
		return result;
	}
	if (!r.Ok() || !r.body || !r.body->IsObject()) {
		result.status = StartStatus::Failed;
		return result;
	}

	LinkRequest link;
	link.linkCode = r.body->GetString("linkCode").value_or("");
	link.userCode = r.body->GetString("userCode").value_or("");
	link.pollSecret = r.body->GetString("pollSecret").value_or("");
	link.verificationUrl = r.body->GetString("verificationUrl").value_or("");
	bool numbersOk = true;
	link.expiresInSeconds = ClampedSeconds(*r.body, "expiresIn", 60, 1800, numbersOk);
	link.intervalSeconds = ClampedSeconds(*r.body, "interval", 2, 30, numbersOk);
	const std::string prefix = api::VerificationUrlPrefix();
	const bool valid = numbersOk && api::IsLinkSecret(link.linkCode) && api::IsLinkSecret(link.pollSecret) &&
			   api::IsUserCode(link.userCode) && link.verificationUrl.rfind(prefix, 0) == 0 &&
			   api::IsAllowedWebUrl(link.verificationUrl);
	if (!valid) {
		result.status = StartStatus::Failed;
		return result;
	}
	result.status = StartStatus::Started;
	result.link = std::move(link);
	return result;
}

PollResult PollLink(const api::Client &client, const LinkRequest &link)
{
	PollResult result;
	json::Value body = json::Value::Object();
	body.Set("linkCode", json::Value::String(link.linkCode));
	body.Set("pollSecret", json::Value::String(link.pollSecret));
	const api::Result r = client.PostJson("/auth/plugin/token", body);
	result.httpStatus = r.status;
	result.errorCode = r.error;

	if (r.Is(404)) {
		result.status = PollStatus::Expired;
		return result;
	}
	if (r.Is(429)) {
		result.status = PollStatus::SlowDown;
		return result;
	}
	if (IsServerOrTransportError(r)) {
		result.status = PollStatus::Network;
		return result;
	}
	if (!r.Ok() || !r.body) {
		result.status = PollStatus::Failed;
		return result;
	}
	const std::string status = r.body->GetString("status").value_or("");
	if (status == "approved") {
		auto token = TokenFromBody(r);
		if (!token) {
			result.status = PollStatus::Failed;
			return result;
		}
		result.status = PollStatus::Approved;
		result.token = std::move(*token);
	} else if (status == "denied") {
		result.status = PollStatus::Denied;
	} else {
		// "pending", or a state this version doesn't know: keep waiting (the
		// wait itself ends at expiresIn).
		result.status = PollStatus::Pending;
	}
	return result;
}

RenewStatus Renew(const api::Client &client, TokenStore &store, AuthEvents &events, std::string_view expected)
{
	std::lock_guard<std::mutex> lock(store.RenewMutex());
	const std::optional<PluginToken> current = store.Current();
	if (!current)
		return RenewStatus::SessionEnded;
	if (!expected.empty() && current->token != expected)
		return RenewStatus::Renewed; // Another thread rotated it while this one waited.

	const api::Result r = client.PostJson("/auth/plugin/token/renew", json::Value::Object(), current->token);
	if (r.Ok()) {
		auto token = TokenFromBody(r);
		if (!token)
			return RenewStatus::Failed;
		token->displayName = current->displayName;
		store.Save(*token); // Saved before anything uses it.
		events.tokenRenewed = true;
		return RenewStatus::Renewed;
	}
	if (r.Is(401)) {
		store.Clear();
		events.sessionEnded = true;
		return RenewStatus::SessionEnded;
	}
	return RenewStatus::Failed;
}

RenewStatus RenewIfDue(const api::Client &client, TokenStore &store, AuthEvents &events, std::time_t now)
{
	const std::optional<PluginToken> current = store.Current();
	if (!current || current->renewAfter.empty())
		return RenewStatus::NotDue;
	const auto renewAfter = ParseIsoUtc(current->renewAfter);
	if (!renewAfter || now < *renewAfter)
		return RenewStatus::NotDue;
	return Renew(client, store, events, current->token);
}

api::Result CallAuthorized(const api::Client &client, TokenStore &store, const BearerCall &call, AuthEvents &events,
			   std::time_t now, bool renewFirst)
{
	if (!store.HasToken())
		return SignedOutResult();
	if (renewFirst && RenewIfDue(client, store, events, now) == RenewStatus::SessionEnded)
		return SignedOutResult();

	const std::string token = store.CurrentToken();
	if (token.empty())
		return SignedOutResult();
	api::Result r = call(token);

	if (r.Is(401, "token_rotated")) {
		// Another part of the plugin renewed meanwhile: use the token it
		// saved, or (same token) renew within the server's grace period.
		// Either way, one retry only.
		const std::string reread = store.Reload();
		if (!reread.empty() && reread != token) {
			r = call(reread);
		} else {
			// Not renewed: either the sign-in is over (events.sessionEnded is
			// set) or the renewal couldn't run now (the sign-in stays and the
			// caller sees the 401).
			if (Renew(client, store, events, token) != RenewStatus::Renewed)
				return r;
			r = call(store.CurrentToken());
		}
		if (r.Is(401, "token_rotated"))
			return r; // Never loop.
	}
	if (r.Is(401)) {
		store.Clear();
		events.sessionEnded = true;
	} else if (r.Is(403, "suspended") || r.Is(403, "account_inactive")) {
		events.accountInactive = true;
	} else if (r.Is(403, "insufficient_scope")) {
		events.scopeError = true;
	}
	return r;
}

bool TrigiInfo::Usable(std::string_view pluginVersion) const
{
	if (!known || !available)
		return false;
	return minVersion.empty() || api::CompareVersions(api::ClientVersion(pluginVersion), minVersion) >= 0;
}

bool TrigiInfo::AcceptsDiagnostics(int version) const
{
	return std::find(diagnosticsVersions.begin(), diagnosticsVersions.end(), version) != diagnosticsVersions.end();
}

MeResult CheckSession(const api::Client &client, TokenStore &store, std::time_t now)
{
	MeResult result;
	// The renewal waits until /me has answered (the start order).
	const api::Result r = CallAuthorized(
		client, store, [&client](const std::string &token) { return client.Get("/auth/plugin/me", token); },
		result.events, now, false);
	result.httpStatus = r.status;
	result.errorCode = r.error;

	if (r.Ok() && r.body && r.body->IsObject()) {
		result.status = MeStatus::Valid;
		result.displayName = r.body->GetString("displayName").value_or("");
		if (const json::Value *trigi = r.body->Find("trigi"); trigi && trigi->IsObject()) {
			result.trigi.known = true;
			result.trigi.available = trigi->GetBool("available").value_or(false);
			result.trigi.minVersion = trigi->GetString("minVersion").value_or("");
			if (const json::Value *versions = trigi->Find("diagnosticsVersions");
			    versions && versions->IsArray()) {
				for (size_t i = 0; i < versions->Size(); ++i) {
					const json::Value &v = versions->At(i);
					if (v.IsNumber())
						result.trigi.diagnosticsVersions.push_back(
							static_cast<int>(v.AsNumber()));
				}
			}
		}
		// The server's dates win over the stored ones.
		if (std::optional<PluginToken> current = store.Current()) {
			PluginToken updated = *current;
			if (const json::Value *session = r.body->Find("session"); session && session->IsObject()) {
				updated.expiresAt = session->GetString("expiresAt").value_or(updated.expiresAt);
				updated.renewAfter = session->GetString("renewAfter").value_or(updated.renewAfter);
			}
			if (!result.displayName.empty())
				updated.displayName = result.displayName;
			if (updated.expiresAt != current->expiresAt || updated.renewAfter != current->renewAfter ||
			    updated.displayName != current->displayName)
				store.Save(updated);
		}
		RenewIfDue(client, store, result.events, now);
		if (result.events.sessionEnded)
			result.status = MeStatus::SessionEnded;
		return result;
	}
	if (result.events.sessionEnded || r.Is(401))
		result.status = MeStatus::SessionEnded;
	else if (result.events.accountInactive)
		result.status = MeStatus::AccountInactive;
	else if (IsServerOrTransportError(r) || r.Is(404))
		result.status = MeStatus::Unreachable;
	else
		result.status = MeStatus::Failed;
	return result;
}

ExchangeStatus ExchangeLegacy(const api::Client &client, TokenStore &store)
{
	if (!store.HasLegacy())
		return ExchangeStatus::NoLegacy;
	const PluginToken legacy = store.ReadLegacy();
	if (legacy.token.empty()) {
		store.DeleteLegacy(); // Unreadable: nothing worth keeping in plain text.
		return ExchangeStatus::NoLegacy;
	}
	json::Value body = json::Value::Object();
	body.Set("client", api::ClientObject(client.PluginVersion()));
	const api::Result r = client.PostJson("/auth/plugin/token/exchange", body, legacy.token);
	if (r.Ok()) {
		auto token = TokenFromBody(r);
		if (!token)
			return ExchangeStatus::RetryLater;
		token->displayName = legacy.displayName;
		store.Save(*token);
		store.DeleteLegacy();
		return ExchangeStatus::Migrated;
	}
	if (r.Is(409, "already_migrated")) {
		store.Save(legacy);
		store.DeleteLegacy();
		return ExchangeStatus::AlreadyMigrated;
	}
	if (r.Is(401)) {
		store.DeleteLegacy();
		return ExchangeStatus::SessionEnded;
	}
	return ExchangeStatus::RetryLater;
}

api::Result Logout(const api::Client &client, const std::string &token)
{
	if (token.empty())
		return SignedOutResult();
	return client.PostJson("/auth/plugin/logout", json::Value::Object(), token, api::Timeout::Short);
}

std::optional<std::time_t> ParseIsoUtc(std::string_view s)
{
	auto num = [&s](size_t pos, size_t len, int &out) {
		if (pos + len > s.size())
			return false;
		out = 0;
		for (size_t i = pos; i < pos + len; ++i) {
			if (s[i] < '0' || s[i] > '9')
				return false;
			out = out * 10 + (s[i] - '0');
		}
		return true;
	};
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	if (!num(0, 4, year) || s.size() < 19 || s[4] != '-' || !num(5, 2, month) || s[7] != '-' || !num(8, 2, day) ||
	    (s[10] != 'T' && s[10] != 't' && s[10] != ' ') || !num(11, 2, hour) || s[13] != ':' ||
	    !num(14, 2, minute) || s[16] != ':' || !num(17, 2, second))
		return std::nullopt;
	if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
		return std::nullopt;
	size_t i = 19;
	if (i < s.size() && s[i] == '.') {
		++i;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9')
			++i;
	}
	const std::string_view zone = s.substr(i);
	if (zone != "Z" && zone != "z" && zone != "+00:00" && zone != "-00:00" && !zone.empty())
		return std::nullopt;
	const long long days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
	const long long t = days * 86400LL + hour * 3600LL + minute * 60LL + second;
	return static_cast<std::time_t>(t);
}

} // namespace trigglow::auth
