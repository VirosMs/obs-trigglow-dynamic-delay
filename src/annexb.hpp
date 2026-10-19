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

#include <cstddef>
#include <cstdint>
#include <vector>

// Annex B byte streams (H.264 and HEVC as hardware encoders hand them out: NAL
// units behind 00 00 01 or 00 00 00 01 start codes), for the delay buffer's
// inter-frame storage codec (v0.7.0, hw-storage-codec.hpp). Pure: no FFmpeg, so
// tests/storage_codec_tests.cpp covers it.
//
// Why: every key the buffer stores must start a decoder that has never seen
// the stream (a decoder reopened, the software one after a hardware failure),
// so its parameter sets (H.264 SPS/PPS, HEVC VPS/SPS/PPS) have to travel with
// it, in band. Encoders are opened without a global header, which normally
// gives exactly that; the setup probe checks it on two keys and, for an
// encoder that only writes them on the first one, the hardware codec puts a
// copy of that first key's parameter sets in front of every later key.
namespace trigglow::annexb {

// The NAL unit type of a NAL whose first byte (the header) is `header`.
uint8_t NalType(storage::CodecId codec, uint8_t header);
// H.264 SPS/PPS (7/8), HEVC VPS/SPS/PPS (32/33/34).
bool IsParameterSet(storage::CodecId codec, uint8_t type);
// A coded slice (H.264 1-5, HEVC 0-31): what a key's parameter sets must come before.
bool IsSlice(storage::CodecId codec, uint8_t type);

struct Nal {
	size_t begin = 0; // Its start code.
	size_t data = 0;  // Its header byte.
	size_t end = 0;   // One past its last byte (the next start code, or the packet's end).
	uint8_t type = 0;
};

// Every NAL of the packet, in order. Bytes before the first start code are
// skipped; a start code with nothing after it ends the walk (a truncated packet).
std::vector<Nal> Split(storage::CodecId codec, const uint8_t *data, size_t bytes);

// Whether every parameter set a decoder needs (H.264: SPS and PPS; HEVC: VPS,
// SPS and PPS) comes before the packet's first slice. False for anything but
// H.264 and HEVC, and for an empty or truncated packet.
bool HasInBandParameterSets(storage::CodecId codec, const uint8_t *data, size_t bytes);

// The packet's parameter-set NALs, each with its start code, in order: what
// PrependParameterSets() puts in front of a key. Empty unless the packet
// HasInBandParameterSets().
std::vector<uint8_t> ExtractParameterSets(storage::CodecId codec, const uint8_t *data, size_t bytes);

// `out` = parameterSets + packet, for a packet that lacks its own (false:
// it has them, or parameterSets is empty, and `out` is left alone).
bool PrependParameterSets(storage::CodecId codec, const std::vector<uint8_t> &parameterSets, const uint8_t *data,
			  size_t bytes, std::vector<uint8_t> &out);

} // namespace trigglow::annexb
