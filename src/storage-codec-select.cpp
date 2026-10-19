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

#include "storage-codec-select.hpp"

#include <algorithm>
#include <cctype>

namespace trigglow::storage {

namespace {
bool ContainsI(std::string_view haystack, std::string_view needle)
{
	if (needle.empty() || needle.size() > haystack.size())
		return false;
	for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		bool match = true;
		for (size_t k = 0; k < needle.size() && match; ++k) {
			const auto a = static_cast<unsigned char>(haystack[i + k]);
			const auto b = static_cast<unsigned char>(needle[k]);
			match = std::tolower(a) == std::tolower(b);
		}
		if (match)
			return true;
	}
	return false;
}

struct Known {
	const char *encoder;
	CodecId codec;
	Backend backend;
	GpuVendor vendor;
};

// The order a setup tries them: HEVC before H.264 (smaller for the same
// picture), NVENC, AMF, then Quick Sync within each.
constexpr Known kCandidates[] = {
	{"hevc_nvenc", CodecId::Hevc, Backend::Nvenc, GpuVendor::Nvidia},
	{"hevc_amf", CodecId::Hevc, Backend::Amf, GpuVendor::Amd},
	{"hevc_qsv", CodecId::Hevc, Backend::Qsv, GpuVendor::Intel},
	{"h264_nvenc", CodecId::H264, Backend::Nvenc, GpuVendor::Nvidia},
	{"h264_amf", CodecId::H264, Backend::Amf, GpuVendor::Amd},
	{"h264_qsv", CodecId::H264, Backend::Qsv, GpuVendor::Intel},
};
} // namespace

GpuVendor VendorFromPciId(uint32_t vendorId)
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
		return GpuVendor::Intel;
	default:
		return GpuVendor::Other;
	}
}

const char *VendorName(GpuVendor vendor)
{
	switch (vendor) {
	case GpuVendor::Unknown:
		return "unknown";
	case GpuVendor::Nvidia:
		return "nvidia";
	case GpuVendor::Amd:
		return "amd";
	case GpuVendor::Intel:
		return "intel";
	case GpuVendor::Other:
		return "other";
	}
	return "?";
}

const char *FamilyName(EncoderFamily family)
{
	switch (family) {
	case EncoderFamily::None:
		return "none";
	case EncoderFamily::Nvenc:
		return "nvenc";
	case EncoderFamily::Amf:
		return "amf";
	case EncoderFamily::Qsv:
		return "qsv";
	case EncoderFamily::Cpu:
		return "cpu";
	case EncoderFamily::Other:
		return "other";
	}
	return "?";
}

EncoderFamily FamilyFromObsEncoderId(std::string_view id)
{
	if (id.empty())
		return EncoderFamily::None;
	if (ContainsI(id, "nvenc"))
		return EncoderFamily::Nvenc;
	if (ContainsI(id, "amf"))
		return EncoderFamily::Amf;
	if (ContainsI(id, "qsv"))
		return EncoderFamily::Qsv;
	if (ContainsI(id, "x264") || ContainsI(id, "x265") || ContainsI(id, "svt_av1") || ContainsI(id, "aom_av1") ||
	    ContainsI(id, "openh264"))
		return EncoderFamily::Cpu;
	return EncoderFamily::Other;
}

EncoderFamily FamilyOf(Backend backend)
{
	switch (backend) {
	case Backend::Nvenc:
		return EncoderFamily::Nvenc;
	case Backend::Amf:
		return EncoderFamily::Amf;
	case Backend::Qsv:
		return EncoderFamily::Qsv;
	case Backend::Cpu:
		return EncoderFamily::Cpu;
	case Backend::D3d12va:
		return EncoderFamily::Other;
	}
	return EncoderFamily::Other;
}

GpuVendor VendorOf(Backend backend)
{
	switch (backend) {
	case Backend::Nvenc:
		return GpuVendor::Nvidia;
	case Backend::Amf:
		return GpuVendor::Amd;
	case Backend::Qsv:
		return GpuVendor::Intel;
	case Backend::Cpu:
	case Backend::D3d12va:
		break;
	}
	return GpuVendor::Unknown;
}

const char *BufferCodecName(BufferCodecMode mode)
{
	return mode == BufferCodecMode::Mjpeg ? "mjpeg" : "auto";
}

BufferCodecSetting ParseBufferCodec(SettingType type, std::string_view text)
{
	BufferCodecSetting setting;
	if (type == SettingType::Missing)
		return setting;
	if (type == SettingType::String && text == "auto")
		return setting;
	if (type == SettingType::String && text == "mjpeg") {
		setting.mode = BufferCodecMode::Mjpeg;
		return setting;
	}
	setting.warn = true;
	return setting;
}

uint32_t CountFamily(const std::vector<EncoderFamily> &families, EncoderFamily family)
{
	return static_cast<uint32_t>(std::count(families.begin(), families.end(), family));
}

std::vector<HwCandidate> RankCandidates(const RankInput &in)
{
	std::vector<HwCandidate> out;
	if (in.mode != BufferCodecMode::Auto || !in.workers || !in.platformSupported)
		return out;
	const bool nvencAllowed = !in.obsEncodersKnown ||
				  CountFamily(in.obsEncoders, EncoderFamily::Nvenc) <= kMaxObsNvencForBuffer;
	for (const Known &known : kCandidates) {
		if (std::find(in.vendors.begin(), in.vendors.end(), known.vendor) == in.vendors.end())
			continue;
		if (known.backend == Backend::Nvenc && !nvencAllowed)
			continue;
		if (known.backend == Backend::Qsv && !in.quickSyncAllowed)
			continue;
		if (std::find(in.blacklist.begin(), in.blacklist.end(), known.encoder) != in.blacklist.end())
			continue;
		HwCandidate candidate;
		candidate.encoder = known.encoder;
		candidate.codec = known.codec;
		candidate.backend = known.backend;
		candidate.vendor = known.vendor;
		candidate.sharesEngine = !in.obsEncodersKnown ||
					 CountFamily(in.obsEncoders, FamilyOf(known.backend)) > 0;
		out.push_back(candidate);
	}
	std::stable_partition(out.begin(), out.end(), [](const HwCandidate &c) { return !c.sharesEngine; });
	return out;
}

EngineShare ComputeEngineShare(Backend bufferEncoder, bool hardwareDecode, GpuVendor decodeVendor, bool obsKnown,
			       const std::vector<EncoderFamily> &obsEncoders)
{
	EngineShare share;
	share.known = obsKnown;
	if (!obsKnown)
		return share;
	const EncoderFamily family = FamilyOf(bufferEncoder);
	share.encode = family != EncoderFamily::Cpu && family != EncoderFamily::Other &&
		       CountFamily(obsEncoders, family) > 0;
	if (hardwareDecode) {
		// AMD VCN and Intel's media engine decode on the engine that encodes;
		// NVIDIA's NVDEC is a separate one.
		if (decodeVendor == GpuVendor::Amd)
			share.decode = CountFamily(obsEncoders, EncoderFamily::Amf) > 0;
		else if (decodeVendor == GpuVendor::Intel)
			share.decode = CountFamily(obsEncoders, EncoderFamily::Qsv) > 0;
	}
	return share;
}

} // namespace trigglow::storage
