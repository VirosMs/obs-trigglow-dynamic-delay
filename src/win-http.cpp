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

#include "win-http.hpp"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <vector>

namespace trigglow::http {

namespace {

class Handle {
public:
	explicit Handle(HINTERNET h = nullptr) : h_(h) {}
	~Handle()
	{
		if (h_)
			WinHttpCloseHandle(h_);
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	HINTERNET get() const { return h_; }
	void reset(HINTERNET h)
	{
		if (h_)
			WinHttpCloseHandle(h_);
		h_ = h;
	}

private:
	HINTERNET h_;
};

std::wstring Widen(const std::string &utf8)
{
	if (utf8.empty())
		return std::wstring();
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (len <= 0)
		return std::wstring();
	std::wstring out(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
	return out;
}

bool HasLineBreakOrNul(const std::string &s)
{
	for (char c : s) {
		if (c == '\r' || c == '\n' || c == '\0')
			return true;
	}
	return false;
}

bool IsValidHeaderName(const std::string &name)
{
	if (name.empty())
		return false;
	for (char c : name) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

bool IsValidTarget(const std::string &s)
{
	if (s.empty())
		return false;
	for (char c : s) {
		if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F)
			return false;
	}
	return true;
}

Error MapError(DWORD code)
{
	switch (code) {
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
		return Error::Resolve;
	case ERROR_WINHTTP_CANNOT_CONNECT:
	case ERROR_WINHTTP_CONNECTION_ERROR:
		return Error::Connect;
	case ERROR_WINHTTP_TIMEOUT:
		return Error::Timeout;
	case ERROR_WINHTTP_OPERATION_CANCELLED:
		return Error::Cancelled;
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
	case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
	case ERROR_WINHTTP_SECURE_INVALID_CA:
	case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
	case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
	case ERROR_WINHTTP_SECURE_INVALID_CERT:
	case ERROR_WINHTTP_SECURE_CERT_REVOKED:
	case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
	case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
		return Error::Tls;
	default:
		return Error::Other;
	}
}

Response Fail(Error error, DWORD code = 0)
{
	Response r;
	r.error = error;
	r.systemError = code;
	return r;
}

Response FailLast()
{
	const DWORD code = GetLastError();
	return Fail(MapError(code), code);
}

void SetDwordOption(HINTERNET handle, DWORD option, DWORD value)
{
	WinHttpSetOption(handle, option, &value, sizeof(value));
}

// --- Asynchronous WinHTTP, waited on step by step ----------------------------------
//
// WinHTTP's synchronous calls can't be interrupted: a request waiting for an
// answer stays blocked until the server or a timeout ends it (up to 90 s for
// Trigi). In asynchronous mode each step completes through StatusCallback(),
// and Send() waits for that completion OR the cancel event, whichever comes
// first. To abort, it closes the request handle and waits for
// HANDLE_CLOSING, WinHTTP's last callback for that handle, so nothing of the
// call is touched once Send() returns.

constexpr DWORD kChunkBytes = 64 * 1024;

struct AsyncCall {
	AsyncCall()
		: step(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
		  wake(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
		  closed(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
		  chunk(kChunkBytes)
	{
	}
	~AsyncCall()
	{
		for (HANDLE h : {step, wake, closed}) {
			if (h)
				CloseHandle(h);
		}
	}
	AsyncCall(const AsyncCall &) = delete;
	AsyncCall &operator=(const AsyncCall &) = delete;
	bool Valid() const { return step && wake && closed; }

	HANDLE step;   // Auto-reset: the step started last completed or failed.
	HANDLE wake;   // Manual-reset: the call was cancelled.
	HANDLE closed; // Manual-reset: WinHTTP is done with the request handle.
	// Written by the callback before it sets `step`, read after that wait.
	DWORD status = 0;
	DWORD error = 0;
	DWORD bytes = 0;
	// The TCP connection to the server was made: a timeout after this means
	// "no answer in time", before it "no connection".
	std::atomic<bool> connected{false};
	std::vector<char> chunk; // WinHttpReadData()'s buffer.
};

void CALLBACK StatusCallback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD infoLength)
{
	AsyncCall *call = reinterpret_cast<AsyncCall *>(context);
	if (!call)
		return;
	switch (status) {
	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
	case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
	case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
		call->status = status;
		call->error = 0;
		if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE)
			call->bytes = info ? *static_cast<const DWORD *>(info) : 0;
		else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
			call->bytes = infoLength;
		else
			call->bytes = 0;
		SetEvent(call->step);
		break;
	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
		const auto *result = static_cast<const WINHTTP_ASYNC_RESULT *>(info);
		call->status = status;
		call->error = result ? result->dwError : static_cast<DWORD>(ERROR_WINHTTP_INTERNAL_ERROR);
		call->bytes = 0;
		SetEvent(call->step);
		break;
	}
	case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
		call->connected.store(true, std::memory_order_release);
		break;
	case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
		// The last callback for this handle: Send() may free `call` as soon
		// as this is set, so nothing touches it afterwards.
		SetEvent(call->closed);
		break;
	default:
		break;
	}
}

// Owns the request handle. Once the status callback is installed, closing it
// waits for HANDLE_CLOSING (a few milliseconds, also when a step is being
// aborted): until then WinHTTP may still use the AsyncCall, its buffer, the
// headers and the request body.
class RequestHandle {
public:
	RequestHandle(HINTERNET h, AsyncCall &call) : h_(h), call_(call) {}
	~RequestHandle()
	{
		if (!h_)
			return;
		WinHttpCloseHandle(h_);
		if (callbackInstalled_)
			WaitForSingleObject(call_.closed, INFINITE);
	}
	RequestHandle(const RequestHandle &) = delete;
	RequestHandle &operator=(const RequestHandle &) = delete;
	HINTERNET get() const { return h_; }
	void CallbackInstalled() { callbackInstalled_ = true; }

private:
	HINTERNET h_;
	AsyncCall &call_;
	bool callbackInstalled_ = false;
};

// The call's overall deadline (Timeouts::totalMs), if it has one.
class Deadline {
public:
	Deadline(std::chrono::steady_clock::time_point start, int totalMs)
		: active_(totalMs > 0),
		  at_(start + std::chrono::milliseconds(totalMs > 0 ? totalMs : 0))
	{
	}
	DWORD RemainingMs() const
	{
		if (!active_)
			return INFINITE;
		const auto left =
			std::chrono::duration_cast<std::chrono::milliseconds>(at_ - std::chrono::steady_clock::now());
		return left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
	}

private:
	bool active_;
	std::chrono::steady_clock::time_point at_;
};

enum class Step : unsigned char { Done, Failed, Cancelled, Deadline };

// Waits for the step started last to complete with `expected`. A cancel wins
// over a completion that arrives at the same moment.
Step WaitStep(AsyncCall &call, DWORD expected, const Deadline &deadline)
{
	const HANDLE handles[2] = {call.wake, call.step};
	const DWORD r = WaitForMultipleObjects(2, handles, FALSE, deadline.RemainingMs());
	if (r == WAIT_OBJECT_0)
		return Step::Cancelled;
	if (r == WAIT_TIMEOUT)
		return Step::Deadline;
	if (r != WAIT_OBJECT_0 + 1) {
		call.error = GetLastError();
		return Step::Failed;
	}
	if (call.status != expected) {
		if (call.status != WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
			call.error = 0; // Out of order: not expected to happen; "other".
		return Step::Failed;
	}
	return Step::Done;
}

Response StepFailure(Step step, const AsyncCall &call)
{
	if (step == Step::Cancelled)
		return Fail(Error::Cancelled);
	if (step == Step::Deadline)
		return Fail(Error::Timeout);
	Error error = MapError(call.error);
	// WinHTTP reports a name resolution or connection that took too long as
	// a timeout too; for the caller that is "no connection".
	if (error == Error::Timeout && !call.connected.load(std::memory_order_acquire))
		error = Error::Connect;
	return Fail(error, call.error);
}

} // namespace

bool Available()
{
	return true;
}

Response Send(const Request &req)
{
	using Clock = std::chrono::steady_clock;
	const Clock::time_point start = Clock::now();

	if (!IsValidTarget(req.host) || !IsValidTarget(req.path) || req.path[0] != '/' || !IsValidTarget(req.method) ||
	    HasLineBreakOrNul(req.userAgent))
		return Fail(Error::Invalid);
	for (const Header &h : req.headers) {
		if (!IsValidHeaderName(h.name) || HasLineBreakOrNul(h.value))
			return Fail(Error::Invalid);
	}
	if (req.cancel && req.cancel->IsCancelled())
		return Fail(Error::Cancelled);

	AsyncCall call;
	if (!call.Valid())
		return FailLast();
	// Cancelling only sets an event; the wait for the current step sees it.
	const tasks::CancelSubscription onCancel(req.cancel, [wake = call.wake] { SetEvent(wake); });
	const Deadline deadline(start, req.timeouts.totalMs);

	const std::wstring agent = Widen(req.userAgent);
	// The system's proxy configuration (WPAD/PAC included), falling back to
	// the static WinHTTP proxy settings on systems without it.
	Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				   WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
	if (!session.get())
		session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
					  WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
	if (!session.get())
		return FailLast();

	WinHttpSetTimeouts(session.get(), req.timeouts.resolveMs, req.timeouts.connectMs, req.timeouts.sendMs,
			   req.timeouts.receiveMs);
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
		SetDwordOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2);
#else
	SetDwordOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2);
#endif
	SetDwordOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, WINHTTP_OPTION_REDIRECT_POLICY_NEVER);

	std::wstring host = Widen(req.host);
	INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
	DWORD openFlags = WINHTTP_FLAG_SECURE;
#ifdef TRIGGLOW_HTTP_LOOPBACK_TESTS
	// Local test harnesses only, never defined in a plugin build: a host of
	// the form "127.0.0.1:<port>" talks plain HTTP to a fake server on this
	// machine, so the redirect, timeout and size rules above can be exercised
	// against canned responses.
	if (req.host.rfind("127.0.0.1:", 0) == 0) {
		host = L"127.0.0.1";
		port = static_cast<INTERNET_PORT>(std::stoi(req.host.substr(10)));
		openFlags = 0;
	}
#endif

	Handle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
	if (!connection.get())
		return FailLast();

	// Declared before the request handle: WinHTTP may still read them while
	// a step is being aborted, until the handle is gone (as `call` and
	// req.body).
	const std::wstring method = Widen(req.method);
	const std::wstring path = Widen(req.path);
	std::wstring headers;
	for (const Header &h : req.headers)
		headers += Widen(h.name) + L": " + Widen(h.value) + L"\r\n";

	RequestHandle request(WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(), nullptr,
						 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, openFlags),
			      call);
	if (!request.get())
		return FailLast();

	// Per request as well: never follow a redirect, keep no cookies, and never
	// answer an authentication challenge with the Windows account.
	SetDwordOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, WINHTTP_OPTION_REDIRECT_POLICY_NEVER);
	SetDwordOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
		       WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS);
	SetDwordOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH);
	// The wait for the response headers has its own timeout; keep it the
	// same as the receive timeout.
	SetDwordOption(request.get(), WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
		       static_cast<DWORD>(req.timeouts.receiveMs));

	DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&call);
	if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
		return FailLast();
	if (WinHttpSetStatusCallback(request.get(), StatusCallback,
				     WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES |
					     WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER,
				     0) == WINHTTP_INVALID_STATUS_CALLBACK)
		return FailLast();
	request.CallbackInstalled();

	LPCWSTR headersPtr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
	const DWORD headersLen = headers.empty() ? 0 : static_cast<DWORD>(headers.size());

	LPVOID bodyPtr = WINHTTP_NO_REQUEST_DATA;
	const DWORD bodyLen = static_cast<DWORD>(req.body.size());
	if (!req.body.empty())
		bodyPtr = const_cast<char *>(req.body.data());

	if (!WinHttpSendRequest(request.get(), headersPtr, headersLen, bodyPtr, bodyLen, bodyLen, context))
		return FailLast();
	Step step = WaitStep(call, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, deadline);
	if (step != Step::Done)
		return StepFailure(step, call);
	if (!WinHttpReceiveResponse(request.get(), nullptr))
		return FailLast();
	step = WaitStep(call, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, deadline);
	if (step != Step::Done)
		return StepFailure(step, call);

	Response result;
	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				 WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX))
		return FailLast();
	result.status = static_cast<int>(statusCode);

	wchar_t retryAfter[64] = {};
	DWORD retryAfterSize = sizeof(retryAfter) - sizeof(wchar_t);
	if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, retryAfter,
				&retryAfterSize, WINHTTP_NO_HEADER_INDEX)) {
		for (const wchar_t *p = retryAfter; *p; ++p) {
			if (*p >= 0x20 && *p < 0x7F)
				result.retryAfter += static_cast<char>(*p);
		}
	}

	for (;;) {
		if (!WinHttpQueryDataAvailable(request.get(), nullptr))
			return FailLast();
		step = WaitStep(call, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, deadline);
		if (step != Step::Done)
			return StepFailure(step, call);
		const DWORD available = call.bytes;
		if (available == 0)
			break;
		if (result.body.size() + available > req.maxResponseBytes)
			return Fail(Error::TooLarge);
		const DWORD want = available < kChunkBytes ? available : kChunkBytes;
		if (!WinHttpReadData(request.get(), call.chunk.data(), want, nullptr))
			return FailLast();
		step = WaitStep(call, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, deadline);
		if (step != Step::Done)
			return StepFailure(step, call);
		if (call.bytes == 0)
			break;
		result.body.append(call.chunk.data(), call.bytes < want ? call.bytes : want);
	}

	result.error = Error::None;
	return result;
}

} // namespace trigglow::http

#else // !_WIN32

namespace trigglow::http {

bool Available()
{
	return false;
}

Response Send(const Request &)
{
	Response r;
	r.error = Error::Unavailable;
	return r;
}

} // namespace trigglow::http

#endif

namespace trigglow::http {

const char *ErrorName(Error error)
{
	switch (error) {
	case Error::None:
		return "none";
	case Error::Unavailable:
		return "unavailable";
	case Error::Invalid:
		return "invalid request";
	case Error::Resolve:
		return "dns";
	case Error::Connect:
		return "connect";
	case Error::Timeout:
		return "timeout";
	case Error::Tls:
		return "tls";
	case Error::TooLarge:
		return "response too large";
	case Error::Cancelled:
		return "cancelled";
	default:
		return "other";
	}
}

} // namespace trigglow::http
