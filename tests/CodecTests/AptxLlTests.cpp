#include "TestFramework.h"

#include <OpenA2dp/Aptx.h>
#include <OpenA2dp/AptxCodec.h>
#include <OpenA2dp/AptxLlCodec.h>
#include <OpenA2dp/AptxStreamEncoder.h>
#include <OpenA2dp/RingBuffer.h>

#include <cstring>
#include <memory>
#include <vector>

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

namespace {

std::unique_ptr<IA2dpEncoder> MakeClassicEncoder()
{
	uint8_t ie[Aptx::kIeLength] = {};
	Aptx::WriteIe(Aptx::kSampleRate48000, ie);
	A2dpCodecConfig config = {};
	config.codec = CodecType::Vendor;
	std::memcpy(config.ie, ie, sizeof(ie));
	config.ieLength = static_cast<uint8_t>(sizeof(ie));
	AptxCodec codec;
	std::unique_ptr<IA2dpEncoder> encoder;
	if (FAILED(codec.CreateEncoder(config, encoder))) {
		return nullptr;
	}
	return encoder;
}

RingBufferView MakeRing(uint8_t* header, std::vector<uint8_t>& data)
{
	RingBufferView::Initialize(reinterpret_cast<RingHeader*>(header),
							   static_cast<uint32_t>(data.size()));
	return RingBufferView(reinterpret_cast<RingHeader*>(header), data.data());
}

} // namespace

TEST(AptxLl_RecordsCoverTwoMilliseconds)
{
	// The same media span must leave as five times as many records when the span is 2 ms
	// instead of 10, and cover the same audio either way.
	alignas(64) uint8_t llPcmHeader[sizeof(RingHeader)] = {};
	alignas(64) uint8_t llFrameHeader[sizeof(RingHeader)] = {};
	std::vector<uint8_t> llPcmData(64 * 1024);
	std::vector<uint8_t> llFrameData(64 * 1024);
	RingBufferView llPcm = MakeRing(llPcmHeader, llPcmData);
	RingBufferView llFrames = MakeRing(llFrameHeader, llFrameData);

	AptxStreamParams llParams;
	llParams.sampleRate = 48000;
	llParams.recordMs = 2;
	AptxStreamEncoder ll;
	REQUIRE(ll.Start(MakeClassicEncoder(), llParams, 0));
	REQUIRE(ll.Pump(10000, llPcm, llFrames, 100));

	alignas(64) uint8_t pcmHeader[sizeof(RingHeader)] = {};
	alignas(64) uint8_t frameHeader[sizeof(RingHeader)] = {};
	std::vector<uint8_t> pcmData(64 * 1024);
	std::vector<uint8_t> frameData(64 * 1024);
	RingBufferView pcm = MakeRing(pcmHeader, pcmData);
	RingBufferView frames = MakeRing(frameHeader, frameData);

	AptxStreamParams classicParams;
	classicParams.sampleRate = 48000;
	AptxStreamEncoder classic;
	REQUIRE(classic.Start(MakeClassicEncoder(), classicParams, 0));
	REQUIRE(classic.Pump(10000, pcm, frames, 100));

	CHECK(ll.Stats().records > 0);
	CHECK_EQ(ll.Stats().records, classic.Stats().records * 5);
	CHECK_EQ(ll.Stats().samples, classic.Stats().samples);
}
