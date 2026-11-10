#include "TestFramework.h"

#include <OpenA2dp/AptxCodec.h>
#include <OpenA2dp/AptxHdCodec.h>
#include <OpenA2dp/AptxStreamEncoder.h>
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

// Interleaved int16 stereo, one sample pair per frame.
void WritePcm(TestRing& ring, int16_t firstSample, uint32_t frames)
{
	for (uint32_t frame = 0; frame < frames; ++frame) {
		int16_t samples[2] = {static_cast<int16_t>(firstSample + static_cast<int16_t>(frame)),
							  static_cast<int16_t>(firstSample - static_cast<int16_t>(frame))};
		REQUIRE(ring.view.Write(samples, sizeof(samples)));
	}
}

std::unique_ptr<IA2dpEncoder> MakeEncoder(uint8_t capabilities)
{
	uint8_t ie[Aptx::kIeLength] = {};
	Aptx::WriteIe(capabilities, ie);
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

// An aptX HD encoder takes 24-in-32, so a classic stream must not accept it.
std::unique_ptr<IA2dpEncoder> MakeWideEncoder()
{
	uint8_t ie[AptxHd::kIeLength] = {};
	AptxHd::WriteIe(AptxHd::kSampleRate48000, ie);
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

// A 16-bit stereo encoder whose frame shape is not the classic one. The stream frames with
// Aptx::kSamplesPerFrame and Aptx::kFrameBytes, so a different shape would read past the
// PCM record or write past the frame stride.
class OddShapeEncoder final : public IA2dpEncoder
{
public:
	OddShapeEncoder(uint32_t samplesPerFrame, size_t frameBytes)
		: samplesPerFrame_(samplesPerFrame), frameBytes_(frameBytes)
	{
	}

	PcmFormat InputFormat() const noexcept override { return PcmFormat{48000, 2, 16}; }
	uint32_t SamplesPerFrame() const noexcept override { return samplesPerFrame_; }
	size_t MaxFrameBytes() const noexcept override { return frameBytes_; }
	HRESULT Encode(const uint8_t*, uint8_t*, size_t, size_t& written) noexcept override
	{
		written = 0;
		return E_FAIL;
	}
	size_t FramesPerPacket(size_t payloadCapacity) const noexcept override
	{
		return frameBytes_ != 0 ? payloadCapacity / frameBytes_ : 0;
	}
	size_t BuildMediaPayload(const uint8_t*, const size_t*, size_t, uint8_t*, size_t) noexcept override
	{
		return 0;
	}

private:
	uint32_t samplesPerFrame_;
	size_t frameBytes_;
};

} // namespace

TEST(AptxStream_StartRejectsUnusableEncoder)
{
	TestRing pcm(64 * 1024);
	AptxStreamEncoder stream;

	CHECK(!stream.Start(nullptr, AptxStreamParams{}, pcm.view, 0));

	// 44.1 kHz encoder with a 48 kHz stream would consume the wrong pace.
	AptxStreamParams params;
	params.sampleRate = 48000;
	CHECK(!stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, pcm.view, 0));
	CHECK(!stream.Active());

	// A 24-bit encoder cannot be fed from the 16-bit ring.
	auto wide = MakeWideEncoder();
	REQUIRE(wide != nullptr);
	CHECK(!stream.Start(std::move(wide), params, pcm.view, 0));

	// The record layout is four samples to four bytes; another shape would overrun a buffer.
	CHECK(!stream.Start(std::make_unique<OddShapeEncoder>(2, Aptx::kFrameBytes), params, pcm.view, 0));
	CHECK(!stream.Start(std::make_unique<OddShapeEncoder>(Aptx::kSamplesPerFrame, 2), params, pcm.view, 0));
}

TEST(AptxStream_PacesTenMillisecondRecords)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 48000;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate48000), params, pcm.view, 1000));
	CHECK_EQ(stream.TargetSamples(1000), uint64_t{2880}); // 60 ms preroll

	WritePcm(pcm, 1, 2880);
	REQUIRE(stream.Pump(1000, pcm.view, frames.view, 8));

	// One record is 10 ms: 480 samples and 120 four-byte frames.
	CHECK_EQ(stream.Stats().records, uint64_t{6});
	CHECK_EQ(stream.Stats().samples, uint64_t{2880});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{0});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
	CHECK_EQ(pcm.view.ReadableBytes(), uint32_t{0});

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.samples, uint32_t{480});
	CHECK_EQ(record.payloadLength, uint32_t{480});
	CHECK_EQ(record.flags, Ioctl::kFrameFlagDiscontinuity);
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.samples, uint32_t{480});
	CHECK_EQ(record.flags, uint32_t{0});
}

TEST(AptxStream_CarriesSamplesAt44k)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 44100;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, pcm.view, 0));

	// 60 ms of 44.1 kHz is 2646 samples, exactly six spans of 441.
	WritePcm(pcm, 1, 2646);
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));

	CHECK_EQ(stream.Stats().records, uint64_t{6});
	CHECK_EQ(stream.Stats().pcmBytes, uint64_t{2646 * 4});
	CHECK_EQ(pcm.view.ReadableBytes(), uint32_t{0});
	// A frame holds four samples, so 441 samples leave one behind; over six records the
	// encoded total trails the clock by the two samples still carried.
	CHECK_EQ(stream.Stats().samples, uint64_t{2644});

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.samples, uint32_t{440}); // 441 samples make 110 whole frames
	CHECK_EQ(record.payloadLength, uint32_t{440});
}

// 441 samples per span never divide into four-sample frames, so the carry must absorb the
// remainder without the encoded total falling further behind the clock on every record.
TEST(AptxStream_44kClockDoesNotDrift)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 44100;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, pcm.view, 0));

	// One second plus the 60 ms preroll is 106 spans of 441 samples, all silence. A clock that
	// advanced by the encoded samples rather than by the span would trail by 26 samples.
	REQUIRE(stream.Pump(1000000, pcm.view, frames.view, 200));
	CHECK_EQ(stream.Stats().records, uint64_t{106});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{46746});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
	// Only the 0..3 samples a record cannot encode are still owed to the clock.
	CHECK(stream.Stats().samples >= 46743);
	CHECK(stream.Stats().samples <= 46746);
	CHECK_EQ(stream.Stats().samples % 4, uint64_t{0});
}

// Four spans are 1764 samples at 44.1 kHz, 441 whole frames. The records therefore run
// 440/440/440/444 samples: the RTP timestamp advances by the encoded count, so any other
// split would leave the stream drifting against the media clock.
TEST(AptxStream_RecordSamplesLockToFourSpans)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 44100;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, pcm.view, 0));

	// The 60 ms preroll is 2646 samples, six spans; four records are enough for one period.
	WritePcm(pcm, 1, 2646);
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 4));

	const uint32_t expected[4] = {440, 440, 440, 444};
	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	uint32_t total = 0;
	for (uint32_t i = 0; i < 4; ++i) {
		REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) ==
				FrameRecord::ReadResult::Record);
		CHECK_EQ(record.samples, expected[i]);
		CHECK_EQ(record.payloadLength, expected[i]); // four bytes per four samples
		total += record.samples;
	}
	// Four spans are exactly 1764 samples, so nothing is owed to the clock here.
	CHECK_EQ(total, uint32_t{1764});
}

TEST(AptxStream_ShortRingBecomesSilence)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 44100;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, pcm.view, 0));

	// One span of PCM for six due records: every missing sample becomes silence, and the one
	// sample the first record cannot fit is still carried.
	WritePcm(pcm, 1, 441);
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));

	CHECK_EQ(stream.Stats().records, uint64_t{6});
	CHECK_EQ(stream.Stats().pcmBytes, uint64_t{441 * 4});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{2646 - 441});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
}
