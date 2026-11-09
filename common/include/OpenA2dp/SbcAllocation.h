#pragma once

// SBC bit allocation (A2DP spec 12.6.3): scale factors to bits per subband.
// Kernel-safe: no STL, no exceptions, integer only, so the driver's fixed-point encoder
// and the codec library allocate bits with the same code instead of two copies of the
// spec algorithm that can drift apart. The frame parameters it reads are SbcConfig's.

#include <OpenA2dp/SbcConfig.h>
#include <OpenA2dp/Types.h>

namespace OpenA2dp::SbcAllocation {

// Loudness offsets [sample rate index][subband] (spec Tables 12.19 / 12.20).
inline constexpr int kOffset4[4][4] = {{-1, 0, 0, 0}, {-2, 0, 0, 1}, {-2, 0, 0, 1}, {-2, 0, 0, 1}};
inline constexpr int kOffset8[4][8] = {
	{-2, 0, 0, 0, 0, 0, 0, 1}, {-3, 0, 0, 0, 0, 0, 1, 2}, {-4, 0, 0, 0, 0, 0, 1, 2}, {-4, 0, 0, 0, 0, 0, 1, 2}};

namespace Detail {

// Bit need per subband: the SNR allocation uses the scale factor as it is, loudness
// subtracts the offset for the rate and halves the rest.
inline void ComputeBitneed(const SbcConfig::Configuration& p, const uint8_t scaleFactors[2][8], int channel,
		int bitneed[2][8]) noexcept
{
	const int fsIndex = SbcConfig::SampleRateIndex(p.sampleRate);
	for (int sb = 0; sb < p.subbands; ++sb)
	{
		if (p.allocation == SbcConfig::AllocationValue(SbcConfig::kAllocationSnr))
		{
			bitneed[channel][sb] = scaleFactors[channel][sb];
		}
		else if (scaleFactors[channel][sb] == 0)
		{
			bitneed[channel][sb] = -5;
		}
		else
		{
			const int offset = p.subbands == 4 ? kOffset4[fsIndex][sb] : kOffset8[fsIndex][sb];
			const int loudness = scaleFactors[channel][sb] - offset;
			bitneed[channel][sb] = loudness > 0 ? loudness / 2 : loudness;
		}
	}
}

inline int CountSlice(int bitneed, int bitslice) noexcept
{
	if (bitneed > bitslice + 1 && bitneed < bitslice + 16)
	{
		return 1;
	}
	return bitneed == bitslice + 1 ? 2 : 0;
}

// Allocates `bitpool` bits over the listed channels (one pool per channel for mono and dual
// channel, a shared pool for stereo and joint stereo). -1000 is below any bit need.
inline void Allocate(const SbcConfig::Configuration& p, const int* channels, int channelCount,
		int bitneed[2][8], int bits[2][8]) noexcept
{
	const int nsb = p.subbands;
	const int bitpool = p.bitpool;

	int maxBitneed = -1000;
	for (int c = 0; c < channelCount; ++c)
	{
		for (int sb = 0; sb < nsb; ++sb)
		{
			if (bitneed[channels[c]][sb] > maxBitneed)
			{
				maxBitneed = bitneed[channels[c]][sb];
			}
		}
	}

	int bitcount = 0;
	int slicecount = 0;
	int bitslice = maxBitneed + 1;
	do
	{
		--bitslice;
		bitcount += slicecount;
		slicecount = 0;
		for (int c = 0; c < channelCount; ++c)
		{
			for (int sb = 0; sb < nsb; ++sb)
			{
				slicecount += CountSlice(bitneed[channels[c]][sb], bitslice);
			}
		}
	} while (bitcount + slicecount < bitpool);

	if (bitcount + slicecount == bitpool)
	{
		bitcount += slicecount;
		--bitslice;
	}

	for (int c = 0; c < channelCount; ++c)
	{
		const int ch = channels[c];
		for (int sb = 0; sb < nsb; ++sb)
		{
			const int b = bitneed[ch][sb] - bitslice;
			bits[ch][sb] = bitneed[ch][sb] < bitslice + 2 ? 0 : (b < 16 ? b : 16);
		}
	}

	// Distribute the remaining bits, interleaving channels per subband.
	int idx = 0;
	const int total = nsb * channelCount;
	while (bitcount < bitpool && idx < total)
	{
		const int ch = channels[idx % channelCount];
		const int sb = idx / channelCount;
		if (bits[ch][sb] >= 2 && bits[ch][sb] < 16)
		{
			++bits[ch][sb];
			++bitcount;
		}
		else if (bitneed[ch][sb] == bitslice + 1 && bitpool > bitcount + 1)
		{
			bits[ch][sb] = 2;
			bitcount += 2;
		}
		++idx;
	}

	idx = 0;
	while (bitcount < bitpool && idx < total)
	{
		const int ch = channels[idx % channelCount];
		const int sb = idx / channelCount;
		if (bits[ch][sb] < 16)
		{
			++bits[ch][sb];
			++bitcount;
		}
		++idx;
	}
}

} // namespace Detail

// Fills bits[channel][subband] from the scale factors of one frame.
inline void CalculateBits(const SbcConfig::Configuration& p, const uint8_t scaleFactors[2][8],
		int bits[2][8]) noexcept
{
	int bitneed[2][8] = {};
	for (int ch = 0; ch < 2; ++ch)
	{
		for (int sb = 0; sb < 8; ++sb)
		{
			bits[ch][sb] = 0;
		}
	}

	const uint8_t mode = p.mode;
	if (mode == SbcConfig::ModeValue(SbcConfig::kModeMono) || mode == SbcConfig::ModeValue(SbcConfig::kModeDual))
	{
		for (int ch = 0; ch < static_cast<int>(SbcConfig::ChannelCount(mode)); ++ch)
		{
			Detail::ComputeBitneed(p, scaleFactors, ch, bitneed);
			const int channels[1] = {ch};
			Detail::Allocate(p, channels, 1, bitneed, bits);
		}
	}
	else
	{
		Detail::ComputeBitneed(p, scaleFactors, 0, bitneed);
		Detail::ComputeBitneed(p, scaleFactors, 1, bitneed);
		const int channels[2] = {0, 1};
		Detail::Allocate(p, channels, 2, bitneed, bits);
	}
}

} // namespace OpenA2dp::SbcAllocation
