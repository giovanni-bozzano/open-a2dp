#pragma once

// SBC frame format (A2DP spec 12.5): header constants, frame length, CRC and how many
// frames a payload carries. Kernel-safe: integer only, so the driver's encoder and the
// codec library measure and check-sum frames with the same code. The frame parameters are
// SbcConfig's.

#include <OpenA2dp/BitWriter.h>
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

// How many whole frames fit in a media payload of `payloadCapacity` bytes (after RTP), at most
// `maxFrames`. A caller that wants shorter packets - less audio buffered per packet, at the
// cost of a packet per 8 ms instead of per 20 - passes a lower cap.
constexpr size_t FramesPerPacket(const SbcConfig::Configuration& p, size_t payloadCapacity,
								 size_t maxFrames = kMaxFramesPerPacket) noexcept
{
	if (payloadCapacity <= 1 || maxFrames == 0)
	{
		return 0;
	}
	size_t n = (payloadCapacity - 1) / FrameLength(p);
	if (n > maxFrames)
	{
		n = maxFrames;
	}
	return n < kMaxFramesPerPacket ? n : kMaxFramesPerPacket;
}

// Writes the fixed header, the joint-stereo mask and the scale factors, then the CRC over
// exactly those bits. The writer must sit at the start of `out`, which is the same buffer it
// writes to; the audio bits follow from where the writer is left.
inline void WriteFrameHeader(BitWriter& w, uint8_t* out, const SbcConfig::Configuration& p,
		uint8_t join, const uint8_t sf[2][8]) noexcept
{
	const int nsb = p.subbands;
	const int nch = SbcConfig::ChannelCount(p.mode);
	// FrameLength treats everything outside mono, dual and stereo as joint stereo.
	const bool joint = p.mode != SbcConfig::ModeValue(SbcConfig::kModeMono) &&
			p.mode != SbcConfig::ModeValue(SbcConfig::kModeDual) &&
			p.mode != SbcConfig::ModeValue(SbcConfig::kModeStereo);

	w.Write(kSyncWord, 8);
	w.Write(static_cast<uint32_t>(SbcConfig::SampleRateIndex(p.sampleRate)), 2);
	w.Write(static_cast<uint32_t>(p.blocks / 4 - 1), 2);
	w.Write(static_cast<uint32_t>(p.mode), 2);
	w.Write(static_cast<uint32_t>(p.allocation), 1);
	w.Write(nsb == 8 ? 1u : 0u, 1);
	w.Write(static_cast<uint32_t>(p.bitpool), 8);
	w.Write(0, 8); // CRC placeholder
	if (joint)
	{
		w.Write(join, static_cast<unsigned>(nsb)); // joined flags, then the RFA bit
	}
	for (int ch = 0; ch < nch; ++ch)
	{
		for (int sb = 0; sb < nsb; ++sb)
		{
			w.Write(sf[ch][sb], 4);
		}
	}

	const size_t crcBits = 16 + (joint ? static_cast<size_t>(nsb) : 0) + 4u * nsb * nch;
	uint8_t crcData[2 + 18] = {};
	crcData[0] = out[1];
	crcData[1] = out[2];
	const size_t tailBytes = (crcBits - 16 + 7) / 8;
	for (size_t i = 0; i < tailBytes; ++i)
	{
		crcData[2 + i] = out[4 + i];
	}
	out[3] = Crc8(crcData, crcBits);
}

// Aligns the writer and zero-fills the unused tail of the frame: the bit allocation need not
// fill every bit the frame length allows. False when the bits written did not fit.
inline bool FinishFrame(BitWriter& w, uint8_t* out, size_t frameLength) noexcept
{
	w.AlignToByte();
	if (w.Overflow() || w.BytesUsed() > frameLength)
	{
		return false;
	}
	for (size_t i = w.BytesUsed(); i < frameLength; ++i)
	{
		out[i] = 0;
	}
	return true;
}

} // namespace OpenA2dp::SbcFrame
