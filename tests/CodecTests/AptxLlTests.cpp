#include "TestFramework.h"

#include <OpenA2dp/Aptx.h>
#include <OpenA2dp/AptxLlCodec.h>

#include <memory>

using namespace OpenA2dp;

TEST(AptxLl_IeRoundTrip)
{
	uint8_t ie[AptxLl::kIeLength] = {};
	uint8_t capabilities = 0;

	CHECK(!AptxLl::ParseIe(nullptr, 0, capabilities));
	AptxLl::WriteIe(AptxLl::kSampleRate48000, ie);
	CHECK(AptxLl::ParseIe(ie, sizeof(ie), capabilities));
	CHECK_EQ(capabilities,
			 static_cast<uint8_t>(AptxLl::kSampleRate48000 | AptxLl::kChannelStereo));
	CHECK_EQ(AptxLl::ValidateIe(ie, sizeof(ie)), uint8_t{0});
	CHECK_EQ(ie[7], AptxLl::kTargetBufferMs);

	// The identity is the reason LL needs its own SEP: classic aptX must not claim it.
	CHECK(!Aptx::ParseIe(ie, sizeof(ie), capabilities));
	Aptx::WriteIe(Aptx::kSampleRate48000, ie);
	CHECK(!AptxLl::ParseIe(ie, sizeof(ie), capabilities));
}

TEST(AptxLl_RejectsConfigurationsItCannotRun)
{
	AptxLlCodec codec;

	// A rate bit outside 44.1/48 kHz and a mono channel mode are both well-formed IEs that
	// cannot run.
	uint8_t ie[AptxLl::kIeLength] = {};
	AptxLl::WriteIe(AptxLl::kSampleRate48000, ie);
	ie[6] = 0x40 | AptxLl::kChannelStereo;
	CHECK_EQ(codec.ValidateConfiguration(ie, sizeof(ie)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency));

	AptxLl::WriteIe(AptxLl::kSampleRate48000, ie);
	ie[6] = AptxLl::kSampleRate48000 | AptxLl::kChannelMono;
	CHECK_EQ(codec.ValidateConfiguration(ie, sizeof(ie)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannelMode));
}

TEST(AptxLl_UsesTheClassicEncoder)
{
	AptxLlCodec codec;
	A2dpCodecConfig config = {};
	config.codec = CodecType::Vendor;
	AptxLl::WriteIe(AptxLl::kSampleRate48000, config.ie);
	config.ieLength = static_cast<uint8_t>(AptxLl::kIeLength);

	std::unique_ptr<IA2dpEncoder> encoder;
	REQUIRE(SUCCEEDED(codec.CreateEncoder(config, encoder)));
	REQUIRE(encoder != nullptr);
	CHECK_EQ(encoder->InputFormat().sampleRate, 48000u);
	CHECK_EQ(encoder->InputFormat().channels, uint16_t{2});
	CHECK_EQ(encoder->InputFormat().bitsPerSample, uint16_t{16});
	CHECK_EQ(encoder->SamplesPerFrame(), Aptx::kSamplesPerFrame);
	CHECK_EQ(encoder->MaxFrameBytes(), Aptx::kFrameBytes);
}
