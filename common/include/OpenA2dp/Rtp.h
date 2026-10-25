#pragma once

// RTP header used on the AVDTP media channel (RFC 3550, no CSRC, no extension).
// Kernel-safe: no STL, no exceptions, no allocation.

#include <stdint.h>
#include <stddef.h>

namespace OpenA2dp::Rtp {

constexpr size_t kHeaderSize = 12;
constexpr uint8_t kDynamicPayloadType = 96;

struct Header {
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc;
	uint8_t payloadType;
	bool marker;
};

inline size_t Write(const Header& h, uint8_t* out, size_t cap) noexcept
{
	if (cap < kHeaderSize) {
		return 0;
	}
	out[0] = 0x80; // V=2, P=0, X=0, CC=0
	out[1] = static_cast<uint8_t>((h.marker ? 0x80 : 0x00) | (h.payloadType & 0x7F));
	out[2] = static_cast<uint8_t>(h.sequence >> 8);
	out[3] = static_cast<uint8_t>(h.sequence);
	out[4] = static_cast<uint8_t>(h.timestamp >> 24);
	out[5] = static_cast<uint8_t>(h.timestamp >> 16);
	out[6] = static_cast<uint8_t>(h.timestamp >> 8);
	out[7] = static_cast<uint8_t>(h.timestamp);
	out[8] = static_cast<uint8_t>(h.ssrc >> 24);
	out[9] = static_cast<uint8_t>(h.ssrc >> 16);
	out[10] = static_cast<uint8_t>(h.ssrc >> 8);
	out[11] = static_cast<uint8_t>(h.ssrc);
	return kHeaderSize;
}

inline bool Parse(const uint8_t* in, size_t len, Header& h) noexcept
{
	if (len < kHeaderSize || (in[0] & 0xC0) != 0x80 || (in[0] & 0x0F) != 0) {
		return false;
	}
	h.marker = (in[1] & 0x80) != 0;
	h.payloadType = static_cast<uint8_t>(in[1] & 0x7F);
	h.sequence = static_cast<uint16_t>((in[2] << 8) | in[3]);
	h.timestamp = (static_cast<uint32_t>(in[4]) << 24) | (static_cast<uint32_t>(in[5]) << 16) |
				  (static_cast<uint32_t>(in[6]) << 8) | in[7];
	h.ssrc = (static_cast<uint32_t>(in[8]) << 24) | (static_cast<uint32_t>(in[9]) << 16) |
			 (static_cast<uint32_t>(in[10]) << 8) | in[11];
	return true;
}

} // namespace OpenA2dp::Rtp
