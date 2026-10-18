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

// A scripted stand-in for the HTTP transport (api::Client takes any
// function), for the tests of the account, Trigi and ticket code: records
// every request and answers from a queue or a handler. No network.

#include "test-support.hpp"
#include "win-http.hpp"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace testsupport {

// A response the fake transport returns.
struct Reply {
	int status = 200;
	std::string body;
	std::string retryAfter;
	trigglow::http::Error error = trigglow::http::Error::None;
};

inline Reply Json(int status, std::string body)
{
	Reply r;
	r.status = status;
	r.body = std::move(body);
	return r;
}

inline Reply NetworkDown()
{
	Reply r;
	r.error = trigglow::http::Error::Connect;
	return r;
}

// Records every request and answers from a queue (or a handler, when set).
// An empty queue answers 599.
class FakeTransport {
public:
	std::function<Reply(const trigglow::http::Request &)> handler;

	void Push(Reply reply)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		replies_.push_back(std::move(reply));
	}

	std::function<trigglow::http::Response(const trigglow::http::Request &)> Fn()
	{
		return [this](const trigglow::http::Request &request) {
			return Handle(request);
		};
	}

	std::vector<trigglow::http::Request> Requests()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return requests_;
	}

	size_t Count()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return requests_.size();
	}

	trigglow::http::Request Last()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return requests_.empty() ? trigglow::http::Request{} : requests_.back();
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		requests_.clear();
		replies_.clear();
	}

private:
	trigglow::http::Response Handle(const trigglow::http::Request &request)
	{
		Reply reply;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			requests_.push_back(request);
			if (!handler) {
				if (replies_.empty()) {
					reply.status = 599;
				} else {
					reply = replies_.front();
					replies_.pop_front();
				}
			}
		}
		if (handler)
			reply = handler(request);
		trigglow::http::Response response;
		response.error = reply.error;
		if (reply.error == trigglow::http::Error::None) {
			response.status = reply.status;
			response.body = reply.body;
			response.retryAfter = reply.retryAfter;
		}
		return response;
	}

	std::mutex mutex_;
	std::vector<trigglow::http::Request> requests_;
	std::deque<Reply> replies_;
};

inline std::string HeaderValue(const trigglow::http::Request &request, std::string_view name)
{
	for (const auto &h : request.headers) {
		if (h.name == name)
			return h.value;
	}
	return std::string();
}

inline bool HasHeader(const trigglow::http::Request &request, std::string_view name)
{
	for (const auto &h : request.headers) {
		if (h.name == name)
			return true;
	}
	return false;
}

} // namespace testsupport
