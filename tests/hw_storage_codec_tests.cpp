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

// Tests of the hardware storage codec (src/hw-storage-codec.*, Windows) over
// the real FFmpeg, with its failures injected (ProviderSettings::injectFailure)
// so they need no particular GPU: a decoder whose D3D11 device can't be
// created (a TDR, out of memory) or whose context won't open leaves nothing
// half-open behind -- before, Flush() right after the failed Decode() (what
// CodecPipeline::DecodePinned() does) crashed in avcodec_flush_buffers() on
// the allocated but unopened context. A setup whose every encoder fails to
// open hands back nothing and leaves no hardware session counted, and a
// setup cancelled before it starts opens nothing.
//
// Built only where the plugin's FFmpeg is already in .deps (tests/CMakeLists.txt).

#include "check.hpp"
#include "hw-storage-codec.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace trigglow;

namespace {

// A packet a decoder accepts as input (an HEVC/H.264 start code and a few
// bytes): what it holds doesn't matter, the decoder fails before or while
// decoding it.
storage::EncodedFrame SomePacket(std::vector<uint8_t> &bytes)
{
	bytes.assign(64, 0);
	bytes[3] = 1;
	bytes[4] = 0x40;
	bytes[5] = 0x01;
	storage::EncodedFrame in;
	in.data = bytes.data();
	in.bytes = bytes.size();
	in.width = 64;
	in.height = 64;
	in.seq = 0;
	in.key = true;
	return in;
}

hwstorage::ProviderSettings Settings(std::set<std::string> *failing)
{
	hwstorage::ProviderSettings settings;
	settings.log = [](PipelineLogLevel, const char *line) {
		std::printf("    log: %s\n", line);
	};
	settings.injectFailure = [failing](const char *step) {
		return failing && failing->count(step) > 0;
	};
	return settings;
}

// The hardware decoder of `codec` with `step` failing: every Decode() fails,
// it says it's broken, and Flush() -- before, after and between -- is safe.
void TestDecoderOpenFailure(storage::CodecId codec, const char *step)
{
	testcheck::g_currentTest = step;
	std::set<std::string> failing{step};
	hwstorage::Provider provider(Settings(&failing));
	std::unique_ptr<storage::Decoder> decoder = provider.MakeHardwareDecoder(codec);
	CHECK(decoder != nullptr);
	if (!decoder)
		return;
	decoder->Flush(); // Never opened yet: nothing to flush.
	std::vector<uint8_t> bytes;
	const storage::EncodedFrame in = SomePacket(bytes);
	DecodedImage image;
	uint64_t outSeq = 0;
	CHECK(decoder->Decode(in, true, image, outSeq) == storage::DecodeStatus::Failed);
	CHECK(decoder->Broken());
	CHECK(image.plane[0] == nullptr);
	decoder->Flush(); // What DecodePinned() does next: crashed on the half-open context.
	CHECK(decoder->Decode(in, true, image, outSeq) == storage::DecodeStatus::Failed); // Not retried.
	CHECK(decoder->Decode(in, false, image, outSeq) == storage::DecodeStatus::Failed);
	decoder->Flush();
	CHECK(decoder->Broken());
	decoder.reset(); // Frees nothing twice.
	std::printf("  %s, %s: failed cleanly\n", storage::CodecName(codec), step);
}

// The software decoder (the emergency path) fed bytes that aren't a picture:
// that packet fails, the decoder isn't broken, Flush() is safe.
void TestSoftwareDecoderGarbage()
{
	testcheck::g_currentTest = "software decoder, garbage";
	hwstorage::Provider provider(Settings(nullptr));
	for (storage::CodecId codec : {storage::CodecId::Hevc, storage::CodecId::H264}) {
		std::unique_ptr<storage::Decoder> decoder = provider.MakeSoftwareDecoder(codec);
		CHECK(decoder != nullptr);
		if (!decoder)
			continue;
		decoder->Flush();
		std::vector<uint8_t> bytes;
		const storage::EncodedFrame in = SomePacket(bytes);
		DecodedImage image;
		uint64_t outSeq = 0;
		CHECK(decoder->Decode(in, true, image, outSeq) != storage::DecodeStatus::Frame);
		CHECK(!decoder->Broken());
		CHECK(!decoder->Hardware());
		decoder->Flush();
	}
	CHECK(provider.MakeSoftwareDecoder(storage::CodecId::Mjpeg) == nullptr);
	CHECK(provider.MakeHardwareDecoder(storage::CodecId::Raw) == nullptr);
}

// One adapter of every vendor, so the ranking has a candidate of each family.
std::vector<GpuAdapterInfo> EveryVendor()
{
	std::vector<GpuAdapterInfo> adapters(3);
	adapters[0].vendorId = 0x10DE;
	adapters[0].name = "test NVIDIA";
	adapters[1].index = 1;
	adapters[1].vendorId = 0x1002;
	adapters[1].name = "test AMD";
	adapters[2].index = 2;
	adapters[2].vendorId = 0x8086;
	adapters[2].name = "test Intel";
	return adapters;
}

// Every encoder fails to open: no session handed back, none left counted
// (the count goes up before avcodec_open2(), so a yield sees a session being
// opened, and back down when the encoder goes).
void TestSetupEveryOpenFails()
{
	testcheck::g_currentTest = "setup, every open fails";
	std::set<std::string> failing{"encoder-open"};
	hwstorage::ProviderSettings settings = Settings(&failing);
	settings.adapters = EveryVendor();
	settings.control = std::make_shared<storage::InterControl>();
	std::shared_ptr<storage::InterControl> control = settings.control;
	hwstorage::Provider provider(settings);
	storage::EncoderConfig frame;
	frame.width = 640;
	frame.height = 360;
	storage::InterSession session = provider.Setup(frame);
	CHECK(session.encoder == nullptr);
	CHECK(session.decoder == nullptr);
	CHECK(!session.cancelled);
	CHECK_EQ(control->openEncoders.load(), 0);
	// Not retried at this size (the registry), and still nothing counted.
	session = provider.Setup(frame);
	CHECK(session.encoder == nullptr);
	CHECK_EQ(control->openEncoders.load(), 0);
}

// A setup cancelled from the start (Disable, shutdown, a yield) opens nothing.
void TestSetupCancelled()
{
	testcheck::g_currentTest = "setup cancelled";
	std::set<std::string> asked;
	hwstorage::ProviderSettings settings;
	settings.adapters = EveryVendor();
	settings.control = std::make_shared<storage::InterControl>();
	settings.injectFailure = [&asked](const char *step) {
		asked.insert(step);
		return true; // Never open anything for real here, whatever happens.
	};
	hwstorage::Provider provider(settings);
	storage::EncoderConfig frame;
	frame.width = 1280;
	frame.height = 720;
	storage::InterSession session = provider.Setup(frame, [] { return true; });
	CHECK(session.encoder == nullptr);
	CHECK(session.cancelled);
	CHECK(asked.empty()); // Not one encoder was opened.
	CHECK_EQ(settings.control->openEncoders.load(), 0);
}

} // namespace

int main()
{
	for (storage::CodecId codec : {storage::CodecId::Hevc, storage::CodecId::H264}) {
		TestDecoderOpenFailure(codec, "decoder-device");
		TestDecoderOpenFailure(codec, "decoder-open");
	}
	TestSoftwareDecoderGarbage();
	TestSetupEveryOpenFails();
	TestSetupCancelled();
	return testcheck::Finish("hw_storage_codec_tests");
}
