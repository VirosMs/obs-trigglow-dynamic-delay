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

#include "annexb.hpp"

namespace trigglow::annexb {

namespace {
// H.264 (ITU-T H.264 table 7-1) and HEVC (H.265 table 7-1) NAL unit types.
constexpr uint8_t kH264Sps = 7;
constexpr uint8_t kH264Pps = 8;
constexpr uint8_t kHevcVps = 32;
constexpr uint8_t kHevcSps = 33;
constexpr uint8_t kHevcPps = 34;

bool Inter(storage::CodecId codec)
{
	return codec == storage::CodecId::H264 || codec == storage::CodecId::Hevc;
}
} // namespace

uint8_t NalType(storage::CodecId codec, uint8_t header)
{
	return codec == storage::CodecId::Hevc ? static_cast<uint8_t>((header >> 1) & 0x3F)
					       : static_cast<uint8_t>(header & 0x1F);
}

bool IsParameterSet(storage::CodecId codec, uint8_t type)
{
	if (codec == storage::CodecId::Hevc)
		return type == kHevcVps || type == kHevcSps || type == kHevcPps;
	if (codec == storage::CodecId::H264)
		return type == kH264Sps || type == kH264Pps;
	return false;
}

bool IsSlice(storage::CodecId codec, uint8_t type)
{
	if (codec == storage::CodecId::Hevc)
		return type <= 31; // VCL NAL units.
	if (codec == storage::CodecId::H264)
		return type >= 1 && type <= 5;
	return false;
}

std::vector<Nal> Split(storage::CodecId codec, const uint8_t *data, size_t bytes)
{
	std::vector<Nal> nals;
	if (!data || bytes < 4)
		return nals;
	size_t i = 0;
	bool open = false;
	while (i + 2 < bytes) {
		if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
			++i;
			continue;
		}
		// A start code at i (00 00 01), or at i - 1 (00 00 00 01).
		const size_t begin = i > 0 && data[i - 1] == 0 ? i - 1 : i;
		if (open)
			nals.back().end = begin;
		const size_t header = i + 3;
		if (header >= bytes) {
			// Truncated: a start code and nothing after it.
			if (open)
				nals.back().end = begin;
			return nals;
		}
		Nal nal;
		nal.begin = begin;
		nal.data = header;
		nal.end = bytes;
		nal.type = NalType(codec, data[header]);
		nals.push_back(nal);
		open = true;
		i = header;
	}
	return nals;
}

bool HasInBandParameterSets(storage::CodecId codec, const uint8_t *data, size_t bytes)
{
	if (!Inter(codec))
		return false;
	bool vps = codec != storage::CodecId::Hevc; // H.264 has no VPS.
	bool sps = false;
	bool pps = false;
	for (const Nal &nal : Split(codec, data, bytes)) {
		if (IsSlice(codec, nal.type))
			break;
		if (codec == storage::CodecId::Hevc) {
			vps = vps || nal.type == kHevcVps;
			sps = sps || nal.type == kHevcSps;
			pps = pps || nal.type == kHevcPps;
		} else {
			sps = sps || nal.type == kH264Sps;
			pps = pps || nal.type == kH264Pps;
		}
	}
	return vps && sps && pps;
}

std::vector<uint8_t> ExtractParameterSets(storage::CodecId codec, const uint8_t *data, size_t bytes)
{
	std::vector<uint8_t> out;
	if (!HasInBandParameterSets(codec, data, bytes))
		return out;
	for (const Nal &nal : Split(codec, data, bytes)) {
		if (IsSlice(codec, nal.type))
			break;
		if (!IsParameterSet(codec, nal.type))
			continue;
		// Always a 4-byte start code in front, whatever the packet used.
		static constexpr uint8_t kStartCode[4] = {0, 0, 0, 1};
		out.insert(out.end(), kStartCode, kStartCode + 4);
		out.insert(out.end(), data + nal.data, data + nal.end);
	}
	return out;
}

bool PrependParameterSets(storage::CodecId codec, const std::vector<uint8_t> &parameterSets, const uint8_t *data,
			  size_t bytes, std::vector<uint8_t> &out)
{
	if (parameterSets.empty() || !data || bytes == 0 || HasInBandParameterSets(codec, data, bytes))
		return false;
	out.clear();
	out.reserve(parameterSets.size() + bytes);
	out.insert(out.end(), parameterSets.begin(), parameterSets.end());
	out.insert(out.end(), data, data + bytes);
	return true;
}

} // namespace trigglow::annexb
