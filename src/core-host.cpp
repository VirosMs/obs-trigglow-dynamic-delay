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

#include "core-host.hpp"
#include "auto-names.hpp"
#include "buffer-mode-controller.hpp"
#include "logging.hpp"
#include "obs-frontend-bridge.hpp"
#include "video-delay-filter.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <obs-module.h>
#include <util/platform.h> // os_file_exists()
}

namespace trigglow::corehost {
namespace {
constexpr const char *kComponent = "core-host";
constexpr const char *kSourceId = "trigglow_delay_controller";
using Clock = std::chrono::steady_clock;

// What the host asks for, read from the controller source's settings.
struct Config {
	std::string liveScene;
	std::string loadingScene;
	uint32_t delaySeconds = 5;
	uint32_t minResolutionHeight = 720;
	bool showOverlay = true;
	uint32_t overlayCorner = 0;
	bool bufferOutputCap = true;
	bool codecWorkers = true;
	bool bufferCodecAuto = true;
	std::string badgePrefix;
	long long badgeToken = 0;
};

// What the controller thread publishes for the source's settings.
struct Published {
	BufferModeStatus status;
	PipelineHealth health;
	VideoDelayFilter::BufferFitEstimate fit;
	bool fillArmed = false;
	Clock::time_point fillDeadline;
};

class Host {
public:
	Host()
	{
		bridge_.SetOverlayImageProvider([this](uint32_t seconds) {
			if (badgePrefix_.empty())
				return std::string();
			std::string path = badgePrefix_ + std::to_string(seconds) + ".png";
			return os_file_exists(path.c_str()) ? path : std::string();
		});
		controller_.SetStatusChangedCallback([this](const BufferModeStatus &status) { OnStatus(status); });
		thread_ = std::thread([this] { Run(); });
	}

	~Host()
	{
		RunAndWait([this] { bridge_.Shutdown(); });
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
		}
		cv_.notify_all();
		thread_.join();
	}

	Host(const Host &) = delete;
	Host &operator=(const Host &) = delete;

	void Post(std::function<void()> task)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (stopping_)
				return;
			tasks_.push_back(std::move(task));
		}
		cv_.notify_all();
	}

	// Runs `task` on the controller thread and waits for it. The task owns
	// everything it uses (captured by value), so a wait that gives up (the
	// controller thread stuck, never seen) leaves nothing dangling.
	void RunAndWait(std::function<void()> task)
	{
		if (std::this_thread::get_id() == thread_.get_id()) {
			task();
			return;
		}
		struct Done {
			std::mutex mutex;
			std::condition_variable cv;
			bool done = false;
		};
		auto done = std::make_shared<Done>();
		Post([task = std::move(task), done] {
			task();
			std::lock_guard<std::mutex> lock(done->mutex);
			done->done = true;
			done->cv.notify_all();
		});
		std::unique_lock<std::mutex> lock(done->mutex);
		if (!done->cv.wait_for(lock, std::chrono::seconds(10), [&] { return done->done; }))
			TRIGGLOW_LOG_WARN(kComponent, "the controller thread took over 10 s, not waiting any longer");
	}

	// Controller thread: the host's settings, then its command.
	void Apply(const Config &config, const std::string &command)
	{
		ApplyConfig(config);
		if (command == "enable")
			controller_.Enable();
		else if (command == "disable")
			controller_.Disable();
		else if (command == "toggle")
			controller_.Toggle();
		else if (command == "output_starting") // OBS's STREAMING/RECORDING/REPLAY_BUFFER_STARTING
			bridge_.DeliverHostEvent(FrontendEvent::StreamingStarting);
		else if (command == "collection_changing") // OBS's SCENE_COLLECTION_CHANGING
			bridge_.DeliverHostEvent(FrontendEvent::SceneCollectionChanging);
		else if (!command.empty())
			TRIGGLOW_LOG_WARN(kComponent, "unknown command \"%s\"", command.c_str());
		PublishExtras();
	}

	Published Snapshot()
	{
		std::lock_guard<std::mutex> lock(publishMutex_);
		return published_;
	}

private:
	void ApplyConfig(const Config &c)
	{
		if (!loaded_) {
			// The plugin settings file of the OBS build, read once.
			controller_.LoadSettings(c.delaySeconds, c.minResolutionHeight, c.liveScene, c.loadingScene,
						 c.showOverlay, c.overlayCorner, c.bufferOutputCap, c.codecWorkers,
						 c.bufferCodecAuto);
			loaded_ = true;
		}
		const BufferModeStatus status = controller_.GetStatus();
		const bool busy = status.state == BufferModeState::Filling || status.state == BufferModeState::Active;
		// The dock locks both scene pickers while the delay runs: a change waits for the next Enable.
		if (!busy && c.liveScene != status.liveSceneName)
			controller_.SetLiveScene(c.liveScene);
		if (!busy && c.loadingScene != status.loadingSceneName)
			controller_.SetLoadingScene(c.loadingScene);
		if (c.delaySeconds != status.delaySeconds)
			controller_.SetDelaySeconds(c.delaySeconds);
		if (c.minResolutionHeight != status.minResolutionHeight)
			controller_.SetMinResolutionHeight(c.minResolutionHeight);
		if (c.overlayCorner != status.overlayCorner)
			controller_.SetOverlayCorner(c.overlayCorner);
		const bool badgesChanged = c.badgePrefix != badgePrefix_ || c.badgeToken != badgeToken_;
		badgePrefix_ = c.badgePrefix;
		badgeToken_ = c.badgeToken;
		if (c.showOverlay != status.showOverlay || badgesChanged)
			controller_.SetShowOverlay(c.showOverlay); // Also redraws the badge.
	}

	// Controller thread: the dock's OnStatusChanged() -- the fill timer, armed
	// once per fill window for the seconds that really apply.
	void OnStatus(const BufferModeStatus &status)
	{
		if (status.state != BufferModeState::Filling) {
			fillArmed_ = false;
		} else if (!fillArmed_ || status.fillGeneration != armedGeneration_) {
			armedGeneration_ = status.fillGeneration;
			const uint32_t applied = status.effectiveDelaySeconds > 0
							 ? std::min(status.effectiveDelaySeconds, status.delaySeconds)
							 : status.delaySeconds;
			fillDeadline_ = Clock::now() + std::chrono::seconds(applied);
			fillArmed_ = true;
		}
		std::lock_guard<std::mutex> lock(publishMutex_);
		published_.status = status;
		published_.fillArmed = fillArmed_;
		published_.fillDeadline = fillDeadline_;
	}

	// Controller thread: what the dock polls once a second (pipeline health,
	// the buffer fit for the settings).
	void PublishExtras()
	{
		const BufferModeStatus status = controller_.GetStatus();
		const PipelineHealth health = controller_.GetPipelineHealth();
		const VideoDelayFilter::BufferFitEstimate fit =
			controller_.EstimateBufferFit(status.delaySeconds, status.minResolutionHeight);
		std::lock_guard<std::mutex> lock(publishMutex_);
		published_.health = health;
		published_.fit = fit;
	}

	void Run()
	{
		Clock::time_point nextTick = Clock::now() + std::chrono::seconds(1);
		std::unique_lock<std::mutex> lock(mutex_);
		for (;;) {
			if (!tasks_.empty()) {
				std::function<void()> task = std::move(tasks_.front());
				tasks_.pop_front();
				lock.unlock();
				task();
				lock.lock();
				continue;
			}
			if (stopping_)
				return;
			const Clock::time_point wake = fillArmed_ ? std::min(nextTick, fillDeadline_) : nextTick;
			cv_.wait_until(lock, wake, [this] { return stopping_ || !tasks_.empty(); });
			if (!tasks_.empty() || stopping_)
				continue;
			lock.unlock();
			const Clock::time_point now = Clock::now();
			if (fillArmed_ && now >= fillDeadline_) {
				fillArmed_ = false;
				controller_.OnFillTimerElapsed();
			}
			if (now >= nextTick) {
				nextTick = now + std::chrono::seconds(1);
				controller_.Tick();
				PublishExtras();
			}
			lock.lock();
		}
	}

	ObsFrontendBridge bridge_;
	BufferModeController controller_{bridge_};
	bool loaded_ = false;
	std::string badgePrefix_; // Controller thread, like everything below up to the mutex.
	long long badgeToken_ = 0;
	bool fillArmed_ = false;
	uint32_t armedGeneration_ = 0;
	Clock::time_point fillDeadline_;

	std::mutex publishMutex_;
	Published published_;

	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::function<void()>> tasks_;
	bool stopping_ = false;
	std::thread thread_;
};

std::mutex g_hostMutex;
std::unique_ptr<Host> g_host;

Host *GetHost()
{
	std::lock_guard<std::mutex> lock(g_hostMutex);
	if (!g_host)
		g_host = std::make_unique<Host>();
	return g_host.get();
}

const char *StateName(BufferModeState state)
{
	switch (state) {
	case BufferModeState::Filling:
		return "filling";
	case BufferModeState::Active:
		return "active";
	case BufferModeState::Error:
		return "error";
	case BufferModeState::Inactive:
		break;
	}
	return "inactive";
}

uint32_t ReadUint(obs_data_t *settings, const char *key)
{
	return static_cast<uint32_t>(std::max<long long>(0, obs_data_get_int(settings, key)));
}

// The controller source's own data: the last command token it ran.
struct ControllerSource {
	long long commandToken = 0;
};

const char *GetName(void *)
{
	return "Trigglow Dynamic Delay controller";
}

void GetDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "delay_seconds", 5);
	obs_data_set_default_int(settings, "min_resolution_height", 720);
	obs_data_set_default_bool(settings, "show_overlay", true);
	obs_data_set_default_bool(settings, "buffer_output_cap", true);
	obs_data_set_default_bool(settings, "codec_workers", true);
	obs_data_set_default_string(settings, "buffer_codec", "auto");
}

// The host's call: the IPC thread of Trigglow Studio's engine (libobs calls
// update right away for a source without video).
void Update(void *data, obs_data_t *settings)
{
	auto *self = static_cast<ControllerSource *>(data);

	obs_video_info ovi = {};
	ovi.fps_num = ReadUint(settings, "host_fps_num");
	ovi.fps_den = ReadUint(settings, "host_fps_den");
	ovi.base_width = ReadUint(settings, "host_base_width");
	ovi.base_height = ReadUint(settings, "host_base_height");
	ovi.output_width = ReadUint(settings, "host_output_width");
	ovi.output_height = ReadUint(settings, "host_output_height");
	ovi.adapter = ReadUint(settings, "host_adapter");
	if (ovi.fps_num > 0 && ovi.fps_den > 0)
		VideoDelayFilter::SetHostVideoInfo(ovi);

	Config config;
	config.liveScene = obs_data_get_string(settings, "live_scene");
	config.loadingScene = obs_data_get_string(settings, "loading_scene");
	config.delaySeconds = std::max<uint32_t>(1, ReadUint(settings, "delay_seconds"));
	config.minResolutionHeight = ReadUint(settings, "min_resolution_height");
	config.showOverlay = obs_data_get_bool(settings, "show_overlay");
	config.overlayCorner = std::min<uint32_t>(3, ReadUint(settings, "overlay_corner"));
	config.bufferOutputCap = obs_data_get_bool(settings, "buffer_output_cap");
	config.codecWorkers = obs_data_get_bool(settings, "codec_workers");
	config.bufferCodecAuto = strcmp(obs_data_get_string(settings, "buffer_codec"), "mjpeg") != 0;
	config.badgePrefix = obs_data_get_string(settings, "badge_prefix");
	config.badgeToken = obs_data_get_int(settings, "badge_token");
	std::string command;
	const long long token = obs_data_get_int(settings, "command_token");
	if (token != self->commandToken) {
		self->commandToken = token;
		command = obs_data_get_string(settings, "command");
	}

	Host *host = GetHost();
	host->RunAndWait([host, config, command] { host->Apply(config, command); });

	const Published p = host->Snapshot();
	const BufferModeStatus &s = p.status;
	const uint32_t applied = s.effectiveDelaySeconds > 0 ? std::min(s.effectiveDelaySeconds, s.delaySeconds)
							     : s.delaySeconds;
	long long remainingMs = 0;
	if (p.fillArmed)
		remainingMs = std::max<long long>(
			0,
			std::chrono::duration_cast<std::chrono::milliseconds>(p.fillDeadline - Clock::now()).count());
	obs_data_set_string(settings, "status_state", StateName(s.state));
	obs_data_set_string(settings, "status_message", s.message.c_str());
	obs_data_set_string(settings, "status_message_key", s.messageKey.c_str());
	obs_data_set_int(settings, "status_fill_generation", s.fillGeneration);
	obs_data_set_int(settings, "status_fill_remaining_ms", remainingMs);
	obs_data_set_int(settings, "status_delay_seconds", s.delaySeconds);
	obs_data_set_int(settings, "status_effective_seconds", s.effectiveDelaySeconds);
	obs_data_set_int(settings, "status_applied_seconds",
			 s.state == BufferModeState::Filling || s.state == BufferModeState::Active ? applied : 0);
	obs_data_set_string(settings, "status_live_scene", s.liveSceneName.c_str());
	obs_data_set_string(settings, "status_wrapper_scene", autonames::kWrapperScene);
	const PipelineHealth &h = p.health;
	obs_data_set_bool(settings, "health_running", h.running);
	obs_data_set_int(settings, "health_level", h.level);
	obs_data_set_int(settings, "health_width", h.width);
	obs_data_set_int(settings, "health_height", h.height);
	obs_data_set_int(settings, "health_full_width", h.fullWidth);
	obs_data_set_int(settings, "health_full_height", h.fullHeight);
	obs_data_set_double(settings, "health_capture_fps", h.captureFps);
	obs_data_set_double(settings, "health_output_fps", h.outputFps);
	obs_data_set_int(settings, "health_cause", static_cast<long long>(h.cause));
	obs_data_set_double(settings, "health_dropped_per_minute", h.droppedPerMinute);
	obs_data_set_double(settings, "health_repeated_per_minute", h.repeatedPerMinute);
	obs_data_set_string(settings, "health_encoder", h.encoder);
	obs_data_set_int(settings, "health_changes", static_cast<long long>(h.changes));
	obs_data_set_int(settings, "fit_width", p.fit.width);
	obs_data_set_int(settings, "fit_height", p.fit.height);
	obs_data_set_double(settings, "fit_actual_seconds", p.fit.actualSeconds);
	obs_data_set_int(settings, "fit_effective_seconds", p.fit.effectiveSeconds);
	obs_data_set_bool(settings, "fit_full_duration", p.fit.fitsFullDuration);
}

void *Create(obs_data_t *settings, obs_source_t * /*source*/)
{
	auto *self = new ControllerSource();
	// A command saved in the settings never runs at creation.
	self->commandToken = obs_data_get_int(settings, "command_token");
	Update(self, settings);
	return self;
}

void Destroy(void *data)
{
	// The controller outlives its source (Shutdown(), at unload): no delay
	// state goes with it.
	delete static_cast<ControllerSource *>(data);
}
} // namespace

void Register()
{
	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	// Nothing to render or mix: only settings. Hidden from source lists.
	info.output_flags = OBS_SOURCE_CAP_DISABLED;
	info.get_name = GetName;
	info.create = Create;
	info.destroy = Destroy;
	info.update = Update;
	info.get_defaults = GetDefaults;
	obs_register_source(&info);
}

void Shutdown()
{
	std::unique_ptr<Host> host;
	{
		std::lock_guard<std::mutex> lock(g_hostMutex);
		host = std::move(g_host);
	}
	host.reset(); // Joins the controller thread, outside g_hostMutex.
}

void QueueTask(void (*task)(void *), void *param)
{
	std::lock_guard<std::mutex> lock(g_hostMutex);
	if (g_host)
		g_host->Post([task, param] { task(param); });
}

} // namespace trigglow::corehost
