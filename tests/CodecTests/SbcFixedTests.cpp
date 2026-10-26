#include "TestFramework.h"
#include "SbcReferenceDecoder.h"

#include <OpenA2dp/SbcFixed.h>
#include <OpenA2dp/Rtp.h>

#include <cmath>
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
	CHECK_EQ(SbcFixed::FramesPerPacket(p, 895 - Rtp::kHeaderSize), size_t{7});
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
