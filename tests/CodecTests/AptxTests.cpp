#include "TestFramework.h"

#include <OpenA2dp/AptxCodec.h>
#include <OpenA2dp/AptxHdCodec.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace OpenA2dp;
using namespace OpenA2dp::Aptx;

namespace {

// The codec-specific IE a sink advertises, with the given sample-rate/channel-mode bits.
void BuildRemoteIe(uint8_t capabilities, uint8_t* ie)
{
	ie[0] = static_cast<uint8_t>(kVendorId & 0xFF);
	ie[1] = static_cast<uint8_t>((kVendorId >> 8) & 0xFF);
	ie[2] = static_cast<uint8_t>((kVendorId >> 16) & 0xFF);
	ie[3] = static_cast<uint8_t>((kVendorId >> 24) & 0xFF);
	ie[4] = static_cast<uint8_t>(kCodecId & 0xFF);
	ie[5] = static_cast<uint8_t>((kCodecId >> 8) & 0xFF);
	ie[6] = capabilities;
}

A2dpCodecConfig ConfigWithCapabilities(uint8_t capabilities)
{
	A2dpCodecConfig config = {};
	config.codec = CodecType::Vendor;
	BuildRemoteIe(capabilities, config.ie);
	config.ieLength = static_cast<uint8_t>(kIeLength);
	return config;
}

std::unique_ptr<IA2dpEncoder> MakeEncoder(uint8_t capabilities, HRESULT* result = nullptr)
{
	AptxCodec codec;
	std::unique_ptr<IA2dpEncoder> encoder;
	const HRESULT hr = codec.CreateEncoder(ConfigWithCapabilities(capabilities), encoder);
	if (result != nullptr) {
		*result = hr;
	}
	return encoder;
}

// Interleaved int16 stereo, four samples per channel.
std::vector<uint8_t> Pcm(int16_t valueLeft, int16_t valueRight)
{
	std::vector<int16_t> samples(kSamplesPerFrame * 2);
	for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
		samples[2 * i] = valueLeft;
		samples[2 * i + 1] = valueRight;
	}
	std::vector<uint8_t> bytes(samples.size() * sizeof(int16_t));
	std::memcpy(bytes.data(), samples.data(), bytes.size());
	return bytes;
}

} // namespace

TEST(Aptx_Capabilities)
{
	AptxCodec codec;
	uint8_t ie[kIeLength] = {};
	REQUIRE(codec.BuildLocalCapabilities(ie, sizeof(ie)) == kIeLength);
	CHECK_EQ(ie[0], uint8_t{0x4F});
	CHECK_EQ(ie[1], uint8_t{0x00});
	CHECK_EQ(ie[2], uint8_t{0x00});
	CHECK_EQ(ie[3], uint8_t{0x00});
	CHECK_EQ(ie[4], uint8_t{0x01});
	CHECK_EQ(ie[5], uint8_t{0x00});
	CHECK_EQ(ie[6], static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000 | kChannelStereo));
	// One byte short is not enough room for the IE.
	CHECK(codec.BuildLocalCapabilities(ie, kIeLength - 1) == 0);
}

TEST(Aptx_SelectConfiguration)
{
	AptxCodec codec;
	uint8_t remote[kIeLength] = {};
	A2dpCodecConfig config = {};

	// Both rates and stereo, no preference: 48 kHz.
	BuildRemoteIe(kSampleRate44100 | kSampleRate48000 | kChannelStereo, remote);
	REQUIRE(codec.SelectConfiguration(remote, kIeLength, CodecPreference{}, config));
	CHECK(config.codec == CodecType::Vendor);
	CHECK_EQ(config.ieLength, static_cast<uint8_t>(kIeLength));
	CHECK_EQ(config.ie[6], static_cast<uint8_t>(kSampleRate48000 | kChannelStereo));

	// A 44.1 kHz preference the sink advertises is honoured.
	CodecPreference preferred;
	preferred.preferredSampleRate = 44100;
	REQUIRE(codec.SelectConfiguration(remote, kIeLength, preferred, config));
	CHECK_EQ(config.ie[6], static_cast<uint8_t>(kSampleRate44100 | kChannelStereo));

	// A 48 kHz preference is kept when the sink only offers 44.1 kHz.
	BuildRemoteIe(kSampleRate44100 | kChannelStereo, remote);
	preferred.preferredSampleRate = 48000;
	REQUIRE(codec.SelectConfiguration(remote, kIeLength, preferred, config));
	CHECK_EQ(config.ie[6], static_cast<uint8_t>(kSampleRate44100 | kChannelStereo));

	// A 44.1 kHz preference falls back to 48 kHz when that is all the sink offers.
	BuildRemoteIe(kSampleRate48000 | kChannelStereo, remote);
	preferred.preferredSampleRate = 44100;
	REQUIRE(codec.SelectConfiguration(remote, kIeLength, preferred, config));
	CHECK_EQ(config.ie[6], static_cast<uint8_t>(kSampleRate48000 | kChannelStereo));

	// An IE with no rate bit at all cannot be configured.
	BuildRemoteIe(kChannelStereo, remote);
	CHECK(!codec.SelectConfiguration(remote, kIeLength, preferred, config));

	// Mono-only sinks are refused, and so is a truncated IE.
	BuildRemoteIe(kSampleRate48000 | kChannelMono, remote);
	CHECK(!codec.SelectConfiguration(remote, kIeLength, preferred, config));
	BuildRemoteIe(kSampleRate48000 | kChannelStereo, remote);
	CHECK(!codec.SelectConfiguration(remote, kIeLength - 1, preferred, config));
}

TEST(Aptx_ValidateConfiguration)
{
	AptxCodec codec;

	uint8_t ie[kIeLength] = {};
	BuildRemoteIe(kSampleRate48000 | kChannelStereo, ie);
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength), uint8_t{0});
	BuildRemoteIe(kSampleRate44100 | kChannelStereo, ie);
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength), uint8_t{0});

	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength - 1),
			 static_cast<uint8_t>(Avdtp::ErrorCode::BadLength));

	// Exactly one rate bit and one channel-mode bit, and stereo at that.
	BuildRemoteIe(kSampleRate44100 | kSampleRate48000 | kChannelStereo, ie);
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength),
			 static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency));
	BuildRemoteIe(kSampleRate48000 | kChannelMono, ie);
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength),
			 static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannelMode));
	BuildRemoteIe(kSampleRate48000, ie);
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength),
			 static_cast<uint8_t>(Avdtp::ErrorCode::InvalidChannelMode));
	BuildRemoteIe(kSampleRate48000 | kChannelStereo, ie);
	ie[3] = 0xD7; // aptX HD's vendor ID
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength),
			 static_cast<uint8_t>(Avdtp::ErrorCode::InvalidCodecType));
}

TEST(Aptx_HdAndClassicDoNotParseEachOther)
{
	// Both codecs share CodecType::Vendor, so only the IDs may decide which one an IE is.
	uint8_t classic[kIeLength] = {};
	uint8_t hd[AptxHd::kIeLength] = {};
	Aptx::WriteIe(kSampleRate48000, classic);
	AptxHd::WriteIe(kSampleRate48000, hd);

	uint8_t capabilities = 0;
	CHECK(Aptx::ParseIe(classic, sizeof(classic), capabilities));
	CHECK(!Aptx::ParseIe(hd, sizeof(hd), capabilities));
	CHECK(AptxHd::ParseIe(hd, sizeof(hd), capabilities));
	CHECK(!AptxHd::ParseIe(classic, sizeof(classic), capabilities));

	// A classic IE is too short to be an HD one, so HD reports the length before the IDs.
	AptxHdCodec hdCodec;
	CHECK_EQ(hdCodec.ValidateConfiguration(classic, sizeof(classic)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::BadLength));
	AptxCodec aptxCodec;
	CHECK_EQ(aptxCodec.ValidateConfiguration(hd, sizeof(hd)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::InvalidCodecType));
}

TEST(Aptx_EncoderFormat)
{
	HRESULT hr = E_FAIL;
	auto encoder = MakeEncoder(kSampleRate48000 | kChannelStereo, &hr);
	REQUIRE(SUCCEEDED(hr));
	REQUIRE(encoder != nullptr);

	const PcmFormat format = encoder->InputFormat();
	CHECK_EQ(format.sampleRate, 48000u);
	CHECK_EQ(format.channels, uint16_t{2});
	CHECK_EQ(format.bitsPerSample, kBitsPerSample);
	CHECK_EQ(encoder->SamplesPerFrame(), kSamplesPerFrame);
	CHECK_EQ(encoder->MaxFrameBytes(), kFrameBytes);
	CHECK_EQ(encoder->FramesPerPacket(648), size_t{162});
	CHECK_EQ(encoder->FramesPerPacket(kFrameBytes - 1), size_t{0});

	// The rate is the only thing the configuration changes about the input format.
	auto at441 = MakeEncoder(kSampleRate44100 | kChannelStereo);
	REQUIRE(at441 != nullptr);
	CHECK_EQ(at441->InputFormat().sampleRate, 44100u);
	CHECK_EQ(at441->SamplesPerFrame(), kSamplesPerFrame);
}

TEST(Aptx_EncoderRejectsInvalidConfiguration)
{
	AptxCodec codec;
	std::unique_ptr<IA2dpEncoder> encoder;

	// Mono, a truncated IE, two rate bits and an HD IE are all refused before any encoder
	// state is built.
	CHECK(codec.CreateEncoder(ConfigWithCapabilities(kSampleRate48000 | kChannelMono), encoder) ==
		  E_INVALIDARG);
	CHECK(encoder == nullptr);
	A2dpCodecConfig truncated = ConfigWithCapabilities(kSampleRate48000 | kChannelStereo);
	truncated.ieLength = static_cast<uint8_t>(kIeLength - 1);
	CHECK(codec.CreateEncoder(truncated, encoder) == E_INVALIDARG);
	CHECK(codec.CreateEncoder(ConfigWithCapabilities(kSampleRate44100 | kSampleRate48000 | kChannelStereo),
							  encoder) == E_INVALIDARG);

	uint8_t hd[AptxHd::kIeLength] = {};
	AptxHd::WriteIe(AptxHd::kSampleRate48000, hd);
	A2dpCodecConfig hdConfig = {};
	hdConfig.codec = CodecType::Vendor;
	std::memcpy(hdConfig.ie, hd, sizeof(hd));
	hdConfig.ieLength = static_cast<uint8_t>(sizeof(hd));
	CHECK(codec.CreateEncoder(hdConfig, encoder) == E_INVALIDARG);
	CHECK(encoder == nullptr);

	// A valid configuration still builds an encoder after those refusals.
	REQUIRE(SUCCEEDED(codec.CreateEncoder(ConfigWithCapabilities(kSampleRate48000 | kChannelStereo), encoder)));
	CHECK(encoder != nullptr);
}

TEST(Aptx_EncodeFrame)
{
	auto encoder = MakeEncoder(kSampleRate48000 | kChannelStereo);
	REQUIRE(encoder != nullptr);

	const std::vector<uint8_t> pcm = Pcm(0x1234, -0x1234);
	std::vector<uint8_t> frame(kFrameBytes);
	size_t written = 0;
	REQUIRE(SUCCEEDED(encoder->Encode(pcm.data(), frame.data(), frame.size(), written)));
	CHECK_EQ(written, kFrameBytes);
	// Smoothly varying input: a codeword of all zeros here would mean the encoder saw no
	// signal at all.
	CHECK(frame[0] != 0 || frame[1] != 0 || frame[2] != 0 || frame[3] != 0);

	// A frame needs room for all four bytes and a full PCM frame to read.
	CHECK(encoder->Encode(pcm.data(), frame.data(), kFrameBytes - 1, written) == E_INVALIDARG);
	CHECK(encoder->Encode(nullptr, frame.data(), frame.size(), written) == E_INVALIDARG);
}

TEST(Aptx_EncodeIsDeterministic)
{
	const std::vector<uint8_t> pcm = Pcm(0x0102, -0x0304);
	std::vector<uint8_t> first(kFrameBytes);
	std::vector<uint8_t> second(kFrameBytes);
	size_t written = 0;

	auto a = MakeEncoder(kSampleRate44100 | kChannelStereo);
	auto b = MakeEncoder(kSampleRate44100 | kChannelStereo);
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	REQUIRE(SUCCEEDED(a->Encode(pcm.data(), first.data(), first.size(), written)));
	REQUIRE(SUCCEEDED(b->Encode(pcm.data(), second.data(), second.size(), written)));
	CHECK(std::memcmp(first.data(), second.data(), kFrameBytes) == 0);
}

TEST(Aptx_BuildMediaPayload)
{
	auto encoder = MakeEncoder(kSampleRate48000 | kChannelStereo);
	REQUIRE(encoder != nullptr);

	const std::vector<uint8_t> frames = {1, 2, 3, 4, 5, 6, 7, 8};
	const size_t sizes[2] = {kFrameBytes, kFrameBytes};
	std::vector<uint8_t> payload(8);
	CHECK_EQ(encoder->BuildMediaPayload(frames.data(), sizes, 2, payload.data(), payload.size()),
			 size_t{8});
	CHECK(std::memcmp(payload.data(), frames.data(), 8) == 0);

	std::vector<uint8_t> small(7);
	CHECK_EQ(encoder->BuildMediaPayload(frames.data(), sizes, 2, small.data(), small.size()),
			 size_t{0});
	CHECK_EQ(encoder->BuildMediaPayload(frames.data(), sizes, 0, payload.data(), payload.size()),
			 size_t{0});
}

TEST(Aptx_Factory)
{
	auto codec = CreateVendorCodec(kVendorId, kCodecId);
	REQUIRE(codec != nullptr);
	CHECK(codec->Type() == CodecType::Vendor);
	CHECK(std::strcmp(codec->Name(), "aptX") == 0);
	CHECK(CreateVendorCodec(kVendorId, 0x0024) == nullptr);
	CHECK(CreateVendorCodec(0x000000D7, kCodecId) == nullptr);
}
