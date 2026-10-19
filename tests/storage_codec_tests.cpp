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

// Unit tests for the storage codec interface (src/storage-codec.hpp, v0.7.0
// RAM design 8.1): the in-flight queue (packets matched to their frames by
// seq, any latency, mismatches), the chain tracker (forced keys, no P-frame
// stored after a loss until the next key), chain ids, the raw codec, the I420
// helpers and the small pure functions -- plus a single-threaded simulation of
// an asynchronous inter-frame encoder (tests/fake-gop-codec.hpp) driven the
// way the pipeline drives one, with random packet losses: every chain stored
// must start on a key and stay consecutive, and the fake decoder must never
// meet a P-frame without its reference.

#include "annexb.hpp"
#include "check.hpp"
#include "fake-gop-codec.hpp"
#include "storage-codec-select.hpp"
#include "storage-codec.hpp"

#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace trigglow;

namespace {

using Queue = storage::InFlightQueue<int>;

void TestInFlightQueue()
{
	testcheck::g_currentTest = "in-flight queue";
	Queue q;
	Queue::Entry e;
	CHECK(q.Empty());
	CHECK(q.Pop(0, e) == Queue::Match::Empty);
	// Latency 0..16 frames: packets come out in Send() order, each matching
	// the oldest entry, through the ring buffer's growth and wrap-around.
	for (uint32_t latency = 0; latency <= 16; ++latency) {
		Queue lq;
		uint64_t nextOut = 0;
		bool inOrder = true;
		for (uint64_t seq = 0; seq < 200; ++seq) {
			lq.Push(seq, 1000 + seq, static_cast<int>(seq * 3));
			while (lq.Size() > latency) {
				Queue::Entry out;
				inOrder = inOrder && lq.Pop(nextOut, out) == Queue::Match::Ok && out.seq == nextOut &&
					  out.submitNs == 1000 + nextOut && out.value == static_cast<int>(nextOut * 3);
				++nextOut;
			}
		}
		CHECK(inOrder);
		CHECK_EQ(lq.Size(), latency);
		CHECK_EQ(lq.MaxDepth(), latency + 1);
	}
	// A packet that isn't the oldest one: a mismatch, nothing popped.
	q.Push(5, 0, 50);
	q.Push(6, 0, 60);
	CHECK(q.Pop(6, e) == Queue::Match::Mismatch);
	CHECK_EQ(q.Size(), 2u);
	CHECK(q.Pop(5, e) == Queue::Match::Ok);
	CHECK_EQ(e.value, 50);
	// ForEach sees them oldest first; Clear hands every one over and empties it.
	q.Push(7, 0, 70);
	std::vector<int> seen;
	q.ForEach([&](Queue::Entry &entry) { seen.push_back(entry.value); });
	CHECK(seen == std::vector<int>({60, 70}));
	std::vector<uint64_t> cleared;
	q.Clear([&](Queue::Entry &entry) { cleared.push_back(entry.seq); });
	CHECK(cleared == std::vector<uint64_t>({6, 7}));
	CHECK(q.Empty());
	CHECK(q.Pop(7, e) == Queue::Match::Empty);
	// Popped entries let go of what they held.
	storage::InFlightQueue<std::shared_ptr<int>> owners;
	auto held = std::make_shared<int>(1);
	owners.Push(0, 0, held);
	storage::InFlightQueue<std::shared_ptr<int>>::Entry taken;
	CHECK(owners.Pop(0, taken) == storage::InFlightQueue<std::shared_ptr<int>>::Match::Ok);
	taken = {};
	CHECK_EQ(held.use_count(), 1);
}

void TestChainTracker()
{
	testcheck::g_currentTest = "chain tracker";
	storage::ChainTracker chain;
	CHECK(!chain.Started());
	chain.Start(7);
	CHECK(chain.Started());
	CHECK_EQ(chain.Chain(), 7u);
	CHECK_EQ(chain.NextSeq(), 0u);
	CHECK(chain.WantKey()); // A chain starts on a key.
	chain.CommitSend(true);
	CHECK(!chain.WantKey());
	CHECK_EQ(chain.NextSeq(), 1u);
	CHECK(chain.ShouldStore(false));
	chain.CommitSend(false);
	chain.CommitSend(false);
	CHECK_EQ(chain.NextSeq(), 3u);
	// A packet lost after its encode: P-frames are no longer stored, the next
	// Send() forces a key, and a stored key mends the chain.
	chain.OnLost();
	CHECK(chain.Broken());
	CHECK(chain.WantKey());
	CHECK(!chain.ShouldStore(false));
	CHECK(chain.ShouldStore(true));
	chain.OnStored(false);
	CHECK(chain.Broken());
	chain.CommitSend(true);
	CHECK(!chain.WantKey());
	CHECK(chain.Broken()); // Until that key is actually stored.
	chain.OnStored(true);
	CHECK(!chain.Broken());
	CHECK(chain.ShouldStore(false));
	// A new session starts over.
	chain.OnLost();
	chain.Start(8);
	CHECK(!chain.Broken() && chain.WantKey() && chain.NextSeq() == 0 && chain.Chain() == 8);
}

void TestChainIds()
{
	testcheck::g_currentTest = "chain ids";
	std::set<uint32_t> ids;
	for (int i = 0; i < 1000; ++i) {
		const uint32_t id = storage::NewChainId();
		CHECK(id != storage::kNoChain);
		ids.insert(id);
	}
	CHECK_EQ(ids.size(), 1000u);
}

void TestPureHelpers()
{
	testcheck::g_currentTest = "pure helpers";
	CHECK(std::string(storage::CodecName(storage::CodecId::Raw)) == "raw");
	CHECK(std::string(storage::CodecName(storage::CodecId::Mjpeg)) == "mjpeg");
	CHECK(std::string(storage::CodecName(storage::CodecId::H264)) == "h264");
	CHECK(std::string(storage::CodecName(storage::CodecId::Hevc)) == "hevc");
	CHECK(storage::ClassOf(storage::CodecId::Raw) == storage::StorageClass::Intra);
	CHECK(storage::ClassOf(storage::CodecId::Mjpeg) == storage::StorageClass::Intra);
	CHECK(storage::ClassOf(storage::CodecId::H264) == storage::StorageClass::Inter);
	CHECK(storage::ClassOf(storage::CodecId::Hevc) == storage::StorageClass::Inter);
	storage::EncoderConfig a, b;
	a.width = b.width = 1920;
	a.height = b.height = 1080;
	a.quality = 5;
	b.quality = 20; // Not geometry: the same encoder is kept.
	CHECK(storage::SameGeometry(a, b));
	b.sliceThreads = 2;
	CHECK(!storage::SameGeometry(a, b));
	b.sliceThreads = 1;
	b.height = 1072;
	CHECK(!storage::SameGeometry(a, b));
	// I420 helpers: size, and packing a strided view.
	CHECK_EQ(I420FrameBytes(64, 32), 64u * 32u + 2u * 32u * 16u);
	CHECK_EQ(I420FrameBytes(2560, 1440), 5529600u);
	std::vector<uint8_t> y(80 * 4), u(48 * 2), v(48 * 2);
	for (size_t i = 0; i < y.size(); ++i)
		y[i] = static_cast<uint8_t>(i);
	for (size_t i = 0; i < u.size(); ++i) {
		u[i] = static_cast<uint8_t>(200 + i);
		v[i] = static_cast<uint8_t>(100 + i);
	}
	I420View strided;
	strided.width = 8;
	strided.height = 4;
	strided.plane[0] = y.data();
	strided.stride[0] = 80;
	strided.plane[1] = u.data();
	strided.stride[1] = 48;
	strided.plane[2] = v.data();
	strided.stride[2] = 48;
	std::vector<uint8_t> packed(I420FrameBytes(8, 4));
	PackI420(strided, packed.data());
	const I420View view = PackedI420View(packed.data(), 8, 4);
	bool same = true;
	for (uint32_t row = 0; row < 4; ++row) {
		for (uint32_t col = 0; col < 8; ++col)
			same = same && view.plane[0][row * view.stride[0] + col] == y[row * 80 + col];
	}
	for (uint32_t row = 0; row < 2; ++row) {
		for (uint32_t col = 0; col < 4; ++col) {
			same = same && view.plane[1][row * view.stride[1] + col] == u[row * 48 + col];
			same = same && view.plane[2][row * view.stride[2] + col] == v[row * 48 + col];
		}
	}
	CHECK(same);
}

void TestRawCodec()
{
	testcheck::g_currentTest = "raw codec";
	storage::EncoderConfig config;
	config.width = 16;
	config.height = 8;
	std::unique_ptr<storage::Encoder> enc = storage::MakeRawEncoder(config);
	CHECK(enc->Codec() == storage::CodecId::Raw);
	CHECK(!enc->InterFrame());
	std::vector<uint8_t> frame(I420FrameBytes(16, 8));
	for (size_t i = 0; i < frame.size(); ++i)
		frame[i] = static_cast<uint8_t>(i * 7);
	const I420View view = PackedI420View(frame.data(), 16, 8);
	storage::Packet packet;
	CHECK(!enc->Receive(packet));
	CHECK(enc->Send(view, 41, false) == storage::SendResult::Ok);
	CHECK_EQ(enc->InFlight(), 1u);
	CHECK(enc->Receive(packet));
	CHECK_EQ(enc->InFlight(), 0u);
	// The frame itself, packed by the pipeline: no copy in the encoder.
	CHECK(packet.rawInput && packet.data == nullptr && packet.key && packet.seq == 41);
	CHECK(packet.codec == storage::CodecId::Raw);
	CHECK_EQ(packet.bytes, frame.size());
	CHECK(!enc->Receive(packet));
	// Another size than the one it was opened for: refused.
	std::vector<uint8_t> other(I420FrameBytes(8, 8));
	CHECK(enc->Send(PackedI420View(other.data(), 8, 8), 42, false) == storage::SendResult::Failed);

	std::unique_ptr<storage::Decoder> dec = storage::MakeRawDecoder();
	storage::EncodedFrame in;
	in.data = frame.data();
	in.bytes = frame.size();
	in.width = 16;
	in.height = 8;
	in.seq = 41;
	in.key = true;
	DecodedImage image;
	uint64_t outSeq = 0;
	CHECK(dec->Decode(in, true, image, outSeq) == storage::DecodeStatus::Frame);
	CHECK_EQ(outSeq, 41u);
	CHECK(image.width == 16 && image.height == 8 && image.owner != nullptr);
	CHECK(image.plane[0] != frame.data()); // Its own copy: the slot is unpinned right after.
	CHECK(std::memcmp(image.plane[0], frame.data(), frame.size()) == 0);
	CHECK(dec->Decode(in, false, image, outSeq) == storage::DecodeStatus::NoFrameYet);
	in.bytes = frame.size() - 1;
	CHECK(dec->Decode(in, true, image, outSeq) == storage::DecodeStatus::Failed);
}

// The fake codec on its own: GOP structure, forced keys, latency, Drain, and
// the decoder telling a P-frame without its reference.
void TestFakeGopCodec()
{
	testcheck::g_currentTest = "fake GOP codec";
	fakegop::Counters counters;
	fakegop::EncoderOptions options;
	options.gop = 3;
	options.latencyFrames = 2;
	options.counters = &counters;
	storage::EncoderConfig config;
	config.width = 16;
	config.height = 8;
	fakegop::FakeGopEncoder enc(config, options);
	CHECK(enc.InterFrame());
	CHECK_EQ(enc.Config().keyframeInterval, 3u);
	std::vector<uint8_t> frame(I420FrameBytes(16, 8));
	std::vector<storage::Packet> got;
	std::vector<std::vector<uint8_t>> bytes;
	for (uint64_t seq = 0; seq < 8; ++seq) {
		const uint64_t stamp = 1000 + seq;
		std::memcpy(frame.data(), &stamp, 8);
		CHECK(enc.Send(PackedI420View(frame.data(), 16, 8), seq, seq == 4) == storage::SendResult::Ok);
		storage::Packet p;
		while (enc.Receive(p)) {
			got.push_back(p);
			bytes.emplace_back(p.data, p.data + p.bytes);
		}
	}
	CHECK_EQ(got.size(), 6u); // Two still in flight.
	CHECK_EQ(enc.InFlight(), 2u);
	enc.Drain(0, [&](const storage::Packet &p) {
		got.push_back(p);
		bytes.emplace_back(p.data, p.data + p.bytes);
	});
	CHECK_EQ(got.size(), 8u);
	// Keys: 0 (first), 3 (GOP), 4 (forced), 7 (GOP from 4).
	const bool keys[8] = {true, false, false, true, true, false, false, true};
	bool layout = true;
	for (size_t i = 0; i < got.size(); ++i)
		layout = layout && got[i].seq == i && got[i].key == keys[i];
	CHECK(layout);
	CHECK_EQ(counters.forcedKeys.load(), 1u);

	fakegop::FakeGopDecoder dec(storage::CodecId::Hevc, &counters);
	auto decode = [&](size_t i, DecodedImage &image) {
		storage::EncodedFrame in;
		in.data = bytes[i].data();
		in.bytes = bytes[i].size();
		in.width = 16;
		in.height = 8;
		in.seq = got[i].seq;
		in.key = got[i].key;
		uint64_t outSeq = 0;
		return dec.Decode(in, true, image, outSeq);
	};
	DecodedImage image;
	for (size_t i = 0; i < 4; ++i) {
		CHECK(decode(i, image) == storage::DecodeStatus::Frame);
		CHECK_EQ(fakegop::ReadStamp(image.plane[0]), 1000 + i);
	}
	CHECK_EQ(counters.corruptDecodes.load(), 0u);
	// Skipping 4 (a key) and decoding 5: its reference is 4, not 3 -> corrupt.
	CHECK(decode(5, image) == storage::DecodeStatus::Frame);
	CHECK_EQ(counters.corruptDecodes.load(), 1u);
	CHECK(fakegop::ReadStamp(image.plane[0]) == (1005 ^ fakegop::kCorruptMask));
	// After a Flush only a key decodes cleanly.
	dec.Flush();
	CHECK(decode(6, image) == storage::DecodeStatus::Frame);
	CHECK_EQ(counters.corruptDecodes.load(), 2u);
	CHECK(decode(7, image) == storage::DecodeStatus::Frame);
	CHECK_EQ(fakegop::ReadStamp(image.plane[0]), 1007u);
	CHECK_EQ(counters.corruptDecodes.load(), 2u);
	CHECK_EQ(counters.badPackets.load(), 0u);
	CHECK_EQ(counters.threadViolations.load(), 0u);
}

// What the pipeline does with an asynchronous inter-frame encoder, on one
// thread and without a ring: frames sent with ChainTracker's seq and forced
// keys, packets matched with InFlightQueue, a random 5% of stored packets
// "lost" (arena full / slot reused), ShouldStore() honoured. Stored packets
// are then decoded in order with ringselect's rule (key, or the next seq of
// the same chain): every chain must start on a key, stay consecutive, and the
// decoder must never see a P-frame without its reference.
void TestAsyncChainSimulation()
{
	testcheck::g_currentTest = "async chain simulation";
	for (uint32_t latency = 0; latency <= 4; ++latency) {
		fakegop::Counters counters;
		fakegop::EncoderOptions options;
		options.gop = 5;
		options.latencyFrames = latency;
		options.counters = &counters;
		storage::EncoderConfig config;
		config.width = 16;
		config.height = 8;
		auto enc = std::make_unique<fakegop::FakeGopEncoder>(config, options);
		storage::ChainTracker chain;
		chain.Start(storage::NewChainId());
		storage::InFlightQueue<uint64_t> inFlight; // value: the frame's stamp.
		struct Stored {
			uint32_t chain;
			uint32_t seq;
			bool key;
			std::vector<uint8_t> bytes;
		};
		std::vector<Stored> stored;
		std::mt19937 rng(latency + 1);
		uint64_t lost = 0, orphans = 0, mismatches = 0;
		auto store = [&](const storage::Packet &p) {
			storage::InFlightQueue<uint64_t>::Entry entry;
			if (inFlight.Pop(p.seq, entry) != storage::InFlightQueue<uint64_t>::Match::Ok) {
				++mismatches;
				return;
			}
			if (!chain.ShouldStore(p.key)) {
				++orphans;
				return;
			}
			if (std::uniform_int_distribution<int>(0, 19)(rng) == 0) {
				++lost;
				chain.OnLost();
				return;
			}
			stored.push_back({chain.Chain(), static_cast<uint32_t>(p.seq), p.key,
					  std::vector<uint8_t>(p.data, p.data + p.bytes)});
			chain.OnStored(p.key);
		};
		std::vector<uint8_t> frame(I420FrameBytes(16, 8));
		for (uint64_t i = 0; i < 2000; ++i) {
			if (i == 1000) {
				// A new session halfway (new ring or size): drain, new chain.
				enc->Drain(0, store);
				enc = std::make_unique<fakegop::FakeGopEncoder>(config, options);
				chain.Start(storage::NewChainId());
			}
			const uint64_t stamp = 5000 + i;
			std::memcpy(frame.data(), &stamp, 8);
			const bool forceKey = chain.WantKey();
			const uint64_t seq = chain.NextSeq();
			CHECK(enc->Send(PackedI420View(frame.data(), 16, 8), seq, forceKey) == storage::SendResult::Ok);
			chain.CommitSend(forceKey);
			inFlight.Push(seq, 0, stamp);
			storage::Packet p;
			while (enc->Receive(p))
				store(p);
		}
		enc->Drain(0, store);
		CHECK_EQ(mismatches, 0u);
		CHECK(lost > 50);
		if (latency > 0)
			CHECK(orphans > 0); // P-frames sent before the loss was known were dropped, not stored.
		// Decode everything stored, in order, with the ring's rule.
		fakegop::FakeGopDecoder dec(storage::CodecId::Hevc, &counters);
		std::map<uint32_t, bool> chainStartedOnKey;
		uint32_t curChain = 0, curSeq = 0;
		bool curValid = false;
		uint64_t decoded = 0, skipped = 0;
		bool consecutive = true;
		for (size_t k = 0; k < stored.size(); ++k) {
			const Stored &s = stored[k];
			if (!chainStartedOnKey.count(s.chain))
				chainStartedOnKey[s.chain] = s.key;
			// Inside a chain, a stored P-frame always follows its predecessor.
			if (!s.key && k > 0 && stored[k - 1].chain == s.chain)
				consecutive = consecutive && s.seq == stored[k - 1].seq + 1;
			const bool decodable = s.key || (curValid && s.chain == curChain && s.seq == curSeq + 1);
			if (!decodable) {
				++skipped;
				continue;
			}
			storage::EncodedFrame in;
			in.data = s.bytes.data();
			in.bytes = s.bytes.size();
			in.width = 16;
			in.height = 8;
			in.seq = s.seq;
			in.key = s.key;
			DecodedImage image;
			uint64_t outSeq = 0;
			CHECK(dec.Decode(in, true, image, outSeq) == storage::DecodeStatus::Frame);
			curValid = true;
			curChain = s.chain;
			curSeq = s.seq;
			++decoded;
		}
		CHECK(consecutive);
		for (const auto &c : chainStartedOnKey)
			CHECK(c.second);
		CHECK_EQ(skipped, 0u); // Nothing orphaned was stored, so nothing has to be skipped.
		CHECK_EQ(decoded, stored.size());
		CHECK_EQ(counters.corruptDecodes.load(), 0u);
		CHECK_EQ(counters.badPackets.load(), 0u);
		// Every forced key follows a loss or starts a session (two); losses in a row share one.
		CHECK(counters.forcedKeys.load() > lost / 2 && counters.forcedKeys.load() <= lost + 2);
	}
}

using Bytes = std::vector<uint8_t>;

Bytes Cat(std::initializer_list<Bytes> parts)
{
	Bytes out;
	for (const Bytes &part : parts)
		out.insert(out.end(), part.begin(), part.end());
	return out;
}

// One NAL behind a 3- or 4-byte start code: its header byte(s) and a payload.
Bytes NalH264(uint8_t type, bool longCode = true)
{
	Bytes nal = longCode ? Bytes{0, 0, 0, 1} : Bytes{0, 0, 1};
	nal.push_back(static_cast<uint8_t>(0x60 | type)); // nal_ref_idc 3.
	nal.insert(nal.end(), {0x11, 0x22, 0x33});
	return nal;
}

Bytes NalHevc(uint8_t type, bool longCode = true)
{
	Bytes nal = longCode ? Bytes{0, 0, 0, 1} : Bytes{0, 0, 1};
	nal.push_back(static_cast<uint8_t>(type << 1));
	nal.push_back(0x01); // nuh_temporal_id_plus1.
	nal.insert(nal.end(), {0xAA, 0xBB, 0xCC, 0xDD});
	return nal;
}

void TestAnnexB()
{
	testcheck::g_currentTest = "annex b";
	using annexb::HasInBandParameterSets;
	const storage::CodecId h264 = storage::CodecId::H264;
	const storage::CodecId hevc = storage::CodecId::Hevc;

	// H.264: SPS (7), PPS (8), IDR slice (5), 4- and 3-byte start codes.
	const Bytes idr = Cat({NalH264(7), NalH264(8, false), NalH264(5)});
	CHECK(HasInBandParameterSets(h264, idr.data(), idr.size()));
	const std::vector<annexb::Nal> nals = annexb::Split(h264, idr.data(), idr.size());
	CHECK_EQ(nals.size(), 3u);
	if (nals.size() == 3) {
		CHECK_EQ(nals[0].type, 7u);
		CHECK_EQ(nals[1].type, 8u);
		CHECK_EQ(nals[2].type, 5u);
		CHECK_EQ(nals[0].begin, 0u);
		CHECK_EQ(nals[0].data, 4u);
		CHECK_EQ(nals[0].end, nals[1].begin);
		CHECK_EQ(nals[1].data, nals[1].begin + 3); // 3-byte start code.
		CHECK_EQ(nals[2].end, idr.size());
	}
	const Bytes noPps = Cat({NalH264(7), NalH264(5)});
	CHECK(!HasInBandParameterSets(h264, noPps.data(), noPps.size()));
	const Bytes noSps = Cat({NalH264(8), NalH264(5)});
	CHECK(!HasInBandParameterSets(h264, noSps.data(), noSps.size()));
	const Bytes pFrame = NalH264(1);
	CHECK(!HasInBandParameterSets(h264, pFrame.data(), pFrame.size()));
	// Parameter sets after the first slice don't count.
	const Bytes late = Cat({NalH264(5), NalH264(7), NalH264(8)});
	CHECK(!HasInBandParameterSets(h264, late.data(), late.size()));
	// An access unit delimiter (9) or SEI (6) in front is fine.
	const Bytes withAud = Cat({NalH264(9), NalH264(6), NalH264(7), NalH264(8), NalH264(5)});
	CHECK(HasInBandParameterSets(h264, withAud.data(), withAud.size()));

	// HEVC: VPS (32), SPS (33), PPS (34), then IDR_W_RADL (19), IDR_N_LP (20) or CRA (21).
	for (uint8_t slice : {uint8_t{19}, uint8_t{20}, uint8_t{21}}) {
		const Bytes key = Cat({NalHevc(32), NalHevc(33), NalHevc(34, false), NalHevc(slice)});
		CHECK(HasInBandParameterSets(hevc, key.data(), key.size()));
		// The same bytes read as H.264 are no H.264 key.
		CHECK(!HasInBandParameterSets(h264, key.data(), key.size()));
	}
	const Bytes noVps = Cat({NalHevc(33), NalHevc(34), NalHevc(19)});
	CHECK(!HasInBandParameterSets(hevc, noVps.data(), noVps.size()));
	const Bytes hevcP = NalHevc(1);
	CHECK(!HasInBandParameterSets(hevc, hevcP.data(), hevcP.size()));
	// Not inter-frame codecs, empty and truncated packets.
	CHECK(!HasInBandParameterSets(storage::CodecId::Mjpeg, idr.data(), idr.size()));
	CHECK(!HasInBandParameterSets(storage::CodecId::Raw, idr.data(), idr.size()));
	CHECK(!HasInBandParameterSets(hevc, nullptr, 0));
	const Bytes onlyCode{0, 0, 0, 1};
	CHECK(annexb::Split(hevc, onlyCode.data(), onlyCode.size()).empty());
	const Bytes cut = Cat({NalHevc(32), NalHevc(33), Bytes{0, 0, 1}});
	CHECK_EQ(annexb::Split(hevc, cut.data(), cut.size()).size(), 2u);
	CHECK(!HasInBandParameterSets(hevc, cut.data(), cut.size()));
	// Garbage before the first start code is skipped.
	const Bytes garbage = Cat({Bytes{0x12, 0x34, 0x00}, NalHevc(32), NalHevc(33), NalHevc(34), NalHevc(19)});
	CHECK(HasInBandParameterSets(hevc, garbage.data(), garbage.size()));

	// Extract + prepend: a key without its parameter sets gets those of the first key.
	const Bytes first = Cat({NalHevc(32), NalHevc(33, false), NalHevc(34), NalHevc(19)});
	const Bytes params = annexb::ExtractParameterSets(hevc, first.data(), first.size());
	CHECK(params == Cat({NalHevc(32), NalHevc(33), NalHevc(34)})); // 4-byte start codes throughout.
	CHECK(annexb::ExtractParameterSets(hevc, hevcP.data(), hevcP.size()).empty());
	const Bytes bare = NalHevc(19);
	Bytes out{9, 9};
	CHECK(annexb::PrependParameterSets(hevc, params, bare.data(), bare.size(), out));
	CHECK(out == Cat({params, bare}));
	CHECK(HasInBandParameterSets(hevc, out.data(), out.size()));
	Bytes untouched{7};
	CHECK(!annexb::PrependParameterSets(hevc, params, first.data(), first.size(), untouched));
	CHECK(untouched == Bytes{7});
	CHECK(!annexb::PrependParameterSets(hevc, Bytes{}, bare.data(), bare.size(), untouched));
	CHECK(annexb::IsSlice(hevc, 19) && annexb::IsSlice(hevc, 1) && !annexb::IsSlice(hevc, 32));
	CHECK(annexb::IsSlice(h264, 5) && annexb::IsSlice(h264, 1) && !annexb::IsSlice(h264, 7));
	CHECK(annexb::IsParameterSet(h264, 7) && annexb::IsParameterSet(h264, 8) && !annexb::IsParameterSet(h264, 5));
}

void TestNv12Planes()
{
	testcheck::g_currentTest = "nv12 planes";
	// 6x2 chroma samples with padded strides, through NV12 and back.
	const uint32_t cw = 6, ch = 2;
	std::vector<uint8_t> u(8 * ch), v(9 * ch), uv(16 * ch, 0xEE), u2(7 * ch), v2(7 * ch);
	for (uint32_t i = 0; i < u.size(); ++i)
		u[i] = static_cast<uint8_t>(i * 3 + 1);
	for (uint32_t i = 0; i < v.size(); ++i)
		v[i] = static_cast<uint8_t>(200 - i);
	InterleaveUv(u.data(), 8, v.data(), 9, cw, ch, uv.data(), 16);
	bool ok = true;
	for (uint32_t row = 0; row < ch; ++row) {
		for (uint32_t col = 0; col < cw; ++col) {
			ok = ok && uv[row * 16 + 2 * col] == u[row * 8 + col];
			ok = ok && uv[row * 16 + 2 * col + 1] == v[row * 9 + col];
		}
		ok = ok && uv[row * 16 + 2 * cw] == 0xEE; // Padding untouched.
	}
	CHECK(ok);
	DeinterleaveUv(uv.data(), 16, cw, ch, u2.data(), 7, v2.data(), 7);
	for (uint32_t row = 0; row < ch; ++row) {
		for (uint32_t col = 0; col < cw; ++col) {
			ok = ok && u2[row * 7 + col] == u[row * 8 + col];
			ok = ok && v2[row * 7 + col] == v[row * 9 + col];
		}
	}
	CHECK(ok);
}

std::vector<std::string> Names(const std::vector<storage::HwCandidate> &candidates)
{
	std::vector<std::string> names;
	for (const storage::HwCandidate &c : candidates)
		names.emplace_back(c.encoder);
	return names;
}

void TestSelection()
{
	testcheck::g_currentTest = "codec selection";
	using storage::BufferCodecMode;
	using storage::EncoderFamily;
	using storage::GpuVendor;
	using storage::SettingType;
	using Names_ = std::vector<std::string>;

	CHECK(storage::VendorFromPciId(0x10DE) == GpuVendor::Nvidia);
	CHECK(storage::VendorFromPciId(0x1002) == GpuVendor::Amd);
	CHECK(storage::VendorFromPciId(0x1022) == GpuVendor::Amd);
	CHECK(storage::VendorFromPciId(0x8086) == GpuVendor::Intel);
	CHECK(storage::VendorFromPciId(0x1414) == GpuVendor::Other);
	CHECK(storage::VendorFromPciId(0) == GpuVendor::Unknown);

	CHECK(storage::FamilyFromObsEncoderId("obs_nvenc_h264_tex") == EncoderFamily::Nvenc);
	CHECK(storage::FamilyFromObsEncoderId("obs_nvenc_hevc_soft") == EncoderFamily::Nvenc);
	CHECK(storage::FamilyFromObsEncoderId("jim_hevc_nvenc") == EncoderFamily::Nvenc);
	CHECK(storage::FamilyFromObsEncoderId("ffmpeg_nvenc") == EncoderFamily::Nvenc);
	CHECK(storage::FamilyFromObsEncoderId("h264_texture_amf") == EncoderFamily::Amf);
	CHECK(storage::FamilyFromObsEncoderId("h265_texture_amf") == EncoderFamily::Amf);
	CHECK(storage::FamilyFromObsEncoderId("amd_amf_h264") == EncoderFamily::Amf);
	CHECK(storage::FamilyFromObsEncoderId("obs_qsv11_v2") == EncoderFamily::Qsv);
	CHECK(storage::FamilyFromObsEncoderId("obs_qsv11_hevc") == EncoderFamily::Qsv);
	CHECK(storage::FamilyFromObsEncoderId("obs_x264") == EncoderFamily::Cpu);
	CHECK(storage::FamilyFromObsEncoderId("ffmpeg_svt_av1") == EncoderFamily::Cpu);
	CHECK(storage::FamilyFromObsEncoderId("ffmpeg_aom_av1") == EncoderFamily::Cpu);
	CHECK(storage::FamilyFromObsEncoderId("com.apple.videotoolbox.videoencoder.ave.avc") == EncoderFamily::Other);
	CHECK(storage::FamilyFromObsEncoderId("") == EncoderFamily::None);

	// buffer_codec: strict, like the other hidden switches.
	auto parse = [](SettingType type, const char *text) {
		return storage::ParseBufferCodec(type, text);
	};
	CHECK(parse(SettingType::Missing, "").mode == BufferCodecMode::Auto && !parse(SettingType::Missing, "").warn);
	CHECK(parse(SettingType::String, "auto").mode == BufferCodecMode::Auto &&
	      !parse(SettingType::String, "auto").warn);
	CHECK(parse(SettingType::String, "mjpeg").mode == BufferCodecMode::Mjpeg &&
	      !parse(SettingType::String, "mjpeg").warn);
	for (const char *bad : {"MJPEG", "Mjpeg", "hevc", "", " mjpeg", "mjpeg ", "AUTO", "raw"}) {
		CHECK(parse(SettingType::String, bad).mode == BufferCodecMode::Auto);
		CHECK(parse(SettingType::String, bad).warn);
	}
	// false, 0, a list: present but not a string.
	CHECK(parse(SettingType::Other, "").mode == BufferCodecMode::Auto && parse(SettingType::Other, "").warn);
	CHECK(std::string(storage::BufferCodecName(BufferCodecMode::Mjpeg)) == "mjpeg");
	CHECK(std::string(storage::BufferCodecName(BufferCodecMode::Auto)) == "auto");

	storage::RankInput all;
	all.vendors = {GpuVendor::Nvidia, GpuVendor::Amd, GpuVendor::Intel};
	// OBS's encoders unknown: every engine may be shared, D4's order.
	CHECK(Names(storage::RankCandidates(all)) ==
	      (Names_{"hevc_nvenc", "hevc_amf", "hevc_qsv", "h264_nvenc", "h264_amf", "h264_qsv"}));
	for (const storage::HwCandidate &c : storage::RankCandidates(all))
		CHECK(c.sharesEngine);
	// Only the vendors present are probed.
	storage::RankInput amd;
	amd.vendors = {GpuVendor::Amd, GpuVendor::Other};
	CHECK(Names(storage::RankCandidates(amd)) == (Names_{"hevc_amf", "h264_amf"}));
	storage::RankInput none;
	CHECK(storage::RankCandidates(none).empty());
	// OBS streams on NVENC, an AMD iGPU is idle: the engine OBS doesn't use first.
	storage::RankInput igpu;
	igpu.vendors = {GpuVendor::Nvidia, GpuVendor::Amd};
	igpu.obsEncodersKnown = true;
	igpu.obsEncoders = {EncoderFamily::Nvenc};
	const std::vector<storage::HwCandidate> ranked = storage::RankCandidates(igpu);
	CHECK(Names(ranked) == (Names_{"hevc_amf", "h264_amf", "hevc_nvenc", "h264_nvenc"}));
	if (ranked.size() == 4) {
		CHECK(!ranked[0].sharesEngine && !ranked[1].sharesEngine);
		CHECK(ranked[2].sharesEngine && ranked[3].sharesEngine);
		CHECK(ranked[0].backend == storage::Backend::Amf && ranked[0].codec == storage::CodecId::Hevc);
		CHECK(ranked[3].codec == storage::CodecId::H264 && ranked[3].vendor == GpuVendor::Nvidia);
	}
	// x264 streams: no hardware engine is shared, D4's order stays.
	storage::RankInput cpu = all;
	cpu.obsEncodersKnown = true;
	cpu.obsEncoders = {EncoderFamily::Cpu, EncoderFamily::Cpu};
	CHECK(Names(storage::RankCandidates(cpu)) ==
	      (Names_{"hevc_nvenc", "hevc_amf", "hevc_qsv", "h264_nvenc", "h264_amf", "h264_qsv"}));
	for (const storage::HwCandidate &c : storage::RankCandidates(cpu))
		CHECK(!c.sharesEngine);
	// NVENC sessions: at most 3 of OBS's own leave the buffer one.
	storage::RankInput sessions;
	sessions.vendors = {GpuVendor::Nvidia};
	sessions.obsEncodersKnown = true;
	sessions.obsEncoders = {EncoderFamily::Nvenc, EncoderFamily::Nvenc, EncoderFamily::Nvenc};
	CHECK(Names(storage::RankCandidates(sessions)) == (Names_{"hevc_nvenc", "h264_nvenc"}));
	sessions.obsEncoders.push_back(EncoderFamily::Nvenc);
	CHECK(storage::RankCandidates(sessions).empty());
	CHECK_EQ(storage::CountFamily(sessions.obsEncoders, EncoderFamily::Nvenc), 4u);
	// The session's blacklist.
	storage::RankInput listed = amd;
	listed.blacklist = {"hevc_amf"};
	CHECK(Names(storage::RankCandidates(listed)) == (Names_{"h264_amf"}));
	// buffer_codec=mjpeg, the sync kill switch, Linux/macOS: none.
	storage::RankInput forced = all;
	forced.mode = BufferCodecMode::Mjpeg;
	CHECK(storage::RankCandidates(forced).empty());
	storage::RankInput sync = all;
	sync.workers = false;
	CHECK(storage::RankCandidates(sync).empty());
	storage::RankInput otherOs = all;
	otherOs.platformSupported = false;
	CHECK(storage::RankCandidates(otherOs).empty());

	// Engine sharing, for the codec policy.
	using storage::Backend;
	const std::vector<EncoderFamily> obsNvenc{EncoderFamily::Nvenc};
	const std::vector<EncoderFamily> obsAmf{EncoderFamily::Amf};
	const std::vector<EncoderFamily> obsQsv{EncoderFamily::Qsv};
	CHECK(storage::ComputeEngineShare(Backend::Amf, true, GpuVendor::Amd, false, {}).Any()); // Unknown.
	CHECK(storage::ComputeEngineShare(Backend::Amf, false, GpuVendor::Amd, true, obsAmf).encode);
	CHECK(storage::ComputeEngineShare(Backend::Nvenc, true, GpuVendor::Nvidia, true, obsNvenc).encode);
	CHECK(!storage::ComputeEngineShare(Backend::Nvenc, true, GpuVendor::Nvidia, true, obsNvenc).decode);
	CHECK(!storage::ComputeEngineShare(Backend::Amf, true, GpuVendor::Nvidia, true, obsNvenc).Any());
	const storage::EngineShare decodeOnly =
		storage::ComputeEngineShare(Backend::Nvenc, true, GpuVendor::Amd, true, obsAmf);
	CHECK(decodeOnly.decode && !decodeOnly.encode && decodeOnly.Any());
	CHECK(storage::ComputeEngineShare(Backend::Nvenc, true, GpuVendor::Intel, true, obsQsv).decode);
	CHECK(!storage::ComputeEngineShare(Backend::Nvenc, false, GpuVendor::Amd, true, obsAmf).Any());
	CHECK(!storage::ComputeEngineShare(Backend::Amf, true, GpuVendor::Amd, true, {EncoderFamily::Cpu}).Any());
}

} // namespace

int main()
{
	TestInFlightQueue();
	TestChainTracker();
	TestChainIds();
	TestPureHelpers();
	TestRawCodec();
	TestFakeGopCodec();
	TestAsyncChainSimulation();
	TestAnnexB();
	TestNv12Planes();
	TestSelection();
	return testcheck::Finish("storage_codec_tests");
}
