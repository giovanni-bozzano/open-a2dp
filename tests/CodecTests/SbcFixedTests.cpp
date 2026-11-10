#include "TestFramework.h"
#include "SbcReferenceDecoder.h"

#include <OpenA2dp/SbcFixed.h>
#include <OpenA2dp/Rtp.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace OpenA2dp;

namespace {

SbcFixed::Params FixedParams(uint32_t rate, uint8_t blocks, uint8_t subbands, SbcFixed::ChannelMode mode,
							 SbcFixed::Allocation alloc, uint8_t bitpool)
{
	SbcFixed::Params p;
	p.sampleRate = rate;
	p.blocks = blocks;
	p.subbands = subbands;
	p.channelMode = mode;
	p.allocation = alloc;
	p.bitpool = bitpool;
	return p;
}

// Encodes a tone with the fixed-point encoder, decodes with the reference decoder and returns the SNR.
double FixedRoundTripSnr(const SbcFixed::Params& p, bool& crcOk)
{
	const int nch = p.Channels();
	const size_t spf = p.SamplesPerFrame();
	const size_t frames = p.sampleRate / 2 / spf;

	SbcFixed::ToneGenerator tone;
	tone.Reset(p.sampleRate, 1000, 12000);
	std::vector<int16_t> pcm(frames * spf * nch);
	tone.Fill(pcm.data(), frames * spf, nch);

	SbcFixed::Encoder encoder(p);
	TestSupport::SbcReferenceDecoder decoder;
	std::vector<int16_t> decoded;
	std::vector<uint8_t> frame(SbcFixed::kMaxFrameBytes);
	crcOk = true;
	for (size_t f = 0; f < frames; ++f) {
		const size_t written = encoder.Encode(pcm.data() + f * spf * nch, frame.data(), frame.size());
		if (written != SbcFixed::FrameLength(p)) {
			crcOk = false;
			return 0;
		}
		TestSupport::SbcReferenceDecoder::FrameInfo info;
		if (!decoder.DecodeFrame(frame.data(), written, decoded, info)) {
			crcOk = false;
			return 0;
		}
		crcOk = crcOk && info.crcOk;
	}

	const size_t total = decoded.size() / nch;
	const size_t skip = 1024;
	double best = -1;
	size_t delay = 0;
	for (size_t d = 0; d < 200; ++d) {
		double xy = 0, xx = 0, yy = 0;
		for (size_t n = skip; n + d < total; ++n) {
			const double x = pcm[n * nch];
			const double y = decoded[(n + d) * nch];
			xy += x * y;
			xx += x * x;
			yy += y * y;
		}
		const double corr = (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 0;
		if (corr > best) {
			best = corr;
			delay = d;
		}
	}
	double signal = 0, noise = 0;
	for (size_t n = skip; n + delay < total; ++n) {
		const double x = pcm[n * nch];
		const double y = decoded[(n + delay) * nch];
		signal += x * x;
		noise += (y - x) * (y - x);
	}
	return noise > 0 ? 10.0 * std::log10(signal / noise) : 200.0;
}

void CheckFixed(const SbcFixed::Params& p, double minSnr)
{
	bool crcOk = false;
	const double snr = FixedRoundTripSnr(p, crcOk);
	std::printf("    fixed fs=%u blk=%u sb=%u mode=%d bp=%u: SNR=%.1f dB\n", p.sampleRate, p.blocks, p.subbands,
				static_cast<int>(p.channelMode), p.bitpool, snr);
	CHECK(crcOk);
	CHECK(snr >= minSnr);
}

} // namespace

TEST(SbcFixed_ParseIe)
{
	const uint8_t ie[4] = {0x21, 0x15, 0x02, 0x35};
	SbcFixed::Params p;
	CHECK(SbcFixed::ParseIe(ie, 4, p));
	CHECK_EQ(p.sampleRate, uint32_t{44100});
	CHECK_EQ(p.blocks, uint8_t{16});
	CHECK_EQ(p.subbands, uint8_t{8});
	CHECK(p.channelMode == SbcFixed::ChannelMode::JointStereo);
	CHECK(p.allocation == SbcFixed::Allocation::Loudness);
	CHECK_EQ(p.bitpool, uint8_t{53});
	CHECK_EQ(SbcFixed::FrameLength(p), size_t{119});
	CHECK_EQ(SbcFixed::Bitrate(p), uint32_t{327993}); // 119 bytes per 128 samples at 44.1 kHz
	CHECK_EQ(SbcFixed::FramesPerPacket(p, 895 - Rtp::kHeaderSize), size_t{7});
}

// The two SBC encoders have to describe the same frame for the same configuration and
// allocate the same bits for the same scale factors; only the analysis filter (Q12 against
// double) may differ. This pins the adapters in both headers to the shared SbcConfig,
// SbcAllocation and SbcFrame code, across mono, stereo and joint stereo, 4 and 8 subbands,
// and both allocation methods.
TEST(SbcFixed_AgreesWithCodecEncoder)
{
	const uint8_t configs[][4] = {
		{0x21, 0x15, 0x02, 0x35}, // 44.1 kHz joint stereo, 16 blocks, 8 subbands, loudness, 53
		{0x12, 0x1A, 0x02, 0x33}, // 48 kHz stereo, 16 blocks, 4 subbands, SNR, 51
		{0x28, 0x16, 0x02, 0x40}, // 44.1 kHz mono, 16 blocks, 8 subbands, SNR, 64
	};
	const size_t payload = 895 - Rtp::kHeaderSize;

	for (const auto& ie : configs)
	{
		SbcFixed::Params fixed;
		Sbc::FrameParams codec;
		REQUIRE(SbcFixed::ParseIe(ie, 4, fixed));
		REQUIRE(Sbc::ParseConfiguredIe(ie, 4, codec));

		SbcEncoder codecEncoder(codec);
		CHECK_EQ(fixed.Channels(), codec.Channels());
		CHECK_EQ(fixed.SamplesPerFrame(), codecEncoder.SamplesPerFrame());
		CHECK_EQ(SbcFixed::FrameLength(fixed), Sbc::FrameLength(codec));
		CHECK_EQ(SbcFixed::Bitrate(fixed), Sbc::Bitrate(codec));
		CHECK_EQ(SbcFixed::FramesPerPacket(fixed, payload), codecEncoder.FramesPerPacket(payload));
		CHECK_EQ(SbcFixed::FrameLength(fixed), codecEncoder.MaxFrameBytes());

		const uint8_t scaleFactors[2][8] = {{2, 4, 6, 8, 10, 12, 14, 15}, {3, 5, 7, 9, 11, 13, 15, 1}};
		int fixedBits[2][8] = {};
		int codecBits[2][8] = {};
		SbcFixed::CalculateBits(fixed, scaleFactors, fixedBits);
		Sbc::CalculateBits(codec, scaleFactors, codecBits);
		CHECK(std::memcmp(fixedBits, codecBits, sizeof(fixedBits)) == 0);

		const size_t spf = fixed.SamplesPerFrame();
		const int nch = fixed.Channels();
		SbcFixed::ToneGenerator tone;
		tone.Reset(fixed.sampleRate, 1000, 12000);
		std::vector<int16_t> pcm(spf * nch);
		tone.Fill(pcm.data(), spf, nch);

		std::vector<uint8_t> fixedFrame(SbcFixed::kMaxFrameBytes);
		const size_t fixedWritten =
				SbcFixed::Encoder(fixed).Encode(pcm.data(), fixedFrame.data(), fixedFrame.size());
		CHECK_EQ(fixedWritten, SbcFixed::FrameLength(fixed));

		std::vector<uint8_t> codecFrame(SbcFixed::kMaxFrameBytes);
		size_t codecWritten = 0;
		REQUIRE(SUCCEEDED(codecEncoder.Encode(reinterpret_cast<const uint8_t*>(pcm.data()),
										codecFrame.data(), codecFrame.size(), codecWritten)));
		CHECK_EQ(codecWritten, fixedWritten);
		// Sync word, the rate/block/mode/allocation/subband bits and the bitpool; the CRC
		// differs because the two analysis filters scale the subbands differently.
		CHECK_EQ(fixedFrame[0], codecFrame[0]);
		CHECK_EQ(fixedFrame[1], codecFrame[1]);
		CHECK_EQ(fixedFrame[2], codecFrame[2]);
	}
}

TEST(SbcFixed_RoundTrip)
{
	using M = SbcFixed::ChannelMode;
	using A = SbcFixed::Allocation;
	CheckFixed(FixedParams(44100, 16, 8, M::JointStereo, A::Loudness, 53), 30.0);
	CheckFixed(FixedParams(48000, 16, 8, M::Stereo, A::Snr, 51), 30.0);
	CheckFixed(FixedParams(44100, 16, 4, M::JointStereo, A::Snr, 53), 30.0);
	CheckFixed(FixedParams(44100, 16, 8, M::Mono, A::Snr, 128), 50.0);
	CheckFixed(FixedParams(32000, 12, 8, M::DualChannel, A::Loudness, 32), 20.0);
}
