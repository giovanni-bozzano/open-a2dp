#include "TestFramework.h"

#include <OpenA2dp/SbcCodec.h>
#include <OpenA2dp/SbcFixed.h>
#include <OpenA2dp/SbcFrame.h>

#include <cstdint>
#include <cstdio>
#include <vector>

// The two encoders share the frame writer in SbcFrame, so checking their CRC byte against
// themselves would prove nothing. Everything here is derived from the frame bytes: the
// header fields are read back, the covered range is recomputed from those fields, and the
// CRC is recomputed with a separate bit-by-bit implementation (A2DP 12.5: x^8+x^4+x^3+x^2+1,
// initial value 0x0F, MSB first).
namespace {

using namespace OpenA2dp;

struct Config {
	uint32_t rate;
	uint8_t blocks;
	uint8_t subbands;
	SbcFixed::ChannelMode mode;
	uint8_t bitpool;
};

struct Header {
	uint32_t rate = 0;
	int blocks = 0;
	int subbands = 0;
	int channels = 0;
	int mode = 0;
	int bitpool = 0;
};

Header ReadHeader(const uint8_t* frame)
{
	static constexpr uint32_t kRates[] = {16000, 32000, 44100, 48000};
	Header h;
	h.rate = kRates[(frame[1] >> 6) & 0x3u];
	h.blocks = static_cast<int>(((frame[1] >> 4) & 0x3u) + 1) * 4;
	h.mode = (frame[1] >> 2) & 0x3;
	h.subbands = (frame[1] & 0x1u) != 0 ? 8 : 4;
	h.channels = h.mode == 0 ? 1 : 2; // 0 mono, 1 dual channel, 2 stereo, 3 joint stereo
	h.bitpool = frame[2];
	return h;
}

// Bits the CRC covers: header bytes 1 and 2, then every bit after the CRC byte up to the
// end of the scale factors, which includes the joint-stereo mask and its RFA bit.
size_t CrcBitCount(const Header& h)
{
	const size_t jointBits = h.mode == 3 ? static_cast<size_t>(h.subbands) : 0;
	return 16 + jointBits + 4u * static_cast<size_t>(h.subbands) * static_cast<size_t>(h.channels);
}

uint8_t ReferenceCrc8(const std::vector<uint8_t>& bits)
{
	uint8_t crc = 0x0F;
	for (const uint8_t bit : bits) {
		const uint8_t feedback = static_cast<uint8_t>(((crc >> 7) & 1u) ^ bit);
		crc = static_cast<uint8_t>(crc << 1);
		if (feedback != 0) {
			crc ^= 0x1D;
		}
	}
	return crc;
}

// The covered bits in transmission order: frame bits 8..23, then frame bit 32 onwards.
std::vector<uint8_t> CoveredBits(const uint8_t* frame, size_t bitCount)
{
	std::vector<uint8_t> bits(bitCount);
	for (size_t i = 0; i < bitCount; ++i) {
		const size_t absolute = i < 16 ? i + 8 : i + 16;
		bits[i] = static_cast<uint8_t>((frame[absolute >> 3] >> (7 - (absolute & 7))) & 1u);
	}
	return bits;
}

bool CrcMatches(const uint8_t* frame, size_t bitCount)
{
	return ReferenceCrc8(CoveredBits(frame, bitCount)) == frame[3];
}

void FlipBit(uint8_t* frame, size_t absoluteBit)
{
	frame[absoluteBit >> 3] ^= static_cast<uint8_t>(1u << (7 - (absoluteBit & 7)));
}

void CheckFrameCrc(const Config& c, const std::vector<uint8_t>& frame)
{
	REQUIRE(frame.size() > 4);
	CHECK_EQ(static_cast<int>(frame[0]), static_cast<int>(SbcFrame::kSyncWord));

	// The bytes carry the configuration we asked for, so the coverage below is the one the
	// encoder was supposed to write.
	const Header h = ReadHeader(frame.data());
	CHECK_EQ(h.rate, c.rate);
	CHECK_EQ(h.blocks, static_cast<int>(c.blocks));
	CHECK_EQ(h.subbands, static_cast<int>(c.subbands));
	CHECK_EQ(h.mode, static_cast<int>(c.mode));
	CHECK_EQ(h.bitpool, static_cast<int>(c.bitpool));

	const size_t crcBits = CrcBitCount(h);
	const size_t audioStart = 32 + (crcBits - 16);
	REQUIRE(audioStart <= frame.size() * 8);
	CHECK(CrcMatches(frame.data(), crcBits));

	// A frame emitted by an encoder matches, but each covered region must bite: the sample
	// rate field, the bitpool, the first tail bit (join mask or first scale factor) and the
	// last scale-factor nibble.
	const size_t coveredProbes[] = {9, 22, 32, audioStart - 1};
	for (const size_t bit : coveredProbes) {
		std::vector<uint8_t> copy = frame;
		FlipBit(copy.data(), bit);
		CHECK(!CrcMatches(copy.data(), crcBits));
	}

	// The audio bits sit after the scale factors and are not covered.
	{
		std::vector<uint8_t> copy = frame;
		FlipBit(copy.data(), audioStart);
		CHECK(CrcMatches(copy.data(), crcBits));
	}

	std::printf("    crc fs=%u blk=%d sb=%d mode=%d bp=%d: %zu covered bits, frame %zu bytes\n",
			h.rate, h.blocks, h.subbands, h.mode, h.bitpool, crcBits, frame.size());
}

std::vector<int16_t> Pcm(const Config& c)
{
	const int channels = SbcConfig::ChannelCount(static_cast<uint8_t>(c.mode));
	const size_t samplesPerFrame = static_cast<size_t>(c.blocks) * c.subbands;
	const size_t frames = 4;
	SbcFixed::ToneGenerator tone;
	tone.Reset(c.rate, 997, 11000);
	std::vector<int16_t> pcm(frames * samplesPerFrame * static_cast<size_t>(channels));
	tone.Fill(pcm.data(), frames * samplesPerFrame, channels);
	return pcm;
}

std::vector<uint8_t> EncodeWithFixed(const Config& c)
{
	SbcFixed::Params p;
	p.sampleRate = c.rate;
	p.blocks = c.blocks;
	p.subbands = c.subbands;
	p.channelMode = c.mode;
	p.allocation = SbcFixed::Allocation::Loudness;
	p.bitpool = c.bitpool;

	const std::vector<int16_t> pcm = Pcm(c);
	const size_t samplesPerFrame = static_cast<size_t>(c.blocks) * c.subbands * p.Channels();
	SbcFixed::Encoder encoder(p);
	std::vector<uint8_t> frame(SbcFixed::FrameLength(p));
	for (size_t f = 0; f + 1 < 4; ++f) {
		CHECK_EQ(encoder.Encode(pcm.data() + f * samplesPerFrame, frame.data(), frame.size()),
				SbcFixed::FrameLength(p));
	}
	return frame;
}

std::vector<uint8_t> EncodeWithCodec(const Config& c)
{
	Sbc::FrameParams p;
	p.sampleRate = c.rate;
	p.blocks = c.blocks;
	p.subbands = c.subbands;
	p.channelMode = static_cast<Sbc::ChannelMode>(c.mode);
	p.allocation = Sbc::Allocation::Loudness;
	p.bitpool = c.bitpool;

	const std::vector<int16_t> pcm = Pcm(c);
	const size_t samplesPerFrame = static_cast<size_t>(c.blocks) * c.subbands * p.Channels();
	SbcEncoder encoder(p);
	std::vector<uint8_t> frame(encoder.MaxFrameBytes());
	size_t written = 0;
	for (size_t f = 0; f + 1 < 4; ++f) {
		CHECK_EQ(encoder.Encode(pcm.data() + f * samplesPerFrame, frame.data(), frame.size(), written), S_OK);
	}
	frame.resize(written);
	return frame;
}

} // namespace

TEST(SbcFrame_CrcCoverage)
{
	const Config configs[] = {
		{44100, 16, 8, SbcFixed::ChannelMode::JointStereo, 53},
		{48000, 16, 8, SbcFixed::ChannelMode::Stereo, 45},
		{16000, 12, 4, SbcFixed::ChannelMode::Mono, 30},
		{32000, 8, 8, SbcFixed::ChannelMode::DualChannel, 35},
	};
	for (const Config& c : configs) {
		CheckFrameCrc(c, EncodeWithFixed(c));
		CheckFrameCrc(c, EncodeWithCodec(c));
	}
}
