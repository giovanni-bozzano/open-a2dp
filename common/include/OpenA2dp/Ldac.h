#pragma once

// Sony LDAC identifiers and codec-specific IE helpers, shared by the driver (which advertises
// and negotiates the codec) and the codec library (which encodes it with the vendored
// ldacBT_* encoder). Kernel-safe: no STL, no exceptions, no allocation.
//
// The IE is eight octets: the vendor and codec IDs, a sampling-frequency mask and a channel
// mode mask. Unlike the aptX codecs, the two masks sit in separate octets, and the channel
// bits are not the aptX ones.

#include <OpenA2dp/Avdtp.h>

namespace OpenA2dp::Ldac {

constexpr uint32_t kVendorId = 0x0000012D;
constexpr uint16_t kCodecId = 0x00AA;
// Vendor and codec IDs, the rate mask and the channel mode mask.
constexpr size_t kIeLength = 8;

constexpr uint8_t kSampleRate44100 = 0x20;
constexpr uint8_t kSampleRate48000 = 0x10;
constexpr uint8_t kSampleRate88200 = 0x08;
constexpr uint8_t kSampleRate96000 = 0x04;
constexpr uint8_t kSampleRateMask = kSampleRate44100 | kSampleRate48000 | kSampleRate88200 |
									kSampleRate96000;

constexpr uint8_t kChannelMono = 0x08;
constexpr uint8_t kChannelDual = 0x04;
constexpr uint8_t kChannelStereo = 0x02;
constexpr uint8_t kChannelMask = kChannelMono | kChannelDual | kChannelStereo;

// 128 samples per channel per encode call at every sample rate: the encoder's input unit, not
// the transport frame, whose length varies with quality and rate.
constexpr uint32_t kSamplesPerFrame = 128;
// One encode call returns up to this many bytes of transport frames.
constexpr size_t kMaxFrameBytes = 1024;
// LDAC is defined around 2-DH5 packets, so a link whose media payload is smaller cannot carry
// it and its highest quality needs the full 679 bytes.
constexpr uint32_t kMinMtuBytes = 679;
// Interleaved 16-bit stereo, the depth the encoder is fed: LDAC's own advantage over lower
// bitrates is its transform and bit allocation, and the endpoint format stays 16-bit.
constexpr uint16_t kBitsPerSample = 16;

// Encode quality modes, the values the ldacBT API takes as EQMID.
constexpr uint8_t kQualityHigh = 0;     // 909 kbps at 44.1 kHz, 990 kbps at 48 kHz
constexpr uint8_t kQualityStandard = 1; // 606 / 660 kbps
constexpr uint8_t kQualityMobile = 2;   // 303 / 330 kbps

constexpr uint32_t BitrateBps(uint8_t quality, uint32_t sampleRate) noexcept
{
	const bool highRate = sampleRate >= 48000;
	switch (quality) {
	case kQualityStandard:
		return highRate ? 660000u : 606000u;
	case kQualityMobile:
		return highRate ? 330000u : 303000u;
	default:
		return highRate ? 990000u : 909000u;
	}
}

// True when the IE belongs to this codec: the vendor and codec IDs are little endian, as A2DP
// writes them. Reads both masks out.
constexpr bool ParseIe(const uint8_t* ie, size_t length, uint8_t& sampleRates,
					   uint8_t& channelModes) noexcept
{
	if (ie == nullptr || length < kIeLength) {
		return false;
	}
	const uint32_t vendor = static_cast<uint32_t>(ie[0]) | (static_cast<uint32_t>(ie[1]) << 8) |
							(static_cast<uint32_t>(ie[2]) << 16) | (static_cast<uint32_t>(ie[3]) << 24);
	const uint16_t codec = static_cast<uint16_t>(ie[4] | (ie[5] << 8));
	if (vendor != kVendorId || codec != kCodecId) {
		return false;
	}
	sampleRates = static_cast<uint8_t>(ie[6] & kSampleRateMask);
	channelModes = static_cast<uint8_t>(ie[7] & kChannelMask);
	return true;
}

// Writes our IE for the rate mask and channel modes we offer, both already masked.
constexpr void WriteIe(uint8_t sampleRates, uint8_t channelModes, uint8_t* ie) noexcept
{
	ie[0] = static_cast<uint8_t>(kVendorId & 0xFF);
	ie[1] = static_cast<uint8_t>((kVendorId >> 8) & 0xFF);
	ie[2] = static_cast<uint8_t>((kVendorId >> 16) & 0xFF);
	ie[3] = static_cast<uint8_t>((kVendorId >> 24) & 0xFF);
	ie[4] = static_cast<uint8_t>(kCodecId & 0xFF);
	ie[5] = static_cast<uint8_t>((kCodecId >> 8) & 0xFF);
	ie[6] = sampleRates;
	ie[7] = channelModes;
}

constexpr bool SingleBit(uint8_t value) noexcept
{
	return value != 0 && (value & static_cast<uint8_t>(value - 1)) == 0;
}

// A configuration the sink proposes: exactly one rate at 44.1 or 48 kHz and stereo. The higher
// rates are in the IE but this pipeline runs the endpoint at 44.1/48 kHz, so they are refused
// rather than negotiated and resampled. Returns 0 or the AVDTP error code, the same contract
// as the codec library's validator.
constexpr uint8_t ValidateIe(const uint8_t* ie, size_t length) noexcept
{
	if (ie == nullptr || length < kIeLength) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::BadLength);
	}
	uint8_t rates = 0;
	uint8_t modes = 0;
	if (!ParseIe(ie, length, rates, modes)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidCodecType);
	}
	if (!SingleBit(rates)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency);
	}
	if (rates != kSampleRate44100 && rates != kSampleRate48000) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
	}
	if (!SingleBit(modes)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidChannelMode);
	}
	if (modes != kChannelStereo) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannelMode);
	}
	return 0;
}

} // namespace OpenA2dp::Ldac
