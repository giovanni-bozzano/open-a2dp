#pragma once

#include <OpenA2dp/IA2dpCodec.h>

namespace OpenA2dp {

// aptX HD, the AOSP vendor codec: 24-bit input, four samples per channel per call and six
// encoded bytes (three per channel) with no codec-specific payload header.
//
// The IE is the codec-specific part of AVDTP's media codec capabilities, i.e. the bytes
// after the media type and codec type: the vendor and codec IDs (little endian, as A2DP
// writes them), one byte of sample-rate and channel-mode bits, and four reserved bytes.
namespace AptxHd {

constexpr uint32_t kVendorId = 0x000000D7;
constexpr uint16_t kCodecId = 0x0024;
constexpr size_t kIeLength = 11;

constexpr uint8_t kSampleRate44100 = 0x20;
constexpr uint8_t kSampleRate48000 = 0x10;
constexpr uint8_t kSampleRateMask = kSampleRate44100 | kSampleRate48000;
constexpr uint8_t kChannelMono = 0x01;
constexpr uint8_t kChannelStereo = 0x02;
constexpr uint8_t kChannelMask = 0x0F;

constexpr uint32_t kSamplesPerFrame = 4;
constexpr size_t kFrameBytes = 6;
// 24-in-32: one sample per 4 bytes, valid bits left-justified, low byte zero.
constexpr uint16_t kBitsPerSample = 24;

} // namespace AptxHd

class AptxHdCodec final : public IA2dpCodec {
public:
	CodecType Type() const noexcept override { return CodecType::Vendor; }
	const char* Name() const noexcept override { return "aptX HD"; }

	size_t BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept override;
	bool SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
							 const CodecPreference& preference, A2dpCodecConfig& out) const noexcept override;
	uint8_t ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept override;
	HRESULT CreateEncoder(const A2dpCodecConfig& config, std::unique_ptr<IA2dpEncoder>& encoder) const override;
};

} // namespace OpenA2dp
