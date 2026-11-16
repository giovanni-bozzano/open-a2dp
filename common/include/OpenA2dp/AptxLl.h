#pragma once

// aptX Low Latency identifiers and codec-specific IE helpers, shared by the driver (which
// advertises and negotiates the codec) and the codec library (which encodes it). Kernel-safe:
// no STL, no exceptions, no allocation.
//
// LL is plain aptX on the wire: the same four-sample, four-byte codewords, the same 16-bit
// stereo 44.1/48 kHz formats. What differs is its A2DP identity - Qualcomm's CSR vendor ID and
// codec ID 0x0002 - and the buffer levels its IE carries, which ask both ends for a much
// shorter buffer than classic aptX. It is therefore a separate SEP, not a mode of Aptx.h.
//
// The IE is the vendor and codec IDs, the sample-rate/channel byte and ten more bytes of
// buffer levels. A sink advertises its levels and accepts a configuration that repeats the
// rate it was offered; the source writes the target level it will keep. The remaining bytes
// are left zero, which is what sinks themselves advertise.

#include <OpenA2dp/Avdtp.h>

namespace OpenA2dp::AptxLl {

constexpr uint32_t kVendorId = 0x0000000A;
constexpr uint16_t kCodecId = 0x0002;
// Vendor and codec IDs, the rate/channel byte and the buffer levels.
constexpr size_t kIeLength = 17;

constexpr uint8_t kSampleRate44100 = 0x20;
constexpr uint8_t kSampleRate48000 = 0x10;
constexpr uint8_t kSampleRateMask = kSampleRate44100 | kSampleRate48000;
constexpr uint8_t kChannelMono = 0x01;
constexpr uint8_t kChannelStereo = 0x02;
constexpr uint8_t kChannelMask = 0x0F;

// The buffer level the source asks for, in milliseconds, written after the rate byte. It is
// the value a low-latency sink advertises; the rest of the levels stay zero.
constexpr uint8_t kTargetBufferMs = 2;

// Four samples per channel become four bytes, as in classic aptX.
constexpr uint32_t kSamplesPerFrame = 4;
constexpr size_t kFrameBytes = 4;
constexpr uint16_t kBitsPerSample = 16;

// True when the IE belongs to this codec: the vendor and codec IDs are little endian, as
// A2DP writes them. Reads the sample-rate/channel-mode byte into `capabilities`. Only the
// first seven bytes are required, because a sink may advertise a shorter IE than the source's
// configuration carries.
constexpr bool ParseIe(const uint8_t* ie, size_t length, uint8_t& capabilities) noexcept
{
	if (ie == nullptr || length < 7)
	{
		return false;
	}
	const uint32_t vendor = static_cast<uint32_t>(ie[0]) | (static_cast<uint32_t>(ie[1]) << 8) |
							(static_cast<uint32_t>(ie[2]) << 16) | (static_cast<uint32_t>(ie[3]) << 24);
	const uint16_t codec = static_cast<uint16_t>(ie[4] | (ie[5] << 8));
	if (vendor != kVendorId || codec != kCodecId)
	{
		return false;
	}
	capabilities = ie[6];
	return true;
}

// Writes our IE for one sample-rate bit and stereo. The buffer levels carry the target only;
// every other level stays zero, which reads as "no preference".
constexpr void WriteIe(uint8_t sampleRateBits, uint8_t* ie) noexcept
{
	ie[0] = static_cast<uint8_t>(kVendorId & 0xFF);
	ie[1] = static_cast<uint8_t>((kVendorId >> 8) & 0xFF);
	ie[2] = static_cast<uint8_t>((kVendorId >> 16) & 0xFF);
	ie[3] = static_cast<uint8_t>((kVendorId >> 24) & 0xFF);
	ie[4] = static_cast<uint8_t>(kCodecId & 0xFF);
	ie[5] = static_cast<uint8_t>((kCodecId >> 8) & 0xFF);
	ie[6] = static_cast<uint8_t>(sampleRateBits | kChannelStereo);
	ie[7] = kTargetBufferMs;
	for (size_t i = 8; i < kIeLength; ++i)
	{
		ie[i] = 0;
	}
}

constexpr bool SingleBit(uint8_t value) noexcept
{
	return value != 0 && (value & static_cast<uint8_t>(value - 1)) == 0;
}

// A configuration the sink proposes: exactly one rate and the stereo channel mode. Returns 0
// or the AVDTP error code, the same contract as the other codec validators.
constexpr uint8_t ValidateIe(const uint8_t* ie, size_t length) noexcept
{
	uint8_t capabilities = 0;
	if (!ParseIe(ie, length, capabilities))
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

} // namespace OpenA2dp::AptxLl
