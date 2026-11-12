#include "TestFramework.h"

#include <OpenA2dp/PcmBacklog.h>

#include <cstring>
#include <vector>

using namespace OpenA2dp;

namespace {

// 16-bit stereo at 44.1 kHz: one frame is four bytes.
constexpr uint32_t kFrameBytes = 4;
constexpr uint32_t kSampleRate = 44100;

struct TestRing {
	alignas(64) uint8_t header[sizeof(RingHeader)];
	std::vector<uint8_t> data;
	RingBufferView view;

	explicit TestRing(uint32_t capacity) : data(capacity, 0)
	{
		RingBufferView::Initialize(reinterpret_cast<RingHeader*>(header), capacity);
		view = RingBufferView(reinterpret_cast<RingHeader*>(header), data.data());
	}
};

// Fills the ring with `frames` frames whose first byte counts up, so what survived a trim is
// identifiable.
void FillFrames(TestRing& ring, uint32_t frames)
{
	std::vector<uint8_t> pcm(frames * kFrameBytes);
	for (uint32_t i = 0; i < pcm.size(); ++i) {
		pcm[i] = static_cast<uint8_t>(i / kFrameBytes);
	}
	REQUIRE(ring.view.Write(pcm.data(), static_cast<uint32_t>(pcm.size())));
}

uint32_t FirstFrameIndex(TestRing& ring)
{
	uint8_t first = 0;
	if (!ring.view.Peek(&first, 1)) {
		return 0xFFFFFFFFu; // empty ring: never equal to a frame index a test expects
	}
	return first;
}

} // namespace

// The over-limit test is what a caller uses to require the excess to persist before it trims;
// it has to agree with Trim's own margin, or a ring would be called over the limit while Trim
// would still decline to drop anything.
TEST(PcmBacklog_OverLimitMatchesTheTrimMargin)
{
	TestRing ring(4096);
	FillFrames(ring, 100); // 400 bytes

	CHECK(!PcmBacklog::OverLimit(ring.view, 800, kFrameBytes));
	// Exactly the allowance plus one frame: Trim leaves it alone, so this is not over.
	CHECK(!PcmBacklog::OverLimit(ring.view, 396, kFrameBytes));
	CHECK(PcmBacklog::OverLimit(ring.view, 392, kFrameBytes));
	// Without a frame size nothing can be measured or dropped.
	CHECK(!PcmBacklog::OverLimit(ring.view, 0, 0));
	// Over the limit by two frames, and Trim drops exactly that much.
	CHECK_EQ(PcmBacklog::Trim(ring.view, 392, kFrameBytes), uint32_t{8});
	CHECK_EQ(ring.view.ReadableBytes(), uint32_t{392});
}

TEST(PcmBacklog_LeavesAFittingRingAlone)
{
	TestRing ring(4096);
	FillFrames(ring, 100); // 400 bytes
	const uint32_t readable = ring.view.ReadableBytes();

	// The allowance covers what is buffered, so nothing is dropped.
	CHECK_EQ(PcmBacklog::Trim(ring.view, 800, kFrameBytes), uint32_t{0});
	CHECK_EQ(ring.view.ReadableBytes(), readable);
	CHECK_EQ(FirstFrameIndex(ring), uint32_t{0});
}

TEST(PcmBacklog_DropsTheOldestFrames)
{
	TestRing ring(4096);
	FillFrames(ring, 400); // 1600 bytes, frames 0..399
	const uint32_t limit = 800; // 200 frames

	// 1600 - 800 = 800 bytes over, already frame aligned.
	CHECK_EQ(PcmBacklog::Trim(ring.view, limit, kFrameBytes), uint32_t{800});
	CHECK_EQ(ring.view.ReadableBytes(), limit);
	CHECK_EQ(FirstFrameIndex(ring), uint32_t{200}); // the newest 200 frames survive
}

TEST(PcmBacklog_TrimsWholeFramesOnly)
{
	TestRing ring(4096);
	FillFrames(ring, 300); // 1200 bytes
	// A limit that is not frame aligned: the remainder must still be whole frames.
	CHECK_EQ(PcmBacklog::Trim(ring.view, 700, kFrameBytes), uint32_t{500});
	CHECK_EQ(ring.view.ReadableBytes(), uint32_t{700});
	CHECK_EQ(ring.view.ReadableBytes() % kFrameBytes, uint32_t{0});
}

TEST(PcmBacklog_IgnoresAFrameOfSlack)
{
	TestRing ring(4096);
	FillFrames(ring, 201); // 804 bytes, one frame over an 800-byte allowance

	// A single frame over is timing noise, not a backlog.
	CHECK_EQ(PcmBacklog::Trim(ring.view, 800, kFrameBytes), uint32_t{0});
	CHECK_EQ(ring.view.ReadableBytes(), uint32_t{804});
}

TEST(PcmBacklog_KeepsTheNewestFrame)
{
	TestRing ring(4096);
	FillFrames(ring, 10); // 40 bytes
	// A zero allowance would drop everything, so the newest frame is always kept.
	CHECK_EQ(PcmBacklog::Trim(ring.view, 0, kFrameBytes), uint32_t{36});
	CHECK_EQ(ring.view.ReadableBytes(), kFrameBytes);
	CHECK_EQ(FirstFrameIndex(ring), uint32_t{9});
}

TEST(PcmBacklog_RejectsAnEmptyFrame)
{
	TestRing ring(4096);
	FillFrames(ring, 10);
	const uint32_t readable = ring.view.ReadableBytes();

	CHECK_EQ(PcmBacklog::Trim(ring.view, 0, 0), uint32_t{0});
	CHECK_EQ(ring.view.ReadableBytes(), readable);
}

TEST(PcmBacklog_LimitFollowsTheFormat)
{
	// 80 ms of 16-bit stereo at 44.1 kHz is about 14 KB, and twice that in 24-in-32.
	CHECK_EQ(PcmBacklog::LimitBytes(kSampleRate, 4, 80), static_cast<uint32_t>(44100 * 4 * 80 / 1000));
	CHECK_EQ(PcmBacklog::LimitBytes(kSampleRate, 8, 80), static_cast<uint32_t>(44100 * 8 * 80 / 1000));
	CHECK_EQ(PcmBacklog::LimitBytes(0, 4, 80), uint32_t{0});
}

TEST(PcmBacklog_TrimsAcrossTheRingWrap)
{
	TestRing ring(512);
	// Move the ring so the next writes wrap, then leave a backlog behind.
	uint8_t scratch[500] = {};
	REQUIRE(ring.view.Write(scratch, 400));
	ring.view.Skip(400);
	FillFrames(ring, 100); // 400 bytes starting at offset 400, so most of it wraps

	const uint32_t dropped = PcmBacklog::Trim(ring.view, 120, kFrameBytes);
	CHECK_EQ(dropped, uint32_t{280});
	CHECK_EQ(ring.view.ReadableBytes(), uint32_t{120});
	CHECK_EQ(FirstFrameIndex(ring), uint32_t{70});
}
