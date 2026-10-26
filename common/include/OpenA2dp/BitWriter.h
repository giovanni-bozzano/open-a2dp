#pragma once

// MSB-first bit writer/reader over a caller-provided buffer.
// Kernel-safe: no STL, no exceptions, no allocation.

#include <OpenA2dp/Types.h>

namespace OpenA2dp {

class BitWriter {
public:
	BitWriter(uint8_t* buffer, size_t capacity) noexcept : buffer_(buffer), capacity_(capacity) {}

	// Writes the low `count` bits of `value` (count <= 32), MSB first.
	bool Write(uint32_t value, unsigned count) noexcept
	{
		for (unsigned i = count; i-- > 0;) {
			const size_t byte = bitPos_ >> 3;
			if (byte >= capacity_) {
				overflow_ = true;
				return false;
			}
			const unsigned shift = 7 - static_cast<unsigned>(bitPos_ & 7);
			if (shift == 7) {
				buffer_[byte] = 0;
			}
			buffer_[byte] = static_cast<uint8_t>(buffer_[byte] | (((value >> i) & 1u) << shift));
			++bitPos_;
		}
		return true;
	}

	void AlignToByte() noexcept
	{
		while ((bitPos_ & 7) != 0) {
			Write(0, 1);
		}
	}

	size_t BitPosition() const noexcept { return bitPos_; }
	size_t BytesUsed() const noexcept { return (bitPos_ + 7) >> 3; }
	bool Overflow() const noexcept { return overflow_; }

private:
	uint8_t* buffer_;
	size_t capacity_;
	size_t bitPos_ = 0;
	bool overflow_ = false;
};

class BitReader {
public:
	BitReader(const uint8_t* buffer, size_t length) noexcept : buffer_(buffer), length_(length) {}

	uint32_t Read(unsigned count) noexcept
	{
		uint32_t value = 0;
		for (unsigned i = 0; i < count; ++i) {
			const size_t byte = bitPos_ >> 3;
			uint32_t bit = 0;
			if (byte < length_) {
				bit = (buffer_[byte] >> (7 - (bitPos_ & 7))) & 1u;
			} else {
				overflow_ = true;
			}
			value = (value << 1) | bit;
			++bitPos_;
		}
		return value;
	}

	size_t BitPosition() const noexcept { return bitPos_; }
	bool Overflow() const noexcept { return overflow_; }

private:
	const uint8_t* buffer_;
	size_t length_;
	size_t bitPos_ = 0;
	bool overflow_ = false;
};

} // namespace OpenA2dp
