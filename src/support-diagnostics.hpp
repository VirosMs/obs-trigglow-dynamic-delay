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

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "pipeline-health.hpp"

// The optional "OBS technical data" a user can attach to a Trigi conversation
// (opt-in, off until the user ticks it; sent once, when the conversation
// starts). Schema "diagnostics" v1: numbers, booleans and fixed enum values
// only, never free text. The snapshot type below can't even hold a scene,
// source, filter, encoder or output name, a path, a URL, the stream service,
// server or key, a user or GPU name, a hardware ID or an IP.
//
// No Qt, no libobs: diagnostics-collector.hpp fills a Snapshot from OBS and
// the plugin; this file only maps and serializes it, clamped to the schema's
// ranges (tests/support_diagnostics_tests.cpp).
namespace trigglow::diag {

inline constexpr int kSchemaVersion = 1;
// Serialized size limit (bytes, compact JSON).
inline constexpr size_t kMaxSerializedBytes = 1024;
inline constexpr size_t kMaxRecentErrors = 5;

enum class OsVersion : uint8_t { Windows10, Windows11, WindowsOther, Macos, Linux };
enum class Arch : uint8_t { X64, Arm64, Other };
enum class Renderer : uint8_t { D3D11, OpenGL, Metal, Other };
enum class GpuVendor : uint8_t { Nvidia, Amd, Intel, Apple, Other, Unknown };
enum class StreamEncoder : uint8_t { X264, Nvenc, Qsv, Amf, AppleVt, Other, None };
enum class DelayState : uint8_t { Inactive, Filling, Active, Error };
enum class LoadCause : uint8_t { None, Graphics, Encode, Decode };
// The plugin's own problem codes (never the text shown in the dock).
enum class ErrorCode : uint8_t {
	LiveSceneMissing,
	SceneCollectionChanged,
	RamInsufficient,
	EncodingOverloaded,
	BufferDegraded,
	FillStalled,
	FilterRemoved,
	ProgramSwitchedToLive,
	GraphicsInitFailed,
	CodecInitFailed,
	Other,
};

struct Size {
	uint32_t w = 0;
	uint32_t h = 0;
};

// PipelineHealth (pipeline-health.hpp), schema side.
struct PipelineSnapshot {
	bool running = false;
	uint32_t level = 0;
	uint32_t maxLevel = 0;
	Size buf;
	Size full;
	double captureFps = 0.0;
	uint32_t encodeContexts = 0;
	uint32_t encodeSlices = 0;
	LoadCause cause = LoadCause::None;
	double droppedPerMin = 0.0;
	double repeatedPerMin = 0.0;
};

struct Snapshot {
	std::array<uint32_t, 3> obs{0, 0, 0}; // OBS version, major.minor.patch.
	OsVersion os = OsVersion::WindowsOther;
	Arch arch = Arch::Other;
	Renderer renderer = Renderer::Other;
	GpuVendor gpuVendor = GpuVendor::Unknown;
	uint64_t ramTotalBytes = 0; // Sent as whole GB.
	StreamEncoder streamEncoder = StreamEncoder::None;
	DelayState state = DelayState::Inactive;
	uint32_t delaySeconds = 0;
	uint32_t effectiveDelaySeconds = 0;
	uint32_t minHeight = 0;
	Size canvas;
	Size output;
	double fps = 0.0;
	bool liveSceneSet = false;    // Whether one is chosen; never its name.
	bool loadingSceneSet = false; // Same.
	bool outputCap = true;        // Hidden "buffer_output_cap" setting (true is the default).
	bool codecWorkers = true;     // Hidden "codec_workers" setting (true is the default).
	std::optional<double> obsLaggedPct;
	std::optional<double> obsSkippedPct;
	std::optional<PipelineSnapshot> pipeline;
	std::vector<ErrorCode> recentErrors; // Most recent first; duplicates and extras are dropped.
};

// The "diagnostics" object, v1: every number clamped to its range and rounded
// to at most 2 decimals, recent errors deduplicated and capped at 5, and the
// compact serialization kept within kMaxSerializedBytes (recent errors, then
// the optional fields, are dropped first if it ever wouldn't fit).
json::Value BuildV1(const Snapshot &snapshot);

// Compact JSON (what is sent) or pretty (the "see what is sent" preview).
// Both come from BuildV1() and hold exactly the same data.
std::string SerializeV1(const Snapshot &snapshot, bool pretty = false);

// --- Mapping helpers (all deterministic, all tested) -------------------------

// obs_get_version()'s packed value (major << 24 | minor << 16 | patch).
std::array<uint32_t, 3> ObsVersionFromPacked(uint32_t packed);

// Windows build number -> windows-10 / windows-11 (build 22000 and later).
OsVersion WindowsVersionFromBuild(uint32_t major, uint32_t build);

// The OS this plugin was built for, with the Windows version left to
// WindowsVersionFromBuild().
Arch CurrentArch();

// obs_video_info.graphics_module ("libobs-d3d11", "libobs-opengl", ...).
Renderer RendererFromGraphicsModule(std::string_view module);

// PCI vendor ID of the adapter OBS renders on (0 = unknown).
GpuVendor GpuVendorFromPciId(uint32_t vendorId);

// Family of the stream output's video encoder from its OBS encoder ID; the ID
// itself is never sent. Empty = no encoder.
StreamEncoder StreamEncoderFromObsId(std::string_view encoderId);

PipelineSnapshot FromPipelineHealth(const PipelineHealth &health);

// Schema strings, for tests and the preview.
const char *ToString(OsVersion v);
const char *ToString(Arch v);
const char *ToString(Renderer v);
const char *ToString(GpuVendor v);
const char *ToString(StreamEncoder v);
const char *ToString(DelayState v);
const char *ToString(LoadCause v);
const char *ToString(ErrorCode v);

} // namespace trigglow::diag
