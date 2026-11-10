#pragma once

// The information element layout the aptX vendor codecs share: a four-byte vendor ID, a
// two-byte codec ID and one byte of sample-rate and channel-mode bits. aptX HD appends four
// reserved bytes after that byte; classic aptX ends there. Kernel-safe: no STL, no
// exceptions, no allocation.

#include <OpenA2dp/Avdtp.h>

namespace OpenA2dp::VendorAptx {

constexpr uint8_t kSampleRate44100 = 0x20;
constexpr uint8_t kSampleRate48000 = 0x10;
constexpr uint8_t kSampleRateMask = kSampleRate44100 | kSampleRate48000;
constexpr uint8_t kChannelMono = 0x01;
constexpr uint8_t kChannelStereo = 0x02;
constexpr uint8_t kChannelMask = 0x0F;

// True when the IE belongs to this codec: the vendor and codec IDs are little endian, as
// A2DP writes them. Reads the sample-rate/channel-mode byte into `capabilities`.
constexpr bool ParseIe(uint32_t vendorId, uint16_t codecId, size_t ieLength, const uint8_t* ie,
					   size_t length, uint8_t& capabilities) noexcept
{
	if (ie == nullptr || length < ieLength || ieLength < 7)
	{
		return false;
	}
	const uint32_t vendor = static_cast<uint32_t>(ie[0]) | (static_cast<uint32_t>(ie[1]) << 8) |
							(static_cast<uint32_t>(ie[2]) << 16) | (static_cast<uint32_t>(ie[3]) << 24);
	const uint16_t codec = static_cast<uint16_t>(ie[4] | (ie[5] << 8));
	if (vendor != vendorId || codec != codecId)
	{
		return false;
	}
	capabilities = ie[6];
	return true;
}

// Writes our IE for one sample-rate bit and stereo. The reserved bytes stay zero.
constexpr void WriteIe(uint32_t vendorId, uint16_t codecId, size_t ieLength, uint8_t sampleRateBits,
					   uint8_t* ie) noexcept
{
	ie[0] = static_cast<uint8_t>(vendorId & 0xFF);
	ie[1] = static_cast<uint8_t>((vendorId >> 8) & 0xFF);
	ie[2] = static_cast<uint8_t>((vendorId >> 16) & 0xFF);
	ie[3] = static_cast<uint8_t>((vendorId >> 24) & 0xFF);
	ie[4] = static_cast<uint8_t>(codecId & 0xFF);
	ie[5] = static_cast<uint8_t>((codecId >> 8) & 0xFF);
	ie[6] = static_cast<uint8_t>(sampleRateBits | kChannelStereo);
	for (size_t i = 7; i < ieLength; ++i)
	{
		ie[i] = 0;
	}
}

constexpr bool SingleBit(uint8_t value) noexcept
{
	return value != 0 && (value & static_cast<uint8_t>(value - 1)) == 0;
}

// A configuration the sink proposes: exactly one rate and the stereo channel mode. Returns 0
// or the AVDTP error code, the same contract as the codec library's validator.
constexpr uint8_t ValidateIe(uint32_t vendorId, uint16_t codecId, size_t ieLength, const uint8_t* ie,
							 size_t length) noexcept
{
	if (ie == nullptr || length < ieLength)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::BadLength);
	}
	uint8_t capabilities = 0;
	if (!ParseIe(vendorId, codecId, ieLength, ie, length, capabilities))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidCodecType);
	}
	const uint8_t rates = static_cast<uint8_t>(capabilities & kSampleRateMask);
	if (!SingleBit(rates))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency);
	}
	if (rates != kSampleRate44100 && rates != kSampleRate48000)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
	}
	const uint8_t modes = static_cast<uint8_t>(capabilities & kChannelMask);
	if (!SingleBit(modes))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidChannelMode);
	}
	if (modes != kChannelStereo)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannelMode);
	}
	return 0;
}

} // namespace OpenA2dp::VendorAptx
