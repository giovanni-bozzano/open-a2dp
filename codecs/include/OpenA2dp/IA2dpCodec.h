#pragma once

// Pluggable A2DP codec interface (user mode).
// A codec knows how to describe its capabilities, negotiate a configuration
// with a remote sink and create an encoder for the selected configuration.

#include <OpenA2dp/Avdtp.h>

#include <windows.h>

#include <memory>
#include <stdint.h>
#include <stddef.h>

namespace OpenA2dp {

using Avdtp::CodecType;

constexpr size_t kMaxCodecIeLength = 32;

struct A2dpCodecConfig {
	CodecType codec = CodecType::Sbc;
	uint8_t ie[kMaxCodecIeLength] = {};
	uint8_t ieLength = 0;
};

struct CodecPreference {
	bool hasForcedCodec = false;
	CodecType forcedCodec = CodecType::Sbc;
	// 0 = no preference, otherwise 44100 / 48000.
	uint32_t preferredSampleRate = 0;
	// SBC: upper bitpool limit we request (sink max still applies).
	uint8_t sbcMaxBitpool = 53;
	// SBC: 0 = no preference, otherwise 4, 8, 12 or 16 blocks per frame.
	uint8_t sbcBlocks = 0;
	// SBC: 0 = no preference, otherwise 4 or 8 subbands.
	uint8_t sbcSubbands = 0;
	// SBC: 0 = no preference, otherwise SbcConfig::kAllocationLoudness or kAllocationSnr.
	uint8_t sbcAllocation = 0;
	// AAC: target bitrate in bits per second (sink max still applies).
	uint32_t aacBitrate = 256000;
};

struct PcmFormat {
	uint32_t sampleRate = 0;
	uint16_t channels = 0;
};

class IA2dpEncoder {
public:
	virtual ~IA2dpEncoder() = default;

	virtual PcmFormat InputFormat() const noexcept = 0;

	// Number of PCM samples per channel consumed by one Encode() call.
	virtual uint32_t SamplesPerFrame() const noexcept = 0;

	// Upper bound of the encoded size of one frame.
	virtual size_t MaxFrameBytes() const noexcept = 0;

	// Encodes exactly SamplesPerFrame() interleaved 16-bit samples per channel.
	// `written` may be 0 when the encoder is still priming (AAC).
	virtual HRESULT Encode(const int16_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept = 0;

	// How many encoded frames fit into one media packet whose payload
	// (after the RTP header) is limited to `payloadCapacity` bytes.
	virtual size_t FramesPerPacket(size_t payloadCapacity) const noexcept = 0;

	// Builds the media payload that follows the RTP header.
	// `frames` holds `frameCount` encoded frames back to back with sizes in `frameSizes`.
	// Returns bytes written, or 0 on failure.
	virtual size_t BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount,
									 uint8_t* out, size_t capacity) noexcept = 0;
};

class IA2dpCodec {
public:
	virtual ~IA2dpCodec() = default;

	virtual CodecType Type() const noexcept = 0;
	virtual const char* Name() const noexcept = 0;

	// Codec-specific information element advertised by our source SEP.
	virtual size_t BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept = 0;

	// INT role: choose a configuration compatible with the sink's capabilities.
	virtual bool SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
									 const CodecPreference& preference, A2dpCodecConfig& out) const noexcept = 0;

	// ACP role: validate a configuration proposed by the sink.
	// Returns 0 if acceptable, otherwise an AVDTP/A2DP error code.
	virtual uint8_t ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept = 0;

	virtual HRESULT CreateEncoder(const A2dpCodecConfig& config, std::unique_ptr<IA2dpEncoder>& encoder) const = 0;
};

std::unique_ptr<IA2dpCodec> CreateCodec(CodecType type);

// Codecs in default priority order (highest first).
size_t GetSupportedCodecs(CodecType* types, size_t capacity) noexcept;

// Picks the best codec/configuration given every codec capability the sink offered.
bool NegotiateConfiguration(const A2dpCodecConfig* remoteCapabilities, size_t remoteCount,
							const CodecPreference& preference, A2dpCodecConfig& selected);

} // namespace OpenA2dp
