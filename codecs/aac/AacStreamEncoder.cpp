#include <OpenA2dp/AacStreamEncoder.h>

#include <OpenA2dp/FrameRecord.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace OpenA2dp {

namespace {

uint64_t PrerollSamples(const AacStreamParams& params) noexcept
{
	return static_cast<uint64_t>(params.sampleRate) * kAacPrerollMs / 1000;
}

} // namespace

AacStreamEncoder::~AacStreamEncoder() = default;

bool AacStreamEncoder::Start(std::unique_ptr<IA2dpEncoder> encoder, const AacStreamParams& params,
							 RingBufferView& pcm, uint64_t startTimeUs) noexcept
{
	Stop();
	if (encoder == nullptr) {
		return false;
	}

	const PcmFormat format = encoder->InputFormat();
	const uint32_t samplesPerFrame = encoder->SamplesPerFrame();
	const size_t maxFrameBytes = encoder->MaxFrameBytes();
	// FrameRecord::Read rejects a record whose sample count is implausible.
	// MaxFrameBytes() is the encoder's worst-case output capacity, not the size of
	// the next frame: a real AAC encoder reports the ISO 14496-3 bound (768 bytes
	// per channel), which is larger than the media payload. Actual payload size is
	// enforced by BuildMediaPayload in EncodeBlock, so only reject a zero capacity.
	if (format.sampleRate != params.sampleRate || format.channels != params.channels ||
		samplesPerFrame == 0 || samplesPerFrame > 4096 || maxFrameBytes == 0) {
		return false;
	}

	const uint32_t bytesPerSample = static_cast<uint32_t>(params.channels) * 2u;
	try {
		pcmBlock_.assign(static_cast<size_t>(samplesPerFrame) * bytesPerSample, 0);
		accessUnit_.assign(maxFrameBytes, 0);
		payload_.assign(Ioctl::kMaxFramePayload, 0);
	} catch (const std::bad_alloc&) {
		return false;
	}

	encoder_ = std::move(encoder);
	params_ = params;
	stats_ = {};
	startTimeUs_ = startTimeUs;
	producedSamples_ = 0;
	samplesPerFrame_ = samplesPerFrame;
	bytesPerSample_ = bytesPerSample;
	// The PCM the endpoint queued before MediaStart predates this stream.
	const uint32_t stale = pcm.ReadableBytes();
	if (stale != 0) {
		pcm.Skip(stale);
	}
	discontinuity_ = true;
	active_ = true;
	return true;
}

void AacStreamEncoder::Stop() noexcept
{
	active_ = false;
	encoder_.reset();
	producedSamples_ = 0;
	discontinuity_ = false;
}

uint64_t AacStreamEncoder::TargetSamples(uint64_t nowUs) const noexcept
{
	const uint64_t elapsedUs = nowUs > startTimeUs_ ? nowUs - startTimeUs_ : 0;
	return elapsedUs * params_.sampleRate / 1000000ull + PrerollSamples(params_);
}

bool AacStreamEncoder::Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames,
							size_t maxRecords) noexcept
{
	if (!active_) {
		return true;
	}

	const uint64_t target = TargetSamples(nowUs);
	for (size_t i = 0; i < maxRecords && producedSamples_ < target; ++i) {
		const uint64_t due = target - producedSamples_;
		const uint32_t dueSamples = static_cast<uint32_t>(std::min<uint64_t>(due, samplesPerFrame_));
		if (!EncodeBlock(pcm, frames, dueSamples)) {
			return false;
		}
	}
	return true;
}

bool AacStreamEncoder::EncodeBlock(RingBufferView& pcm, RingBufferView& frames,
								   uint32_t dueSamples) noexcept
{
	// A whole 1024-sample block is always encoded; a short block is zero-filled,
	// so a late or empty PCM ring becomes silence instead of a stalled stream.
	std::memset(pcmBlock_.data(), 0, pcmBlock_.size());
	const uint32_t wanted = dueSamples * bytesPerSample_;
	uint32_t available = std::min(pcm.ReadableBytes(), wanted);
	available -= available % bytesPerSample_;
	if (available != 0 && !pcm.Read(pcmBlock_.data(), available)) {
		available = 0;
	}

	const uint32_t availableSamples = available / bytesPerSample_;
	stats_.pcmBytes += available;
	stats_.silenceSamples += samplesPerFrame_ - availableSamples;

	size_t written = 0;
	const HRESULT hr = encoder_->Encode(reinterpret_cast<const int16_t*>(pcmBlock_.data()),
										accessUnit_.data(), accessUnit_.size(), written);
	if (FAILED(hr) || written > accessUnit_.size()) {
		++stats_.encodeErrors;
		return false;
	}
	if (written == 0) {
		// The encoder is priming: the PCM is consumed, but there is no frame yet.
		++stats_.primed;
		producedSamples_ += samplesPerFrame_;
		return true;
	}

	const size_t frameSizes[1] = {written};
	const size_t payload =
		encoder_->BuildMediaPayload(accessUnit_.data(), frameSizes, 1, payload_.data(), payload_.size());
	if (payload == 0) {
		++stats_.encodeErrors;
		return false;
	}

	const uint32_t flags = discontinuity_ ? Ioctl::kFrameFlagDiscontinuity : 0;
	if (FrameRecord::Write(frames, payload_.data(), static_cast<uint32_t>(payload), samplesPerFrame_, flags)) {
		++stats_.records;
		stats_.samples += samplesPerFrame_;
		discontinuity_ = false;
	} else {
		// The frame ring is full: the driver fell behind, so the next record is a
		// discontinuity. The PCM is already consumed, so nothing is retried.
		++stats_.droppedRecords;
		discontinuity_ = true;
	}

	producedSamples_ += samplesPerFrame_;
	return true;
}

} // namespace OpenA2dp
