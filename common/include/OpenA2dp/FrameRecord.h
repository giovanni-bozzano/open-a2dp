#pragma once

// Frame records carried by the service -> driver ring: one AAC/LATM AudioMuxElement
// per record. Kernel-safe and user-mode safe: no STL, no exceptions, no allocation.
// The layout and the lifecycle rules are in common/include/OpenA2dp/Ioctl.h and
// docs/AAC-Service-Boundary.md.

#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/RingBuffer.h>

namespace OpenA2dp::FrameRecord {

enum class ReadResult : uint8_t {
	Empty,	 // no complete record available
	Record,	 // out is valid
	Dropped, // record skipped (payload too large or bad sample count)
	Corrupt, // bad header: the consumer cannot recover, stop the stream
};

struct Record {
	const uint8_t* payload = nullptr;
	uint32_t payloadLength = 0;
	uint32_t samples = 0;
	uint32_t flags = 0;
};

// Producer side. Writes header + payload with one RingBufferView::Write, so a
// consumer never sees a partial record. Returns false when the record does not fit.
inline bool Write(RingBufferView& ring, const uint8_t* payload, uint32_t payloadLength, uint32_t samples,
				  uint32_t flags) noexcept
{
	if (payload == nullptr || payloadLength == 0 || payloadLength > Ioctl::kMaxFramePayload) {
		return false;
	}
	const uint32_t size = static_cast<uint32_t>(sizeof(Ioctl::FrameRecordHeader)) + payloadLength;
	if (size > Ioctl::kMaxFrameRecordBytes) {
		return false;
	}

	uint8_t record[Ioctl::kMaxFrameRecordBytes];
	const Ioctl::FrameRecordHeader header{
		static_cast<uint16_t>(Ioctl::kFrameRecordMagic),
		static_cast<uint16_t>(size),
		samples,
		flags,
	};
	memcpy(record, &header, sizeof(header));
	memcpy(record + sizeof(header), payload, payloadLength);
	return ring.Write(record, size);
}

// Consumer side. Peeks one record into storage (which must hold
// kMaxFrameRecordBytes) and skips it. A record whose payload does not fit a media
// packet, or whose sample count is implausible, is skipped and reported as Dropped.
inline ReadResult Read(RingBufferView& ring, uint8_t* storage, uint32_t storageBytes, Record& out) noexcept
{
	out = {};
	if (storage == nullptr || storageBytes < Ioctl::kMaxFrameRecordBytes) {
		return ReadResult::Empty;
	}

	Ioctl::FrameRecordHeader header{};
	if (!ring.Peek(&header, sizeof(header))) {
		return ReadResult::Empty;
	}
	if (header.magic != Ioctl::kFrameRecordMagic || header.size <= sizeof(header) ||
		header.size > Ioctl::kMaxFrameRecordBytes) {
		return ReadResult::Corrupt;
	}
	if (!ring.Peek(storage, header.size)) {
		return ReadResult::Empty;
	}
	ring.Skip(header.size);

	const uint32_t payloadLength = header.size - static_cast<uint32_t>(sizeof(header));
	if (payloadLength > Ioctl::kMaxFramePayload || header.samples == 0 || header.samples > 4096) {
		return ReadResult::Dropped;
	}

	out.payload = storage + sizeof(header);
	out.payloadLength = payloadLength;
	out.samples = header.samples;
	out.flags = header.flags;
	return ReadResult::Record;
}

} // namespace OpenA2dp::FrameRecord
