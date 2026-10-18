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

#include "diagnostics-collector.hpp"
#include "buffer-mode-controller.hpp"
#include "hardware-info.hpp"

extern "C" {
#include <obs.h>
#include <obs-frontend-api.h>
}

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif

namespace trigglow {

namespace {

diag::OsVersion CurrentOsVersion()
{
#if defined(_WIN32)
	// GetVersionEx() reports what the host's manifest allows; RtlGetVersion()
	// reports the real build.
	struct VersionInfo {
		ULONG size;
		ULONG major;
		ULONG minor;
		ULONG build;
		ULONG platform;
		WCHAR servicePack[128];
	};
	using RtlGetVersionFn = LONG(WINAPI *)(VersionInfo *);
	VersionInfo info{};
	info.size = sizeof(info);
	if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
		auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
		if (fn && fn(&info) == 0)
			return diag::WindowsVersionFromBuild(info.major, info.build);
	}
	return diag::OsVersion::WindowsOther;
#elif defined(__APPLE__)
	return diag::OsVersion::Macos;
#else
	return diag::OsVersion::Linux;
#endif
}

diag::GpuVendor GpuVendorOfAdapter(uint32_t adapterIndex)
{
#ifdef _WIN32
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
		return diag::GpuVendor::Unknown;
	diag::GpuVendor vendor = diag::GpuVendor::Unknown;
	IDXGIAdapter1 *adapter = nullptr;
	if (SUCCEEDED(factory->EnumAdapters1(adapterIndex, &adapter)) && adapter) {
		DXGI_ADAPTER_DESC1 desc{};
		// Only the numeric vendor ID is kept; never the adapter's name.
		if (SUCCEEDED(adapter->GetDesc1(&desc)))
			vendor = diag::GpuVendorFromPciId(desc.VendorId);
		adapter->Release();
	}
	factory->Release();
	return vendor;
#else
	(void)adapterIndex;
	return diag::GpuVendor::Unknown;
#endif
}

diag::StreamEncoder StreamEncoderFamily()
{
	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output)
		return diag::StreamEncoder::None;
	obs_encoder_t *encoder = obs_output_get_video_encoder(output);
	const char *id = encoder ? obs_encoder_get_id(encoder) : nullptr;
	const diag::StreamEncoder family = diag::StreamEncoderFromObsId(id ? id : "");
	obs_output_release(output);
	return family;
}

diag::DelayState StateOf(BufferModeState state)
{
	switch (state) {
	case BufferModeState::Filling:
		return diag::DelayState::Filling;
	case BufferModeState::Active:
		return diag::DelayState::Active;
	case BufferModeState::Error:
		return diag::DelayState::Error;
	default:
		return diag::DelayState::Inactive;
	}
}

} // namespace

diag::Snapshot CollectDiagnostics(const BufferModeController &controller)
{
	diag::Snapshot s;
	s.obs = diag::ObsVersionFromPacked(obs_get_version());
	s.os = CurrentOsVersion();
	s.arch = diag::CurrentArch();
	s.ramTotalBytes = QueryTotalSystemRamBytes();
	s.streamEncoder = StreamEncoderFamily();

	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi)) {
		s.renderer = diag::RendererFromGraphicsModule(ovi.graphics_module ? ovi.graphics_module : "");
		s.canvas = {ovi.base_width, ovi.base_height};
		s.output = {ovi.output_width, ovi.output_height};
		if (ovi.fps_den > 0)
			s.fps = static_cast<double>(ovi.fps_num) / static_cast<double>(ovi.fps_den);
		s.gpuVendor = GpuVendorOfAdapter(ovi.adapter);
	}

	const BufferModeStatus status = controller.GetStatus();
	s.state = StateOf(status.state);
	s.delaySeconds = status.delaySeconds;
	s.effectiveDelaySeconds = status.effectiveDelaySeconds;
	s.minHeight = status.minResolutionHeight;
	s.liveSceneSet = !status.liveSceneName.empty();
	s.loadingSceneSet = !status.loadingSceneName.empty();
	const BufferModeController::SettingsSnapshot settings = controller.SaveSettings();
	s.outputCap = settings.bufferOutputCap;
	s.codecWorkers = settings.codecWorkers;

	const uint32_t total = obs_get_total_frames();
	if (total > 0)
		s.obsLaggedPct = 100.0 * static_cast<double>(obs_get_lagged_frames()) / static_cast<double>(total);
	if (video_t *video = obs_get_video()) {
		const uint32_t frames = video_output_get_total_frames(video);
		if (frames > 0)
			s.obsSkippedPct = 100.0 * static_cast<double>(video_output_get_skipped_frames(video)) /
					  static_cast<double>(frames);
	}

	const PipelineHealth health = controller.GetPipelineHealth();
	if (health.running || health.maxLevel > 0)
		s.pipeline = diag::FromPipelineHealth(health);

	// Problem codes for what can be seen right now (most relevant first).
	if (status.state == BufferModeState::Error && status.liveSceneName.empty())
		s.recentErrors.push_back(diag::ErrorCode::LiveSceneMissing);
	if (health.Degraded() && health.cause == PipelineLoad::Encode)
		s.recentErrors.push_back(diag::ErrorCode::EncodingOverloaded);
	if (health.Degraded())
		s.recentErrors.push_back(diag::ErrorCode::BufferDegraded);
	if (status.state != BufferModeState::Inactive && status.effectiveDelaySeconds > 0 &&
	    status.effectiveDelaySeconds < status.delaySeconds)
		s.recentErrors.push_back(diag::ErrorCode::RamInsufficient);
	return s;
}

} // namespace trigglow
