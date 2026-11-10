#pragma once

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/IA2dpCodec.h>

namespace OpenA2dp {

namespace Aac {

using AacConfig::kIeLength;
using AacConfig::kObjectMpeg2Lc;
using AacConfig::kObjectMpeg4Lc;
using AacConfig::kChannels1;
using AacConfig::kChannels2;

using AacConfig::kSamplesPerFrame;

using AacConfig::kSupportedBitrates;
using AacConfig::Params;

// 12-bit sampling frequency field (byte1 + high nibble of byte2), bit 11 = 8000 Hz.
using AacConfig::SampleRateMask;
using AacConfig::ReadSampleRateField;

// MPEG-4 samplingFrequencyIndex, or -1.
int SamplingFrequencyIndex(uint32_t sampleRate) noexcept;

using AacConfig::WriteIe;
using AacConfig::ParseConfiguredIe;

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
	HRESULT Encode(const uint8_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept override;
	size_t FramesPerPacket(size_t payloadCapacity) const noexcept override;
	size_t BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount,
							 uint8_t* out, size_t capacity) noexcept override;

private:
	struct Impl;
	Aac::Params params_;
	std::unique_ptr<Impl> impl_;
};

} // namespace OpenA2dp
