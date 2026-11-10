#pragma once

#include <OpenA2dp/Aptx.h>
#include <OpenA2dp/IA2dpCodec.h>

namespace OpenA2dp {

// Classic aptX, the AOSP vendor codec: 16-bit input, four samples per channel per call and
// four encoded bytes (a 16-bit codeword per channel) with no codec-specific payload header.
// The identifiers and IE helpers are in Aptx.h, which the driver shares.

class AptxCodec final : public IA2dpCodec {
public:
	CodecType Type() const noexcept override { return CodecType::Vendor; }
	const char* Name() const noexcept override { return "aptX"; }

	size_t BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept override;
	bool SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
							 const CodecPreference& preference, A2dpCodecConfig& out) const noexcept override;
	uint8_t ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept override;
	HRESULT CreateEncoder(const A2dpCodecConfig& config, std::unique_ptr<IA2dpEncoder>& encoder) const override;
};

} // namespace OpenA2dp
