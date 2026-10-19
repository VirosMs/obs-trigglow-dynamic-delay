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

#include "ui-tasks.hpp"
#include "logging.hpp"

#include <chrono>

#include <QCoreApplication>
#include <QObject>

namespace trigglow::uitasks {

namespace {

constexpr const char *kComponent = "tasks";

// Both only touched on the UI thread (the waker only uses its own copy of the
// dispatcher's address, and only while the registry isn't shut down).
tasks::Registry *g_registry = nullptr;
QObject *g_dispatcher = nullptr;

} // namespace

void Start()
{
	if (g_registry)
		return;
	g_dispatcher = new QObject();
	if (QCoreApplication *app = QCoreApplication::instance())
		g_dispatcher->moveToThread(app->thread());
	QObject *dispatcher = g_dispatcher;
	g_registry = new tasks::Registry([dispatcher] {
		// Any thread, with the registry's lock held: only post. Deleting the
		// dispatcher (Shutdown(), after the registry stopped calling this)
		// discards a call still queued.
		QMetaObject::invokeMethod(
			dispatcher,
			[] {
				if (g_registry)
					g_registry->Drain();
			},
			Qt::QueuedConnection);
	});
}

void Shutdown()
{
	if (!g_registry)
		return;
	const auto start = std::chrono::steady_clock::now();
	const size_t running = g_registry->Running();
	const bool allEnded = g_registry->Shutdown(tasks::kShutdownLimit);
	const long long ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	const size_t left = g_registry->Running();

	delete g_dispatcher; // With any Drain() still queued for it.
	g_dispatcher = nullptr;
	tasks::Registry *registry = g_registry;
	g_registry = nullptr;
	// Never blocks. A job still running keeps the registry's shared state
	// and, on Windows, the plugin's DLL loaded until it ends.
	delete registry;

	if (allEnded)
		TRIGGLOW_LOG_INFO(kComponent, "background jobs stopped (%zu running, %lld ms)", running, ms);
	else
		TRIGGLOW_LOG_WARN(kComponent,
				  "%zu of %zu background jobs still running after %lld ms; they end on their own "
				  "and deliver nothing",
				  left, running, ms);
}

tasks::Registry *Registry()
{
	return g_registry;
}

void LogJobFailure(const char *component)
{
	TRIGGLOW_LOG_WARN(component, "a background call failed unexpectedly");
}

void LogJobRefused(const char *component)
{
	TRIGGLOW_LOG_WARN(component, "a background call couldn't be started");
}

} // namespace trigglow::uitasks
