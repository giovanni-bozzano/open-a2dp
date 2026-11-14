#pragma once

// Single-producer / single-consumer byte ring living in memory shared between
// the kernel driver and the user-mode encoder service.
// Kernel-safe: no STL, no exceptions, no allocation. Capacity must be a power of two.
//
// The producer may also drop the oldest unread bytes (Discard) to keep a stalled consumer near
// live audio. Both sides then move the tail, so both do it with a compare-and-swap, and the
// consumer validates every read: it copies first and commits only if the tail did not move
// meanwhile. The producer never overwrites unread bytes without first moving the tail past
// them, so a read that commits is never torn, and one that does not is retried.

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
	alignas(64) volatile LONG tail; // total bytes read or discarded, wraps at 2^32
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
		// Tail first: it only grows, so a head read after it is never behind it. A discard and
		// writes between the two reads can make the difference exceed the capacity; that span is
		// being overwritten, which a validated read detects.
		const uint32_t tail = static_cast<uint32_t>(ReadAcquire(&header_->tail));
		const uint32_t head = static_cast<uint32_t>(ReadAcquire(&header_->head));
		const uint32_t readable = head - tail;
		return readable < Capacity() ? readable : Capacity();
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

	// Consumer side. Reads all bytes or nothing. The copy is committed only if the producer did
	// not discard under it; otherwise the bytes may already be overwritten, so it reads again
	// from where the discard left the tail.
	bool Read(void* dst, uint32_t len) noexcept
	{
		return len <= Capacity() && ReadUpTo(dst, len, len == 0 ? 1 : len) == len;
	}

	// Consumer side. Reads as many whole `granule`-byte units as are readable, up to `maxLen`,
	// and returns the bytes read. Validated like Read, and sized from the readable bytes at the
	// moment of the copy, so a discard between the caller's check and the read costs only what it
	// discarded, never the rest.
	uint32_t ReadUpTo(void* dst, uint32_t maxLen, uint32_t granule) noexcept
	{
		for (;;) {
			const uint32_t tail = static_cast<uint32_t>(ReadAcquire(&header_->tail));
			const uint32_t head = static_cast<uint32_t>(ReadAcquire(&header_->head));
			const uint32_t readable = head - tail < Capacity() ? head - tail : Capacity();
			uint32_t len = maxLen < readable ? maxLen : readable;
			len -= len % granule;
			if (len == 0) {
				return 0;
			}
			Copy(tail, dst, len, false);
			if (static_cast<uint32_t>(InterlockedCompareExchange(&header_->tail, static_cast<LONG>(tail + len),
																   static_cast<LONG>(tail))) == tail) {
				return len;
			}
		}
	}

	// Drops up to `len` of the oldest unread bytes and returns how many it dropped. The producer
	// uses it to keep a stalled consumer near live audio; it is safe against a concurrent Read.
	uint32_t Discard(uint32_t len) noexcept
	{
		for (;;) {
			const uint32_t tail = static_cast<uint32_t>(ReadAcquire(&header_->tail));
			const uint32_t readable = static_cast<uint32_t>(ReadAcquire(&header_->head)) - tail;
			const uint32_t drop = len < readable ? len : readable;
			if (drop == 0) {
				return 0;
			}
			if (static_cast<uint32_t>(InterlockedCompareExchange(&header_->tail, static_cast<LONG>(tail + drop),
																   static_cast<LONG>(tail))) == tail) {
				return drop;
			}
		}
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

	// Consumer side, after Peek, on a ring whose producer never discards (the frame ring).
	void Skip(uint32_t len) noexcept
	{
		const uint32_t tail = static_cast<uint32_t>(header_->tail);
		WriteRelease(&header_->tail, static_cast<LONG>(tail + len));
	}

	// Copies len bytes `offset` bytes after the read position without consuming them, so a
	// consumer can walk records ahead of the tail before deciding to skip any.
	bool PeekAt(uint32_t offset, void* dst, uint32_t len) const noexcept
	{
		const uint32_t readable = ReadableBytes();
		if (offset > readable || len > readable - offset) {
			return false;
		}
		const uint32_t tail = static_cast<uint32_t>(ReadAcquire(&header_->tail));
		const_cast<RingBufferView*>(this)->Copy(tail + offset, dst, len, false);
		return true;
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
