#include <OpenA2dp/AptxHdCodec.h>

#include "aptXHDbtenc.h"

#include <cstring>
#include <vector>

namespace OpenA2dp {

using namespace AptxHd;

namespace {

bool SingleBit(uint8_t value) noexcept
{
	return value != 0 && (value & (value - 1)) == 0;
}

// Reads the codec-specific IE and reports its sample-rate/channel-mode byte.
bool ParseIe(const uint8_t* ie, size_t length, uint8_t& capabilities) noexcept
{
	if (ie == nullptr || length < kIeLength) {
		return false;
	}
	const uint32_t vendorId = static_cast<uint32_t>(ie[0]) | (static_cast<uint32_t>(ie[1]) << 8) |
							  (static_cast<uint32_t>(ie[2]) << 16) | (static_cast<uint32_t>(ie[3]) << 24);
	const uint16_t codecId = static_cast<uint16_t>(ie[4] | (ie[5] << 8));
	if (vendorId != kVendorId || codecId != kCodecId) {
		return false;
	}
	capabilities = ie[6];
	return true;
}

void BuildIe(uint8_t sampleRateBits, uint8_t* ie) noexcept
{
	ie[0] = static_cast<uint8_t>(kVendorId & 0xFF);
	ie[1] = static_cast<uint8_t>((kVendorId >> 8) & 0xFF);
	ie[2] = static_cast<uint8_t>((kVendorId >> 16) & 0xFF);
	ie[3] = static_cast<uint8_t>((kVendorId >> 24) & 0xFF);
	ie[4] = static_cast<uint8_t>(kCodecId & 0xFF);
	ie[5] = static_cast<uint8_t>((kCodecId >> 8) & 0xFF);
	ie[6] = static_cast<uint8_t>(sampleRateBits | kChannelStereo);
	for (size_t i = 7; i < kIeLength; ++i) {
		ie[i] = 0;
	}
}

// aptX HD is a stereo codec, so the encoder is as well; mono sinks and streams fall back to
// a codec that supports them.
class AptxHdEncoder final : public IA2dpEncoder {
public:
	explicit AptxHdEncoder(uint32_t sampleRate) noexcept : sampleRate_(sampleRate)
	{
		const int stateBytes = SizeofAptxhdbtenc();
		if (stateBytes > 0) {
			state_.resize((static_cast<size_t>(stateBytes) + sizeof(uint32_t) - 1) / sizeof(uint32_t));
			initialised_ = aptxhdbtenc_init(state_.data(), 0) == 0;
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
		// 24-in-32 is left-justified, so shifting right by 8 yields the 24-bit value the
		// encoder takes, sign-extended in an int32 (AOSP's packed 24-bit expansion does the
		// same in two steps).
		const auto* samples = reinterpret_cast<const int32_t*>(pcm);
		int32_t left[kSamplesPerFrame] = {};
		int32_t right[kSamplesPerFrame] = {};
		for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
			left[i] = samples[2 * i] >> 8;
			right[i] = samples[2 * i + 1] >> 8;
		}
		uint32_t encoded[2] = {};
		if (aptxhdbtenc_encodestereo(state_.data(), left, right, encoded) != 0) {
			return E_FAIL;
		}
		// Two codewords, three bytes each, most significant byte first, as the A2DP vendor
		// media payload carries them.
		out[0] = static_cast<uint8_t>(encoded[0] >> 16);
		out[1] = static_cast<uint8_t>(encoded[0] >> 8);
		out[2] = static_cast<uint8_t>(encoded[0]);
		out[3] = static_cast<uint8_t>(encoded[1] >> 16);
		out[4] = static_cast<uint8_t>(encoded[1] >> 8);
		out[5] = static_cast<uint8_t>(encoded[1]);
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

size_t AptxHdCodec::BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept
{
	if (ie == nullptr || capacity < kIeLength) {
		return 0;
	}
	// Both rates and stereo, the configuration the codec is meant to run at. HD's value over
	// classic aptX is the 24-bit depth, which the IE does not carry.
	BuildIe(static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000), ie);
	return kIeLength;
}

bool AptxHdCodec::SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
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
	BuildIe(rate, out.ie);
	out.ieLength = static_cast<uint8_t>(kIeLength);
	return true;
}

uint8_t AptxHdCodec::ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept
{
	if (ieLength < kIeLength) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::BadLength);
	}
	uint8_t capabilities = 0;
	if (!ParseIe(ie, ieLength, capabilities)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidCodecType);
	}
	const uint8_t rates = static_cast<uint8_t>(capabilities & kSampleRateMask);
	if (!SingleBit(rates)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency);
	}
	if (rates != kSampleRate44100 && rates != kSampleRate48000) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
	}
	const uint8_t modes = static_cast<uint8_t>(capabilities & kChannelMask);
	if (!SingleBit(modes)) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::InvalidChannelMode);
	}
	if (modes != kChannelStereo) {
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannelMode);
	}
	return 0;
}

HRESULT AptxHdCodec::CreateEncoder(const A2dpCodecConfig& config,
								   std::unique_ptr<IA2dpEncoder>& encoder) const
{
	if (config.codec != CodecType::Vendor || ValidateConfiguration(config.ie, config.ieLength) != 0) {
		return E_INVALIDARG;
	}
	const uint8_t rate = static_cast<uint8_t>(config.ie[6] & kSampleRateMask);
	encoder = std::make_unique<AptxHdEncoder>(rate == kSampleRate44100 ? 44100u : 48000u);
	return S_OK;
}

} // namespace OpenA2dp
