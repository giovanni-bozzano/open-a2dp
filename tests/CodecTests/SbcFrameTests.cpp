#include "TestFramework.h"
#include "SbcReferenceDecoder.h"

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

	// The whole joint-stereo mask sits inside the range, RFA bit included: the mask is
	// subbands bits wide, of which only the first subbands-1 are join flags.
	if (h.mode == 3) {
		for (size_t bit = 32; bit < 32 + static_cast<size_t>(h.subbands); ++bit) {
			std::vector<uint8_t> copy = frame;
			FlipBit(copy.data(), bit);
			CHECK(!CrcMatches(copy.data(), crcBits));
		}
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
		CHECK_EQ(encoder.Encode(reinterpret_cast<const uint8_t*>(pcm.data() + f * samplesPerFrame),
							frame.data(), frame.size(), written), S_OK);
	}
	frame.resize(written);
	return frame;
}

void PutBits(std::vector<uint8_t>& frame, size_t& bitPos, uint32_t value, unsigned count)
{
	for (unsigned i = 0; i < count; ++i) {
		if (((value >> (count - 1 - i)) & 1u) != 0) {
			frame[bitPos >> 3] |= static_cast<uint8_t>(0x80u >> (bitPos & 7));
		}
		++bitPos;
	}
}

// A joint-stereo frame built from the spec layout instead of by an encoder, so the mask can
// take patterns the encoders may never emit. The CRC byte is correct for the bits written.
std::vector<uint8_t> SyntheticJointFrame(int nsb, uint8_t join, uint8_t bitpool, int blocks)
{
	const int nch = 2;
	const size_t scaleFactorBits = 4u * static_cast<size_t>(nsb) * static_cast<size_t>(nch);
	const size_t frameLength = 4 + (4u * static_cast<size_t>(nsb) * static_cast<size_t>(nch)) / 8 +
			(static_cast<size_t>(nsb) + static_cast<size_t>(blocks) * bitpool + 7) / 8;
	std::vector<uint8_t> frame(frameLength, 0);

	size_t pos = 0;
	PutBits(frame, pos, SbcFrame::kSyncWord, 8);
	PutBits(frame, pos, 2, 2); // 44.1 kHz
	PutBits(frame, pos, static_cast<uint32_t>(blocks / 4 - 1), 2);
	PutBits(frame, pos, 3, 2); // joint stereo
	PutBits(frame, pos, 0, 1); // loudness
	PutBits(frame, pos, nsb == 8 ? 1u : 0u, 1);
	PutBits(frame, pos, bitpool, 8);
	PutBits(frame, pos, 0, 8); // CRC placeholder
	PutBits(frame, pos, join, static_cast<unsigned>(nsb)); // join flags, then the RFA bit
	for (size_t i = 0; i < scaleFactorBits / 4; ++i) {
		PutBits(frame, pos, static_cast<uint32_t>((i * 5) & 0xF), 4);
	}

	const size_t crcBits = CrcBitCount(ReadHeader(frame.data()));
	frame[3] = ReferenceCrc8(CoveredBits(frame.data(), crcBits));
	return frame;
}

// Mask patterns at the edges: no subband joined, only the first, only the last joinable one,
// and all of them. Every mask bit, the RFA bit included, must be covered.
void CheckJointMaskBoundaries(int nsb)
{
	const uint8_t allJoined = static_cast<uint8_t>(((1u << (nsb - 1)) - 1u) << 1);
	const uint8_t patterns[] = {0x00, static_cast<uint8_t>(1u << (nsb - 1)), 0x02, allJoined};
	for (const uint8_t join : patterns) {
		const std::vector<uint8_t> frame = SyntheticJointFrame(nsb, join, 53, 16);
		const size_t crcBits = CrcBitCount(ReadHeader(frame.data()));
		CHECK(CrcMatches(frame.data(), crcBits));

		// The whole mask, RFA included, plus the first scale-factor bit right after it.
		for (size_t bit = 32; bit < 32 + static_cast<size_t>(nsb) + 1; ++bit) {
			std::vector<uint8_t> copy = frame;
			FlipBit(copy.data(), bit);
			CHECK(!CrcMatches(copy.data(), crcBits));
		}

		// The spec decoder parses its own coverage out of the frame, so it can disagree with
		// the model above: accept the mask, and reject the same frame with RFA flipped.
		TestSupport::SbcReferenceDecoder decoder;
		std::vector<int16_t> pcm;
		TestSupport::SbcReferenceDecoder::FrameInfo info;
		CHECK(decoder.DecodeFrame(frame.data(), frame.size(), pcm, info));
		CHECK(info.crcOk);

		std::vector<uint8_t> rfa = frame;
		FlipBit(rfa.data(), 32 + static_cast<size_t>(nsb) - 1);
		CHECK(decoder.DecodeFrame(rfa.data(), rfa.size(), pcm, info));
		CHECK(!info.crcOk);

		std::printf("    joint nsb=%d mask=0x%02X: accepted, %zu covered bits\n", nsb, join, crcBits);
	}
}

} // namespace

TEST(SbcFrame_JointStereoMaskCoverage)
{
	CheckJointMaskBoundaries(8);
	CheckJointMaskBoundaries(4);
}

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
