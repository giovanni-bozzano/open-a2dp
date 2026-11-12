#include <OpenA2dp/LdacCodec.h>

#include <ldacBT.h>

#include <cstring>
#include <vector>

namespace OpenA2dp {

using namespace Ldac;

namespace {

// LDAC frames are variable length and the encoder decides each one, so the payload carries the
// bytes it returned unchanged: the transport frame already holds what the sink needs.
class LdacEncoder final : public IA2dpEncoder {
public:
	LdacEncoder(uint32_t sampleRate, uint8_t quality) noexcept
		: sampleRate_(sampleRate), quality_(quality)
	{
		handle_ = ldacBT_get_handle();
		initialised_ = handle_ != nullptr && Init();
	}

	~LdacEncoder() override
	{
		if (handle_ != nullptr) {
			ldacBT_close_handle(handle_);
			ldacBT_free_handle(handle_);
		}
	}

	LdacEncoder(const LdacEncoder&) = delete;
	LdacEncoder& operator=(const LdacEncoder&) = delete;

	PcmFormat InputFormat() const noexcept override
	{
		return PcmFormat{sampleRate_, 2, kBitsPerSample};
	}
	uint32_t SamplesPerFrame() const noexcept override { return kSamplesPerFrame; }
	size_t MaxFrameBytes() const noexcept override { return kMaxFrameBytes; }

	HRESULT Encode(const uint8_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept override
	{
		written = 0;
		if (!initialised_ || pcm == nullptr || out == nullptr || capacity < kMaxFrameBytes) {
			return E_INVALIDARG;
		}			// The encoder consumes its input unit as bytes of PCM; one unit is 128 interleaved
			// 16-bit stereo samples.
		int pcmUsed = static_cast<int>(kSamplesPerFrame * 2 * sizeof(int16_t));
		int streamBytes = 0;
		int frames = 0;
		const int result = ldacBT_encode(handle_, const_cast<uint8_t*>(pcm), &pcmUsed, buffer_.data(),
										 &streamBytes, &frames);
		if (result != 0 || streamBytes < 0 || static_cast<size_t>(streamBytes) > kMaxFrameBytes) {
			// The library documents that continuing after an error means closing the handle and
			// initialising it again; the frames it was carrying are lost either way.
			++errors_;
			initialised_ = Reopen();
			return E_FAIL;
		}
		// Nothing to hand back yet: the encoder is buffering frames until a media packet is
		// full. The samples were consumed, which is why the caller counts them anyway.
		if (streamBytes == 0 || frames == 0 || frames > 4) {
			return S_OK;
		}
		std::memcpy(out, buffer_.data(), static_cast<size_t>(streamBytes));
		written = static_cast<size_t>(streamBytes);
		return S_OK;
	}

	size_t FramesPerPacket(size_t payloadCapacity) const noexcept override
	{
		// One encode output is one media packet, whatever it holds. A link too small for LDAC's
		// 2-DH5 MTU is reported as no capacity at all, which keeps the stream encoder from
		// starting on it.
		return payloadCapacity >= kMinMtuBytes ? 1 : 0;
	}

	size_t BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount,
							 uint8_t* out, size_t capacity) noexcept override
	{
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

	uint64_t ErrorCount() const noexcept { return errors_; }

private:
	bool Init() noexcept
	{
		const int cm = LDAC_CCI_STEREO;
		return ldacBT_init_handle_encode(handle_, static_cast<int>(kMinMtuBytes),
										static_cast<int>(quality_), cm, LDACBT_SMPL_FMT_S16,
										static_cast<int>(sampleRate_)) == 0;
	}

	// After an error the handle is closed and initialised again, as the encoder documents; a
	// failure here leaves the stream without an encoder and Encode() fails for good.
	bool Reopen() noexcept
	{
		ldacBT_close_handle(handle_);
		return Init();
	}

	HANDLE_LDAC_BT handle_ = nullptr;
	uint32_t sampleRate_;
	uint8_t quality_;
	bool initialised_ = false;
	uint64_t errors_ = 0;
	std::vector<uint8_t> buffer_ = std::vector<uint8_t>(kMaxFrameBytes);
};

} // namespace

size_t LdacCodec::BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept
{
	if (ie == nullptr || capacity < kIeLength) {
		return 0;
	}
	// Every rate the encoder takes; the endpoint offers each of them as well, so any bit here
	// can be negotiated and fed.
	WriteIe(static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000 | kSampleRate88200 | kSampleRate96000),
		kChannelStereo, ie);
	return kIeLength;
}

bool LdacCodec::SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
									const CodecPreference& preference,
									A2dpCodecConfig& out) const noexcept
{
	uint8_t rates = 0;
	uint8_t modes = 0;
	if (!ParseIe(remoteIe, remoteIeLength, rates, modes)) {
		return false;
	}
	if ((modes & kChannelStereo) == 0) {
		return false;
	}
	// The endpoint's rate when the sink offers it, else the sink's own preference: 48 kHz ahead
	// of 44.1, and the high rates only after those two.
	uint8_t rate = 0;
	const uint8_t preferred = RateBit(preference.preferredSampleRate);
	if (preferred != 0 && (rates & preferred) != 0) {
		rate = preferred;
	} else {
		for (const uint8_t candidate :
				{static_cast<uint8_t>(kSampleRate48000), static_cast<uint8_t>(kSampleRate44100),
				 static_cast<uint8_t>(kSampleRate96000), static_cast<uint8_t>(kSampleRate88200)}) {
			if ((rates & candidate) != 0) {
				rate = candidate;
				break;
			}
		}
	}
	if (rate == 0) {
		return false;
	}
	out = {};
	out.codec = CodecType::Vendor;
	WriteIe(rate, kChannelStereo, out.ie);
	out.ieLength = static_cast<uint8_t>(kIeLength);
	return true;
}

uint8_t LdacCodec::ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept
{
	return ValidateIe(ie, ieLength);
}

HRESULT LdacCodec::CreateEncoder(const A2dpCodecConfig& config,
								 std::unique_ptr<IA2dpEncoder>& encoder) const
{
	if (config.codec != CodecType::Vendor || ValidateConfiguration(config.ie, config.ieLength) != 0) {
		return E_INVALIDARG;
	}
	const uint32_t sampleRate = RateValue(static_cast<uint8_t>(config.ie[6] & kSampleRateMask));
	encoder = std::make_unique<LdacEncoder>(sampleRate, quality_);
	return S_OK;
}

} // namespace OpenA2dp
