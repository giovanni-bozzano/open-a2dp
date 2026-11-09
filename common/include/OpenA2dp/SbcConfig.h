#pragma once

// SBC configuration selection (A2DP spec 4.3.2). Kernel-safe: no STL, no exceptions.

#include <OpenA2dp/Types.h>

namespace OpenA2dp::SbcConfig {

constexpr size_t kIeLength = 4;
constexpr uint8_t kMinBitpool = 2;
constexpr uint8_t kMaxBitpool = 250;

// Codec IE bit masks (A2DP spec 4.3.2): byte 0 holds the sample rates and channel modes,
// byte 1 the block counts, the subbands per frame and the allocation method. The driver's
// selector and the SBC codec share this one table, so an advertised capability and the
// configuration selected from it cannot drift apart.
constexpr uint8_t kFs16000 = 0x80;
constexpr uint8_t kFs32000 = 0x40;
constexpr uint8_t kFs44100 = 0x20;
constexpr uint8_t kFs48000 = 0x10;
constexpr uint8_t kModeMono = 0x08;
constexpr uint8_t kModeDual = 0x04;
constexpr uint8_t kModeStereo = 0x02;
constexpr uint8_t kModeJoint = 0x01;
constexpr uint8_t kBlocks4 = 0x80;
constexpr uint8_t kBlocks8 = 0x40;
constexpr uint8_t kBlocks12 = 0x20;
constexpr uint8_t kBlocks16 = 0x10;
constexpr uint8_t kSubbands4 = 0x08;
constexpr uint8_t kSubbands8 = 0x04;
constexpr uint8_t kAllocationLoudness = 0x01;
constexpr uint8_t kAllocationSnr = 0x02;

// Every bit of a field, which is what "supports everything the encoder can do" means.
constexpr uint8_t kAllRates = kFs16000 | kFs32000 | kFs44100 | kFs48000;
constexpr uint8_t kAllModes = kModeMono | kModeDual | kModeStereo | kModeJoint;
constexpr uint8_t kAllBlocks = kBlocks4 | kBlocks8 | kBlocks12 | kBlocks16;
constexpr uint8_t kAllSubbands = kSubbands4 | kSubbands8;
constexpr uint8_t kAllAllocations = kAllocationLoudness | kAllocationSnr;

// Everything the encoder supports.
constexpr uint8_t kLocalCapabilities[kIeLength] = {
	kAllRates | kAllModes, kAllBlocks | kAllSubbands | kAllAllocations, kMinBitpool, kMaxBitpool};

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

constexpr uint8_t BlocksBit(uint8_t blocks) noexcept
{
	return blocks == 4 ? kBlocks4
		: blocks == 8 ? kBlocks8
		: blocks == 12 ? kBlocks12
		: blocks == 16 ? kBlocks16
		: 0;
}

constexpr uint8_t SubbandsBit(uint8_t subbands) noexcept
{
	return subbands == 4 ? kSubbands4 : subbands == 8 ? kSubbands8 : 0;
}

constexpr uint8_t SampleRateBit(uint32_t rate) noexcept
{
	return rate == 16000 ? kFs16000
		: rate == 32000 ? kFs32000
		: rate == 44100 ? kFs44100
		: rate == 48000 ? kFs48000
		: 0;
}

// The sample rates and channel modes an advertised capability byte (IE byte 0) offers, as
// bit masks the driver publishes to the CLI: bit 0 = 44100 Hz / mono, bit 1 = 48000 Hz /
// stereo. 0 means the sink has not been queried yet.
constexpr uint32_t AdvertisedRates(uint8_t capabilities) noexcept
{
	return (capabilities & kFs44100 ? 1u : 0u) | (capabilities & kFs48000 ? 2u : 0u);
}

constexpr uint32_t AdvertisedChannels(uint8_t capabilities) noexcept
{
	return (capabilities & kModeMono ? 1u : 0u)
			| (capabilities & (kModeDual | kModeStereo | kModeJoint) ? 2u : 0u);
}

// Whether the sink's advertised capabilities (byte 0 of its IE, 0 = unknown) cover an
// endpoint format; 0 means "any" for both fields.
constexpr bool SupportsFormat(uint8_t capabilities, uint32_t rate, uint8_t channels) noexcept
{
	if (capabilities == 0)
	{
		return false;
	}
	if (rate != 0 && (AdvertisedRates(capabilities) & (rate == 44100 ? 1u : 2u)) == 0)
	{
		return false;
	}
	return channels == 0 || (AdvertisedChannels(capabilities) & channels) != 0;
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
	const uint8_t fsOrder[] = {kFs44100, kFs48000, kFs32000, kFs16000};
	const uint8_t modeOrder[] = {kModeJoint, kModeStereo, kModeDual, kModeMono};
	const uint8_t blocksOrder[] = {kBlocks16, kBlocks12, kBlocks8, kBlocks4};
	const uint8_t subbandsOrder[] = {kSubbands8, kSubbands4};
	const uint8_t allocOrder[] = {kAllocationLoudness, kAllocationSnr};

	const uint8_t fsAvail = remoteIe[0] & kLocalCapabilities[0] & kAllRates;
	uint8_t fs = SampleRateBit(preferredSampleRate) & fsAvail;
	if (!fs) {
		fs = PickFirst(fsAvail, fsOrder, 4);
	}
	// Prefer the mode that matches the endpoint's channel count: mono for a mono stream,
	// joint stereo otherwise. Falls back to the sink's own capabilities.
	const uint8_t modeAvail = remoteIe[0] & kAllModes;
	uint8_t mode = 0;
	if (frame.channels == 1 && (modeAvail & kModeMono)) {
		mode = kModeMono;
	} else if (frame.channels == 2 && (modeAvail & kModeJoint)) {
		mode = kModeJoint;
	}
	if (!mode) {
		mode = PickFirst(modeAvail, modeOrder, 4);
	}
	const uint8_t blocks = PickPreferred(remoteIe[1] & kAllBlocks, BlocksBit(frame.blocks), blocksOrder, 4);
	const uint8_t subbands =
			PickPreferred(remoteIe[1] & kAllSubbands, SubbandsBit(frame.subbands), subbandsOrder, 2);
	const uint8_t alloc = PickPreferred(remoteIe[1] & kAllAllocations, frame.allocation, allocOrder, 2);
	if (!fs || !mode || !blocks || !subbands || !alloc) {
		return false;
	}

	const unsigned nsb = subbands == kSubbands4 ? 4u : 8u;
	const bool singleChannelLimit = mode == kModeMono || mode == kModeDual;
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
