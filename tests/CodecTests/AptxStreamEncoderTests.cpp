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

// The same sweep written as 24-in-32, left-justified like the endpoint writes it.
void WriteWidePcm(TestRing& ring, int16_t firstSample, uint32_t frames)
{
	for (uint32_t frame = 0; frame < frames; ++frame) {
		const int32_t left = static_cast<int32_t>(firstSample + static_cast<int16_t>(frame)) * 65536;
		const int32_t right = static_cast<int32_t>(firstSample - static_cast<int16_t>(frame)) * 65536;
		int32_t samples[2] = {left, right};
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

	CHECK(!stream.Start(nullptr, AptxStreamParams{}, 0));

	// 44.1 kHz encoder with a 48 kHz stream would consume the wrong pace.
	AptxStreamParams params;
	params.sampleRate = 48000;
	CHECK(!stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, 0));
	CHECK(!stream.Active());

	// A 24-bit encoder cannot be fed from the 16-bit ring.
	auto wide = MakeWideEncoder();
	REQUIRE(wide != nullptr);
	CHECK(!stream.Start(std::move(wide), params, 0));

	// The ring is either 16-bit or 24-in-32; any other depth has no layout.
	params.bitsPerSample = 20;
	CHECK(!stream.Start(MakeEncoder(Aptx::kSampleRate48000), params, 0));

	// The record layout is four samples to four bytes; another shape would overrun a buffer.
	CHECK(!stream.Start(std::make_unique<OddShapeEncoder>(2, Aptx::kFrameBytes), params, 0));
	CHECK(!stream.Start(std::make_unique<OddShapeEncoder>(Aptx::kSamplesPerFrame, 2), params, 0));
}

TEST(AptxStream_PacesTenMillisecondRecords)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 48000;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate48000), params, 1000));
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
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, 0));

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
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, 0));

	// One second plus the 60 ms preroll is 106 spans of 441 samples. The ring is empty at the first
	// pump, so all of it is lead silence - sent on purpose, not counted as a gap. A clock that
	// advanced by the encoded samples rather than by the span would trail by 26 samples.
	REQUIRE(stream.Pump(1000000, pcm.view, frames.view, 200));
	CHECK_EQ(stream.Stats().records, uint64_t{106});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{0});
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
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, 0));

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

// Stop() drops the encoder with the carried samples, so a restarted stream must not prepend
// audio from before the stop to its first record.
TEST(AptxStream_StopClearsCarriedSamples)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 44100;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, 0));

	// Three spans leave three samples carried, which an uncleared carry would report as a
	// 444-sample first record after the restart.
	WritePcm(pcm, 1, 441 * 3);
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 3));
	CHECK_EQ(stream.Stats().records, uint64_t{3});

	stream.Stop();
	CHECK(!stream.Active());
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, 0));
	CHECK_EQ(stream.Stats().records, uint64_t{0});

	// A separate frame ring, so the first record read is the restarted stream's own.
	TestRing restarted(64 * 1024);
	WritePcm(pcm, 1, 441);
	REQUIRE(stream.Pump(0, pcm.view, restarted.view, 1));
	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(restarted.view, storage, sizeof(storage), record) ==
			FrameRecord::ReadResult::Record);
	CHECK_EQ(record.samples, uint32_t{440});
}

// A frame ring the service cannot keep up with drops records instead of stalling the clock,
// and the next record that fits is marked so the receiver knows audio is missing.
TEST(AptxStream_FullFrameRingDropsRecords)
{
	TestRing pcm(64 * 1024);
	TestRing frames(1024); // two records of four bytes per frame

	AptxStreamParams params;
	params.sampleRate = 48000;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate48000), params, 0));

	// Six due records of 480 samples into room for two.
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{2});
	CHECK_EQ(stream.Stats().droppedRecords, uint64_t{4});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
	CHECK_EQ(stream.Stats().samples, uint64_t{960});

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) ==
			FrameRecord::ReadResult::Record);
	CHECK_EQ(record.flags, Ioctl::kFrameFlagDiscontinuity);
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) ==
			FrameRecord::ReadResult::Record);
	CHECK_EQ(record.flags, uint32_t{0});
}

TEST(AptxStream_ShortRingWaitsThenBecomesSilence)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 44100;
	AptxStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(Aptx::kSampleRate44100), params, 0));

	// One span of PCM for six due spans: the other five are the lead, in front of it.
	WritePcm(pcm, 1, 441);
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{6});
	CHECK_EQ(stream.Stats().pcmBytes, uint64_t{441 * 4});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{0});

	// With the ring empty, the next span waits for its PCM until it is a span plus 20 ms overdue,
	// and only then is it sent as silence - a gap, counted as one.
	REQUIRE(stream.Pump(29000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{6});
	REQUIRE(stream.Pump(30000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{7});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{441});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
}

// The endpoint runs 24-in-32 while aptX HD can, so classic must encode from that ring too.
// Narrowing left-justified 24-bit samples rounds back to the original int16 value, so both
// rings must produce the same codewords and the same timestamp accounting.
TEST(AptxStream_24BitRingMatches16Bit)
{
	TestRing pcm16(64 * 1024);
	TestRing pcm24(64 * 1024);
	TestRing frames16(64 * 1024);
	TestRing frames24(64 * 1024);

	AptxStreamParams params;
	params.sampleRate = 48000;
	AptxStreamEncoder narrow;
	REQUIRE(narrow.Start(MakeEncoder(Aptx::kSampleRate48000), params, 0));

	AptxStreamParams wideParams = params;
	wideParams.bitsPerSample = 24;
	AptxStreamEncoder wide;
	REQUIRE(wide.Start(MakeEncoder(Aptx::kSampleRate48000), wideParams, 0));

	WritePcm(pcm16, 1000, 4800);
	WriteWidePcm(pcm24, 1000, 4800);
	REQUIRE(narrow.Pump(1000000, pcm16.view, frames16.view, 8));
	REQUIRE(wide.Pump(1000000, pcm24.view, frames24.view, 8));

	CHECK_EQ(wide.Stats().records, narrow.Stats().records);
	CHECK_EQ(wide.Stats().samples, narrow.Stats().samples);
	CHECK_EQ(wide.Stats().encodeErrors, uint64_t{0});
	// Both streams consumed the same number of frames: 8 bytes per frame wide, 4 narrow.
	CHECK_EQ((4800 * 8 - pcm24.view.ReadableBytes()) / 8,
		(4800 * 4 - pcm16.view.ReadableBytes()) / 4);
	CHECK_EQ(frames24.view.ReadableBytes(), frames16.view.ReadableBytes());

	uint8_t storage16[Ioctl::kMaxFrameRecordBytes];
	uint8_t storage24[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record16;
	FrameRecord::Record record24;
	for (uint64_t i = 0; i < narrow.Stats().records; ++i) {
		REQUIRE(FrameRecord::Read(frames16.view, storage16, sizeof(storage16), record16) ==
				FrameRecord::ReadResult::Record);
		REQUIRE(FrameRecord::Read(frames24.view, storage24, sizeof(storage24), record24) ==
				FrameRecord::ReadResult::Record);
		CHECK_EQ(record24.samples, record16.samples);
		REQUIRE(record24.payloadLength == record16.payloadLength);
		CHECK(std::memcmp(record24.payload, record16.payload, record16.payloadLength) == 0);
	}
}
