#pragma once

// AAC configuration selection (A2DP spec 4.5.2). Kernel-safe: no STL, no exceptions.

#include <OpenA2dp/Avdtp.h>
#include <OpenA2dp/Types.h>

namespace OpenA2dp::AacConfig {

constexpr size_t kIeLength = 6;

// Object type (byte 0).
constexpr uint8_t kObjectMpeg2Lc = 0x80;
constexpr uint8_t kObjectMpeg4Lc = 0x40;

// Channels (byte 2, bits 3..2).
constexpr uint8_t kChannels1 = 0x08;
constexpr uint8_t kChannels2 = 0x04;

// Bitrates the Media Foundation AAC encoder accepts in CBR, ascending. It is a discrete
// set: 224, 288 and 384 kbps are rejected outright, 256 and 320 kbps are available.
constexpr uint32_t kSupportedBitrates[] = {96000, 128000, 160000, 192000, 256000, 320000};
constexpr size_t kSupportedBitrateCount = sizeof(kSupportedBitrates) / sizeof(kSupportedBitrates[0]);
constexpr uint32_t kMaxBitrate = kSupportedBitrates[kSupportedBitrateCount - 1];

constexpr uint32_t kSamplesPerFrame = 1024;

// Bytes to keep free in a media payload beyond the average frame: the LATM header (about
// eleven bytes) and the overshoot of individual CBR frames. Without fragmentation an
// access unit has to fit one RTP packet, so a bitrate is only usable when it leaves this.
constexpr size_t kMediaHeadroom = 32;

// Highest supported bitrate whose average frame plus kMediaHeadroom fits payloadBytes,
// which excludes the RTP header. 0 when even the lowest bitrate does not fit.
constexpr uint32_t MaxBitrateForMtu(size_t payloadBytes, uint32_t sampleRate) noexcept
{
	if (sampleRate == 0) {
		return 0;
	}
	for (size_t i = kSupportedBitrateCount; i-- > 0;) {
		const uint32_t bitrate = kSupportedBitrates[i];
		const size_t average = static_cast<size_t>(bitrate) / 8 * kSamplesPerFrame / sampleRate;
		if (average + kMediaHeadroom <= payloadBytes) {
			return bitrate;
		}
	}
	return 0;
}

struct Params {
	uint8_t objectType = kObjectMpeg2Lc;
	uint32_t sampleRate = 44100;
	uint8_t channels = 2;
	bool vbr = false;
	uint32_t bitrate = kMaxBitrate; // bits per second, 0 = unspecified
};

// Order of the 12-bit sampling frequency field, MSB (bit 11) first.
constexpr uint32_t kFieldRates[12] = {8000, 11025, 12000, 16000, 22050, 24000,
									  32000, 44100, 48000, 64000, 88200, 96000};

constexpr bool SingleBit(uint32_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

constexpr uint16_t SampleRateMask(uint32_t sampleRate) noexcept
{
	for (int i = 0; i < 12; ++i) {
		if (kFieldRates[i] == sampleRate) {
			return static_cast<uint16_t>(1u << (11 - i));
		}
	}
	return 0;
}

constexpr uint16_t ReadSampleRateField(const uint8_t* ie) noexcept
{
	return static_cast<uint16_t>((ie[1] << 4) | (ie[2] >> 4));
}

constexpr uint32_t ReadBitrate(const uint8_t* ie) noexcept
{
	return (static_cast<uint32_t>(ie[3] & 0x7F) << 16) | (static_cast<uint32_t>(ie[4]) << 8) | ie[5];
}

constexpr bool EncoderSupportsRate(uint32_t rate) noexcept { return rate == 44100 || rate == 48000; }

// Highest supported bitrate <= requested and <= limit (0 = no limit), or 0.
constexpr uint32_t ClampBitrate(uint32_t requested, uint32_t limit) noexcept
{
	uint32_t chosen = 0;
	for (size_t i = 0; i < kSupportedBitrateCount; ++i) {
		const uint32_t b = kSupportedBitrates[i];
		if (b <= requested && (limit == 0 || b <= limit)) {
			chosen = b;
		}
	}
	return chosen;
}

constexpr void WriteIe(const Params& p, uint8_t* ie) noexcept
{
	const uint16_t fs = SampleRateMask(p.sampleRate);
	ie[0] = p.objectType;
	ie[1] = static_cast<uint8_t>(fs >> 4);
	ie[2] = static_cast<uint8_t>(((fs & 0x0F) << 4) | (p.channels == 1 ? kChannels1 : kChannels2));
	ie[3] = static_cast<uint8_t>((p.vbr ? 0x80 : 0x00) | ((p.bitrate >> 16) & 0x7F));
	ie[4] = static_cast<uint8_t>(p.bitrate >> 8);
	ie[5] = static_cast<uint8_t>(p.bitrate);
}

constexpr bool ParseConfiguredIe(const uint8_t* ie, size_t len, Params& out) noexcept
{
	if (len < kIeLength) {
		return false;
	}
	const uint8_t object = ie[0];
	const uint16_t fs = ReadSampleRateField(ie);
	const uint8_t channels = ie[2] & 0x0C;
	if (!SingleBit(object) || !SingleBit(fs) || !SingleBit(channels)) {
		return false;
	}
	if (object != kObjectMpeg2Lc && object != kObjectMpeg4Lc) {
		return false;
	}
	int bit = 0;
	while (!(fs & (1u << (11 - bit)))) {
		++bit;
	}
	out.objectType = object;
	out.sampleRate = kFieldRates[bit];
	out.channels = channels == kChannels1 ? 1 : 2;
	out.vbr = (ie[3] & 0x80) != 0;
	out.bitrate = ReadBitrate(ie);
	return true;
}

// Everything the driver can play: MPEG-2/4 LC, 44.1/48 kHz, mono/stereo, CBR up to the
// highest bitrate one media packet carries. That is 256 kbps for the 895-byte A2DP MTU
// (883 payload bytes, one access unit per packet); the encoder's own ceiling is 320 kbps,
// which needs a larger MTU.
constexpr uint32_t kAdvertisedMaxBitrate = 256000;
constexpr uint8_t kLocalCapabilities[kIeLength] = {
	kObjectMpeg2Lc | kObjectMpeg4Lc,
	static_cast<uint8_t>((SampleRateMask(44100) | SampleRateMask(48000)) >> 4),
	static_cast<uint8_t>((((SampleRateMask(44100) | SampleRateMask(48000)) & 0x0F) << 4) | kChannels1 | kChannels2),
	static_cast<uint8_t>((kAdvertisedMaxBitrate >> 16) & 0x7F),
	static_cast<uint8_t>(kAdvertisedMaxBitrate >> 8),
	static_cast<uint8_t>(kAdvertisedMaxBitrate),
};

// Picks one value per field from the sink capabilities and writes a configured IE.
// preferredSampleRate / preferredChannels: 0 = no preference. bitratePreference: 0 =
// highest supported. payloadBytes: bytes one media packet can carry excluding the RTP
// header, or 0 to leave the bitrate to the sink and the encoder. Returns false if nothing
// is compatible.
constexpr bool Select(const uint8_t* remoteIe, size_t remoteIeLength, uint32_t preferredSampleRate,
					  uint32_t bitratePreference, uint8_t out[kIeLength],
					  uint8_t preferredChannels = 0, size_t payloadBytes = 0) noexcept
{
	if (remoteIeLength < kIeLength) {
		return false;
	}
	Params p;
	if (remoteIe[0] & kObjectMpeg2Lc) {
		p.objectType = kObjectMpeg2Lc;
	} else if (remoteIe[0] & kObjectMpeg4Lc) {
		p.objectType = kObjectMpeg4Lc;
	} else {
		return false;
	}

	const uint16_t remoteFs = ReadSampleRateField(remoteIe);
	if (EncoderSupportsRate(preferredSampleRate) && (remoteFs & SampleRateMask(preferredSampleRate))) {
		p.sampleRate = preferredSampleRate;
	} else if (remoteFs & SampleRateMask(44100)) {
		p.sampleRate = 44100;
	} else if (remoteFs & SampleRateMask(48000)) {
		p.sampleRate = 48000;
	} else {
		return false;
	}

	// The endpoint's channel count wins when the sink offers it, so the encoder gets the
	// PCM Windows produced; otherwise the sink's own preference decides.
	if (preferredChannels == 1 && (remoteIe[2] & kChannels1)) {
		p.channels = 1;
	} else if (preferredChannels == 2 && (remoteIe[2] & kChannels2)) {
		p.channels = 2;
	} else if (remoteIe[2] & kChannels2) {
		p.channels = 2;
	} else if (remoteIe[2] & kChannels1) {
		p.channels = 1;
	} else {
		return false;
	}

	const uint32_t remoteMax = ReadBitrate(remoteIe);
	uint32_t limit = remoteMax;
	if (payloadBytes != 0) {
		const uint32_t mtuLimit = MaxBitrateForMtu(payloadBytes, p.sampleRate);
		if (mtuLimit == 0) {
			return false;
		}
		if (limit == 0 || mtuLimit < limit) {
			limit = mtuLimit;
		}
	}
	p.vbr = false;
	p.bitrate = ClampBitrate(bitratePreference == 0 ? kMaxBitrate : bitratePreference, limit);
	if (p.bitrate == 0) {
		p.bitrate = kSupportedBitrates[0];
		if (limit != 0 && limit < p.bitrate) {
			return false;
		}
	}

	WriteIe(p, out);
	return true;
}

// Checks a sink-proposed configuration; returns 0 or an AVDTP error code.
constexpr uint8_t Validate(const uint8_t* ie, size_t ieLength) noexcept
{
	if (ieLength < kIeLength) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::BadLength);
	}
	if (!SingleBit(ie[0])) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidObjectType);
	}
	if (ie[0] != kObjectMpeg2Lc && ie[0] != kObjectMpeg4Lc) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedObjectType);
	}
	if (!SingleBit(ReadSampleRateField(ie))) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency);
	}
	Params p;
	if (!ParseConfiguredIe(ie, ieLength, p)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidChannels);
	}
	if (!EncoderSupportsRate(p.sampleRate)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
	}
	if (p.vbr) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedBitRate);
	}
	return 0;
}

} // namespace OpenA2dp::AacConfig
