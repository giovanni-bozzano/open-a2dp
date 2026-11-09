#include "TestFramework.h"

#include <OpenA2dp/AacCodec.h>
#include <OpenA2dp/Ioctl.h>

#include "aac/LatmWriter.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace OpenA2dp;
using namespace OpenA2dp::Aac;

TEST(Aac_SampleRateField)
{
	uint8_t ie[6] = {};
	Params p;
	p.sampleRate = 48000;
	p.channels = 2;
	p.bitrate = 192000;
	WriteIe(p, ie);
	CHECK_EQ(ReadSampleRateField(ie), SampleRateMask(48000));
	CHECK_EQ(SampleRateMask(44100), uint16_t{0x010});
	CHECK_EQ(SampleRateMask(48000), uint16_t{0x008});
	CHECK_EQ(ie[1], uint8_t{0x00});
	CHECK_EQ(ie[2], uint8_t{0x84});
	CHECK_EQ(ie[3], uint8_t{0x02});
	CHECK_EQ(ie[4], uint8_t{0xEE});
	CHECK_EQ(ie[5], uint8_t{0x00});
}

TEST(Aac_SelectConfiguration_TypicalSink)
{
	AacCodec codec;
	// MPEG-2 LC, 44.1k + 48k, 1/2 channels, VBR, 320 kbps max.
	const uint8_t remote[6] = {0x80, 0x01, 0x8C, 0x84, 0xE2, 0x00};
	// The driver tells the selector how much one media packet carries (895 - 12), so the
	// 320 kbps the encoder accepts is capped at the 256 kbps a 44.1 kHz packet can hold.
	CodecPreference pref;
	pref.mediaPayloadBytes = 883;
	A2dpCodecConfig cfg;
	REQUIRE(codec.SelectConfiguration(remote, 6, pref, cfg));
	Params p;
	REQUIRE(ParseConfiguredIe(cfg.ie, cfg.ieLength, p));
	CHECK_EQ(p.objectType, kObjectMpeg2Lc);
	CHECK_EQ(p.sampleRate, uint32_t{44100});
	CHECK_EQ(p.channels, uint8_t{2});
	CHECK(!p.vbr);
	CHECK_EQ(p.bitrate, uint32_t{256000});
	CHECK_EQ(codec.ValidateConfiguration(cfg.ie, cfg.ieLength), uint8_t{0});
}

TEST(Aac_SelectConfiguration_RespectsSinkBitrateAndRate)
{
	AacCodec codec;
	// MPEG-4 LC only, 48k only, stereo, max 128 kbps.
	const uint8_t remote[6] = {0x40, 0x00, 0x84, 0x01, 0xF4, 0x00};
	CodecPreference pref;
	pref.preferredSampleRate = 44100;
	A2dpCodecConfig cfg;
	REQUIRE(codec.SelectConfiguration(remote, 6, pref, cfg));
	Params p;
	REQUIRE(ParseConfiguredIe(cfg.ie, cfg.ieLength, p));
	CHECK_EQ(p.objectType, kObjectMpeg4Lc);
	CHECK_EQ(p.sampleRate, uint32_t{48000});
	CHECK_EQ(p.bitrate, uint32_t{128000});
}

TEST(Aac_Select_PrefersEndpointChannels)
{
	// MPEG-4 LC, 44.1 and 48 kHz, mono and stereo, 192 kbps.
	uint8_t remote[6] = {};
	remote[0] = AacConfig::kObjectMpeg4Lc;
	const uint16_t rates = AacConfig::SampleRateMask(44100) | AacConfig::SampleRateMask(48000);
	remote[1] = static_cast<uint8_t>(rates >> 4);
	remote[2] = static_cast<uint8_t>(((rates & 0x0F) << 4) | AacConfig::kChannels1 | AacConfig::kChannels2);
	remote[3] = 0x02;
	remote[4] = 0xEE;

	uint8_t ie[6] = {};
	AacConfig::Params params;
	REQUIRE(AacConfig::Select(remote, 6, 48000, 0, ie, 1));
	REQUIRE(AacConfig::ParseConfiguredIe(ie, 6, params));
	CHECK_EQ(params.sampleRate, uint32_t{48000});
	CHECK_EQ(params.channels, uint8_t{1});

	REQUIRE(AacConfig::Select(remote, 6, 44100, 0, ie, 2));
	REQUIRE(AacConfig::ParseConfiguredIe(ie, 6, params));
	CHECK_EQ(params.sampleRate, uint32_t{44100});
	CHECK_EQ(params.channels, uint8_t{2});

	// No preference keeps the sink's own order (stereo first).
	REQUIRE(AacConfig::Select(remote, 6, 0, 0, ie));
	REQUIRE(AacConfig::ParseConfiguredIe(ie, 6, params));
	CHECK_EQ(params.channels, uint8_t{2});
}

// The ceiling the media MTU sets, per endpoint rate: 44.1 kHz cannot carry 320 kbps in one
// packet, 48 kHz can, and the advertised capability is the encoder ceiling.
TEST(Aac_LocalCapabilitiesMatchTheMediaMtu)
{
	const size_t payload = Ioctl::kMaxFramePayload;
	CHECK_EQ(AacConfig::MaxBitrateForMtu(payload, 44100), uint32_t{256000});
	CHECK_EQ(AacConfig::MaxBitrateForMtu(payload, 48000), uint32_t{320000});
	CHECK_EQ(AacConfig::ReadBitrate(AacConfig::kLocalCapabilities), uint32_t{320000});
}

TEST(Aac_MaxBitrateForMtu)
{
	// One 895-byte media packet carries 883 payload bytes; 256 kbps leaves the LATM
	// header and the CBR overshoot room at both rates, 320 kbps only at 48 kHz.
	CHECK_EQ(AacConfig::MaxBitrateForMtu(883, 44100), uint32_t{256000});
	CHECK_EQ(AacConfig::MaxBitrateForMtu(883, 48000), uint32_t{320000});
	CHECK_EQ(AacConfig::MaxBitrateForMtu(1488, 44100), uint32_t{320000});
	CHECK_EQ(AacConfig::MaxBitrateForMtu(1488, 48000), uint32_t{320000});
	CHECK_EQ(AacConfig::MaxBitrateForMtu(400, 44100), uint32_t{128000});
	CHECK_EQ(AacConfig::MaxBitrateForMtu(300, 44100), uint32_t{96000});
	CHECK_EQ(AacConfig::MaxBitrateForMtu(64, 44100), uint32_t{0});
}

TEST(Aac_Select_HonoursMtuBitrateLimit)
{
	// MPEG-4 LC, 44.1 kHz, stereo, sink allows up to 512 kbps.
	uint8_t remote[6] = {};
	remote[0] = AacConfig::kObjectMpeg4Lc;
	const uint16_t rates = AacConfig::SampleRateMask(44100);
	remote[1] = static_cast<uint8_t>(rates >> 4);
	remote[2] = static_cast<uint8_t>(((rates & 0x0F) << 4) | AacConfig::kChannels2);
	remote[3] = 0x07; // 512000 bps
	remote[4] = 0xD0;

	uint8_t ie[6] = {};
	AacConfig::Params p;
	REQUIRE(AacConfig::Select(remote, 6, 44100, 0, ie, 2, 883));
	REQUIRE(AacConfig::ParseConfiguredIe(ie, 6, p));
	CHECK_EQ(p.bitrate, uint32_t{256000});

	// Without a media-MTU limit the encoder ceiling wins over the sink's maximum.
	REQUIRE(AacConfig::Select(remote, 6, 44100, 0, ie, 2));
	REQUIRE(AacConfig::ParseConfiguredIe(ie, 6, p));
	CHECK_EQ(p.bitrate, uint32_t{320000});
}

TEST(Aac_SelectConfiguration_BitratePreference)
{
	AacCodec codec;
	// MPEG-2 LC, 44.1k + 48k, mono + stereo, CBR, 192 kbps max.
	const uint8_t remote[6] = {0x80, 0x01, 0x8C, 0x02, 0xEE, 0x00};
	const struct
	{
		uint32_t requested;
		uint32_t expected;
	} cases[] = {{96000, 96000}, {150000, 128000}, {192000, 192000}, {400000, 192000}};
	for (const auto& c : cases)
	{
		CodecPreference pref;
		pref.aacBitrate = c.requested;
		A2dpCodecConfig cfg;
		REQUIRE(codec.SelectConfiguration(remote, 6, pref, cfg));
		Params p;
		REQUIRE(ParseConfiguredIe(cfg.ie, cfg.ieLength, p));
		CHECK_EQ(p.bitrate, c.expected);
	}
}

TEST(Aac_SelectConfiguration_RejectsUnsupported)
{
	AacCodec codec;
	A2dpCodecConfig cfg;
	const uint8_t ltpOnly[6] = {0x20, 0x01, 0x84, 0x03, 0xE8, 0x00};
	CHECK(!codec.SelectConfiguration(ltpOnly, 6, CodecPreference{}, cfg));
	const uint8_t only32k[6] = {0x80, 0x02, 0x04, 0x03, 0xE8, 0x00};
	CHECK(!codec.SelectConfiguration(only32k, 6, CodecPreference{}, cfg));
}

TEST(Aac_ValidateConfiguration)
{
	AacCodec codec;
	const uint8_t twoObjects[6] = {0xC0, 0x01, 0x04, 0x02, 0xEE, 0x00};
	CHECK_EQ(codec.ValidateConfiguration(twoObjects, 6), static_cast<uint8_t>(Avdtp::ErrorCode::InvalidObjectType));
	const uint8_t vbr[6] = {0x80, 0x01, 0x04, 0x82, 0xEE, 0x00};
	CHECK_EQ(codec.ValidateConfiguration(vbr, 6), static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedBitRate));
	const uint8_t rate32k[6] = {0x80, 0x02, 0x04, 0x02, 0xEE, 0x00};
	CHECK_EQ(codec.ValidateConfiguration(rate32k, 6), static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency));
}

TEST(Aac_Latm_RoundTrip)
{
	std::vector<uint8_t> au(600);
	for (size_t i = 0; i < au.size(); ++i) {
		au[i] = static_cast<uint8_t>(i * 7 + 3);
	}
	Latm::StreamConfig config;
	config.sampleRate = 48000;
	config.channels = 2;
	std::vector<uint8_t> out(au.size() + Latm::MaxOverhead(au.size()));
	const size_t len = Latm::WriteAudioMuxElement(config, au.data(), au.size(), out.data(), out.size());
	REQUIRE(len > au.size());
	CHECK(len <= out.size());

	auto parsed = std::make_unique<Latm::ParsedElement>();
	REQUIRE(Latm::ParseAudioMuxElement(out.data(), len, *parsed));
	CHECK_EQ(parsed->config.audioObjectType, uint8_t{2});
	CHECK_EQ(parsed->config.sampleRate, uint32_t{48000});
	CHECK_EQ(parsed->config.channels, uint8_t{2});
	CHECK_EQ(parsed->accessUnitLength, au.size());
	CHECK(std::equal(au.begin(), au.end(), parsed->accessUnit));
}

TEST(Aac_Latm_KnownHeaderBits)
{
	// useSameStreamMux=0, version=0, allSame=1, numSubFrames=0 -> first byte 0b0010_0000 = 0x20
	const uint8_t au[3] = {1, 2, 3};
	Latm::StreamConfig config; // AAC LC, 44.1k (index 4), stereo
	uint8_t out[32] = {};
	const size_t len = Latm::WriteAudioMuxElement(config, au, sizeof(au), out, sizeof(out));
	REQUIRE(len > 0);
	CHECK_EQ(out[0], uint8_t{0x20});
	// numSubFrames low bit 0, numProgram 0000, numLayer 000 -> 0x00
	CHECK_EQ(out[1], uint8_t{0x00});
	// objectType 00010, freqIndex (4 = 0100) high 3 bits 010 -> 0b00010_010 = 0x12
	CHECK_EQ(out[2], uint8_t{0x12});
	// freqIndex low bit 0, channels 0010, GASpecificConfig 000 -> 0b0_0010_000 = 0x10
	CHECK_EQ(out[3], uint8_t{0x10});
}

TEST(Aac_MediaFoundation_EncodeSine)
{
	AacCodec codec;
	Params p;
	p.sampleRate = 44100;
	p.channels = 2;
	p.bitrate = 192000;
	A2dpCodecConfig cfg;
	cfg.codec = CodecType::Aac;
	WriteIe(p, cfg.ie);
	cfg.ieLength = kIeLength;

	std::unique_ptr<IA2dpEncoder> encoder;
	const HRESULT hr = codec.CreateEncoder(cfg, encoder);
	if (FAILED(hr)) {
		std::printf("    Media Foundation AAC encoder unavailable (hr=0x%08lX), e.g. Windows N without Media Feature Pack\n",
					static_cast<unsigned long>(hr));
	}
	REQUIRE(SUCCEEDED(hr));
	CHECK_EQ(encoder->SamplesPerFrame(), uint32_t{1024});

	const size_t frames = 200;
	std::vector<int16_t> pcm(1024 * 2);
	std::vector<uint8_t> au(encoder->MaxFrameBytes() * 2);
	std::vector<uint8_t> payload(2048);
	size_t produced = 0;
	size_t totalBytes = 0;
	size_t maxAu = 0;
	size_t n = 0;
	for (size_t f = 0; f < frames; ++f) {
		for (size_t i = 0; i < 1024; ++i, ++n) {
			const double s = 12000.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / 44100.0);
			pcm[i * 2] = static_cast<int16_t>(s);
			pcm[i * 2 + 1] = static_cast<int16_t>(-s);
		}
		size_t written = 0;
		REQUIRE(SUCCEEDED(encoder->Encode(pcm.data(), au.data(), au.size(), written)));
		if (written == 0) {
			continue;
		}
		++produced;
		totalBytes += written;
		maxAu = std::max(maxAu, written);

		const size_t sizes[1] = {written};
		const size_t len = encoder->BuildMediaPayload(au.data(), sizes, 1, payload.data(), payload.size());
		CHECK(len > written);
		auto parsed = std::make_unique<Latm::ParsedElement>();
		CHECK(Latm::ParseAudioMuxElement(payload.data(), len, *parsed));
		CHECK_EQ(parsed->accessUnitLength, written);
	}

	const double avg = produced ? static_cast<double>(totalBytes) / produced : 0;
	std::printf("    AUs=%zu avg=%.1f bytes max=%zu bytes (expected avg ~557)\n", produced, avg, maxAu);
	CHECK(produced >= frames - 5);
	CHECK(avg > 400 && avg < 700);
	CHECK(maxAu <= encoder->MaxFrameBytes());
	CHECK_EQ(encoder->FramesPerPacket(895 - 12), size_t{1});
}

// The highest bitrate each endpoint rate can carry has to fit one 895-byte media packet
// without fragmentation, which is what the selector and the advertised ceiling promise.
TEST(Aac_MediaFoundation_EncodedFramesFitMediaPayload)
{
	const struct {
		uint32_t sampleRate;
		uint32_t bitrate;
	} cases[] = {{44100, 256000}, {48000, 320000}};

	AacCodec codec;
	for (const auto& c : cases) {
		Params p;
		p.sampleRate = c.sampleRate;
		p.channels = 2;
		p.bitrate = c.bitrate;
		A2dpCodecConfig cfg;
		cfg.codec = CodecType::Aac;
		WriteIe(p, cfg.ie);
		cfg.ieLength = kIeLength;

		std::unique_ptr<IA2dpEncoder> encoder;
		const HRESULT hr = codec.CreateEncoder(cfg, encoder);
		if (FAILED(hr)) {
			std::printf("    Media Foundation AAC encoder unavailable (hr=0x%08lX)\n",
						static_cast<unsigned long>(hr));
		}
		REQUIRE(SUCCEEDED(hr));

		std::vector<int16_t> pcm(1024 * 2);
		std::vector<uint8_t> au(encoder->MaxFrameBytes() * 2);
		std::vector<uint8_t> payload(2048);
		size_t produced = 0;
		size_t maxPayload = 0;
		size_t n = 0;
		for (size_t f = 0; f < 100; ++f) {
			for (size_t i = 0; i < 1024; ++i, ++n) {
				const double s =
					12000.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / c.sampleRate);
				pcm[i * 2] = static_cast<int16_t>(s);
				pcm[i * 2 + 1] = static_cast<int16_t>(-s);
			}
			size_t written = 0;
			REQUIRE(SUCCEEDED(encoder->Encode(pcm.data(), au.data(), au.size(), written)));
			if (written == 0) {
				continue;
			}
			const size_t sizes[1] = {written};
			const size_t len = encoder->BuildMediaPayload(au.data(), sizes, 1, payload.data(), payload.size());
			REQUIRE(len != 0);
			maxPayload = std::max(maxPayload, len);
			++produced;
		}
		std::printf("    %u bps @ %u Hz: %zu AUs, max media payload %zu bytes\n", c.bitrate, c.sampleRate,
					produced, maxPayload);
		CHECK(produced > 80);
		CHECK(maxPayload <= Ioctl::kMaxFramePayload);
		// The headroom the selector keeps has to cover the measured overhead.
		CHECK_EQ(AacConfig::MaxBitrateForMtu(Ioctl::kMaxFramePayload, c.sampleRate), c.bitrate);
	}
}
