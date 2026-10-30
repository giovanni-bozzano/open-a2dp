#include "TestFramework.h"

#include <OpenA2dp/FrameRecord.h>

#include <cstring>

using namespace OpenA2dp;

namespace {

struct TestRing {
	alignas(64) uint8_t header[sizeof(RingHeader)];
	alignas(64) uint8_t data[1024];
	RingBufferView view;

	TestRing()
	{
		RingBufferView::Initialize(reinterpret_cast<RingHeader*>(header), sizeof(data));
		view = RingBufferView(reinterpret_cast<RingHeader*>(header), data);
	}
};

uint8_t PayloadByte(size_t i)
{
	return static_cast<uint8_t>(i * 7 + 3);
}

void FillPayload(uint8_t* payload, size_t length)
{
	for (size_t i = 0; i < length; ++i) {
		payload[i] = PayloadByte(i);
	}
}

bool PayloadMatches(const uint8_t* payload, size_t length)
{
	for (size_t i = 0; i < length; ++i) {
		if (payload[i] != PayloadByte(i)) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST(FrameRecord_RoundTrip)
{
	TestRing ring;
	uint8_t payload[100];
	FillPayload(payload, sizeof(payload));

	REQUIRE(FrameRecord::Write(ring.view, payload, sizeof(payload), Ioctl::kAacSamplesPerFrame,
							   Ioctl::kFrameFlagDiscontinuity));
	CHECK_EQ(ring.view.ReadableBytes(), static_cast<uint32_t>(sizeof(Ioctl::FrameRecordHeader) + sizeof(payload)));

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(ring.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
	CHECK_EQ(record.payloadLength, static_cast<uint32_t>(sizeof(payload)));
	CHECK_EQ(record.samples, Ioctl::kAacSamplesPerFrame);
	CHECK_EQ(record.flags, Ioctl::kFrameFlagDiscontinuity);
	CHECK(PayloadMatches(record.payload, record.payloadLength));
	CHECK_EQ(ring.view.ReadableBytes(), uint32_t{0});
}

TEST(FrameRecord_EmptyRing)
{
	TestRing ring;
	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	CHECK(FrameRecord::Read(ring.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Empty);
	CHECK(record.payload == nullptr);
}

TEST(FrameRecord_IncompleteRecordIsEmpty)
{
	TestRing ring;
	Ioctl::FrameRecordHeader header{Ioctl::kFrameRecordMagic, 64, Ioctl::kAacSamplesPerFrame, 0};
	REQUIRE(ring.view.Write(&header, sizeof(header)));

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	CHECK(FrameRecord::Read(ring.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Empty);
	// The header is left in place for the next attempt.
	CHECK_EQ(ring.view.ReadableBytes(), static_cast<uint32_t>(sizeof(header)));
}

TEST(FrameRecord_CorruptHeader)
{
	TestRing ring;
	Ioctl::FrameRecordHeader header{0x1234, 64, Ioctl::kAacSamplesPerFrame, 0};
	REQUIRE(ring.view.Write(&header, sizeof(header)));

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	CHECK(FrameRecord::Read(ring.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Corrupt);
	// A corrupt header is not consumed.
	CHECK_EQ(ring.view.ReadableBytes(), static_cast<uint32_t>(sizeof(header)));
}

TEST(FrameRecord_RejectsImpossibleSize)
{
	TestRing ring;
	uint8_t raw[sizeof(Ioctl::FrameRecordHeader) + 16] = {};
	auto* header = reinterpret_cast<Ioctl::FrameRecordHeader*>(raw);
	header->magic = Ioctl::kFrameRecordMagic;
	header->size = static_cast<uint16_t>(sizeof(Ioctl::FrameRecordHeader));
	REQUIRE(ring.view.Write(raw, sizeof(raw)));

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	CHECK(FrameRecord::Read(ring.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Corrupt);
}

TEST(FrameRecord_DropsOversizedPayload)
{
	TestRing ring;
	const uint32_t payloadLength = Ioctl::kMaxFramePayload + 1;
	uint8_t raw[sizeof(Ioctl::FrameRecordHeader) + payloadLength] = {};
	auto* header = reinterpret_cast<Ioctl::FrameRecordHeader*>(raw);
	header->magic = Ioctl::kFrameRecordMagic;
	header->size = static_cast<uint16_t>(sizeof(Ioctl::FrameRecordHeader) + payloadLength);
	header->samples = Ioctl::kAacSamplesPerFrame;
	REQUIRE(ring.view.Write(raw, sizeof(raw)));

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	CHECK(FrameRecord::Read(ring.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Dropped);
	// The bad record is skipped so the next one can be read.
	CHECK_EQ(ring.view.ReadableBytes(), uint32_t{0});
}

TEST(FrameRecord_WriteRejectsInvalidInput)
{
	TestRing ring;
	uint8_t payload[8] = {};

	CHECK(!FrameRecord::Write(ring.view, nullptr, 8, Ioctl::kAacSamplesPerFrame, 0));
	CHECK(!FrameRecord::Write(ring.view, payload, 0, Ioctl::kAacSamplesPerFrame, 0));

	uint8_t tooLarge[Ioctl::kMaxFramePayload + 1] = {};
	CHECK(!FrameRecord::Write(ring.view, tooLarge, sizeof(tooLarge), Ioctl::kAacSamplesPerFrame, 0));
	CHECK_EQ(ring.view.ReadableBytes(), uint32_t{0});
}

TEST(FrameRecord_WriteRejectsFullRing)
{
	TestRing ring;
	uint8_t payload[200];
	FillPayload(payload, sizeof(payload));

	while (ring.view.WritableBytes() >= sizeof(Ioctl::FrameRecordHeader) + sizeof(payload)) {
		REQUIRE(FrameRecord::Write(ring.view, payload, sizeof(payload), Ioctl::kAacSamplesPerFrame, 0));
	}
	CHECK(!FrameRecord::Write(ring.view, payload, sizeof(payload), Ioctl::kAacSamplesPerFrame, 0));
}

TEST(FrameRecord_WrapAround)
{
	TestRing ring;
	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;

	// Fill most of the ring, drain it, then write across the wrap point.
	uint8_t payload[180];
	FillPayload(payload, sizeof(payload));
	for (int i = 0; i < 200; ++i) {
		REQUIRE(FrameRecord::Write(ring.view, payload, sizeof(payload), Ioctl::kAacSamplesPerFrame, 0));
		REQUIRE(FrameRecord::Read(ring.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
		REQUIRE(PayloadMatches(record.payload, record.payloadLength));
	}

	for (int i = 0; i < 3; ++i) {
		REQUIRE(FrameRecord::Write(ring.view, payload, sizeof(payload), Ioctl::kAacSamplesPerFrame, 0));
	}
	for (int i = 0; i < 3; ++i) {
		REQUIRE(FrameRecord::Read(ring.view, storage, sizeof(storage), record) == FrameRecord::ReadResult::Record);
		REQUIRE(PayloadMatches(record.payload, record.payloadLength));
	}
	CHECK_EQ(ring.view.ReadableBytes(), uint32_t{0});
}
