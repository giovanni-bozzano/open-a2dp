#pragma once

#include <OpenA2dp/IA2dpCodec.h>

namespace OpenA2dp {

namespace Aac {

constexpr size_t kIeLength = 6;

// Object type (byte 0).
constexpr uint8_t kObjectMpeg2Lc = 0x80;
constexpr uint8_t kObjectMpeg4Lc = 0x40;
constexpr uint8_t kObjectMpeg4Ltp = 0x20;
constexpr uint8_t kObjectMpeg4Scalable = 0x10;

// Channels (byte 2, bits 3..2).
constexpr uint8_t kChannels1 = 0x08;
constexpr uint8_t kChannels2 = 0x04;

constexpr uint32_t kSamplesPerFrame = 1024;

// Bitrates supported by the Media Foundation AAC encoder.
constexpr uint32_t kSupportedBitrates[] = {96000, 128000, 160000, 192000};

struct Params {
	uint8_t objectType = kObjectMpeg2Lc;
	uint32_t sampleRate = 44100;
	uint8_t channels = 2;
	bool vbr = false;
	uint32_t bitrate = 192000; // bits per second, 0 = unspecified
};

// 12-bit sampling frequency field (byte1 + high nibble of byte2), bit 11 = 8000 Hz.
uint16_t SampleRateMask(uint32_t sampleRate) noexcept;
uint16_t ReadSampleRateField(const uint8_t* ie) noexcept;

// MPEG-4 samplingFrequencyIndex, or -1.
int SamplingFrequencyIndex(uint32_t sampleRate) noexcept;

void WriteIe(const Params& p, uint8_t* ie) noexcept;
bool ParseConfiguredIe(const uint8_t* ie, size_t len, Params& out) noexcept;

} // namespace Aac

class AacCodec final : public IA2dpCodec {
public:
	CodecType Type() const noexcept override { return CodecType::Aac; }
	const char* Name() const noexcept override { return "AAC"; }

	size_t BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept override;
	bool SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
							 const CodecPreference& preference, A2dpCodecConfig& out) const noexcept override;
	uint8_t ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept override;

	// Requires COM (MTA recommended) to be initialized on the calling thread.
	HRESULT CreateEncoder(const A2dpCodecConfig& config, std::unique_ptr<IA2dpEncoder>& encoder) const override;
};

class AacEncoder final : public IA2dpEncoder {
public:
	explicit AacEncoder(const Aac::Params& params) noexcept;
	~AacEncoder() override;

	AacEncoder(const AacEncoder&) = delete;
	AacEncoder& operator=(const AacEncoder&) = delete;

	HRESULT Initialize() noexcept;

	const Aac::Params& Params() const noexcept { return params_; }

	PcmFormat InputFormat() const noexcept override;
	uint32_t SamplesPerFrame() const noexcept override;
	size_t MaxFrameBytes() const noexcept override;
	HRESULT Encode(const int16_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept override;
	size_t FramesPerPacket(size_t payloadCapacity) const noexcept override;
	size_t BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount,
							 uint8_t* out, size_t capacity) noexcept override;

private:
	struct Impl;
	Aac::Params params_;
	std::unique_ptr<Impl> impl_;
};

} // namespace OpenA2dp
