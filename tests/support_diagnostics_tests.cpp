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

// Tests for src/support-diagnostics.*: the optional technical data sent with
// a Trigi conversation holds only the schema's fields, with the schema's
// types and ranges, and stays within 1 KB.

#include "support-diagnostics.hpp"
#include "test-support.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>

using namespace trigglow;
using namespace trigglow::diag;

namespace {

const std::set<std::string> kTopKeys = {"v",
					"obs",
					"osVersion",
					"arch",
					"renderer",
					"gpuVendor",
					"ramTotalGb",
					"streamEncoder",
					"state",
					"delayS",
					"effectiveDelayS",
					"minHeight",
					"canvas",
					"output",
					"fps",
					"liveSceneSet",
					"loadingSceneSet",
					"outputCap",
					"codecWorkers",
					"obsLaggedPct",
					"obsSkippedPct",
					"pipeline",
					"recentErrors"};
const std::set<std::string> kPipelineKeys = {"running", "level",         "maxLevel",       "buf",
					     "full",    "captureFps",    "encodeContexts", "encodeSlices",
					     "cause",   "droppedPerMin", "repeatedPerMin"};
const std::map<std::string, std::set<std::string>> kEnums = {
	{"osVersion", {"windows-10", "windows-11", "windows-other", "macos", "linux"}},
	{"arch", {"x64", "arm64", "other"}},
	{"renderer", {"d3d11", "opengl", "metal", "other"}},
	{"gpuVendor", {"nvidia", "amd", "intel", "apple", "other", "unknown"}},
	{"streamEncoder", {"x264", "nvenc", "qsv", "amf", "apple_vt", "other", "none"}},
	{"state", {"inactive", "filling", "active", "error"}},
	{"cause", {"none", "graphics", "encode", "decode"}},
};
const std::set<std::string> kErrorCodes = {"live_scene_missing",
					   "scene_collection_changed",
					   "ram_insufficient",
					   "encoding_overloaded",
					   "buffer_degraded",
					   "fill_stalled",
					   "filter_removed",
					   "program_switched_to_live",
					   "graphics_init_failed",
					   "codec_init_failed",
					   "other"};

bool HasAtMostTwoDecimals(double v)
{
	const double scaled = v * 100.0;
	return std::fabs(scaled - std::round(scaled)) < 1e-6;
}

// Walks the object: only known keys, only numbers/booleans/known enum
// strings/size objects, every number with at most 2 decimals.
bool ConformsToSchema(const json::Value &v)
{
	if (!v.IsObject())
		return false;
	for (size_t i = 0; i < v.Keys().size(); ++i) {
		const std::string &key = v.Keys()[i];
		const json::Value &value = v.Values()[i];
		if (!kTopKeys.count(key))
			return false;
		if (value.IsString()) {
			auto it = kEnums.find(key);
			if (it == kEnums.end() || !it->second.count(value.AsString()))
				return false;
		} else if (value.IsNumber()) {
			if (!HasAtMostTwoDecimals(value.AsNumber()) || value.AsNumber() < 0)
				return false;
		} else if (value.IsObject() && (key == "canvas" || key == "output")) {
			if (value.Size() != 2 || !value.GetNumber("w") || !value.GetNumber("h"))
				return false;
		} else if (value.IsObject() && key == "pipeline") {
			for (size_t k = 0; k < value.Keys().size(); ++k) {
				const std::string &pk = value.Keys()[k];
				const json::Value &pv = value.Values()[k];
				if (!kPipelineKeys.count(pk))
					return false;
				if (pv.IsString() && (pk != "cause" || !kEnums.at("cause").count(pv.AsString())))
					return false;
				if (pv.IsNumber() && !HasAtMostTwoDecimals(pv.AsNumber()))
					return false;
				if (pv.IsArray() || pv.IsNull())
					return false;
			}
		} else if (value.IsArray() && key == "obs") {
			if (value.Size() != 3)
				return false;
			for (size_t k = 0; k < 3; ++k) {
				if (!value.At(k).IsNumber() || value.At(k).AsNumber() > 999)
					return false;
			}
		} else if (value.IsArray() && key == "recentErrors") {
			if (value.Size() > kMaxRecentErrors)
				return false;
			for (size_t k = 0; k < value.Size(); ++k) {
				if (!value.At(k).IsString() || !kErrorCodes.count(value.At(k).AsString()))
					return false;
			}
		} else if (!value.IsBool()) {
			return false;
		}
	}
	return true;
}

Snapshot Typical()
{
	Snapshot s;
	s.obs = {31, 0, 2};
	s.os = OsVersion::Windows11;
	s.arch = Arch::X64;
	s.renderer = Renderer::D3D11;
	s.gpuVendor = GpuVendor::Nvidia;
	s.ramTotalBytes = 34359738368ULL - 300000000ULL; // ~31.7 GiB -> 32
	s.streamEncoder = StreamEncoder::Nvenc;
	s.state = DelayState::Active;
	s.delaySeconds = 30;
	s.effectiveDelaySeconds = 28;
	s.minHeight = 720;
	s.canvas = {2560, 1440};
	s.output = {1920, 1080};
	s.fps = 60.0 * 1000.0 / 1001.0; // 59.94...
	s.liveSceneSet = true;
	s.loadingSceneSet = false;
	return s;
}

void TestTypical()
{
	testcheck::g_currentTest = "typical snapshot";
	const std::string text = SerializeV1(Typical());
	const auto v = json::Parse(text);
	CHECK(v.has_value());
	CHECK(ConformsToSchema(*v));
	CHECK_STR(text,
		  "{\"v\":1,\"obs\":[31,0,2],\"osVersion\":\"windows-11\",\"arch\":\"x64\",\"renderer\":\"d3d11\","
		  "\"gpuVendor\":\"nvidia\",\"ramTotalGb\":32,\"streamEncoder\":\"nvenc\",\"state\":\"active\","
		  "\"delayS\":30,\"effectiveDelayS\":28,\"minHeight\":720,\"canvas\":{\"w\":2560,\"h\":1440},"
		  "\"output\":{\"w\":1920,\"h\":1080},\"fps\":59.94,\"liveSceneSet\":true,\"loadingSceneSet\":false,"
		  "\"outputCap\":true,\"codecWorkers\":true,\"recentErrors\":[]}");
	// Optional fields are left out when unknown.
	CHECK(!v->Has("pipeline"));
	CHECK(!v->Has("obsLaggedPct"));
	// The pretty preview holds exactly the same data.
	const auto pretty = json::Parse(SerializeV1(Typical(), true));
	CHECK(pretty.has_value() && json::Serialize(*pretty) == text);
}

void TestClampingAndLimits()
{
	testcheck::g_currentTest = "clamping and limits";
	Snapshot s = Typical();
	s.obs = {1234, 5, 70000};
	s.ramTotalBytes = ~0ULL;
	s.delaySeconds = 99999;
	s.effectiveDelaySeconds = 4000000000u;
	s.minHeight = 99999;
	s.canvas = {99999, 0};
	s.output = {16385, 16384};
	s.fps = 1000.0;
	s.obsLaggedPct = 123.456;
	s.obsSkippedPct = -5.0;
	PipelineSnapshot p;
	p.running = true;
	p.level = 40;
	p.maxLevel = 17;
	p.buf = {1280, 720};
	p.full = {1920, 1080};
	p.captureFps = 29.999;
	p.encodeContexts = 9;
	p.encodeSlices = 99;
	p.cause = LoadCause::Encode;
	p.droppedPerMin = 5e9;
	p.repeatedPerMin = NAN;
	s.pipeline = p;
	s.recentErrors = {ErrorCode::EncodingOverloaded, ErrorCode::EncodingOverloaded,
			  ErrorCode::BufferDegraded,     ErrorCode::FillStalled,
			  ErrorCode::RamInsufficient,    ErrorCode::FilterRemoved,
			  ErrorCode::LiveSceneMissing,   ErrorCode::Other};

	const auto v = json::Parse(SerializeV1(s));
	CHECK(v.has_value());
	CHECK(ConformsToSchema(*v));
	const json::Value &obs = *v->Find("obs");
	CHECK(obs.At(0).AsNumber() == 999 && obs.At(1).AsNumber() == 5 && obs.At(2).AsNumber() == 999);
	CHECK(v->GetNumber("ramTotalGb").value_or(-1) == 4096);
	CHECK(v->GetNumber("delayS").value_or(-1) == 600);
	CHECK(v->GetNumber("effectiveDelayS").value_or(-1) == 600);
	CHECK(v->GetNumber("minHeight").value_or(-1) == 4320);
	CHECK(v->Find("canvas")->GetNumber("w").value_or(-1) == 16384);
	CHECK(v->Find("output")->GetNumber("w").value_or(-1) == 16384);
	CHECK(v->GetNumber("fps").value_or(-1) == 240);
	CHECK(v->GetNumber("obsLaggedPct").value_or(-1) == 100);
	CHECK(v->GetNumber("obsSkippedPct").value_or(-1) == 0);
	const json::Value &pv = *v->Find("pipeline");
	CHECK(pv.GetNumber("level").value_or(-1) == 16);
	CHECK(pv.GetNumber("maxLevel").value_or(-1) == 16);
	CHECK(pv.GetNumber("captureFps").value_or(-1) == 30);
	CHECK(pv.GetNumber("encodeContexts").value_or(-1) == 8);
	CHECK(pv.GetNumber("encodeSlices").value_or(-1) == 16);
	CHECK(pv.GetNumber("droppedPerMin").value_or(-1) == 1e6);
	CHECK(pv.GetNumber("repeatedPerMin").value_or(-1) == 0);
	CHECK_STR(pv.GetString("cause").value_or(""), "encode");
	// Duplicates dropped, at most 5, in order.
	const json::Value &errors = *v->Find("recentErrors");
	CHECK(errors.Size() == 5);
	CHECK_STR(errors.At(0).AsString(), "encoding_overloaded");
	CHECK_STR(errors.At(1).AsString(), "buffer_degraded");
	CHECK_STR(errors.At(4).AsString(), "filter_removed");
}

void TestWorstCaseSize()
{
	testcheck::g_currentTest = "worst-case size";
	Snapshot s;
	s.obs = {999, 999, 999};
	s.os = OsVersion::WindowsOther;
	s.ramTotalBytes = 4096ULL << 30;
	s.streamEncoder = StreamEncoder::AppleVt;
	s.delaySeconds = s.effectiveDelaySeconds = 600;
	s.minHeight = 4320;
	s.canvas = s.output = {16384, 16384};
	s.fps = 239.99;
	s.obsLaggedPct = 99.99;
	s.obsSkippedPct = 99.99;
	PipelineSnapshot p;
	p.level = p.maxLevel = 16;
	p.buf = p.full = {16384, 16384};
	p.captureFps = 239.99;
	p.encodeContexts = 8;
	p.encodeSlices = 16;
	p.cause = LoadCause::Graphics;
	p.droppedPerMin = p.repeatedPerMin = 999999.99;
	s.pipeline = p;
	s.recentErrors = {ErrorCode::ProgramSwitchedToLive, ErrorCode::SceneCollectionChanged,
			  ErrorCode::GraphicsInitFailed, ErrorCode::EncodingOverloaded, ErrorCode::LiveSceneMissing};
	const std::string text = SerializeV1(s);
	std::printf("  worst case: %zu bytes\n", text.size());
	CHECK(text.size() <= kMaxSerializedBytes);
	const auto v = json::Parse(text);
	CHECK(v && ConformsToSchema(*v));
	CHECK(v && v->Find("recentErrors")->Size() == 5); // Fits without dropping anything.
}

void TestMappings()
{
	testcheck::g_currentTest = "mappings";
	CHECK(StreamEncoderFromObsId("") == StreamEncoder::None);
	CHECK(StreamEncoderFromObsId("obs_x264") == StreamEncoder::X264);
	CHECK(StreamEncoderFromObsId("jim_nvenc") == StreamEncoder::Nvenc);
	CHECK(StreamEncoderFromObsId("obs_nvenc_hevc_tex") == StreamEncoder::Nvenc);
	CHECK(StreamEncoderFromObsId("ffmpeg_nvenc") == StreamEncoder::Nvenc);
	CHECK(StreamEncoderFromObsId("obs_qsv11_v2") == StreamEncoder::Qsv);
	CHECK(StreamEncoderFromObsId("h264_texture_amf") == StreamEncoder::Amf);
	CHECK(StreamEncoderFromObsId("av1_texture_amf") == StreamEncoder::Amf);
	CHECK(StreamEncoderFromObsId("com.apple.videotoolbox.videoencoder.ave.avc") == StreamEncoder::AppleVt);
	CHECK(StreamEncoderFromObsId("ffmpeg_svt_av1") == StreamEncoder::Other);

	CHECK(GpuVendorFromPciId(0x10DE) == GpuVendor::Nvidia);
	CHECK(GpuVendorFromPciId(0x1002) == GpuVendor::Amd);
	CHECK(GpuVendorFromPciId(0x8086) == GpuVendor::Intel);
	CHECK(GpuVendorFromPciId(0x106B) == GpuVendor::Apple);
	CHECK(GpuVendorFromPciId(0x1414) == GpuVendor::Other); // Microsoft Basic Render Driver.
	CHECK(GpuVendorFromPciId(0) == GpuVendor::Unknown);

	CHECK(WindowsVersionFromBuild(10, 19045) == OsVersion::Windows10);
	CHECK(WindowsVersionFromBuild(10, 22000) == OsVersion::Windows11);
	CHECK(WindowsVersionFromBuild(10, 26100) == OsVersion::Windows11);
	CHECK(WindowsVersionFromBuild(6, 9600) == OsVersion::WindowsOther);

	CHECK(RendererFromGraphicsModule("libobs-d3d11") == Renderer::D3D11);
	CHECK(RendererFromGraphicsModule("libobs-opengl") == Renderer::OpenGL);
	CHECK(RendererFromGraphicsModule("libobs-metal") == Renderer::Metal);
	CHECK(RendererFromGraphicsModule("") == Renderer::Other);

	const auto version = ObsVersionFromPacked((31u << 24) | (1u << 16) | 2u);
	CHECK(version[0] == 31 && version[1] == 1 && version[2] == 2);

	PipelineHealth h;
	h.running = true;
	h.level = 2;
	h.maxLevel = 5;
	h.width = 1280;
	h.height = 720;
	h.fullWidth = 1920;
	h.fullHeight = 1080;
	h.captureFps = 30;
	h.encodeContexts = 2;
	h.encodeSlices = 4;
	h.cause = PipelineLoad::Decode;
	h.droppedPerMinute = 1.5;
	h.repeatedPerMinute = 0.25;
	const PipelineSnapshot p = FromPipelineHealth(h);
	CHECK(p.running && p.level == 2 && p.maxLevel == 5 && p.buf.w == 1280 && p.full.h == 1080);
	CHECK(p.cause == LoadCause::Decode && p.droppedPerMin == 1.5 && p.repeatedPerMin == 0.25);
	CHECK(CurrentArch() == Arch::X64 || CurrentArch() == Arch::Arm64 || CurrentArch() == Arch::Other);
}

} // namespace

int main()
{
	TestTypical();
	TestClampingAndLimits();
	TestWorstCaseSize();
	TestMappings();
	return testcheck::Finish("support_diagnostics_tests");
}
