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

#include "support-service.hpp"
#include "auth-manager.hpp"
#include "bug-report.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "ui-tasks.hpp"
#include "win-http.hpp"

#include <fstream>
#include <iterator>
#include <optional>

#include <QPointer>

extern "C" {
#include <obs-module.h>
}

// After obs-module.h: its declarations of the logging functions come first.
#include "plugin-support.h"

namespace trigglow {

namespace {

constexpr const char *kTrigi = "trigi";     // Log component of the conversation.
constexpr const char *kSupport = "support"; // Log component of tickets.
constexpr const char *kPrefsFile = "trigglow-support.json";

std::string Utf8(const QString &s)
{
	return std::string(s.toUtf8().constData());
}

// "HTTP 403 client_disabled", "no connection (timeout)": status and code only.
void LogFailure(const char *component, const char *what, const support::CallInfo &info)
{
	if (info.httpStatus > 0)
		TRIGGLOW_LOG_WARN(component, "%s: HTTP %d %s", what, info.httpStatus,
				  api::LoggableCode(info.errorCode).c_str());
	else
		TRIGGLOW_LOG_WARN(component, "%s: %s", what, support::StatusName(info.status));
}

// The redacted log for a ticket: the one already prepared, or read now unless
// the job was cancelled meanwhile (the read itself can't be interrupted, so it
// isn't started then).
std::shared_ptr<const std::string> LogForTicket(std::shared_ptr<const std::string> prepared,
						const tasks::CancelToken &cancel)
{
	if (prepared || cancel.IsCancelled())
		return prepared;
	RedactedObsLog fresh = ReadCurrentObsLogRedacted();
	if (!fresh.found)
		return nullptr;
	return std::make_shared<const std::string>(std::move(fresh.text));
}

} // namespace

template<typename R>
void SupportService::RunAsync(std::function<R(const tasks::CancelToken &)> work, std::function<void(const R &)> done)
{
	QPointer<SupportService> self(this);
	uitasks::RunAsync<R>(tasks_, kSupport, std::move(work), [self, done = std::move(done)](const R &result) {
		if (self)
			done(result);
	});
}

SupportService::SupportService(AuthManager &auth, DiagnosticsProvider diagnostics, QObject *parent)
	: QObject(parent),
	  auth_(auth),
	  diagnostics_(std::move(diagnostics)),
	  tasks_(uitasks::Registry())
{
	if (!auth_.ConfigDir().empty())
		prefsPath_ = (auth_.ConfigDir() / kPrefsFile).u8string();
	LoadPrefs();
}

SupportService::~SupportService()
{
	// The dialog's own calls end now (tasks_, destroyed first): their
	// answers have nobody to go to.
	tasks_.CancelAndWait(tasks::kOwnerLimit);

	// A conversation with questions, no ticket and no "it helped": tell the
	// server it was abandoned (in the background, short timeouts). Owned by
	// no one, so closing the dialog doesn't cancel it; OBS closing does.
	tasks::Registry *registry = uitasks::Registry();
	if (!HasConversation() || ticketCreated_ || resolved_ || !auth_.Store() || !registry)
		return;
	try {
		const api::Client client = auth_.ApiClient();
		std::shared_ptr<TokenStore> store = auth_.Store();
		std::shared_ptr<const auth::Renewer> renewer = auth_.TokenRenewer();
		const std::string sessionId = sessionId_;
		const bool started = registry->Run([client, store, renewer, sessionId](const tasks::TaskContext &ctx) {
			const support::CallInfo r =
				support::SendEvent({client.WithCancel(&ctx.Token()), *store, renewer.get()}, sessionId,
						   support::Event::CloseAbandoned);
			if (!r.Ok() && !ctx.Cancelled())
				TRIGGLOW_LOG_INFO(kTrigi, "close event: HTTP %d", r.httpStatus);
		});
		if (!started && !registry->IsShutDown())
			TRIGGLOW_LOG_WARN(kTrigi, "close event not sent");
	} catch (...) {
		// The server closes the conversation on its own.
		TRIGGLOW_LOG_WARN(kTrigi, "close event not sent");
	}
}

// --- Gate ---------------------------------------------------------------------------

SupportService::TrigiGate SupportService::Gate() const
{
	if (!http::Available())
		return TrigiGate::NoHttpClient;
	if (!auth_.IsLoggedIn())
		return TrigiGate::SignedOut;
	if (!auth_.HasPluginToken())
		return TrigiGate::LegacySignIn;
	if (auth_.AccountInactive())
		return TrigiGate::AccountInactive;
	if (disabled_)
		return TrigiGate::Disabled;
	if (outdated_)
		return TrigiGate::Outdated;
	const auth::TrigiInfo &trigi = auth_.Trigi();
	if (!trigi.known)
		return TrigiGate::Unknown;
	if (!trigi.available)
		return TrigiGate::Disabled;
	if (!trigi.Usable(PLUGIN_VERSION))
		return TrigiGate::Outdated;
	return TrigiGate::Available;
}

QString SupportService::TrigiMinVersion() const
{
	if (!minVersion_.empty())
		return QString::fromStdString(minVersion_);
	return QString::fromStdString(auth_.Trigi().minVersion);
}

// --- Preferences ------------------------------------------------------------------------

void SupportService::LoadPrefs()
{
	if (prefsPath_.empty())
		return;
	std::ifstream file(std::filesystem::u8path(prefsPath_), std::ios::binary);
	if (!file)
		return;
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const auto v = json::Parse(text);
	if (!v || !v->IsObject())
		return;
	prefs_.noticeAccepted = static_cast<int>(v->GetNumber("trigiNoticeAccepted").value_or(0));
	prefs_.includeDiagnostics = v->GetBool("includeDiagnostics").value_or(false);
}

void SupportService::SavePrefs() const
{
	if (prefsPath_.empty())
		return;
	json::Value v = json::Value::Object();
	v.Set("trigiNoticeAccepted", json::Value::Number(prefs_.noticeAccepted));
	v.Set("includeDiagnostics", json::Value::Bool(prefs_.includeDiagnostics));
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(prefsPath_).parent_path(), ec);
	std::ofstream file(std::filesystem::u8path(prefsPath_), std::ios::binary | std::ios::trunc);
	file << json::Serialize(v, true) << '\n';
}

bool SupportService::NoticeAccepted() const
{
	return prefs_.noticeAccepted >= kNoticeVersion;
}

void SupportService::AcceptNotice()
{
	prefs_.noticeAccepted = kNoticeVersion;
	SavePrefs();
}

bool SupportService::IncludeDiagnostics() const
{
	return prefs_.includeDiagnostics && DiagnosticsSupported();
}

void SupportService::SetIncludeDiagnostics(bool include)
{
	prefs_.includeDiagnostics = include;
	SavePrefs();
}

bool SupportService::DiagnosticsSupported() const
{
	return static_cast<bool>(diagnostics_) && auth_.Trigi().AcceptsDiagnostics(diag::kSchemaVersion);
}

QString SupportService::DiagnosticsPreview() const
{
	if (!diagnostics_)
		return QString();
	return QString::fromStdString(diag::SerializeV1(diagnostics_(), true));
}

std::string SupportService::Locale() const
{
	const char *locale = obs_get_locale();
	return api::NormalizeLocale(locale ? locale : "");
}

int SupportService::RetryAfterSecondsLeft() const
{
	const auto left = std::chrono::duration_cast<std::chrono::seconds>(retryAt_ - std::chrono::steady_clock::now());
	return left.count() > 0 ? static_cast<int>(left.count()) + 1 : 0;
}

// --- Request queue -----------------------------------------------------------------------

void SupportService::Enqueue(std::function<void()> start)
{
	queue_.push_back(std::move(start));
	if (!inFlight_)
		Finish();
}

void SupportService::Finish()
{
	inFlight_ = false;
	if (queue_.empty())
		return;
	std::function<void()> start = std::move(queue_.front());
	queue_.erase(queue_.begin());
	inFlight_ = true;
	start();
}

void SupportService::NoteStatus(const support::CallInfo &info, const char *what)
{
	auth_.ApplyAuthEvents(info.authEvents);
	switch (info.status) {
	case support::Status::Ok:
		return;
	case support::Status::RateLimited:
		retryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(info.retryAfterSeconds);
		TRIGGLOW_LOG_INFO(kTrigi, "rate limited");
		return;
	case support::Status::Disabled:
		disabled_ = true;
		break;
	case support::Status::Outdated:
		outdated_ = true;
		minVersion_ = info.minVersion;
		break;
	case support::Status::ConversationGone:
		sessionId_.clear();
		questions_ = 0;
		break;
	default:
		break;
	}
	LogFailure(kTrigi, what, info);
}

// --- Conversation ------------------------------------------------------------------------

SupportService::AskRefusal SupportService::Ask(const QString &question, std::function<void(const AskResult &)> done)
{
	if (Gate() != TrigiGate::Available)
		return AskRefusal::NotAvailable;
	if (busy_)
		return AskRefusal::Busy;
	if (RetryAfterSecondsLeft() > 0)
		return AskRefusal::RateLimited;
	if (!NoticeAccepted())
		return AskRefusal::NoticeNotAccepted;
	const std::string text = support::Trim(Utf8(question));
	const size_t chars = chat::CountChars(text);
	if (chars == 0)
		return AskRefusal::Empty;
	if (chars > support::kMaxQuestionChars)
		return AskRefusal::TooLong;

	busy_ = true;
	// The technical data is only ever sent once, with a new conversation.
	// Snapshot here (UI thread); serialized on the network thread.
	std::optional<diag::Snapshot> snapshot;
	if (sessionId_.empty() && IncludeDiagnostics())
		snapshot = diagnostics_();

	struct Outcome {
		AskResult result;
		std::string sessionId;
		bool sessionTried = false;
		bool sessionOk = false;
		bool sessionAvailable = false;
		bool messageSent = false;
	};
	const api::Client client = auth_.ApiClient();
	std::shared_ptr<TokenStore> store = auth_.Store();
	std::shared_ptr<const auth::Renewer> renewer = auth_.TokenRenewer();
	const std::string sessionId = sessionId_;
	const std::string locale = Locale();
	const int generation = generation_;

	Enqueue([this, client, store, renewer, sessionId, locale, snapshot, text, chars, generation, done]() {
		RunAsync<Outcome>(
			[client, store, renewer, sessionId, locale, snapshot, text](const tasks::CancelToken &cancel) {
				Outcome o;
				const api::Client cancellable = client.WithCancel(&cancel);
				const support::Context ctx{cancellable, *store, renewer.get()};
				std::string sid = sessionId;
				if (sid.empty()) {
					o.sessionTried = true;
					std::optional<json::Value> diagnostics;
					if (snapshot)
						diagnostics = diag::BuildV1(*snapshot);
					const support::SessionResult s =
						support::CreateSession(ctx, locale, diagnostics);
					if (!s.Ok()) {
						static_cast<support::CallInfo &>(o.result.reply) = s;
						return o;
					}
					o.sessionOk = true;
					o.sessionAvailable = s.available;
					sid = s.sessionId;
					o.sessionId = sid;
					o.result.conversationStarted = true;
					if (!s.available) {
						o.result.reply.status = support::Status::Ok;
						o.result.reply.authEvents = s.authEvents;
						o.result.reply.kind = support::ReplyKind::Unavailable;
						o.result.reply.next = support::NextStep::TicketHere;
						return o;
					}
					o.result.reply.authEvents = s.authEvents;
				}
				const auth::AuthEvents earlier = o.result.reply.authEvents;
				o.sessionId = sid;
				o.messageSent = true;
				o.result.reply = support::Ask(ctx, sid, text);
				o.result.reply.authEvents.Merge(earlier);
				return o;
			},
			[this, generation, chars, withDiagnostics = snapshot.has_value(), done](const Outcome &o) {
				busy_ = false;
				if (generation != generation_) {
					Finish();
					return; // A conversation the dialog already forgot.
				}
				if (o.sessionOk) {
					sessionId_ = o.sessionId;
					sessionHasDiagnostics_ = withDiagnostics;
					questions_ = 0;
					TRIGGLOW_LOG_INFO(kTrigi, "session started (available=%s)",
							  o.sessionAvailable ? "true" : "false");
				}
				if (o.messageSent) {
					++questions_;
					TRIGGLOW_LOG_INFO(kTrigi, "message sent (len=%zu, diagnostics=%s)", chars,
							  sessionHasDiagnostics_ ? "on" : "off");
				}
				const support::ReplyResult &reply = o.result.reply;
				if (reply.Ok()) {
					auth_.ApplyAuthEvents(reply.authEvents);
					if (reply.kind == support::ReplyKind::Answer)
						TRIGGLOW_LOG_INFO(kTrigi, "reply (HTTP %d, len=%zu, action=%s)",
								  reply.httpStatus, reply.replyChars, reply.action);
					else if (reply.kind == support::ReplyKind::Refused)
						TRIGGLOW_LOG_INFO(kTrigi, "refused (action=%s)", reply.action);
					else
						TRIGGLOW_LOG_INFO(kTrigi, "unavailable");
				} else {
					NoteStatus(reply, o.messageSent ? "message" : "session");
				}
				Finish();
				done(o.result);
			});
	});
	return AskRefusal::None;
}

bool SupportService::RequestSummary(std::function<void(const support::SummaryResult &)> done)
{
	if (busy_ || sessionId_.empty() || !auth_.HasPluginToken())
		return false;
	busy_ = true;
	const api::Client client = auth_.ApiClient();
	std::shared_ptr<TokenStore> store = auth_.Store();
	std::shared_ptr<const auth::Renewer> renewer = auth_.TokenRenewer();
	const std::string sessionId = sessionId_;
	const int generation = generation_;
	Enqueue([this, client, store, renewer, sessionId, generation, done]() {
		RunAsync<support::SummaryResult>(
			[client, store, renewer, sessionId](const tasks::CancelToken &cancel) {
				return support::Summarize({client.WithCancel(&cancel), *store, renewer.get()},
							  sessionId);
			},
			[this, generation, done](const support::SummaryResult &r) {
				busy_ = false;
				if (generation == generation_)
					NoteStatus(r, "summary");
				Finish();
				done(r);
			});
	});
	return true;
}

void SupportService::QueueEvent(support::Event event)
{
	if (sessionId_.empty() || !auth_.HasPluginToken())
		return;
	const api::Client client = auth_.ApiClient();
	std::shared_ptr<TokenStore> store = auth_.Store();
	std::shared_ptr<const auth::Renewer> renewer = auth_.TokenRenewer();
	const std::string sessionId = sessionId_;
	Enqueue([this, client, store, renewer, sessionId, event]() {
		RunAsync<support::CallInfo>(
			[client, store, renewer, sessionId, event](const tasks::CancelToken &cancel) {
				return support::SendEvent({client.WithCancel(&cancel), *store, renewer.get()},
							  sessionId, event);
			},
			[this](const support::CallInfo &r) {
				// Errors are only worth their HTTP code in the log.
				auth_.ApplyAuthEvents(r.authEvents);
				if (!r.Ok())
					LogFailure(kTrigi, "event", r);
				Finish();
			});
	});
}

void SupportService::SendFeedback(bool helpful)
{
	QueueEvent(helpful ? support::Event::HelpfulYes : support::Event::HelpfulNo);
}

void SupportService::MarkResolved()
{
	resolved_ = true;
	QueueEvent(support::Event::CloseResolved);
}

void SupportService::NotifyHandoff()
{
	QueueEvent(support::Event::HandoffClicked);
}

void SupportService::ForgetConversation()
{
	++generation_;
	sessionId_.clear();
	sessionHasDiagnostics_ = false;
	questions_ = 0;
	busy_ = false;
}

// --- Tickets -----------------------------------------------------------------------------

QString SupportService::TicketFooter() const
{
	const char *obs = obs_get_version_string();
	return QString::fromStdString(support::TicketFooter(PLUGIN_VERSION, obs ? obs : ""));
}

bool SupportService::CanSubmitSignedIn() const
{
	return http::Available() && auth_.HasPluginToken();
}

void SupportService::SubmitTicket(const QString &subject, const QString &message, bool linkConversation, bool attachLog,
				  std::function<void(const TicketOutcome &)> done)
{
	struct Outcome {
		TicketOutcome ticket;
		auth::AuthEvents events;
		std::string ticketId;
	};
	const api::Client client = auth_.ApiClient();
	std::shared_ptr<TokenStore> store = auth_.Store();
	std::shared_ptr<const auth::Renewer> renewer = auth_.TokenRenewer();
	const std::string s = Utf8(subject);
	const std::string m = Utf8(message);
	const std::string botSessionId = linkConversation ? sessionId_ : std::string();
	std::shared_ptr<const std::string> prepared = preparedLog_;

	RunAsync<Outcome>(
		[client, store, renewer, s, m, botSessionId, attachLog, prepared](const tasks::CancelToken &cancel) {
			Outcome o;
			const api::Client cancellable = client.WithCancel(&cancel);
			const support::Context ctx{cancellable, *store, renewer.get()};
			const support::TicketResult t = support::CreateTicket(ctx, s, m, botSessionId);
			o.events = t.authEvents;
			o.ticket.status = t.status;
			o.ticket.retryAfterSeconds = t.retryAfterSeconds;
			if (!t.Ok())
				return o;
			o.ticket.created = true;
			o.ticket.ticketUrl = QUrl(QString::fromStdString(api::AppSupportTicketUrl(t.ticketId)));
			if (attachLog) {
				const std::shared_ptr<const std::string> log = LogForTicket(prepared, cancel);
				if (log && !log->empty()) {
					o.ticket.attachmentTried = true;
					const support::CallInfo a = support::AttachLog(ctx, t.ticketId, *log);
					o.events.Merge(a.authEvents);
					o.ticket.attachmentSent = a.Ok();
					o.ticket.attachmentStatus = a.status;
				} else {
					o.ticket.logNotFound = true;
				}
			}
			return o;
		},
		[this, done](const Outcome &o) {
			auth_.ApplyAuthEvents(o.events);
			if (o.ticket.created) {
				ticketCreated_ = true;
				TRIGGLOW_LOG_INFO(kSupport, "ticket created (attachment=%s)",
						  o.ticket.attachmentSent ? "yes" : "no");
				if (o.ticket.attachmentTried && !o.ticket.attachmentSent)
					TRIGGLOW_LOG_WARN(kSupport, "log attachment failed: %s",
							  support::StatusName(o.ticket.attachmentStatus));
			} else {
				support::CallInfo info;
				info.status = o.ticket.status;
				LogFailure(kSupport, "ticket", info);
			}
			done(o.ticket);
		});
}

void SupportService::CheckGuestForm(std::function<void(GuestForm)> done)
{
	const api::Client client = auth_.ApiClient();
	RunAsync<support::PublicConfigResult>(
		[client](const tasks::CancelToken &cancel) {
			return support::FetchPublicConfig(client.WithCancel(&cancel));
		},
		[done](const support::PublicConfigResult &r) {
			if (!r.Ok())
				done(GuestForm::Unknown);
			else
				done(r.challengeRequired ? GuestForm::WebOnly : GuestForm::Available);
		});
}

void SupportService::SubmitGuestTicket(const QString &name, const QString &email, const QString &subject,
				       const QString &message, bool attachLog,
				       std::function<void(const TicketOutcome &)> done)
{
	const api::Client client = auth_.ApiClient();
	const std::string n = Utf8(name);
	const std::string e = Utf8(email);
	const std::string s = Utf8(subject);
	const std::string m = Utf8(message);
	std::shared_ptr<const std::string> prepared = preparedLog_;

	RunAsync<TicketOutcome>(
		[client, n, e, s, m, attachLog, prepared](const tasks::CancelToken &cancel) {
			TicketOutcome o;
			const api::Client cancellable = client.WithCancel(&cancel);
			const support::PublicTicketResult t = support::CreatePublicTicket(cancellable, n, e, s, m);
			o.status = t.status;
			o.retryAfterSeconds = t.retryAfterSeconds;
			o.needsWebForm = t.status == support::Status::CaptchaRequired;
			if (!t.Ok())
				return o;
			o.created = true;
			o.ticketUrl =
				QUrl(QString::fromStdString(api::PublicSupportTicketUrl(t.ticketId, t.accessToken)));
			if (attachLog) {
				const std::shared_ptr<const std::string> log = LogForTicket(prepared, cancel);
				if (log && !log->empty()) {
					o.attachmentTried = true;
					const support::CallInfo a =
						support::AttachLogPublic(cancellable, t.ticketId, t.accessToken, *log);
					o.attachmentSent = a.Ok();
					o.attachmentStatus = a.status;
				} else {
					o.logNotFound = true;
				}
			}
			return o;
		},
		[this, done](const TicketOutcome &o) {
			if (o.created) {
				ticketCreated_ = true;
				TRIGGLOW_LOG_INFO(kSupport, "ticket created (attachment=%s)",
						  o.attachmentSent ? "yes" : "no");
			} else {
				support::CallInfo info;
				info.status = o.status;
				LogFailure(kSupport, "guest ticket", info);
			}
			done(o);
		});
}

// --- The log -----------------------------------------------------------------------------

void SupportService::PrepareLog(std::function<void(const PreparedLog &)> done)
{
	RunAsync<PreparedLog>(
		[](const tasks::CancelToken &cancel) {
			PreparedLog p;
			if (cancel.IsCancelled())
				return p; // The dialog is closing: nobody waits for it.
			RedactedObsLog log = ReadCurrentObsLogRedacted();
			p.found = log.found;
			p.capped = log.capped;
			if (log.found) {
				p.preview = logredact::MakePreview(log.text);
				p.text = std::make_shared<const std::string>(std::move(log.text));
			}
			return p;
		},
		[this, done](const PreparedLog &p) {
			if (p.found)
				preparedLog_ = p.text;
			done(p);
		});
}

} // namespace trigglow
