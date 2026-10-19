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

#include "storage-codec.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Which storage codec the delay buffer may use (v0.7.0 RAM design 2.7, 4.1
// and 5): the hidden "buffer_codec" setting, the families of OBS's own video
// encoders, and the ranking of the hardware encoders worth probing. Pure, no
// OBS, no FFmpeg: tests/storage_codec_tests.cpp covers it.
namespace trigglow::storage {

enum class GpuVendor : uint8_t { Unknown, Nvidia, Amd, Intel, Other };

// PCI vendor id (DXGI_ADAPTER_DESC1::VendorId): 0x10DE NVIDIA, 0x1002/0x1022 AMD, 0x8086 Intel.
GpuVendor VendorFromPciId(uint32_t vendorId);
const char *VendorName(GpuVendor vendor);

// The engine family of an encoder: NVENC, AMD VCN (AMF), Intel Quick Sync, or the CPU.
enum class EncoderFamily : uint8_t { None, Nvenc, Amf, Qsv, Cpu, Other };

const char *FamilyName(EncoderFamily family);
// From obs_encoder_get_id(): obs_nvenc_*, jim_*nvenc and ffmpeg_*nvenc are
// NVENC; *_texture_amf and amd_amf_* AMF; obs_qsv11* Quick Sync; x264, SVT-AV1,
// AOM and OpenH264 the CPU; anything else Other ("" is None).
EncoderFamily FamilyFromObsEncoderId(std::string_view id);
EncoderFamily FamilyOf(Backend backend);
GpuVendor VendorOf(Backend backend);

// The hidden "buffer_codec" key of buffer-mode-settings.json (design 5):
// "auto" (the default) lets the buffer use a hardware inter-frame codec;
// "mjpeg" forces MJPEG.
enum class BufferCodecMode : uint8_t { Auto, Mjpeg };

const char *BufferCodecName(BufferCodecMode mode);

// How the key came out of the JSON file. libobs drops a JSON null on load,
// so null reads as Missing.
enum class SettingType : uint8_t { Missing, String, Other };

struct BufferCodecSetting {
	BufferCodecMode mode = BufferCodecMode::Auto;
	bool warn = false; // Present but neither "auto" nor "mjpeg": kept at auto, with a WARN.
};

// Strict, like the other hidden switches: only the exact lowercase strings
// "auto" and "mjpeg" count; anything else ("MJPEG", "hevc", false, 0) stays
// at auto and asks for a WARN, so a typo never changes the codec silently.
BufferCodecSetting ParseBufferCodec(SettingType type, std::string_view text);

// A hardware encoder worth probing, in the order the setup thread tries them.
struct HwCandidate {
	const char *encoder = ""; // FFmpeg encoder name: "hevc_nvenc"...
	CodecId codec = CodecId::Hevc;
	Backend backend = Backend::Cpu;
	GpuVendor vendor = GpuVendor::Unknown;
	bool sharesEngine = true; // OBS encodes on the same engine (or that's unknown).
};

// NVENC is only used while OBS itself has at most this many NVENC encoders
// running: one session stays free even with the old 5-session limit.
constexpr uint32_t kMaxObsNvencForBuffer = 3;

struct RankInput {
	BufferCodecMode mode = BufferCodecMode::Auto;
	bool workers = true;                    // Codec worker threads (codec_workers): hardware needs them.
	bool platformSupported = true;          // v0.7.0: Windows only.
	std::vector<GpuVendor> vendors;         // The GPUs present: no probing a vendor that isn't there.
	bool obsEncodersKnown = false;          // Whether obsEncoders below is real.
	std::vector<EncoderFamily> obsEncoders; // One per active OBS video encoder.
	std::vector<std::string> blacklist;     // Encoders that failed hard this session.
	// false: no Quick Sync candidate (OBS runs elevated, see Provider::Setup()).
	bool quickSyncAllowed = true;
};

uint32_t CountFamily(const std::vector<EncoderFamily> &families, EncoderFamily family);

// Design D4: hevc_nvenc, hevc_amf, hevc_qsv, then the h264 ones, keeping only
// the vendors present, NVENC only under kMaxObsNvencForBuffer, Quick Sync only
// when quickSyncAllowed, nothing blacklisted; engines OBS doesn't encode on
// first (an iGPU, say), each group in that order. Empty with buffer_codec=mjpeg, without workers, or off
// Windows: MJPEG then.
std::vector<HwCandidate> RankCandidates(const RankInput &in);

// Whether the buffer's hardware codec shares an engine with OBS's encoders
// (design 4.1): its encoder is of the same family as one of them, or its
// decode runs on an AMD or Intel GPU whose video engine also encodes for OBS
// (NVIDIA decodes on NVDEC, apart from NVENC). Unknown OBS encoders count as
// shared.
struct EngineShare {
	bool known = false;
	bool encode = false;
	bool decode = false;
	bool Any() const { return !known || encode || decode; }
};

EngineShare ComputeEngineShare(Backend bufferEncoder, bool hardwareDecode, GpuVendor decodeVendor, bool obsKnown,
			       const std::vector<EncoderFamily> &obsEncoders);

} // namespace trigglow::storage
