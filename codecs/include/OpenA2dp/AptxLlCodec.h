#pragma once

#include <OpenA2dp/AptxLl.h>
#include <OpenA2dp/IA2dpCodec.h>

namespace OpenA2dp {

// aptX Low Latency: the classic aptX bitstream under the CSR vendor identity, with the buffer
// levels its IE carries. The encoder is the vendored classic one, because the codewords are
// the same; only the configuration and the A2DP identity differ.
class AptxLlCodec final : public IA2dpCodec {
public:
	CodecType Type() const noexcept override { return CodecType::Vendor; }
	const char* Name() const noexcept override { return "aptX LL"; }

	size_t BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept override;
	bool SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
							 const CodecPreference& preference, A2dpCodecConfig& out) const noexcept override;
	uint8_t ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept override;
	HRESULT CreateEncoder(const A2dpCodecConfig& config, std::unique_ptr<IA2dpEncoder>& encoder) const override;
};

} // namespace OpenA2dp
