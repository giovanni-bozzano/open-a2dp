#include "TestFramework.h"
#include "SbcReferenceDecoder.h"

#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/SbcCodec.h>
#include <OpenA2dp/SbcConfig.h>

#include <cmath>
#include <numbers>
#include <vector>

using namespace OpenA2dp;
using namespace OpenA2dp::Sbc;

namespace {

// Sine per channel plus deterministic low-pass noise (shared by both channels) so that
// the delay search in RoundTrip has a single unambiguous correlation peak.
std::vector<int16_t> MakeSine(uint32_t rate, int channels, size_t samplesPerChannel, double freqL, double freqR, double amplitude)
{
	std::vector<int16_t> pcm(samplesPerChannel * channels);
	uint32_t seed = 12345;
	double noise = 0.0;
	for (size_t n = 0; n < samplesPerChannel; ++n) {
		seed = seed * 1664525u + 1013904223u;
		const double white = (static_cast<double>(seed >> 8) / static_cast<double>(1u << 24)) * 2.0 - 1.0;
		noise = 0.8 * noise + 0.2 * white;
		for (int ch = 0; ch < channels; ++ch) {
			const double f = ch == 0 ? freqL : freqR;
			const double s = amplitude * 0.6 * std::sin(2.0 * std::numbers::pi * f * n / rate) + amplitude * 2.0 * noise;
			pcm[n * channels + ch] = static_cast<int16_t>(std::lround(s));
		}
	}
	return pcm;
}

struct RoundTripResult {
	double snrDb = 0;
	double gain = 0;
	size_t delay = 0;
	bool allCrcOk = true;
	bool decodeOk = true;
};

// Encodes `pcm`, decodes with the reference decoder and measures SNR on channel `ch`
// after compensating for the codec delay and gain.
RoundTripResult RoundTrip(const FrameParams& p, const std::vector<int16_t>& pcm, int ch)
{
	SbcEncoder encoder(p);
	TestSupport::SbcReferenceDecoder decoder;
	const int nch = p.Channels();
	const size_t spf = encoder.SamplesPerFrame();
	const size_t frames = pcm.size() / nch / spf;

	RoundTripResult result;
	std::vector<int16_t> decoded;
	std::vector<uint8_t> frame(encoder.MaxFrameBytes());
	for (size_t f = 0; f < frames; ++f) {
		size_t written = 0;
		if (FAILED(encoder.Encode(pcm.data() + f * spf * nch, frame.data(), frame.size(), written)) ||
			written != FrameLength(p)) {
			result.decodeOk = false;
			return result;
		}
		TestSupport::SbcReferenceDecoder::FrameInfo info;
		if (!decoder.DecodeFrame(frame.data(), written, decoded, info)) {
			result.decodeOk = false;
			return result;
		}
		result.allCrcOk = result.allCrcOk && info.crcOk;
	}

	const size_t total = decoded.size() / nch;
	const size_t skip = 1024; // ignore filter warm-up
	double bestCorr = -1;
	for (size_t d = 0; d < 200; ++d) {
		double xy = 0, xx = 0, yy = 0;
		for (size_t n = skip; n + d < total; ++n) {
			const double x = pcm[n * nch + ch];
			const double y = decoded[(n + d) * nch + ch];
			xy += x * y;
			xx += x * x;
			yy += y * y;
		}
		const double corr = (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 0;
		if (corr > bestCorr) {
			bestCorr = corr;
			result.delay = d;
			result.gain = xx > 0 ? xy / xx : 0;
		}
	}

	double signal = 0, noise = 0;
	for (size_t n = skip; n + result.delay < total; ++n) {
		const double x = pcm[n * nch + ch];
		const double y = decoded[(n + result.delay) * nch + ch];
		signal += x * x;
		noise += (y - x) * (y - x);
	}
	result.snrDb = noise > 0 ? 10.0 * std::log10(signal / noise) : 200.0;
	return result;
}

FrameParams Params(uint32_t rate, uint8_t blocks, uint8_t subbands, ChannelMode mode, Allocation alloc, uint8_t bitpool)
{
	FrameParams p;
	p.sampleRate = rate;
	p.blocks = blocks;
	p.subbands = subbands;
	p.channelMode = mode;
	p.allocation = alloc;
	p.bitpool = bitpool;
	return p;
}

void CheckRoundTrip(const FrameParams& p, double minSnr)
{
	const auto pcm = MakeSine(p.sampleRate, p.Channels(), static_cast<size_t>(p.sampleRate) / 2, 1000.0, 1500.0, 12000.0);
	for (int ch = 0; ch < p.Channels(); ++ch) {
		const auto r = RoundTrip(p, pcm, ch);
		std::printf("    fs=%u blk=%u sb=%u mode=%d alloc=%d bp=%u ch=%d: SNR=%.1f dB gain=%.4f delay=%zu\n",
					p.sampleRate, p.blocks, p.subbands, static_cast<int>(p.channelMode),
					static_cast<int>(p.allocation), p.bitpool, ch, r.snrDb, r.gain, r.delay);
		CHECK(r.decodeOk);
		CHECK(r.allCrcOk);
		CHECK(r.gain > 0.95 && r.gain < 1.05);
		CHECK(r.snrDb >= minSnr);
	}
}

} // namespace

TEST(Sbc_FrameLength_HighQualityJointStereo)
{
	const auto p = Params(44100, 16, 8, ChannelMode::JointStereo, Allocation::Loudness, 53);
	CHECK_EQ(FrameLength(p), size_t{119});
	CHECK(Bitrate(p) > 327000 && Bitrate(p) < 329000);
}

TEST(Sbc_FrameLength_OtherModes)
{
	// Mono, 4 subbands, 8 blocks, bitpool 31: 4 + 2 + ceil(8*31/8) = 37
	CHECK_EQ(FrameLength(Params(48000, 8, 4, ChannelMode::Mono, Allocation::Snr, 31)), size_t{37});
	// Dual, 8 subbands, 16 blocks, bitpool 32: 4 + 8 + ceil(16*2*32/8) = 140
	CHECK_EQ(FrameLength(Params(48000, 16, 8, ChannelMode::DualChannel, Allocation::Loudness, 32)), size_t{140});
	// Stereo, 8 subbands, 16 blocks, bitpool 53: 4 + 8 + ceil(16*53/8) = 118
	CHECK_EQ(FrameLength(Params(44100, 16, 8, ChannelMode::Stereo, Allocation::Loudness, 53)), size_t{118});
}

TEST(Sbc_Crc8_EmptyIsInitialValue)
{
	const uint8_t none[1] = {};
	CHECK_EQ(Crc8(none, 0), uint8_t{0x0F});
}

TEST(Sbc_BitAllocation_UsesWholeBitpool)
{
	const auto p = Params(44100, 16, 8, ChannelMode::JointStereo, Allocation::Loudness, 53);
	const uint8_t sf[2][8] = {{12, 11, 10, 9, 8, 7, 5, 3}, {11, 10, 9, 8, 7, 6, 4, 2}};
	int bits[2][8] = {};
	CalculateBits(p, sf, bits);
	int sum = 0;
	for (auto& ch : bits) {
		for (int b : ch) {
			CHECK(b >= 0 && b <= 16);
			sum += b;
		}
	}
	CHECK_EQ(sum, 53);
}

TEST(Sbc_SelectConfiguration_TypicalSink)
{
	SbcCodec codec;
	const uint8_t remote[4] = {0xFF, 0xFF, 2, 53};
	A2dpCodecConfig cfg;
	REQUIRE(codec.SelectConfiguration(remote, 4, CodecPreference{}, cfg));
	CHECK_EQ(cfg.ie[0], uint8_t{kFs44100 | kModeJoint});
	CHECK_EQ(cfg.ie[1], uint8_t{kBlocks16 | kSubbands8 | kAllocationLoudness});
	CHECK_EQ(cfg.ie[2], uint8_t{2});
	CHECK_EQ(cfg.ie[3], uint8_t{53});
	CHECK_EQ(codec.ValidateConfiguration(cfg.ie, cfg.ieLength), uint8_t{0});
}

// A pinned endpoint format is refused at SET_PREFERENCE against the same advertised
// capabilities the selector uses, so the mask helpers must read the IE the way it does.
TEST(Sbc_AdvertisedFormatsMatchTheSelector)
{
	CHECK_EQ(SbcConfig::AdvertisedRates(0xF0), uint32_t{3});
	CHECK_EQ(SbcConfig::AdvertisedRates(0x20), uint32_t{1});
	CHECK_EQ(SbcConfig::AdvertisedRates(0x10), uint32_t{2});
	CHECK_EQ(SbcConfig::AdvertisedRates(0x0F), uint32_t{0});
	CHECK_EQ(SbcConfig::AdvertisedChannels(0x0F), uint32_t{3});
	CHECK_EQ(SbcConfig::AdvertisedChannels(0x08), uint32_t{1});
	CHECK_EQ(SbcConfig::AdvertisedChannels(0x05), uint32_t{2});
	CHECK_EQ(SbcConfig::AdvertisedChannels(0xF0), uint32_t{0});

	const uint8_t stereoOnly44 = 0x21; // 44.1 kHz, joint stereo
	CHECK(SbcConfig::SupportsFormat(stereoOnly44, 44100, 0));
	CHECK(SbcConfig::SupportsFormat(stereoOnly44, 44100, 2));
	CHECK(!SbcConfig::SupportsFormat(stereoOnly44, 44100, 1));
	CHECK(!SbcConfig::SupportsFormat(stereoOnly44, 48000, 0));
	CHECK(SbcConfig::SupportsFormat(stereoOnly44, 0, 0)); // no pin
	CHECK(!SbcConfig::SupportsFormat(0, 44100, 2));      // sink not queried yet
}

TEST(Sbc_SelectConfiguration_PreferenceAndLimits)
{
	SbcCodec codec;
	const uint8_t remote[4] = {kFs48000 | kFs44100 | kModeStereo | kModeMono, kBlocks8 | kSubbands4 | kAllocationSnr, 10, 35};
	CodecPreference pref;
	pref.preferredSampleRate = 48000;
	pref.sbcMaxBitpool = 30;
	A2dpCodecConfig cfg;
	REQUIRE(codec.SelectConfiguration(remote, 4, pref, cfg));
	CHECK_EQ(cfg.ie[0], uint8_t{kFs48000 | kModeStereo});
	CHECK_EQ(cfg.ie[1], uint8_t{kBlocks8 | kSubbands4 | kAllocationSnr});
	CHECK_EQ(cfg.ie[2], uint8_t{10});
	CHECK_EQ(cfg.ie[3], uint8_t{30});
}

TEST(Sbc_SelectConfiguration_ClampsBitpoolPreference)
{
	SbcCodec codec;
	// 44.1 kHz joint stereo, 16 blocks, 8 subbands, sink bitpool 11..53.
	const uint8_t remote[4] = {kFs44100 | kModeJoint, kBlocks16 | kSubbands8 | kAllocationLoudness, 11, 53};
	A2dpCodecConfig cfg;

	CodecPreference low;
	low.sbcMaxBitpool = 4; // below what the sink accepts
	REQUIRE(codec.SelectConfiguration(remote, 4, low, cfg));
	CHECK_EQ(cfg.ie[2], uint8_t{11});
	CHECK_EQ(cfg.ie[3], uint8_t{11});

	CodecPreference high;
	high.sbcMaxBitpool = 120; // above what the sink accepts
	REQUIRE(codec.SelectConfiguration(remote, 4, high, cfg));
	CHECK_EQ(cfg.ie[3], uint8_t{53});

	REQUIRE(codec.SelectConfiguration(remote, 4, CodecPreference{}, cfg));
	CHECK_EQ(cfg.ie[3], uint8_t{53});
}

TEST(Sbc_SelectConfiguration_PrefersFrameParameters)
{
	SbcCodec codec;
	const uint8_t remote[4] = {kFs44100 | kFs48000 | kModeJoint | kModeStereo,
							 kBlocks4 | kBlocks8 | kBlocks12 | kBlocks16 | kSubbands4 | kSubbands8 |
								 kAllocationSnr | kAllocationLoudness,
							 2, 53};
	A2dpCodecConfig cfg;

	CodecPreference pref;
	pref.sbcBlocks = 8;
	pref.sbcSubbands = 4;
	pref.sbcAllocation = kAllocationSnr;
	REQUIRE(codec.SelectConfiguration(remote, 4, pref, cfg));
	CHECK_EQ(cfg.ie[1], uint8_t{kBlocks8 | kSubbands4 | kAllocationSnr});

	// A pinned value the sink does not offer falls back to the selector's own order.
	const uint8_t narrow[4] = {kFs44100 | kModeJoint, kBlocks16 | kSubbands8 | kAllocationLoudness, 2, 53};
	CodecPreference unavailable;
	unavailable.sbcBlocks = 4;
	unavailable.sbcSubbands = 4;
	unavailable.sbcAllocation = kAllocationSnr;
	REQUIRE(codec.SelectConfiguration(narrow, 4, unavailable, cfg));
	CHECK_EQ(cfg.ie[1], uint8_t{kBlocks16 | kSubbands8 | kAllocationLoudness});
}

TEST(Sbc_Select_PrefersEndpointChannels)
{
	// 44.1 kHz, joint stereo and mono, 16 blocks, 8 subbands, loudness, bitpool 2..53.
	const uint8_t remote[4] = {0x29, 0x15, 2, 53};
	uint8_t ie[4] = {};

	SbcConfig::FramePreference mono;
	mono.channels = 1;
	REQUIRE(SbcConfig::Select(remote, 4, 0, 53, ie, mono));
	CHECK_EQ(ie[0], uint8_t{0x28});

	SbcConfig::FramePreference stereo;
	stereo.channels = 2;
	REQUIRE(SbcConfig::Select(remote, 4, 0, 53, ie, stereo));
	CHECK_EQ(ie[0], uint8_t{0x21});

	// A sink without the requested channel mode still gets a usable configuration.
	const uint8_t stereoOnly[4] = {0x21, 0x15, 2, 53};
	REQUIRE(SbcConfig::Select(stereoOnly, 4, 0, 53, ie, mono));
	CHECK_EQ(ie[0], uint8_t{0x21});
}

// Every bitpool the preference can request must still produce frames that fit one RTP
// payload, otherwise a stored value makes the stream unsendable.
TEST(Sbc_SelectedFramesFitMediaPayload)
{
	SbcCodec codec;
	const uint8_t remote[4] = {kFs44100 | kModeJoint, kBlocks16 | kSubbands8 | kAllocationLoudness, 2, 250};
	const uint8_t presets[] = {2, 35, 53, 250};
	for (const uint8_t bitpool : presets)
	{
		CodecPreference pref;
		pref.sbcMaxBitpool = bitpool;
		A2dpCodecConfig cfg;
		REQUIRE(codec.SelectConfiguration(remote, 4, pref, cfg));
		FrameParams params;
		REQUIRE(ParseConfiguredIe(cfg.ie, cfg.ieLength, params));
		CHECK_EQ(params.bitpool, bitpool);
		CHECK(FrameLength(params) <= Ioctl::kMaxFramePayload);
		std::unique_ptr<IA2dpEncoder> encoder;
		REQUIRE(SUCCEEDED(codec.CreateEncoder(cfg, encoder)));
		CHECK(encoder->MaxFrameBytes() <= Ioctl::kMaxFramePayload);
		CHECK(encoder->FramesPerPacket(Ioctl::kMaxFramePayload) >= 1);
	}
}

TEST(Sbc_SelectConfiguration_RejectsNoCommonMode)
{
	SbcCodec codec;
	const uint8_t remote[4] = {kFs44100, kBlocks16 | kSubbands8 | kAllocationLoudness, 2, 53}; // no channel mode
	A2dpCodecConfig cfg;
	CHECK(!codec.SelectConfiguration(remote, 4, CodecPreference{}, cfg));
}

TEST(Sbc_ValidateConfiguration_RejectsAmbiguous)
{
	SbcCodec codec;
	const uint8_t twoRates[4] = {kFs44100 | kFs48000 | kModeJoint, kBlocks16 | kSubbands8 | kAllocationLoudness, 2, 53};
	CHECK_EQ(codec.ValidateConfiguration(twoRates, 4), static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency));
	const uint8_t badPool[4] = {kFs44100 | kModeMono, kBlocks16 | kSubbands8 | kAllocationLoudness, 2, 200};
	CHECK_EQ(codec.ValidateConfiguration(badPool, 4), static_cast<uint8_t>(Avdtp::ErrorCode::InvalidMaximumBitpoolValue));
}

TEST(Sbc_Encode_SilenceProducesValidFrames)
{
	const auto p = Params(44100, 16, 8, ChannelMode::JointStereo, Allocation::Loudness, 53);
	SbcEncoder encoder(p);
	std::vector<int16_t> pcm(encoder.SamplesPerFrame() * 2, 0);
	uint8_t frame[512];
	size_t written = 0;
	REQUIRE(SUCCEEDED(encoder.Encode(pcm.data(), frame, sizeof(frame), written)));
	CHECK_EQ(written, size_t{119});
	CHECK_EQ(frame[0], kSyncWord);
	CHECK_EQ(frame[1], uint8_t{(2 << 6) | (3 << 4) | (3 << 2) | (0 << 1) | 1});
	CHECK_EQ(frame[2], uint8_t{53});
}

TEST(Sbc_Encode_RejectsSmallBuffer)
{
	SbcEncoder encoder(Params(44100, 16, 8, ChannelMode::JointStereo, Allocation::Loudness, 53));
	std::vector<int16_t> pcm(256, 0);
	uint8_t frame[64];
	size_t written = 0;
	CHECK(encoder.Encode(pcm.data(), frame, sizeof(frame), written) == E_INVALIDARG);
}

TEST(Sbc_RoundTrip_HighQualityJointStereo)
{
	CheckRoundTrip(Params(44100, 16, 8, ChannelMode::JointStereo, Allocation::Loudness, 53), 25.0);
}

TEST(Sbc_RoundTrip_Stereo48k)
{
	CheckRoundTrip(Params(48000, 16, 8, ChannelMode::Stereo, Allocation::Loudness, 53), 17.0);
}

TEST(Sbc_RoundTrip_FourSubbandsSnr)
{
	CheckRoundTrip(Params(44100, 16, 4, ChannelMode::JointStereo, Allocation::Snr, 53), 30.0);
}

TEST(Sbc_RoundTrip_FilterbankNearTransparent)
{
	CheckRoundTrip(Params(44100, 16, 8, ChannelMode::Mono, Allocation::Snr, 128), 60.0);
	CheckRoundTrip(Params(44100, 16, 4, ChannelMode::Mono, Allocation::Snr, 64), 60.0);
}

TEST(Sbc_RoundTrip_EightSubbandsSnr)
{
	CheckRoundTrip(Params(44100, 16, 8, ChannelMode::JointStereo, Allocation::Snr, 53), 30.0);
}

TEST(Sbc_RoundTrip_Mono)
{
	CheckRoundTrip(Params(44100, 16, 8, ChannelMode::Mono, Allocation::Loudness, 31), 18.0);
}

TEST(Sbc_RoundTrip_DualChannel)
{
	CheckRoundTrip(Params(32000, 12, 8, ChannelMode::DualChannel, Allocation::Loudness, 32), 20.0);
}

TEST(Sbc_RoundTrip_JointStereoIdenticalChannels)
{
	const auto p = Params(44100, 16, 8, ChannelMode::JointStereo, Allocation::Loudness, 53);
	const auto pcm = MakeSine(44100, 2, 22050, 440.0, 440.0, 16000.0);
	const auto r = RoundTrip(p, pcm, 0);
	std::printf("    identical L/R: SNR=%.1f dB gain=%.4f\n", r.snrDb, r.gain);
	CHECK(r.allCrcOk);
	CHECK(r.snrDb >= 30.0);
}

TEST(Sbc_MediaPayload)
{
	SbcEncoder encoder(Params(44100, 16, 8, ChannelMode::JointStereo, Allocation::Loudness, 53));
	CHECK_EQ(encoder.FramesPerPacket(895 - 12), size_t{7});
	CHECK_EQ(encoder.FramesPerPacket(5000), size_t{15});

	std::vector<uint8_t> frames(119 * 5, 0xAB);
	const size_t sizes[5] = {119, 119, 119, 119, 119};
	std::vector<uint8_t> payload(1024);
	const size_t len = encoder.BuildMediaPayload(frames.data(), sizes, 5, payload.data(), payload.size());
	CHECK_EQ(len, size_t{1 + 119 * 5});
	CHECK_EQ(payload[0], uint8_t{5});
	CHECK_EQ(payload[1], uint8_t{0xAB});
	CHECK_EQ(encoder.BuildMediaPayload(frames.data(), sizes, 5, payload.data(), 100), size_t{0});
}
