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

#include "hardware-info.hpp"
#include "storage-codec-select.hpp"
#include "storage-codec.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The delay buffer's hardware inter-frame storage codec (v0.7.0 RAM design
// 2.3, 2.7, D3/D4), Windows only (TRIGGLOW_HAVE_HW_STORAGE), over the
// vendored FFmpeg: no new DLL, the encoders come with avcodec and their
// runtimes with the GPU driver. No OBS dependency, so the benchmarks link it.
//
// Encoders, in RankCandidates() order: hevc_nvenc, hevc_amf, hevc_qsv, then
// the h264 ones. Constant QP (HEVC 20, H.264 18), a key every half second, no
// B-frames, the fastest preset, no lookahead, parameter sets in band on every
// key (checked by the probe; copied in front of later keys when an encoder
// only writes them once). Frames come in system memory, I420 (NV12 for Quick
// Sync, which only takes that); NVENC and AMF run on a D3D11 device of their
// GPU's adapter. pts is a counter of the encoder's own, matched back to the
// pipeline's seq (no B-frames: packets come out in order).
//
// Decoders: D3D11VA on OBS's own adapter (obs_video_info::adapter), low
// delay, the picture downloaded as NV12 and split into I420 on the decode
// worker; libavcodec's software HEVC/H.264 for the emergency path. On NVIDIA
// the decode runs on NVDEC, apart from NVENC; on AMD and Intel it shares the
// video engine with the encode (what the codec policy weighs).
//
// The setup probe (Provider::Setup(), on the pipeline's setup thread, up to
// about a second per candidate): open at the capture size, 8 synthetic frames
// with keys forced at 0 and 5, every packet out in order within 500 ms, both
// keys with their parameter sets in band, a D3D11VA decode of 0-7 and, after
// a flush, of 5-7 again (a key starts a decoder), right sizes and PSNR-Y > 30
// dB (a garbage detector, not a quality bar). What passes is handed over open
// (the encoder to worker 0, the decoder to the decode worker); the session's
// registry remembers it, so a later frame size only reopens it with a short
// check. A failure isn't retried at that size; a hard error blacklists the
// encoder for the session.
namespace trigglow::hwstorage {

// The hardware path is worth trying this session: a candidate passed a probe
// and isn't blacklisted (VideoDelayFilter sizes the next cycle's ring for the
// inter class on it, design 6.4). Any thread.
bool RegistryHasWorkingEncoder();

struct ProviderSettings {
	uint32_t obsAdapterIndex = 0; // obs_video_info::adapter: where the decoder runs.
	uint32_t fpsNum = 60;         // The output frame rate (the encoders' time base and GOP).
	uint32_t fpsDen = 1;
	int hevcQp = 20;
	int h264Qp = 18;
	PipelineLogFn log;
	std::function<uint64_t()> clockNs;              // The pipeline's clock (drain deadlines).
	std::shared_ptr<storage::InterControl> control; // openEncoders bookkeeping. Optional.
	// Only these adapters are considered (tests and benchmarks); empty: QueryGpuAdapters().
	std::vector<GpuAdapterInfo> adapters;
	// Tests only (tests/hw_storage_codec_tests.cpp), empty in the plugin:
	// asked before each step that fails on real machines now and then --
	// "decoder-device" (the decoder's D3D11 device, lost after a TDR or out of
	// memory), "decoder-open" and "encoder-open" (avcodec_open2()) -- and
	// true makes that step fail the way FFmpeg would.
	std::function<bool(const char *step)> injectFailure;
};

class Provider {
public:
	explicit Provider(ProviderSettings settings);

	// Setup thread (CodecPipeline::Options::interSetup): the first candidate
	// that passes the probe at this frame size, open, with the decoder the
	// probe warmed up; an empty session if none does.
	storage::InterSession Setup(const storage::EncoderConfig &frame);
	// Decode worker (CodecPipeline::Options::decoderFactory and
	// softwareDecoderFactory): opened lazily on first use, on that thread.
	std::unique_ptr<storage::Decoder> MakeHardwareDecoder(storage::CodecId codec) const;
	std::unique_ptr<storage::Decoder> MakeSoftwareDecoder(storage::CodecId codec) const;

	// UI thread: the families of OBS's active video encoders (set_obs_encoders).
	void SetObsEncoders(bool known, std::vector<storage::EncoderFamily> families);
	// Graphics thread: OBS's frame rate (the encoders' time base and GOP), at
	// every new ring; the next setup uses it.
	void SetFrameRate(uint32_t fpsNum, uint32_t fpsDen);
	// Graphics thread: an encoder that failed hard is not probed again this session.
	void Blacklist(const std::string &encoder, const char *reason);

	bool AnyHardware() const { return !adapters_.empty(); }
	bool HasVendor(storage::GpuVendor vendor) const;
	storage::GpuVendor DecodeVendor() const;
	const std::string &DecodeAdapterName() const { return decodeAdapterName_; }
	uint32_t KeyframeInterval() const;
	int QpFor(storage::CodecId codec) const;
	// OBS's encoders as last reported: whether known, and the families.
	void ObsEncoders(bool &known, std::vector<storage::EncoderFamily> &families) const;

private:
	ProviderSettings settings_;
	std::vector<GpuAdapterInfo> adapters_;
	std::string decodeAdapterName_;
	mutable std::mutex mutex_; // Guards the OBS encoders snapshot and the frame rate.
	uint32_t fpsNum_ = 60;
	uint32_t fpsDen_ = 1;
	bool obsKnown_ = false;
	std::vector<storage::EncoderFamily> obsFamilies_;
};

} // namespace trigglow::hwstorage
