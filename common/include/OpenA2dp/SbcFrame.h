#pragma once

// SBC frame format (A2DP spec 12.5): header constants, frame length, CRC and how many
// frames a payload carries. Kernel-safe: integer only, so the driver's encoder and the
// codec library measure and check-sum frames with the same code. The frame parameters are
// SbcConfig's.

#include <OpenA2dp/SbcConfig.h>
#include <OpenA2dp/Types.h>

namespace OpenA2dp::SbcFrame {

constexpr uint8_t kSyncWord = 0x9C;
constexpr size_t kMaxFramesPerPacket = 15;
// Header, the widest scale-factor block and the longest audio payload the bitpool allows.
constexpr size_t kMaxFrameBytes = 4 + (4 * 8 * 2) / 8 + (8 + 16 * 250 + 7) / 8;

// Frame length from the header fields: 4-byte header, the scale factors (4 bits per
// subband and channel), then the audio bits the mode and bitpool imply.
constexpr size_t FrameLength(const SbcConfig::Configuration& p) noexcept
{
	const size_t nch = SbcConfig::ChannelCount(p.mode);
	const size_t length = 4 + (4 * p.subbands * nch) / 8;
	size_t audioBits = 0;
	if (p.mode == SbcConfig::ModeValue(SbcConfig::kModeMono) ||
			p.mode == SbcConfig::ModeValue(SbcConfig::kModeDual))
	{
		audioBits = static_cast<size_t>(p.blocks) * nch * p.bitpool;
	}
	else if (p.mode == SbcConfig::ModeValue(SbcConfig::kModeStereo))
	{
		audioBits = static_cast<size_t>(p.blocks) * p.bitpool;
	}
	else
	{
		// Joint stereo carries one join bitmask ahead of the audio bits.
		audioBits = p.subbands + static_cast<size_t>(p.blocks) * p.bitpool;
	}
	return length + (audioBits + 7) / 8;
}

// Bits per second the configured frame rate implies, for the status report.
constexpr uint32_t Bitrate(const SbcConfig::Configuration& p) noexcept
{
	return static_cast<uint32_t>((8ull * FrameLength(p) * p.sampleRate) /
			(static_cast<uint64_t>(p.subbands) * p.blocks));
}

// CRC-8 (x^8+x^4+x^3+x^2+1, init 0x0F) over `bitLength` bits, MSB first. It covers header
// bytes 1 and 2 and then everything after the CRC byte up to the end of the scale factors.
constexpr uint8_t Crc8(const uint8_t* data, size_t bitLength) noexcept
{
	uint8_t crc = 0x0F;
	for (size_t i = 0; i < bitLength; ++i)
	{
		const unsigned bit = (data[i >> 3] >> (7 - (i & 7))) & 1u;
		const unsigned feedback = ((crc >> 7) & 1u) ^ bit;
		crc = static_cast<uint8_t>(crc << 1);
		if (feedback)
		{
			crc ^= 0x1D;
		}
	}
	return crc;
}

// The SBC media payload header (A2DP 4.3.4): F=S=L=0, frame count.
constexpr uint8_t MediaPayloadHeader(size_t frameCount) noexcept
{
	return static_cast<uint8_t>(frameCount & 0x0F);
}

// How many whole frames fit in a media payload of `payloadCapacity` bytes (after RTP).
constexpr size_t FramesPerPacket(const SbcConfig::Configuration& p, size_t payloadCapacity) noexcept
{
	if (payloadCapacity <= 1)
	{
		return 0;
	}
	const size_t n = (payloadCapacity - 1) / FrameLength(p);
	return n < kMaxFramesPerPacket ? n : kMaxFramesPerPacket;
}

} // namespace OpenA2dp::SbcFrame
