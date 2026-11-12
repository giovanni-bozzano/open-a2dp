#pragma once

#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/Ldac.h>

namespace OpenA2dp {

// Sony LDAC through the vendored AOSP encoder (`ldacBT_*`). Encoding is done in user mode by
// OpenA2dpSvc.exe, like AAC and the aptX codecs, so the driver only advertises and negotiates
// the codec. Each call takes 128 interleaved 16-bit stereo samples per channel; the encoder
// packs transport frames into a whole media packet sized by the L2CAP MTU, so most calls only
// buffer their samples and every second or third call returns a packet's worth of frames
// (about 5.8 ms at 44.1 kHz, high quality). Its output is the media payload as it goes on the
// wire, frame headers included, so nothing is added around it.
//
// The quality mode is the encoder's EQMID: high (990 kbps), standard (660 kbps) or mobile
// (330 kbps). It is not in the A2DP IE, so it is a local setting and both ends stay in step
// without negotiating it.

class LdacCodec final : public IA2dpCodec {
public:
	explicit LdacCodec(uint8_t quality = Ldac::kQualityHigh) noexcept : quality_(quality) {}

	CodecType Type() const noexcept override { return CodecType::Vendor; }
	const char* Name() const noexcept override { return "LDAC"; }

	uint8_t Quality() const noexcept { return quality_; }

	size_t BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept override;
	bool SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
							 const CodecPreference& preference, A2dpCodecConfig& out) const noexcept override;
	uint8_t ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept override;
	HRESULT CreateEncoder(const A2dpCodecConfig& config,
						  std::unique_ptr<IA2dpEncoder>& encoder) const override;

private:
	uint8_t quality_;
};

} // namespace OpenA2dp
