#pragma once

// SBC configuration selection (A2DP spec 4.3.2). Kernel-safe: no STL, no exceptions.

#include <OpenA2dp/Types.h>

namespace OpenA2dp::SbcConfig {

constexpr size_t kIeLength = 4;
constexpr uint8_t kMinBitpool = 2;
constexpr uint8_t kMaxBitpool = 250;

// Everything the encoder supports.
constexpr uint8_t kLocalCapabilities[kIeLength] = {0xFF, 0xFF, kMinBitpool, kMaxBitpool};

constexpr uint8_t PickFirst(uint8_t available, const uint8_t* order, size_t count) noexcept
{
	for (size_t i = 0; i < count; ++i) {
		if (available & order[i]) {
			return order[i];
		}
	}
	return 0;
}

// The preferred value when the sink offers it, otherwise the first available one.
constexpr uint8_t PickPreferred(uint8_t available, uint8_t preferred, const uint8_t* order,
								size_t count) noexcept
{
	if (preferred != 0 && (available & preferred) != 0) {
		return preferred;
	}
	return PickFirst(available, order, count);
}

// Frame parameters a preference can pin. Zero leaves the choice to the selector.
struct FramePreference {
	uint8_t blocks = 0;     // 4, 8, 12 or 16
	uint8_t subbands = 0;   // 4 or 8
	uint8_t allocation = 0; // kAllocationLoudness or kAllocationSnr
	uint8_t channels = 0;   // 1 or 2; 0 keeps the selector's joint-stereo-first order
};

constexpr uint8_t kAllocationLoudness = 0x01;
constexpr uint8_t kAllocationSnr = 0x02;

constexpr uint8_t BlocksBit(uint8_t blocks) noexcept
{
	return blocks == 4 ? 0x80 : blocks == 8 ? 0x40 : blocks == 12 ? 0x20 : blocks == 16 ? 0x10 : 0;
}

constexpr uint8_t SubbandsBit(uint8_t subbands) noexcept
{
	return subbands == 4 ? 0x08 : subbands == 8 ? 0x04 : 0;
}

constexpr uint8_t SampleRateBit(uint32_t rate) noexcept
{
	return rate == 16000 ? 0x80 : rate == 32000 ? 0x40 : rate == 44100 ? 0x20 : rate == 48000 ? 0x10 : 0;
}

// Picks one value per field from the sink capabilities and writes a configured IE.
// preferredSampleRate: 0 = no preference. Returns false if nothing is compatible.
constexpr bool Select(const uint8_t* remoteIe, size_t remoteIeLength, uint32_t preferredSampleRate,
					  uint8_t maxBitpoolPreference, uint8_t out[kIeLength],
					  const FramePreference& frame = {}) noexcept
{
	if (remoteIeLength < kIeLength) {
		return false;
	}
	const uint8_t fsOrder[] = {0x20, 0x10, 0x40, 0x80};
	const uint8_t modeOrder[] = {0x01, 0x02, 0x04, 0x08};
	const uint8_t blocksOrder[] = {0x10, 0x20, 0x40, 0x80};
	const uint8_t subbandsOrder[] = {0x04, 0x08};
	const uint8_t allocOrder[] = {0x01, 0x02};

	const uint8_t fsAvail = remoteIe[0] & kLocalCapabilities[0] & 0xF0;
	uint8_t fs = SampleRateBit(preferredSampleRate) & fsAvail;
	if (!fs) {
		fs = PickFirst(fsAvail, fsOrder, 4);
	}
	// Prefer the mode that matches the endpoint's channel count: mono for a mono stream,
	// joint stereo otherwise. Falls back to the sink's own capabilities.
	const uint8_t modeAvail = remoteIe[0] & 0x0F;
	uint8_t mode = 0;
	if (frame.channels == 1 && (modeAvail & 0x08)) {
		mode = 0x08; // mono
	} else if (frame.channels == 2 && (modeAvail & 0x01)) {
		mode = 0x01; // joint stereo
	}
	if (!mode) {
		mode = PickFirst(modeAvail, modeOrder, 4);
	}
	const uint8_t blocks = PickPreferred(remoteIe[1] & 0xF0, BlocksBit(frame.blocks), blocksOrder, 4);
	const uint8_t subbands = PickPreferred(remoteIe[1] & 0x0C, SubbandsBit(frame.subbands), subbandsOrder, 2);
	const uint8_t alloc = PickPreferred(remoteIe[1] & 0x03, frame.allocation, allocOrder, 2);
	if (!fs || !mode || !blocks || !subbands || !alloc) {
		return false;
	}

	const unsigned nsb = subbands == 0x08 ? 4u : 8u;
	const bool singleChannelLimit = mode == 0x08 || mode == 0x04;
	unsigned modeLimit = (singleChannelLimit ? 16u : 32u) * nsb;
	if (modeLimit > kMaxBitpool) {
		modeLimit = kMaxBitpool;
	}

	const uint8_t minBitpool = remoteIe[2] > kMinBitpool ? remoteIe[2] : kMinBitpool;
	unsigned maxBitpool = remoteIe[3] < modeLimit ? remoteIe[3] : modeLimit;
	const unsigned preference = maxBitpoolPreference > minBitpool ? maxBitpoolPreference : minBitpool;
	if (preference < maxBitpool) {
		maxBitpool = preference;
	}
	if (maxBitpool < minBitpool) {
		return false;
	}

	out[0] = static_cast<uint8_t>(fs | mode);
	out[1] = static_cast<uint8_t>(blocks | subbands | alloc);
	out[2] = minBitpool;
	out[3] = static_cast<uint8_t>(maxBitpool);
	return true;
}

} // namespace OpenA2dp::SbcConfig
