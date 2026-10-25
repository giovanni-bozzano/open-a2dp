#pragma once

// AVDTP / A2DP protocol constants shared by the driver, the service and tests.
// Kernel-safe: no STL, no exceptions, no allocation.

#include <stdint.h>
#include <stddef.h>

namespace OpenA2dp::Avdtp {

constexpr uint16_t kPsm = 0x0019;

enum class Signal : uint8_t {
	Discover = 0x01,
	GetCapabilities = 0x02,
	SetConfiguration = 0x03,
	GetConfiguration = 0x04,
	Reconfigure = 0x05,
	Open = 0x06,
	Start = 0x07,
	Close = 0x08,
	Suspend = 0x09,
	Abort = 0x0A,
	SecurityControl = 0x0B,
	GetAllCapabilities = 0x0C,
	DelayReport = 0x0D,
};

enum class MessageType : uint8_t {
	Command = 0,
	GeneralReject = 1,
	ResponseAccept = 2,
	ResponseReject = 3,
};

enum class PacketType : uint8_t {
	Single = 0,
	Start = 1,
	Continue = 2,
	End = 3,
};

enum class ServiceCategory : uint8_t {
	MediaTransport = 0x01,
	Reporting = 0x02,
	Recovery = 0x03,
	ContentProtection = 0x04,
	HeaderCompression = 0x05,
	Multiplexing = 0x06,
	MediaCodec = 0x07,
	DelayReporting = 0x08,
};

enum class MediaType : uint8_t {
	Audio = 0x00,
	Video = 0x01,
	Multimedia = 0x02,
};

enum class CodecType : uint8_t {
	Sbc = 0x00,
	Mpeg12 = 0x01,
	Aac = 0x02,
	Atrac = 0x04,
	Vendor = 0xFF,
};

enum class SepType : uint8_t {
	Source = 0,
	Sink = 1,
};

enum class ErrorCode : uint8_t {
	BadHeaderFormat = 0x01,
	BadLength = 0x11,
	BadAcpSeid = 0x12,
	SepInUse = 0x13,
	SepNotInUse = 0x14,
	BadServCategory = 0x17,
	BadPayloadFormat = 0x18,
	NotSupportedCommand = 0x19,
	InvalidCapabilities = 0x1A,
	BadRecoveryType = 0x22,
	BadMediaTransportFormat = 0x23,
	BadRecoveryFormat = 0x25,
	BadRohcFormat = 0x26,
	BadCpFormat = 0x27,
	BadMultiplexingFormat = 0x28,
	UnsupportedConfiguration = 0x29,
	BadState = 0x31,
	// A2DP codec-specific errors.
	InvalidCodecType = 0xC1,
	NotSupportedCodecType = 0xC2,
	InvalidSamplingFrequency = 0xC3,
	NotSupportedSamplingFrequency = 0xC4,
	InvalidChannelMode = 0xC5,
	NotSupportedChannelMode = 0xC6,
	InvalidSubbands = 0xC7,
	NotSupportedSubbands = 0xC8,
	InvalidAllocationMethod = 0xC9,
	NotSupportedAllocationMethod = 0xCA,
	InvalidMinimumBitpoolValue = 0xCB,
	NotSupportedMinimumBitpoolValue = 0xCC,
	InvalidMaximumBitpoolValue = 0xCD,
	NotSupportedMaximumBitpoolValue = 0xCE,
	InvalidObjectType = 0xD6,
	NotSupportedObjectType = 0xD7,
	InvalidChannels = 0xD8,
	NotSupportedChannels = 0xD9,
	InvalidBitRate = 0xDC,
	NotSupportedBitRate = 0xDD,
};

// Signaling header (single packet): [label:4|packetType:2|messageType:2][rfa:2|signalId:6]
struct SignalHeader {
	uint8_t transactionLabel;
	PacketType packetType;
	MessageType messageType;
	Signal signal;
};

inline size_t WriteSignalHeader(const SignalHeader& h, uint8_t* out, size_t cap) noexcept
{
	if (cap < 2) {
		return 0;
	}
	out[0] = static_cast<uint8_t>(((h.transactionLabel & 0x0F) << 4) |
								  ((static_cast<uint8_t>(h.packetType) & 0x03) << 2) |
								  (static_cast<uint8_t>(h.messageType) & 0x03));
	out[1] = static_cast<uint8_t>(static_cast<uint8_t>(h.signal) & 0x3F);
	return 2;
}

inline bool ParseSignalHeader(const uint8_t* in, size_t len, SignalHeader& h) noexcept
{
	if (len < 2) {
		return false;
	}
	h.transactionLabel = static_cast<uint8_t>(in[0] >> 4);
	h.packetType = static_cast<PacketType>((in[0] >> 2) & 0x03);
	h.messageType = static_cast<MessageType>(in[0] & 0x03);
	h.signal = static_cast<Signal>(in[1] & 0x3F);
	return h.packetType == PacketType::Single;
}

// Stream End Point info in a DISCOVER response: [seid:6|inUse:1|rfa:1][mediaType:4|tsep:1|rfa:3]
struct SepInfo {
	uint8_t seid;
	bool inUse;
	MediaType mediaType;
	SepType sepType;
};

inline SepInfo ParseSepInfo(const uint8_t in[2]) noexcept
{
	SepInfo s{};
	s.seid = static_cast<uint8_t>(in[0] >> 2);
	s.inUse = (in[0] & 0x02) != 0;
	s.mediaType = static_cast<MediaType>(in[1] >> 4);
	s.sepType = static_cast<SepType>((in[1] >> 3) & 0x01);
	return s;
}

inline void WriteSepInfo(const SepInfo& s, uint8_t out[2]) noexcept
{
	out[0] = static_cast<uint8_t>(((s.seid & 0x3F) << 2) | (s.inUse ? 0x02 : 0x00));
	out[1] = static_cast<uint8_t>(((static_cast<uint8_t>(s.mediaType) & 0x0F) << 4) |
								  ((static_cast<uint8_t>(s.sepType) & 0x01) << 3));
}

// Locates the Media Codec capability inside a capabilities/configuration payload.
// On success returns pointer to the codec-specific IE (after media type and codec type bytes).
inline const uint8_t* FindMediaCodec(const uint8_t* caps, size_t len, CodecType& codec, size_t& ieLen) noexcept
{
	size_t pos = 0;
	while (pos + 2 <= len) {
		const auto category = static_cast<ServiceCategory>(caps[pos]);
		const size_t losc = caps[pos + 1];
		if (pos + 2 + losc > len) {
			return nullptr;
		}
		if (category == ServiceCategory::MediaCodec && losc >= 2) {
			codec = static_cast<CodecType>(caps[pos + 3]);
			ieLen = losc - 2;
			return caps + pos + 4;
		}
		pos += 2 + losc;
	}
	return nullptr;
}

} // namespace OpenA2dp::Avdtp
