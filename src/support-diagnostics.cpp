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

#include "support-diagnostics.hpp"

#include <algorithm>
#include <cmath>

namespace trigglow::diag {

namespace {

double Round2Clamp(double value, double lo, double hi)
{
	if (!std::isfinite(value))
		value = lo;
	value = std::round(value * 100.0) / 100.0;
	return std::clamp(value, lo, hi);
}

double IntClamp(uint64_t value, uint64_t hi)
{
	return static_cast<double>(std::min<uint64_t>(value, hi));
}

json::Value SizeValue(const Size &s)
{
	json::Value v = json::Value::Object();
	v.Set("w", json::Value::Number(IntClamp(s.w, 16384)));
	v.Set("h", json::Value::Number(IntClamp(s.h, 16384)));
	return v;
}

bool ContainsI(std::string_view s, std::string_view lowerNeedle)
{
	if (lowerNeedle.size() > s.size())
		return false;
	for (size_t i = 0; i + lowerNeedle.size() <= s.size(); ++i) {
		bool match = true;
		for (size_t k = 0; k < lowerNeedle.size(); ++k) {
			char c = s[i + k];
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
			if (c != lowerNeedle[k]) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

json::Value Build(const Snapshot &s, size_t maxErrors, bool withOptional)
{
	json::Value v = json::Value::Object();
	v.Set("v", json::Value::Number(kSchemaVersion));
	json::Value obs = json::Value::Array();
	for (uint32_t part : s.obs)
		obs.Push(json::Value::Number(IntClamp(part, 999)));
	v.Set("obs", std::move(obs));
	v.Set("osVersion", json::Value::String(ToString(s.os)));
	v.Set("arch", json::Value::String(ToString(s.arch)));
	v.Set("renderer", json::Value::String(ToString(s.renderer)));
	v.Set("gpuVendor", json::Value::String(ToString(s.gpuVendor)));
	const double gb = std::round(static_cast<double>(s.ramTotalBytes) / (1024.0 * 1024.0 * 1024.0));
	v.Set("ramTotalGb", json::Value::Number(std::clamp(gb, 0.0, 4096.0)));
	v.Set("streamEncoder", json::Value::String(ToString(s.streamEncoder)));
	v.Set("state", json::Value::String(ToString(s.state)));
	v.Set("delayS", json::Value::Number(IntClamp(s.delaySeconds, 600)));
	v.Set("effectiveDelayS", json::Value::Number(IntClamp(s.effectiveDelaySeconds, 600)));
	v.Set("minHeight", json::Value::Number(IntClamp(s.minHeight, 4320)));
	v.Set("canvas", SizeValue(s.canvas));
	v.Set("output", SizeValue(s.output));
	v.Set("fps", json::Value::Number(Round2Clamp(s.fps, 0.0, 240.0)));
	v.Set("liveSceneSet", json::Value::Bool(s.liveSceneSet));
	v.Set("loadingSceneSet", json::Value::Bool(s.loadingSceneSet));
	v.Set("outputCap", json::Value::Bool(s.outputCap));
	v.Set("codecWorkers", json::Value::Bool(s.codecWorkers));
	if (withOptional && s.obsLaggedPct)
		v.Set("obsLaggedPct", json::Value::Number(Round2Clamp(*s.obsLaggedPct, 0.0, 100.0)));
	if (withOptional && s.obsSkippedPct)
		v.Set("obsSkippedPct", json::Value::Number(Round2Clamp(*s.obsSkippedPct, 0.0, 100.0)));
	if (withOptional && s.pipeline) {
		const PipelineSnapshot &p = *s.pipeline;
		json::Value pv = json::Value::Object();
		pv.Set("running", json::Value::Bool(p.running));
		pv.Set("level", json::Value::Number(IntClamp(p.level, 16)));
		pv.Set("maxLevel", json::Value::Number(IntClamp(p.maxLevel, 16)));
		pv.Set("buf", SizeValue(p.buf));
		pv.Set("full", SizeValue(p.full));
		pv.Set("captureFps", json::Value::Number(Round2Clamp(p.captureFps, 0.0, 240.0)));
		pv.Set("encodeContexts", json::Value::Number(IntClamp(p.encodeContexts, 8)));
		pv.Set("encodeSlices", json::Value::Number(IntClamp(p.encodeSlices, 16)));
		pv.Set("cause", json::Value::String(ToString(p.cause)));
		pv.Set("droppedPerMin", json::Value::Number(Round2Clamp(p.droppedPerMin, 0.0, 1e6)));
		pv.Set("repeatedPerMin", json::Value::Number(Round2Clamp(p.repeatedPerMin, 0.0, 1e6)));
		v.Set("pipeline", std::move(pv));
	}
	json::Value errors = json::Value::Array();
	std::vector<ErrorCode> seen;
	for (ErrorCode code : s.recentErrors) {
		if (seen.size() >= maxErrors)
			break;
		if (std::find(seen.begin(), seen.end(), code) != seen.end())
			continue;
		seen.push_back(code);
		errors.Push(json::Value::String(ToString(code)));
	}
	v.Set("recentErrors", std::move(errors));
	return v;
}

} // namespace

json::Value BuildV1(const Snapshot &snapshot)
{
	// Fits by construction today (about 950 bytes with every field at its
	// longest); the fallbacks only guard future edits of this file.
	for (size_t errors = kMaxRecentErrors + 1; errors-- > 0;) {
		json::Value v = Build(snapshot, errors, true);
		if (json::Serialize(v).size() <= kMaxSerializedBytes)
			return v;
	}
	return Build(snapshot, 0, false);
}

std::string SerializeV1(const Snapshot &snapshot, bool pretty)
{
	return json::Serialize(BuildV1(snapshot), pretty);
}

std::array<uint32_t, 3> ObsVersionFromPacked(uint32_t packed)
{
	return {packed >> 24, (packed >> 16) & 0xFF, packed & 0xFFFF};
}

OsVersion WindowsVersionFromBuild(uint32_t major, uint32_t build)
{
	if (major != 10)
		return OsVersion::WindowsOther;
	return build >= 22000 ? OsVersion::Windows11 : OsVersion::Windows10;
}

Arch CurrentArch()
{
#if defined(_M_X64) || defined(__x86_64__)
	return Arch::X64;
#elif defined(_M_ARM64) || defined(__aarch64__)
	return Arch::Arm64;
#else
	return Arch::Other;
#endif
}

Renderer RendererFromGraphicsModule(std::string_view module)
{
	if (ContainsI(module, "d3d11"))
		return Renderer::D3D11;
	if (ContainsI(module, "opengl"))
		return Renderer::OpenGL;
	if (ContainsI(module, "metal"))
		return Renderer::Metal;
	return Renderer::Other;
}

GpuVendor GpuVendorFromPciId(uint32_t vendorId)
{
	switch (vendorId) {
	case 0:
		return GpuVendor::Unknown;
	case 0x10DE:
		return GpuVendor::Nvidia;
	case 0x1002:
	case 0x1022:
		return GpuVendor::Amd;
	case 0x8086:
	case 0x8087:
		return GpuVendor::Intel;
	case 0x106B:
		return GpuVendor::Apple;
	default:
		return GpuVendor::Other;
	}
}

StreamEncoder StreamEncoderFromObsId(std::string_view id)
{
	if (id.empty())
		return StreamEncoder::None;
	if (ContainsI(id, "x264"))
		return StreamEncoder::X264;
	if (ContainsI(id, "nvenc"))
		return StreamEncoder::Nvenc;
	if (ContainsI(id, "qsv"))
		return StreamEncoder::Qsv;
	if (ContainsI(id, "amf") || ContainsI(id, "amd_"))
		return StreamEncoder::Amf;
	if (ContainsI(id, "videotoolbox") || ContainsI(id, "apple"))
		return StreamEncoder::AppleVt;
	return StreamEncoder::Other;
}

PipelineSnapshot FromPipelineHealth(const PipelineHealth &h)
{
	PipelineSnapshot p;
	p.running = h.running;
	p.level = h.level;
	p.maxLevel = h.maxLevel;
	p.buf = {h.width, h.height};
	p.full = {h.fullWidth, h.fullHeight};
	p.captureFps = h.captureFps;
	p.encodeContexts = h.encodeContexts;
	p.encodeSlices = h.encodeSlices;
	switch (h.cause) {
	case PipelineLoad::Graphics:
		p.cause = LoadCause::Graphics;
		break;
	case PipelineLoad::Encode:
		p.cause = LoadCause::Encode;
		break;
	case PipelineLoad::Decode:
		p.cause = LoadCause::Decode;
		break;
	default:
		p.cause = LoadCause::None;
	}
	p.droppedPerMin = h.droppedPerMinute;
	p.repeatedPerMin = h.repeatedPerMinute;
	return p;
}

const char *ToString(OsVersion v)
{
	switch (v) {
	case OsVersion::Windows10:
		return "windows-10";
	case OsVersion::Windows11:
		return "windows-11";
	case OsVersion::Macos:
		return "macos";
	case OsVersion::Linux:
		return "linux";
	default:
		return "windows-other";
	}
}

const char *ToString(Arch v)
{
	switch (v) {
	case Arch::X64:
		return "x64";
	case Arch::Arm64:
		return "arm64";
	default:
		return "other";
	}
}

const char *ToString(Renderer v)
{
	switch (v) {
	case Renderer::D3D11:
		return "d3d11";
	case Renderer::OpenGL:
		return "opengl";
	case Renderer::Metal:
		return "metal";
	default:
		return "other";
	}
}

const char *ToString(GpuVendor v)
{
	switch (v) {
	case GpuVendor::Nvidia:
		return "nvidia";
	case GpuVendor::Amd:
		return "amd";
	case GpuVendor::Intel:
		return "intel";
	case GpuVendor::Apple:
		return "apple";
	case GpuVendor::Other:
		return "other";
	default:
		return "unknown";
	}
}

const char *ToString(StreamEncoder v)
{
	switch (v) {
	case StreamEncoder::X264:
		return "x264";
	case StreamEncoder::Nvenc:
		return "nvenc";
	case StreamEncoder::Qsv:
		return "qsv";
	case StreamEncoder::Amf:
		return "amf";
	case StreamEncoder::AppleVt:
		return "apple_vt";
	case StreamEncoder::Other:
		return "other";
	default:
		return "none";
	}
}

const char *ToString(DelayState v)
{
	switch (v) {
	case DelayState::Filling:
		return "filling";
	case DelayState::Active:
		return "active";
	case DelayState::Error:
		return "error";
	default:
		return "inactive";
	}
}

const char *ToString(LoadCause v)
{
	switch (v) {
	case LoadCause::Graphics:
		return "graphics";
	case LoadCause::Encode:
		return "encode";
	case LoadCause::Decode:
		return "decode";
	default:
		return "none";
	}
}

const char *ToString(ErrorCode v)
{
	switch (v) {
	case ErrorCode::LiveSceneMissing:
		return "live_scene_missing";
	case ErrorCode::SceneCollectionChanged:
		return "scene_collection_changed";
	case ErrorCode::RamInsufficient:
		return "ram_insufficient";
	case ErrorCode::EncodingOverloaded:
		return "encoding_overloaded";
	case ErrorCode::BufferDegraded:
		return "buffer_degraded";
	case ErrorCode::FillStalled:
		return "fill_stalled";
	case ErrorCode::FilterRemoved:
		return "filter_removed";
	case ErrorCode::ProgramSwitchedToLive:
		return "program_switched_to_live";
	case ErrorCode::GraphicsInitFailed:
		return "graphics_init_failed";
	case ErrorCode::CodecInitFailed:
		return "codec_init_failed";
	default:
		return "other";
	}
}

} // namespace trigglow::diag
