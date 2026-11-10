#include "TestFramework.h"

#include <OpenA2dp/AptxHdCodec.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace OpenA2dp;
using namespace OpenA2dp::AptxHd;

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
	std::memset(ie + 7, 0, kIeLength - 7);
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
	AptxHdCodec codec;
	std::unique_ptr<IA2dpEncoder> encoder;
	const HRESULT hr = codec.CreateEncoder(ConfigWithCapabilities(capabilities), encoder);
	if (result != nullptr) {
		*result = hr;
	}
	return encoder;
}

// 24-in-32: the low byte of every sample is zero, as the Windows 24-bit formats carry them.
std::vector<uint8_t> Pcm(int32_t valueLeft, int32_t valueRight)
{
	std::vector<int32_t> samples(kSamplesPerFrame * 2);
	for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
		samples[2 * i] = valueLeft;
		samples[2 * i + 1] = valueRight;
	}
	std::vector<uint8_t> bytes(samples.size() * sizeof(int32_t));
	std::memcpy(bytes.data(), samples.data(), bytes.size());
	return bytes;
}

} // namespace

TEST(AptxHd_Capabilities)
{
	AptxHdCodec codec;
	uint8_t ie[kIeLength] = {};
	REQUIRE(codec.BuildLocalCapabilities(ie, sizeof(ie)) == kIeLength);
	CHECK_EQ(ie[0], uint8_t{0xD7});
	CHECK_EQ(ie[1], uint8_t{0x00});
	CHECK_EQ(ie[2], uint8_t{0x00});
	CHECK_EQ(ie[3], uint8_t{0x00});
	CHECK_EQ(ie[4], uint8_t{0x24});
	CHECK_EQ(ie[5], uint8_t{0x00});
	CHECK_EQ(ie[6], static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000 | kChannelStereo));
	CHECK_EQ(ie[7], uint8_t{0x00});
	CHECK_EQ(ie[10], uint8_t{0x00});
	// One byte short is not enough room for the IE.
	CHECK(codec.BuildLocalCapabilities(ie, kIeLength - 1) == 0);
}

TEST(AptxHd_SelectConfiguration)
{
	AptxHdCodec codec;
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

	// Mono-only sinks and foreign vendor IDs are refused.
	BuildRemoteIe(kSampleRate48000 | kChannelMono, remote);
	CHECK(!codec.SelectConfiguration(remote, kIeLength, preferred, config));
	BuildRemoteIe(kSampleRate48000 | kChannelStereo, remote);
	remote[0] = 0x4F; // aptX classic's vendor ID.
	CHECK(!codec.SelectConfiguration(remote, kIeLength, preferred, config));
	CHECK(!codec.SelectConfiguration(remote, kIeLength - 1, preferred, config));
}

TEST(AptxHd_ValidateConfiguration)
{
	AptxHdCodec codec;

	uint8_t ie[kIeLength] = {};
	BuildRemoteIe(kSampleRate48000 | kChannelStereo, ie);
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength), uint8_t{0});
	BuildRemoteIe(kSampleRate44100 | kChannelStereo, ie);
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength), uint8_t{0});

	BuildRemoteIe(kSampleRate48000 | kChannelStereo, ie);
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
	ie[3] = 0x01; // vendor ID 0x010000D7
	CHECK_EQ(codec.ValidateConfiguration(ie, kIeLength),
			 static_cast<uint8_t>(Avdtp::ErrorCode::InvalidCodecType));
}

TEST(AptxHd_EncoderFormat)
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
	// AOSP packs 648 bytes of aptX HD into one media packet at this MTU.
	CHECK_EQ(encoder->FramesPerPacket(648), size_t{108});
	CHECK_EQ(encoder->FramesPerPacket(kFrameBytes - 1), size_t{0});
}

TEST(AptxHd_EncodeFrame)
{
	auto encoder = MakeEncoder(kSampleRate48000 | kChannelStereo);
	REQUIRE(encoder != nullptr);

	const std::vector<uint8_t> pcm = Pcm(0, 0);
	std::vector<uint8_t> frame(kFrameBytes);
	size_t written = 0;
	REQUIRE(SUCCEEDED(encoder->Encode(pcm.data(), frame.data(), frame.size(), written)));
	CHECK_EQ(written, kFrameBytes);

	// A frame needs room for all six bytes and a full PCM frame to read.
	CHECK(encoder->Encode(pcm.data(), frame.data(), kFrameBytes - 1, written) == E_INVALIDARG);
	CHECK(encoder->Encode(nullptr, frame.data(), frame.size(), written) == E_INVALIDARG);
}

TEST(AptxHd_EncodeIgnoresBitsBelow24)
{
	// 24-in-32 keeps the valid bits in the high three bytes; anything in the low byte is not
	// sample data and must not reach the encoder.
	auto low = MakeEncoder(kSampleRate48000 | kChannelStereo);
	auto withDirt = MakeEncoder(kSampleRate48000 | kChannelStereo);
	REQUIRE(low != nullptr);
	REQUIRE(withDirt != nullptr);

	std::vector<uint8_t> clean = Pcm(0x12345600, -0x00123400);
	std::vector<uint8_t> dirty = Pcm(0x123456FF, -0x001234FF);
	std::vector<uint8_t> cleanFrame(kFrameBytes);
	std::vector<uint8_t> dirtyFrame(kFrameBytes);
	size_t cleanWritten = 0;
	size_t dirtyWritten = 0;
	REQUIRE(SUCCEEDED(low->Encode(clean.data(), cleanFrame.data(), cleanFrame.size(), cleanWritten)));
	REQUIRE(SUCCEEDED(withDirt->Encode(dirty.data(), dirtyFrame.data(), dirtyFrame.size(), dirtyWritten)));
	CHECK_EQ(cleanWritten, dirtyWritten);
	CHECK(std::memcmp(cleanFrame.data(), dirtyFrame.data(), kFrameBytes) == 0);
}

TEST(AptxHd_EncodeIsDeterministic)
{
	const std::vector<uint8_t> pcm = Pcm(0x01000000, -0x02000000);
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

TEST(AptxHd_BuildMediaPayload)
{
	auto encoder = MakeEncoder(kSampleRate48000 | kChannelStereo);
	REQUIRE(encoder != nullptr);

	const std::vector<uint8_t> frames = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	const size_t sizes[2] = {kFrameBytes, kFrameBytes};
	std::vector<uint8_t> payload(12);
	CHECK_EQ(encoder->BuildMediaPayload(frames.data(), sizes, 2, payload.data(), payload.size()),
			 size_t{12});
	CHECK(std::memcmp(payload.data(), frames.data(), 12) == 0);

	std::vector<uint8_t> small(11);
	CHECK_EQ(encoder->BuildMediaPayload(frames.data(), sizes, 2, small.data(), small.size()),
			 size_t{0});
	CHECK_EQ(encoder->BuildMediaPayload(frames.data(), sizes, 0, payload.data(), payload.size()),
			 size_t{0});
}

TEST(AptxHd_Factory)
{
	auto codec = CreateVendorCodec(kVendorId, kCodecId);
	REQUIRE(codec != nullptr);
	CHECK(codec->Type() == CodecType::Vendor);
	CHECK(std::strcmp(codec->Name(), "aptX HD") == 0);
	CHECK(CreateVendorCodec(kVendorId, 0x0001) == nullptr);
	CHECK(CreateVendorCodec(0x0000004F, kCodecId) == nullptr);
}
