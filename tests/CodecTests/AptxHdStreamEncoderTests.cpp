#include "TestFramework.h"

#include <OpenA2dp/AptxHdCodec.h>
#include <OpenA2dp/AptxHdStreamEncoder.h>
#include <OpenA2dp/FrameRecord.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace OpenA2dp;

namespace {

struct TestRing {
	alignas(64) uint8_t header[sizeof(RingHeader)];
	std::vector<uint8_t> data;
	RingBufferView view;

	explicit TestRing(uint32_t capacity) : data(capacity)
	{
		RingBufferView::Initialize(reinterpret_cast<RingHeader*>(header), capacity);
		view = RingBufferView(reinterpret_cast<RingHeader*>(header), data.data());
	}
};

// 24-in-32 stereo PCM: the low byte is always zero, as Windows writes it.
void WriteWidePcm(TestRing& ring, int32_t firstSample, uint32_t frames)
{
	for (uint32_t frame = 0; frame < frames; ++frame) {
		int32_t samples[2] = {(firstSample + static_cast<int32_t>(frame)) << 8,
							  (firstSample - static_cast<int32_t>(frame)) << 8};
		REQUIRE(ring.view.Write(samples, sizeof(samples)));
	}
}

std::unique_ptr<IA2dpEncoder> MakeEncoder(uint8_t capabilities)
{
	uint8_t ie[AptxHd::kIeLength] = {};
	AptxHd::WriteIe(capabilities, ie);
	A2dpCodecConfig config = {};
	config.codec = CodecType::Vendor;
	std::memcpy(config.ie, ie, sizeof(ie));
	config.ieLength = static_cast<uint8_t>(sizeof(ie));

	AptxHdCodec codec;
	std::unique_ptr<IA2dpEncoder> encoder;
	if (FAILED(codec.CreateEncoder(config, encoder))) {
		return nullptr;
	}
	return encoder;
}

} // namespace

TEST(AptxHdStream_StartRejectsUnusableEncoder)
{
	TestRing pcm(64 * 1024);
	AptxHdStreamEncoder stream;

	CHECK(!stream.Start(nullptr, AptxHdStreamParams{}, pcm.view, 0));

	// 44.1 kHz encoder with a 48 kHz stream would consume the wrong pace.
	AptxHdStreamParams params;
	params.sampleRate = 48000;
	CHECK(!stream.Start(MakeEncoder(AptxHd::kSampleRate44100), params, pcm.view, 0));
	CHECK(!stream.Active());
}

TEST(AptxHdStream_PacesTenMillisecondRecords)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxHdStreamParams params;
	params.sampleRate = 48000;
	AptxHdStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(AptxHd::kSampleRate48000), params, pcm.view, 1000));
	CHECK_EQ(stream.TargetSamples(1000), uint64_t{2880}); // 60 ms preroll

	WriteWidePcm(pcm, 1, 480);
	REQUIRE(stream.Pump(1000, pcm.view, frames.view, 8));

	// One record is 10 ms: 120 frames of four samples.
	CHECK_EQ(stream.Stats().records, uint64_t{6});
	CHECK_EQ(stream.Stats().samples, uint64_t{2880});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
	CHECK_EQ(pcm.view.ReadableBytes(), uint32_t{0});

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.samples, uint32_t{480});
	CHECK_EQ(record.payloadLength, uint32_t{720});
	CHECK_EQ(record.flags, Ioctl::kFrameFlagDiscontinuity);
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.flags, uint32_t{0});
}

TEST(AptxHdStream_ShortRingBecomesSilence)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxHdStreamParams params;
	params.sampleRate = 44100;
	AptxHdStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(AptxHd::kSampleRate44100), params, pcm.view, 0));

	// 60 ms of 44.1 kHz is 2646 samples; only one 440-sample record exists.
	WriteWidePcm(pcm, 1, 440);
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));

	CHECK_EQ(stream.Stats().records, uint64_t{7}); // 2646 samples need seven 440-sample records
	CHECK_EQ(stream.Stats().pcmBytes, uint64_t{440 * 8});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{3080 - 440});
}
