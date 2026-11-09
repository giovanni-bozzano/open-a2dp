#include <OpenA2dp/SbcCodec.h>
#include <OpenA2dp/SbcConfig.h>

#include "SbcTables.h"

#include <algorithm>
#include <climits>

namespace OpenA2dp {

namespace Sbc {

int SampleRateIndex(uint32_t sampleRate) noexcept
{
	switch (sampleRate) {
	case 16000: return 0;
	case 32000: return 1;
	case 44100: return 2;
	case 48000: return 3;
	default: return -1;
	}
}

size_t FrameLength(const FrameParams& p) noexcept
{
	const size_t nch = p.Channels();
	size_t length = 4 + (4 * p.subbands * nch) / 8;
	size_t audioBits = 0;
	switch (p.channelMode) {
	case ChannelMode::Mono:
	case ChannelMode::DualChannel:
		audioBits = static_cast<size_t>(p.blocks) * nch * p.bitpool;
		break;
	case ChannelMode::Stereo:
		audioBits = static_cast<size_t>(p.blocks) * p.bitpool;
		break;
	case ChannelMode::JointStereo:
		audioBits = p.subbands + static_cast<size_t>(p.blocks) * p.bitpool;
		break;
	}
	return length + (audioBits + 7) / 8;
}

uint32_t Bitrate(const FrameParams& p) noexcept
{
	return static_cast<uint32_t>((8ull * FrameLength(p) * p.sampleRate) / (static_cast<uint64_t>(p.subbands) * p.blocks));
}

uint8_t Crc8(const uint8_t* data, size_t bitLength) noexcept
{
	uint8_t crc = 0x0F;
	for (size_t i = 0; i < bitLength; ++i) {
		const unsigned bit = (data[i >> 3] >> (7 - (i & 7))) & 1u;
		const unsigned feedback = ((crc >> 7) & 1u) ^ bit;
		crc = static_cast<uint8_t>(crc << 1);
		if (feedback) {
			crc ^= 0x1D;
		}
	}
	return crc;
}

namespace {

void ComputeBitneed(const FrameParams& p, const uint8_t scaleFactors[2][8], int ch, int bitneed[2][8]) noexcept
{
	const int fsIndex = SampleRateIndex(p.sampleRate);
	for (int sb = 0; sb < p.subbands; ++sb) {
		if (p.allocation == Allocation::Snr) {
			bitneed[ch][sb] = scaleFactors[ch][sb];
		} else if (scaleFactors[ch][sb] == 0) {
			bitneed[ch][sb] = -5;
		} else {
			const int offset = p.subbands == 4 ? Tables::kOffset4[fsIndex][sb] : Tables::kOffset8[fsIndex][sb];
			const int loudness = scaleFactors[ch][sb] - offset;
			bitneed[ch][sb] = loudness > 0 ? loudness / 2 : loudness;
		}
	}
}

int CountSlice(int bitneed, int bitslice) noexcept
{
	if (bitneed > bitslice + 1 && bitneed < bitslice + 16) {
		return 1;
	}
	if (bitneed == bitslice + 1) {
		return 2;
	}
	return 0;
}

// Allocates `bitpool` bits over the listed channels (1 for mono/dual per channel, 2 for stereo/joint).
void Allocate(const FrameParams& p, const int* channels, int channelCount, int bitneed[2][8], int bits[2][8]) noexcept
{
	const int nsb = p.subbands;
	const int bitpool = p.bitpool;

	int maxBitneed = INT_MIN;
	for (int c = 0; c < channelCount; ++c) {
		for (int sb = 0; sb < nsb; ++sb) {
			maxBitneed = std::max(maxBitneed, bitneed[channels[c]][sb]);
		}
	}

	int bitcount = 0;
	int slicecount = 0;
	int bitslice = maxBitneed + 1;
	do {
		--bitslice;
		bitcount += slicecount;
		slicecount = 0;
		for (int c = 0; c < channelCount; ++c) {
			for (int sb = 0; sb < nsb; ++sb) {
				slicecount += CountSlice(bitneed[channels[c]][sb], bitslice);
			}
		}
	} while (bitcount + slicecount < bitpool);

	if (bitcount + slicecount == bitpool) {
		bitcount += slicecount;
		--bitslice;
	}

	for (int c = 0; c < channelCount; ++c) {
		const int ch = channels[c];
		for (int sb = 0; sb < nsb; ++sb) {
			bits[ch][sb] = bitneed[ch][sb] < bitslice + 2 ? 0 : std::min(bitneed[ch][sb] - bitslice, 16);
		}
	}

	// Distribute remaining bits, interleaving channels per subband.
	int idx = 0;
	const int total = nsb * channelCount;
	while (bitcount < bitpool && idx < total) {
		const int ch = channels[idx % channelCount];
		const int sb = idx / channelCount;
		if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
			++bits[ch][sb];
			++bitcount;
		} else if (bitneed[ch][sb] == bitslice + 1 && bitpool > bitcount + 1) {
			bits[ch][sb] = 2;
			bitcount += 2;
		}
		++idx;
	}

	idx = 0;
	while (bitcount < bitpool && idx < total) {
		const int ch = channels[idx % channelCount];
		const int sb = idx / channelCount;
		if (bits[ch][sb] < 16) {
			++bits[ch][sb];
			++bitcount;
		}
		++idx;
	}
}

} // namespace

void CalculateBits(const FrameParams& p, const uint8_t scaleFactors[2][8], int bits[2][8]) noexcept
{
	int bitneed[2][8] = {};
	for (int ch = 0; ch < 2; ++ch) {
		for (int sb = 0; sb < 8; ++sb) {
			bits[ch][sb] = 0;
		}
	}

	if (p.channelMode == ChannelMode::Mono || p.channelMode == ChannelMode::DualChannel) {
		for (int ch = 0; ch < p.Channels(); ++ch) {
			ComputeBitneed(p, scaleFactors, ch, bitneed);
			const int channels[1] = {ch};
			Allocate(p, channels, 1, bitneed, bits);
		}
	} else {
		ComputeBitneed(p, scaleFactors, 0, bitneed);
		ComputeBitneed(p, scaleFactors, 1, bitneed);
		const int channels[2] = {0, 1};
		Allocate(p, channels, 2, bitneed, bits);
	}
}

// The shared decode lives in SbcConfig; this only maps it to the codec's own parameter type.
static_assert(static_cast<uint8_t>(ChannelMode::Mono) == 0 &&
		static_cast<uint8_t>(ChannelMode::DualChannel) == 1 &&
		static_cast<uint8_t>(ChannelMode::Stereo) == 2 &&
		static_cast<uint8_t>(ChannelMode::JointStereo) == 3, "ChannelMode must match the frame header");
static_assert(static_cast<uint8_t>(Allocation::Loudness) == 0 && static_cast<uint8_t>(Allocation::Snr) == 1,
		"Allocation must match the frame header");

bool ParseConfiguredIe(const uint8_t* ie, size_t len, FrameParams& out) noexcept
{
	SbcConfig::Configuration config;
	if (!SbcConfig::ParseConfiguredIe(ie, len, config)) {
		return false;
	}
	FrameParams parsed;
	parsed.sampleRate = config.sampleRate;
	parsed.channelMode = static_cast<ChannelMode>(config.mode);
	parsed.blocks = config.blocks;
	parsed.subbands = config.subbands;
	parsed.allocation = static_cast<Allocation>(config.allocation);
	parsed.bitpool = config.bitpool;
	out = parsed;
	return true;
}

} // namespace Sbc

using namespace Sbc;

size_t SbcCodec::BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept
{
	if (capacity < kIeLength) {
		return 0;
	}
	// The same table the driver's selector picks from.
	for (size_t i = 0; i < kIeLength; ++i) {
		ie[i] = SbcConfig::kLocalCapabilities[i];
	}
	return kIeLength;
}

bool SbcCodec::SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
								   const CodecPreference& preference, A2dpCodecConfig& out) const noexcept
{
	uint8_t ie[kIeLength];
	SbcConfig::FramePreference frame;
	frame.blocks = preference.sbcBlocks;
	frame.subbands = preference.sbcSubbands;
	frame.allocation = preference.sbcAllocation;
	if (!SbcConfig::Select(remoteIe, remoteIeLength, preference.preferredSampleRate,
						   preference.sbcMaxBitpool, ie, frame)) {
		return false;
	}
	out = {};
	out.codec = CodecType::Sbc;
	for (size_t i = 0; i < kIeLength; ++i) {
		out.ie[i] = ie[i];
	}
	out.ieLength = kIeLength;
	return true;
}

uint8_t SbcCodec::ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept
{
	if (ieLength < kIeLength) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::BadLength);
	}
	if (!SbcConfig::SingleBit(ie[0] & kAllRates)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency);
	}
	if (!SbcConfig::SingleBit(ie[0] & kAllModes)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidChannelMode);
	}
	if (!SbcConfig::SingleBit(ie[1] & kAllSubbands)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSubbands);
	}
	if (!SbcConfig::SingleBit(ie[1] & kAllAllocations)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidAllocationMethod);
	}
	if (!SbcConfig::SingleBit(ie[1] & kAllBlocks)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::BadPayloadFormat);
	}
	if (ie[2] < kMinBitpool || ie[2] > kMaxBitpool) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidMinimumBitpoolValue);
	}
	FrameParams p;
	if (!ParseConfiguredIe(ie, ieLength, p)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidMaximumBitpoolValue);
	}
	return 0;
}

HRESULT SbcCodec::CreateEncoder(const A2dpCodecConfig& config, std::unique_ptr<IA2dpEncoder>& encoder) const
{
	FrameParams p;
	if (config.codec != CodecType::Sbc || !ParseConfiguredIe(config.ie, config.ieLength, p)) {
		return E_INVALIDARG;
	}
	encoder = std::make_unique<SbcEncoder>(p);
	return S_OK;
}

} // namespace OpenA2dp
