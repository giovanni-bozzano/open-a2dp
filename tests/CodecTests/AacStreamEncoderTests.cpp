#include "TestFramework.h"

#include <OpenA2dp/AacCodec.h>
#include <OpenA2dp/AacStreamEncoder.h>
#include <OpenA2dp/FrameRecord.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace OpenA2dp;

namespace {

// A ring with its own storage; the frame ring can be made deliberately small to
// exercise the "service is ahead of the driver" path.
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

// Deterministic stand-in for AacEncoder: fixed-size access units, optional
// priming, optional failure, and it keeps every PCM block it was given.
class FakeEncoder final : public IA2dpEncoder {
public:
	uint32_t sampleRate = 44100;
	uint16_t channels = 2;
	uint32_t samplesPerFrame = Ioctl::kAacSamplesPerFrame;
	size_t maxFrameBytes = 100; // declared worst-case capacity
	size_t frameBytes = 100;    // bytes the encoder actually produces
	size_t latmOverhead = 3;
	int primingFrames = 0;
	int failOnCall = -1; // 0-based Encode call that fails
	bool failPayload = false;

	int calls = 0;
	std::vector<std::vector<int16_t>> blocks;

	PcmFormat InputFormat() const noexcept override { return PcmFormat{sampleRate, channels}; }
	uint32_t SamplesPerFrame() const noexcept override { return samplesPerFrame; }
	size_t MaxFrameBytes() const noexcept override { return maxFrameBytes; }
	size_t FramesPerPacket(size_t) const noexcept override { return 1; }

	HRESULT Encode(const uint8_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept override
	{
		written = 0;
		const int call = calls++;
		const auto* samples = reinterpret_cast<const int16_t*>(pcm);
		blocks.emplace_back(samples, samples + static_cast<size_t>(samplesPerFrame) * channels);
		if (failOnCall >= 0 && call == failOnCall) {
			return E_FAIL;
		}
		if (primingFrames > 0) {
			--primingFrames;
			return S_OK;
		}
		if (capacity < frameBytes) {
			return E_FAIL;
		}
		for (size_t i = 0; i < frameBytes; ++i) {
			out[i] = static_cast<uint8_t>(0xA0 + (i & 0x1F));
		}
		written = frameBytes;
		return S_OK;
	}

	size_t BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount, uint8_t* out,
							 size_t capacity) noexcept override
	{
		if (failPayload) {
			return 0;
		}
		size_t total = latmOverhead;
		for (size_t i = 0; i < frameCount; ++i) {
			total += frameSizes[i];
		}
		if (total > capacity) {
			return 0;
		}
		std::memset(out, 0x7F, latmOverhead);
		size_t offset = latmOverhead;
		for (size_t i = 0; i < frameCount; ++i) {
			std::memcpy(out + offset, frames, frameSizes[i]);
			frames += frameSizes[i];
			offset += frameSizes[i];
		}
		return total;
	}
};

constexpr uint32_t kStereoBytesPerSample = 4;
constexpr uint32_t kBlockBytes = Ioctl::kAacSamplesPerFrame * kStereoBytesPerSample;

std::unique_ptr<FakeEncoder> MakeEncoder()
{
	return std::make_unique<FakeEncoder>();
}

void WritePcm(TestRing& ring, int16_t firstSample, uint32_t samples)
{
	std::vector<int16_t> pcm(static_cast<size_t>(samples) * 2);
	for (size_t i = 0; i < pcm.size(); ++i) {
		pcm[i] = static_cast<int16_t>(firstSample + static_cast<int16_t>(i));
	}
	REQUIRE(ring.view.Write(pcm.data(), static_cast<uint32_t>(pcm.size() * sizeof(int16_t))));
}

} // namespace

TEST(AacStream_TargetIncludesPreroll)
{
	TestRing pcm(64 * 1024);
	auto encoder = MakeEncoder();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 1000));

	// 60 ms of 44.1 kHz = 2646 samples.
	CHECK_EQ(stream.TargetSamples(1000), uint64_t{2646});
	CHECK_EQ(stream.TargetSamples(0), uint64_t{2646}); // clock behind start
	CHECK_EQ(stream.TargetSamples(101000), uint64_t{2646 + 4410});
	CHECK_EQ(stream.TargetSamples(1001000), uint64_t{2646 + 44100});
}

TEST(AacStream_PacingLeadComesFromParams)
{
	TestRing pcm(64 * 1024);
	auto encoder = MakeEncoder();
	AacStreamEncoder stream;
	AacStreamParams params;
	params.prerollMs = Ioctl::kMinPrerollMs; // 20 ms of 44.1 kHz = 882 samples
	REQUIRE(stream.Start(std::move(encoder), params, 1000));

	CHECK_EQ(stream.TargetSamples(1000), uint64_t{882});
	CHECK_EQ(stream.TargetSamples(1001000), uint64_t{882 + 44100});
}

TEST(AacStream_StartKeepsBufferedPcm)
{
	TestRing pcm(64 * 1024);
	WritePcm(pcm, 1, 512);
	const uint32_t buffered = pcm.view.ReadableBytes();
	REQUIRE(buffered > 0);

	// Which buffered PCM is still playback is the driver's call (it drops only what predates the
	// stream), so starting leaves the ring alone and the first records carry this audio.
	auto encoder = MakeEncoder();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	CHECK_EQ(pcm.view.ReadableBytes(), buffered);
}

TEST(AacStream_StartRejectsUnusableEncoder)
{
	TestRing pcm(64 * 1024);
	AacStreamEncoder stream;

	CHECK(!stream.Start(nullptr, AacStreamParams{}, 0));

	auto wrongRate = MakeEncoder();
	wrongRate->sampleRate = 48000;
	CHECK(!stream.Start(std::move(wrongRate), AacStreamParams{}, 0));

	auto wrongChannels = MakeEncoder();
	wrongChannels->channels = 1;
	CHECK(!stream.Start(std::move(wrongChannels), AacStreamParams{}, 0));

	auto noCapacity = MakeEncoder();
	noCapacity->maxFrameBytes = 0;
	CHECK(!stream.Start(std::move(noCapacity), AacStreamParams{}, 0));
	CHECK(!stream.Active());
}

TEST(AacStream_StartAcceptsWorstCaseCapacity)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	// A real stereo AAC encoder declares the ISO 14496-3 bound, which is larger
	// than one media payload; each frame it actually emits is smaller.
	auto encoder = MakeEncoder();
	encoder->maxFrameBytes = 768 * 2;
	encoder->frameBytes = 557;
	FakeEncoder* fake = encoder.get();

	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 0, 4096);
	REQUIRE(stream.Pump(100000, pcm.view, frames.view, 8));

	// 7056 samples are due: 2960 of lead in front of the 4096 buffered make six whole blocks, and
	// the seventh waits for its PCM.
	CHECK_EQ(stream.Stats().records, uint64_t{6});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
	CHECK_EQ(fake->calls, 6);
}

TEST(AacStream_RealEncoderStartsAndProducesRecords)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	Aac::Params config;
	config.sampleRate = 44100;
	config.channels = 2;
	config.bitrate = 192000;
	config.objectType = Aac::kObjectMpeg2Lc;

	uint8_t ie[Aac::kIeLength] = {};
	Aac::WriteIe(config, ie);

	A2dpCodecConfig codecConfig;
	codecConfig.codec = CodecType::Aac;
	codecConfig.ieLength = Aac::kIeLength;
	std::memcpy(codecConfig.ie, ie, sizeof(ie));

	AacCodec codec;
	std::unique_ptr<IA2dpEncoder> encoder;
	REQUIRE(SUCCEEDED(codec.CreateEncoder(codecConfig, encoder)));
	// The regression this test covers: the real stereo encoder declares the
	// ISO 14496-3 worst-case capacity, larger than one media payload.
	CHECK(encoder->MaxFrameBytes() > Ioctl::kMaxFramePayload);

	AacStreamParams params;
	params.sampleRate = config.sampleRate;
	params.channels = config.channels;
	params.bitrate = config.bitrate;
	params.objectType = config.objectType;

	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), params, 0));
	WritePcm(pcm, 0, 8192);
	REQUIRE(stream.Pump(100000, pcm.view, frames.view, 8));

	CHECK(stream.Stats().records > 0);
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
}

TEST(AacStream_OversizeActualFrameIsFatal)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	// Capacity alone is not a promise: an actual frame that cannot fit the media
	// payload must still stop the stream instead of writing a corrupt record.
	auto encoder = MakeEncoder();
	encoder->maxFrameBytes = 768 * 2;
	encoder->frameBytes = Ioctl::kMaxFramePayload + 1;
	encoder->latmOverhead = 0;

	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 0, 4096);
	CHECK(!stream.Pump(100000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{0});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{1});
}

// The preroll is silence in front of the PCM buffered at start, never padding behind it: the
// buffered audio has to run straight into the live audio that follows.
TEST(AacStream_PumpPutsThePrerollInFrontOfBufferedPcm)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	FakeEncoder* fake = encoder.get();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 5000));
	WritePcm(pcm, 1, 2048);

	// 2646 samples are due: 598 of lead, then the 2048 buffered. Two blocks are whole; the third
	// would be 598 samples of audio and the rest silence, so it waits for its PCM instead.
	REQUIRE(stream.Pump(5000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{2});
	CHECK_EQ(stream.Stats().samples, uint64_t{2 * Ioctl::kAacSamplesPerFrame});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{0});
	CHECK_EQ(stream.Stats().pcmBytes, uint64_t{(2048 - 598) * kStereoBytesPerSample});
	CHECK_EQ(fake->calls, 2);
	REQUIRE(fake->blocks.size() == size_t{2});
	CHECK_EQ(fake->blocks[0][0], int16_t{0});
	CHECK_EQ(fake->blocks[0][597 * 2 + 1], int16_t{0});
	CHECK_EQ(fake->blocks[0][598 * 2], int16_t{1});
	CHECK_EQ(fake->blocks[1][0], int16_t{1 + (1024 - 598) * 2});

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.samples, Ioctl::kAacSamplesPerFrame);
	CHECK_EQ(record.silenceSamples, uint32_t{0});
	CHECK_EQ(record.flags, Ioctl::kFrameFlagDiscontinuity);
	CHECK_EQ(record.payloadLength, static_cast<uint32_t>(fake->frameBytes + fake->latmOverhead));
	CHECK_EQ(record.payload[0], uint8_t{0x7F});
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.flags, uint32_t{0});
}

TEST(AacStream_DoesNotOutrunTheClock)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 0, 4096);
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{3});

	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{3});

	// 10 ms later one more frame is due.
	REQUIRE(stream.Pump(10000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{4});
}

TEST(AacStream_PartialTickStillEncodesAWholeBlock)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	FakeEncoder* fake = encoder.get();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 0, 8192);

	// Consume the preroll first, then let one 10 ms tick make 441 samples due. A record
	// still covers 1024 samples of real PCM: filling the other 583 with silence would
	// put a gap inside every block at the tick rate.
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{3});
	REQUIRE(stream.Pump(10000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{4});
	CHECK_EQ(stream.Stats().pcmBytes, uint64_t{4 * kBlockBytes});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{0});
	REQUIRE(fake->blocks.size() == size_t{4});
	CHECK_EQ(fake->blocks[3][0], int16_t{3 * Ioctl::kAacSamplesPerFrame * 2});
	CHECK(fake->blocks[3][441] != int16_t{0});
}

TEST(AacStream_HonoursRecordLimit)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 0, 8192);
	REQUIRE(stream.Pump(1000000, pcm.view, frames.view, 2));
	CHECK_EQ(stream.Stats().records, uint64_t{2});
	REQUIRE(stream.Pump(1000000, pcm.view, frames.view, 3));
	CHECK_EQ(stream.Stats().records, uint64_t{5});
}

// A block whose PCM is late waits for it; only once it is a block plus the copy slack overdue is
// its PCM missing, and then the shortfall is zero-filled and reported as the record's silence.
TEST(AacStream_PadsABlockOnlyOnceItsPcmIsMissing)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	FakeEncoder* fake = encoder.get();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 1234, Ioctl::kAacSamplesPerFrame);

	// 2646 due: 1622 of lead and the 1024 buffered fill two blocks; 598 samples of PCM remain.
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{2});
	// 20 ms later 1480 samples are due, still less than a block plus 20 ms of slack: wait.
	REQUIRE(stream.Pump(20000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{2});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{0});
	// At 30 ms 1921 are due: the PCM is missing, so the block is sent with a silent tail.
	REQUIRE(stream.Pump(30000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{3});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{1024 - 598});
	REQUIRE(fake->blocks.size() == size_t{3});
	CHECK(fake->blocks[2][0] != int16_t{0});
	CHECK_EQ(fake->blocks[2][598 * 2], int16_t{0});

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.silenceSamples, uint32_t{1024 - 598});
}

TEST(AacStream_PrimingProducesNoRecords)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	encoder->primingFrames = 2;
	FakeEncoder* fake = encoder.get();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 0, 4096);

	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(fake->calls, 3);
	CHECK_EQ(stream.Stats().primed, uint64_t{2});
	CHECK_EQ(stream.Stats().records, uint64_t{1});
	// Primed blocks still consume PCM, so the clock stays satisfied.
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{1});
}

TEST(AacStream_FullFrameRingDropsAndFlagsDiscontinuity)
{
	TestRing pcm(64 * 1024);
	TestRing frames(256); // holds two records of 12 + 103 bytes

	auto encoder = MakeEncoder();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 0, 8192);
	REQUIRE(stream.Pump(100000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, uint64_t{2});
	CHECK_EQ(stream.Stats().droppedRecords, uint64_t{5});

	// Drain the ring and pump once more: the new record restarts the sequence.
	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	while (FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record) {
	}
	REQUIRE(stream.Pump(200000, pcm.view, frames.view, 1));
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.flags, Ioctl::kFrameFlagDiscontinuity);
}

TEST(AacStream_EncodeFailureIsFatal)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	encoder->failOnCall = 0;
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	CHECK(!stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{1});
	CHECK_EQ(stream.Stats().records, uint64_t{0});
}

TEST(AacStream_PayloadFailureIsFatal)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	encoder->failPayload = true;
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	CHECK(!stream.Pump(0, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{1});
}

TEST(AacStream_StopStopsEncoding)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), AacStreamParams{}, 0));
	WritePcm(pcm, 0, 4096);
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	const uint64_t records = stream.Stats().records;

	stream.Stop();
	CHECK(!stream.Active());
	REQUIRE(stream.Pump(1000000, pcm.view, frames.view, 8));
	CHECK_EQ(stream.Stats().records, records);
}

TEST(AacStream_Converts24In32RingToEncoderInput)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	auto encoder = MakeEncoder();
	FakeEncoder* fake = encoder.get();
	AacStreamParams params;
	params.bitsPerSample = 24;
	AacStreamEncoder stream;
	REQUIRE(stream.Start(std::move(encoder), params, 0));

	// Two blocks of 24-in-32 stereo: the top 16 bits are the AAC input, the low byte is
	// always zero in Windows' left-justified layout.
	for (uint32_t frame = 0; frame < 2 * Ioctl::kAacSamplesPerFrame; ++frame) {
		const int32_t samples[2] = {0x12345600, 0x56789A00};
		REQUIRE(pcm.view.Write(samples, sizeof(samples)));
	}

	REQUIRE(stream.Pump(0, pcm.view, frames.view, 8));
	// 60 ms of preroll asks for 2646 samples: 598 of lead in front of the 2048 buffered make two
	// whole blocks, and the rest waits for the PCM that follows it.
	REQUIRE(fake->blocks.size() == size_t{2});
	CHECK_EQ(stream.Stats().pcmBytes, uint64_t{(2 * Ioctl::kAacSamplesPerFrame - 598) * 8});
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{0});
	CHECK_EQ(fake->blocks[0][598 * 2], int16_t{0x1234});     // 0x56 rounds down
	CHECK_EQ(fake->blocks[0][598 * 2 + 1], int16_t{0x5679}); // 0x9A rounds up
}
