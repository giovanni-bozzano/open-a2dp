#pragma once

// Single-producer / single-consumer byte ring living in memory shared between
// the kernel driver and the user-mode encoder service.
// Kernel-safe: no STL, no exceptions, no allocation. Capacity must be a power of two.

#include <OpenA2dp/Types.h>
#include <string.h>

#if defined(_KERNEL_MODE)
#include <wdm.h>
#else
#include <windows.h>
#endif

namespace OpenA2dp {

#pragma warning(push)
#pragma warning(disable : 4324) // structure padded due to alignment specifier
struct alignas(64) RingHeader {
	alignas(64) volatile LONG head; // written by producer (total bytes written, wraps at 2^32)
	alignas(64) volatile LONG tail; // written by consumer (total bytes read, wraps at 2^32)
	alignas(64) uint32_t capacity;  // bytes, power of two
	uint32_t reserved[15];
};
#pragma warning(pop)

static_assert(sizeof(RingHeader) == 192, "RingHeader layout is part of the driver/service ABI");

class RingBufferView {
public:
	RingBufferView() noexcept = default;

	RingBufferView(RingHeader* header, uint8_t* data) noexcept : header_(header), data_(data) {}

	static bool IsValidCapacity(uint32_t capacity) noexcept
	{
		return capacity != 0 && (capacity & (capacity - 1)) == 0;
	}

	static void Initialize(RingHeader* header, uint32_t capacity) noexcept
	{
		memset(header, 0, sizeof(*header));
		header->capacity = capacity;
	}

	uint32_t Capacity() const noexcept { return header_->capacity; }

	uint32_t ReadableBytes() const noexcept
	{
		const uint32_t head = static_cast<uint32_t>(ReadAcquire(&header_->head));
		const uint32_t tail = static_cast<uint32_t>(ReadAcquire(&header_->tail));
		return head - tail;
	}

	uint32_t WritableBytes() const noexcept { return Capacity() - ReadableBytes(); }

	// Producer side. Writes all bytes or nothing.
	bool Write(const void* src, uint32_t len) noexcept
	{
		if (len > WritableBytes()) {
			return false;
		}
		const uint32_t head = static_cast<uint32_t>(header_->head);
		Copy(head, src, len, true);
		WriteRelease(&header_->head, static_cast<LONG>(head + len));
		return true;
	}

	// Consumer side. Reads all bytes or nothing.
	bool Read(void* dst, uint32_t len) noexcept
	{
		if (!Peek(dst, len)) {
			return false;
		}
		Skip(len);
		return true;
	}

	bool Peek(void* dst, uint32_t len) const noexcept
	{
		if (len > ReadableBytes()) {
			return false;
		}
		const uint32_t tail = static_cast<uint32_t>(header_->tail);
		const_cast<RingBufferView*>(this)->Copy(tail, dst, len, false);
		return true;
	}

	void Skip(uint32_t len) noexcept
	{
		const uint32_t tail = static_cast<uint32_t>(header_->tail);
		WriteRelease(&header_->tail, static_cast<LONG>(tail + len));
	}

private:
	void Copy(uint32_t position, const void* buffer, uint32_t len, bool toRing) noexcept
	{
		const uint32_t mask = Capacity() - 1;
		const uint32_t offset = position & mask;
		const uint32_t first = (len < Capacity() - offset) ? len : Capacity() - offset;
		auto* bytes = static_cast<uint8_t*>(const_cast<void*>(buffer));
		if (toRing) {
			memcpy(data_ + offset, bytes, first);
			memcpy(data_, bytes + first, len - first);
		} else {
			memcpy(bytes, data_ + offset, first);
			memcpy(bytes + first, data_, len - first);
		}
	}

	RingHeader* header_ = nullptr;
	uint8_t* data_ = nullptr;
};

} // namespace OpenA2dp
