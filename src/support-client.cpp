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

#include "support-client.hpp"

namespace trigglow::support {

namespace {

bool EndsWith(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void Fill(CallInfo &info, const api::Result &r, const auth::AuthEvents &events)
{
	info.status = StatusFromResult(r, events);
	info.httpStatus = r.status;
	info.errorCode = r.error;
	info.authEvents = events;
	if (info.status == Status::RateLimited)
		info.retryAfterSeconds = r.retryAfterSeconds > 0 ? r.retryAfterSeconds : 60;
	if (info.status == Status::Outdated)
		info.minVersion = r.BodyString("minVersion").value_or("");
}

void Reject(CallInfo &info, Status status)
{
	info.status = status;
}

std::string SessionPath(std::string_view sessionId, std::string_view tail)
{
	return "/api/support/bot/sessions/" + std::string(sessionId) + std::string(tail);
}

api::Result Authorized(const Context &ctx, auth::AuthEvents &events, const auth::BearerCall &call)
{
	return auth::CallAuthorized(ctx.client, ctx.store, call, events, std::time(nullptr), true, ctx.renewer);
}

bool IsAttachmentError(std::string_view code)
{
	return code == "file_too_large" || code == "invalid_type" || code == "invalid_mime" ||
	       code == "attachment_limit" || code == "missing_file";
}

} // namespace

const char *StatusName(Status status)
{
	switch (status) {
	case Status::Ok:
		return "ok";
	case Status::SignedOut:
		return "signed out";
	case Status::SessionEnded:
		return "session ended";
	case Status::AccountInactive:
		return "account inactive";
	case Status::Disabled:
		return "disabled";
	case Status::Outdated:
		return "outdated";
	case Status::RateLimited:
		return "rate limited";
	case Status::ConversationGone:
		return "conversation gone";
	case Status::Invalid:
		return "invalid";
	case Status::ScopeError:
		return "scope error";
	case Status::Network:
		return "no connection";
	case Status::Timeout:
		return "timeout";
	case Status::Unavailable:
		return "unavailable on this platform";
	case Status::BadResponse:
		return "unexpected response";
	case Status::CaptchaRequired:
		return "human check required";
	case Status::AttachmentRejected:
		return "attachment rejected";
	case Status::TicketClosed:
		return "ticket closed";
	case Status::NotFound:
		return "not found";
	}
	return "?";
}

Status StatusFromResult(const api::Result &r, const auth::AuthEvents &events)
{
	if (r.outcome == api::Outcome::Unavailable)
		return Status::Unavailable;
	if (r.outcome == api::Outcome::Network) {
		if (r.transport == http::Error::Invalid)
			return Status::Invalid;
		return r.TimedOut() ? Status::Timeout : Status::Network;
	}
	if (r.Ok())
		return Status::Ok;
	if (r.Is(401, "signed_out"))
		return Status::SignedOut;
	if (events.sessionEnded || r.status == 401)
		return Status::SessionEnded;
	switch (r.status) {
	case 400:
		if (r.error == "captcha_failed")
			return Status::CaptchaRequired;
		if (IsAttachmentError(r.error))
			return Status::AttachmentRejected;
		return Status::Invalid;
	case 403:
		if (r.error == "client_disabled")
			return Status::Disabled;
		if (r.error == "suspended" || r.error == "account_inactive")
			return Status::AccountInactive;
		if (r.error == "insufficient_scope")
			return Status::ScopeError;
		return Status::ScopeError;
	case 404:
		return Status::NotFound;
	case 409:
		if (r.error == "session_closed")
			return Status::ConversationGone;
		if (r.error == "ticket_closed")
			return Status::TicketClosed;
		return Status::Invalid;
	case 426:
		return Status::Outdated;
	case 429:
		return Status::RateLimited;
	default:
		break;
	}
	if (r.status >= 500)
		return Status::Network;
	// 3xx (never followed) and anything else.
	return Status::BadResponse;
}

std::string Trim(std::string_view text)
{
	auto space = [](char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r';
	};
	while (!text.empty() && space(text.front()))
		text.remove_prefix(1);
	while (!text.empty() && space(text.back()))
		text.remove_suffix(1);
	return std::string(text);
}

SessionResult CreateSession(const Context &ctx, std::string_view locale, const std::optional<json::Value> &diagnostics,
			    int noticeVersion)
{
	SessionResult result;
	if (!ctx.store.HasToken()) {
		Reject(result, Status::SignedOut);
		return result;
	}
	// Exactly these keys: the server refuses any other.
	json::Value body = json::Value::Object();
	body.Set("locale", json::Value::String(api::NormalizeLocale(locale)));
	body.Set("client", api::ClientObject(ctx.client.PluginVersion()));
	body.Set("noticeVersion", json::Value::Number(noticeVersion));
	if (diagnostics && diagnostics->IsObject())
		body.Set("diagnostics", *diagnostics);

	auth::AuthEvents events;
	const api::Result r = Authorized(ctx, events, [&](const std::string &token) {
		return ctx.client.PostJson("/api/support/bot/sessions", body, token);
	});
	Fill(result, r, events);
	if (!result.Ok())
		return result;
	const std::string id = r.BodyString("sessionId").value_or("");
	const std::optional<bool> available = r.body ? r.body->GetBool("available") : std::nullopt;
	if (!api::IsUuid(id) || !available) {
		result.status = Status::BadResponse;
		return result;
	}
	result.sessionId = id;
	result.available = *available;
	return result;
}

bool ParseReplyBody(const json::Value &body, ReplyResult &out)
{
	if (!body.IsObject())
		return false;
	const json::Value *reply = body.Find("reply");
	const json::Value *action = body.Find("action");
	out.next = NextStep::None;
	out.lines.clear();
	out.replyChars = 0;

	std::string actionType;
	std::string category;
	if (action && action->IsObject()) {
		actionType = action->GetString("type").value_or("");
		category = action->GetString("category").value_or("");
	}
	out.action = actionType == "ticket" ? "ticket" : actionType == "human" ? "human" : "none";

	if (reply && reply->IsString()) {
		out.kind = ReplyKind::Answer;
		const std::string sanitized = chat::Sanitize(reply->AsString());
		out.replyChars = chat::CountChars(sanitized);
		out.lines = chat::Parse(sanitized);
		if (actionType == "human" || (actionType == "ticket" && category == "dynamic_delay"))
			out.next = NextStep::TicketHere;
		else if (actionType == "ticket")
			out.next = NextStep::WebSupport;
		return true;
	}
	if (body.Has("refused")) {
		out.kind = ReplyKind::Refused;
		if (actionType == "human")
			out.next = NextStep::WriteToTeam;
		return true;
	}
	// The third shape ("unavailable"). Anything else is unexpected.
	if (body.Has("unavailable")) {
		out.kind = ReplyKind::Unavailable;
		out.next = NextStep::TicketHere;
		return true;
	}
	return false;
}

ReplyResult Ask(const Context &ctx, std::string_view sessionId, std::string_view question)
{
	ReplyResult result;
	const std::string text = Trim(question);
	const size_t chars = chat::CountChars(text);
	if (chars == 0 || chars > kMaxQuestionChars || !api::IsUuid(sessionId)) {
		Reject(result, Status::Invalid);
		return result;
	}
	if (!ctx.store.HasToken()) {
		Reject(result, Status::SignedOut);
		return result;
	}
	json::Value body = json::Value::Object();
	body.Set("message", json::Value::String(text));
	const std::string path = SessionPath(sessionId, "/messages");
	auth::AuthEvents events;
	const api::Result r = Authorized(ctx, events, [&](const std::string &token) {
		return ctx.client.PostJson(path, body, token, api::Timeout::Conversation);
	});
	Fill(result, r, events);
	if (result.status == Status::NotFound)
		result.status = Status::ConversationGone;
	if (!result.Ok())
		return result;
	if (!r.body || !ParseReplyBody(*r.body, result))
		result.status = Status::BadResponse;
	return result;
}

SummaryResult Summarize(const Context &ctx, std::string_view sessionId)
{
	SummaryResult result;
	if (!api::IsUuid(sessionId)) {
		Reject(result, Status::Invalid);
		return result;
	}
	if (!ctx.store.HasToken()) {
		Reject(result, Status::SignedOut);
		return result;
	}
	json::Value body = json::Value::Object();
	body.Set("category", json::Value::String("dynamic_delay"));
	const std::string path = SessionPath(sessionId, "/summary");
	auth::AuthEvents events;
	const api::Result r = Authorized(ctx, events, [&](const std::string &token) {
		return ctx.client.PostJson(path, body, token, api::Timeout::Conversation);
	});
	Fill(result, r, events);
	if (result.status == Status::NotFound)
		result.status = Status::ConversationGone;
	if (!result.Ok())
		return result;
	const auto subject = r.BodyString("subject");
	const auto message = r.BodyString("message");
	if (!subject || !message) {
		result.status = Status::BadResponse;
		return result;
	}
	// Plain text for editable fields: no control characters, emojis or bidi
	// tricks either.
	result.subject = chat::Sanitize(*subject);
	result.message = chat::Sanitize(*message);
	result.ai = r.body && r.body->GetBool("ai").value_or(false);
	return result;
}

json::Value EventBody(Event event)
{
	json::Value body = json::Value::Object();
	json::Value meta = json::Value::Object();
	switch (event) {
	case Event::HelpfulYes:
	case Event::HelpfulNo:
		body.Set("kind", json::Value::String("feedback"));
		meta.Set("helpful", json::Value::Bool(event == Event::HelpfulYes));
		body.Set("meta", std::move(meta));
		break;
	case Event::CloseResolved:
	case Event::CloseAbandoned:
		body.Set("kind", json::Value::String("close"));
		meta.Set("status", json::Value::String(event == Event::CloseResolved ? "resolved" : "abandoned"));
		body.Set("meta", std::move(meta));
		break;
	case Event::HandoffClicked:
		body.Set("kind", json::Value::String("handoff_clicked"));
		break;
	}
	return body;
}

CallInfo SendEvent(const Context &ctx, std::string_view sessionId, Event event)
{
	CallInfo result;
	if (!api::IsUuid(sessionId)) {
		Reject(result, Status::Invalid);
		return result;
	}
	if (!ctx.store.HasToken()) {
		Reject(result, Status::SignedOut);
		return result;
	}
	const json::Value body = EventBody(event);
	const std::string path = SessionPath(sessionId, "/events");
	const api::Timeout timeout = event == Event::CloseAbandoned ? api::Timeout::Short : api::Timeout::Standard;
	auth::AuthEvents events;
	const api::Result r = Authorized(ctx, events, [&](const std::string &token) {
		return ctx.client.PostJson(path, body, token, timeout);
	});
	Fill(result, r, events);
	return result;
}

std::string TicketFooter(std::string_view pluginVersion, std::string_view obsVersion)
{
	const char *os = "Windows";
	if (api::CurrentOs() == api::Os::Macos)
		os = "macOS";
	else if (api::CurrentOs() == api::Os::Linux)
		os = "Linux";
	return "\n\n--\nDynamic Delay " + api::ClientVersion(pluginVersion) + " \xC2\xB7 OBS " +
	       std::string(obsVersion) + " \xC2\xB7 " + os;
}

TicketResult CreateTicket(const Context &ctx, std::string_view subject, std::string_view message,
			  std::string_view botSessionId)
{
	TicketResult result;
	const std::string s = Trim(subject);
	const std::string m = Trim(message);
	const size_t subjectChars = chat::CountChars(s);
	const size_t messageChars = chat::CountChars(m);
	if (subjectChars < kSubjectMinChars || subjectChars > kSubjectMaxChars || messageChars == 0 ||
	    messageChars > kMessageMaxChars || (!botSessionId.empty() && !api::IsUuid(botSessionId))) {
		Reject(result, Status::Invalid);
		return result;
	}
	if (!ctx.store.HasToken()) {
		Reject(result, Status::SignedOut);
		return result;
	}
	json::Value body = json::Value::Object();
	body.Set("subject", json::Value::String(s));
	body.Set("category", json::Value::String("dynamic_delay"));
	body.Set("message", json::Value::String(m));
	if (!botSessionId.empty())
		body.Set("botSessionId", json::Value::String(std::string(botSessionId)));
	auth::AuthEvents events;
	const api::Result r = Authorized(ctx, events, [&](const std::string &token) {
		return ctx.client.PostJson("/api/support/tickets", body, token);
	});
	Fill(result, r, events);
	if (!result.Ok())
		return result;
	// Only "id" is read.
	const std::string id = r.BodyString("id").value_or("");
	if (!api::IsUuid(id)) {
		result.status = Status::BadResponse;
		return result;
	}
	result.ticketId = id;
	return result;
}

CallInfo AttachLog(const Context &ctx, std::string_view ticketId, const std::string &redactedLog)
{
	CallInfo result;
	if (!api::IsUuid(ticketId) || redactedLog.empty()) {
		Reject(result, Status::Invalid);
		return result;
	}
	if (!ctx.store.HasToken()) {
		Reject(result, Status::SignedOut);
		return result;
	}
	const std::string path = "/api/support/tickets/" + std::string(ticketId) + "/attachments";
	auth::AuthEvents events;
	const api::Result r = Authorized(ctx, events, [&](const std::string &token) {
		return ctx.client.PostFile(path, kLogFileName, "text/plain", redactedLog, token);
	});
	Fill(result, r, events);
	return result;
}

PublicConfigResult FetchPublicConfig(const api::Client &client)
{
	PublicConfigResult result;
	const api::Result r = client.Get("/api/support/public/config");
	Fill(result, r, {});
	if (!result.Ok())
		return result;
	if (!r.body || !r.body->IsObject()) {
		result.status = Status::BadResponse;
		return result;
	}
	// The config names the site key of the human check the public form uses,
	// or null when it uses none. Any non-empty "...SiteKey" means the form
	// can't be filled in from OBS.
	const auto &keys = r.body->Keys();
	const auto &values = r.body->Values();
	for (size_t i = 0; i < keys.size(); ++i) {
		if (EndsWith(keys[i], "SiteKey") && values[i].IsString() && !values[i].AsString().empty())
			result.challengeRequired = true;
	}
	return result;
}

PublicTicketResult CreatePublicTicket(const api::Client &client, std::string_view guestName,
				      std::string_view guestEmail, std::string_view subject, std::string_view message)
{
	PublicTicketResult result;
	const std::string name = Trim(guestName);
	const std::string email = Trim(guestEmail);
	const std::string s = Trim(subject);
	const std::string m = Trim(message);
	const size_t nameChars = chat::CountChars(name);
	const size_t subjectChars = chat::CountChars(s);
	const size_t messageChars = chat::CountChars(m);
	if (nameChars < kGuestNameMinChars || nameChars > kGuestNameMaxChars || email.empty() ||
	    email.find('@') == std::string::npos || subjectChars < kSubjectMinChars ||
	    subjectChars > kSubjectMaxChars || messageChars < kGuestMessageMinChars ||
	    messageChars > kMessageMaxChars) {
		Reject(result, Status::Invalid);
		return result;
	}
	json::Value body = json::Value::Object();
	body.Set("guestName", json::Value::String(name));
	body.Set("guestEmail", json::Value::String(email));
	body.Set("subject", json::Value::String(s));
	body.Set("category", json::Value::String("dynamic_delay"));
	body.Set("message", json::Value::String(m));
	const api::Result r = client.PostJson("/api/support/public/tickets", body);
	Fill(result, r, {});
	if (!result.Ok())
		return result;
	const std::string id = r.BodyString("ticketId").value_or("");
	const std::string access = r.BodyString("accessToken").value_or("");
	if (!api::IsUuid(id) || !api::IsPluginToken(access)) {
		result.status = Status::BadResponse;
		return result;
	}
	result.ticketId = id;
	result.accessToken = access;
	return result;
}

CallInfo AttachLogPublic(const api::Client &client, std::string_view ticketId, std::string_view accessToken,
			 const std::string &redactedLog)
{
	CallInfo result;
	if (!api::IsUuid(ticketId) || !api::IsPluginToken(accessToken) || redactedLog.empty()) {
		Reject(result, Status::Invalid);
		return result;
	}
	// The access token goes in a header, never in the URL.
	const std::string path = "/api/support/public/tickets/" + std::string(ticketId) + "/attachments";
	const api::Result r = client.PostFile(path, kLogFileName, "text/plain", redactedLog, {},
					      {{"X-Ticket-Token", std::string(accessToken)}});
	Fill(result, r, {});
	return result;
}

} // namespace trigglow::support
