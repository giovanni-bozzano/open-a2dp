#pragma once

// aptX HD identifiers and codec-specific IE helpers, shared by the driver (which advertises
// and negotiates the codec) and the codec library (which encodes it). Kernel-safe: no STL,
// no exceptions, no allocation.

#include <OpenA2dp/Avdtp.h>
#include <OpenA2dp/VendorAptx.h>

namespace OpenA2dp::AptxHd {

constexpr uint32_t kVendorId = 0x000000D7;
constexpr uint16_t kCodecId = 0x0024;
// Vendor and codec IDs, the rate byte and four reserved bytes.
constexpr size_t kIeLength = 11;

using VendorAptx::kChannelMask;
using VendorAptx::kChannelMono;
using VendorAptx::kChannelStereo;
using VendorAptx::kSampleRate44100;
using VendorAptx::kSampleRate48000;
using VendorAptx::kSampleRateMask;

constexpr uint32_t kSamplesPerFrame = 4;
constexpr size_t kFrameBytes = 6;
// 24-in-32: one sample per 4 bytes, valid bits left-justified, low byte zero.
constexpr uint16_t kBitsPerSample = 24;

constexpr bool ParseIe(const uint8_t* ie, size_t length, uint8_t& capabilities) noexcept
{
	return VendorAptx::ParseIe(kVendorId, kCodecId, kIeLength, ie, length, capabilities);
}

constexpr void WriteIe(uint8_t sampleRateBits, uint8_t* ie) noexcept
{
	VendorAptx::WriteIe(kVendorId, kCodecId, kIeLength, sampleRateBits, ie);
}

constexpr uint8_t ValidateIe(const uint8_t* ie, size_t length) noexcept
{
	return VendorAptx::ValidateIe(kVendorId, kCodecId, kIeLength, ie, length);
}

} // namespace OpenA2dp::AptxHd
