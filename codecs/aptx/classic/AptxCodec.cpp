#include <OpenA2dp/AptxCodec.h>

#include "aptXbtenc.h"

#include <cstring>
#include <vector>

namespace OpenA2dp {

using namespace Aptx;

namespace {

// aptX is a stereo codec, so the encoder is as well; mono sinks and streams fall back to a
// codec that supports them.
class AptxEncoder final : public IA2dpEncoder {
public:
	explicit AptxEncoder(uint32_t sampleRate) noexcept : sampleRate_(sampleRate)
	{
		const int stateBytes = SizeofAptxbtenc();
		if (stateBytes > 0) {
			state_.resize((static_cast<size_t>(stateBytes) + sizeof(uint32_t) - 1) / sizeof(uint32_t));
			initialised_ = aptxbtenc_init(state_.data(), 0) == 0;
		}
	}

	PcmFormat InputFormat() const noexcept override
	{
		return PcmFormat{sampleRate_, 2, kBitsPerSample};
	}
	uint32_t SamplesPerFrame() const noexcept override { return kSamplesPerFrame; }
	size_t MaxFrameBytes() const noexcept override { return kFrameBytes; }

	HRESULT Encode(const uint8_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept override
	{
		written = 0;
		if (!initialised_ || pcm == nullptr || out == nullptr || capacity < kFrameBytes) {
			return E_INVALIDARG;
		}
		// The encoder takes four int32 samples per channel and keeps the low 16 bits, so the
		// endpoint's int16 samples are widened without scaling.
		const auto* samples = reinterpret_cast<const int16_t*>(pcm);
		int32_t left[kSamplesPerFrame] = {};
		int32_t right[kSamplesPerFrame] = {};
		for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
			left[i] = samples[2 * i];
			right[i] = samples[2 * i + 1];
		}
		int16_t encoded[2] = {};
		if (aptxbtenc_encodestereo(state_.data(), left, right, encoded) != 0) {
			return E_FAIL;
		}
		// One codeword per channel, most significant byte first, as the A2DP vendor media
		// payload carries them.
		out[0] = static_cast<uint8_t>(static_cast<uint16_t>(encoded[0]) >> 8);
		out[1] = static_cast<uint8_t>(encoded[0]);
		out[2] = static_cast<uint8_t>(static_cast<uint16_t>(encoded[1]) >> 8);
		out[3] = static_cast<uint8_t>(encoded[1]);
		written = kFrameBytes;
		return S_OK;
	}

	size_t FramesPerPacket(size_t payloadCapacity) const noexcept override
	{
		return payloadCapacity / kFrameBytes;
	}

	size_t BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount,
							 uint8_t* out, size_t capacity) noexcept override
	{
		// No codec-specific payload header: the frames follow the RTP header back to back.
		if (frameCount != 0 && (frames == nullptr || frameSizes == nullptr)) {
			return 0;
		}
		size_t total = 0;
		size_t offset = 0;
		for (size_t i = 0; i < frameCount; ++i) {
			if (out == nullptr || frameSizes[i] > capacity - total) {
				return 0;
			}
			std::memcpy(out + total, frames + offset, frameSizes[i]);
			total += frameSizes[i];
			offset += frameSizes[i];
		}
		return total;
	}

private:
	uint32_t sampleRate_;
	// The vendored state needs 4-byte alignment, which uint32_t storage guarantees.
	std::vector<uint32_t> state_;
	bool initialised_ = false;
};

} // namespace

size_t AptxCodec::BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept
{
	if (ie == nullptr || capacity < kIeLength)
	{
		return 0;
	}
	// Both rates and stereo: classic aptX has no depth field, so 16 bits is the codec.
	WriteIe(static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000), ie);
	return kIeLength;
}

bool AptxCodec::SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
									const CodecPreference& preference, A2dpCodecConfig& out) const noexcept
{
	uint8_t capabilities = 0;
	if (!ParseIe(remoteIe, remoteIeLength, capabilities)) {
		return false;
	}
	if ((capabilities & kChannelStereo) == 0) {
		return false;
	}
	const uint8_t rates = static_cast<uint8_t>(capabilities & kSampleRateMask);
	uint8_t rate = 0;
	if (preference.preferredSampleRate == 44100 && (rates & kSampleRate44100) != 0) {
		rate = kSampleRate44100;
	} else if (preference.preferredSampleRate == 48000 && (rates & kSampleRate48000) != 0) {
		rate = kSampleRate48000;
	} else if ((rates & kSampleRate48000) != 0) {
		rate = kSampleRate48000;
	} else if ((rates & kSampleRate44100) != 0) {
		rate = kSampleRate44100;
	} else {
		return false;
	}
	out = {};
	out.codec = CodecType::Vendor;
	WriteIe(rate, out.ie);
	out.ieLength = static_cast<uint8_t>(kIeLength);
	return true;
}

uint8_t AptxCodec::ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept
{
	return ValidateIe(ie, ieLength);
}

HRESULT AptxCodec::CreateEncoder(const A2dpCodecConfig& config,
								 std::unique_ptr<IA2dpEncoder>& encoder) const
{
	if (config.codec != CodecType::Vendor || ValidateConfiguration(config.ie, config.ieLength) != 0) {
		return E_INVALIDARG;
	}
	const uint8_t rate = static_cast<uint8_t>(config.ie[6] & kSampleRateMask);
	encoder = std::make_unique<AptxEncoder>(rate == kSampleRate44100 ? 44100u : 48000u);
	return S_OK;
}

} // namespace OpenA2dp
