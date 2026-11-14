#include <OpenA2dp/AacStreamEncoder.h>

#include <OpenA2dp/FrameRecord.h>
#include <OpenA2dp/LatencyBudget.h>
#include <OpenA2dp/PcmDepth.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace OpenA2dp {

namespace {

uint64_t PrerollSamples(const AacStreamParams& params) noexcept
{
	return static_cast<uint64_t>(params.sampleRate) * Ioctl::EffectivePrerollMs(params.prerollMs) / 1000;
}

} // namespace

AacStreamEncoder::~AacStreamEncoder() = default;

bool AacStreamEncoder::Start(std::unique_ptr<IA2dpEncoder> encoder, const AacStreamParams& params,
							 uint64_t startTimeUs) noexcept
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
		format.bitsPerSample != 16 || samplesPerFrame == 0 || samplesPerFrame > 4096 || maxFrameBytes == 0) {
		return false;
	}
	if (params.bitsPerSample != 16 && params.bitsPerSample != 24) {
		return false;
	}

	const uint32_t bytesPerSample = static_cast<uint32_t>(params.channels) * 2u;
	const uint32_t ringBytesPerSample =
		static_cast<uint32_t>(params.channels) * (params.bitsPerSample == 24 ? 4u : 2u);
	try {
		pcmBlock_.assign(static_cast<size_t>(samplesPerFrame) * bytesPerSample, 0);
		if (params.bitsPerSample == 24) {
			ringBlock_.assign(static_cast<size_t>(samplesPerFrame) * ringBytesPerSample, 0);
		}
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
	ringBytesPerSample_ = ringBytesPerSample;
	convert24_ = params.bitsPerSample == 24;
	// Nothing is discarded here: the driver drops the PCM that predates the stream and keeps the
	// beginning of playback, so what the ring holds is the first audio this stream has to encode.
	leadSilence_ = 0;
	leadKnown_ = false;
	discontinuity_ = true;
	active_ = true;
	return true;
}

void AacStreamEncoder::Stop() noexcept
{
	active_ = false;
	encoder_.reset();
	producedSamples_ = 0;
	leadSilence_ = 0;
	leadKnown_ = false;
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
	if (!leadKnown_) {
		// The PCM buffered at start is the beginning of playback: the lead goes in front of it.
		leadSilence_ = LatencyBudget::LeadSilenceSamples(target, pcm.ReadableBytes() / ringBytesPerSample_);
		leadKnown_ = true;
	}
	for (size_t i = 0; i < maxRecords && producedSamples_ < target; ++i) {
		const uint64_t available = pcm.ReadableBytes() / ringBytesPerSample_ + leadSilence_;
		if (!LatencyBudget::UnitReady(available, samplesPerFrame_, target - producedSamples_, params_.sampleRate)) {
			break;
		}
		if (!EncodeBlock(pcm, frames)) {
			return false;
		}
	}
	return true;
}

bool AacStreamEncoder::EncodeBlock(RingBufferView& pcm, RingBufferView& frames) noexcept
{
	// Every record covers a whole 1024-sample block. Lead silence owed at start goes first; the
	// rest is read from the ring, and only a ring that is still short once the block is overdue
	// (Pump) leaves a zero-filled tail - an underrun, counted as the record's silence.
	std::memset(pcmBlock_.data(), 0, pcmBlock_.size());
	const uint32_t lead = static_cast<uint32_t>(std::min<uint64_t>(leadSilence_, samplesPerFrame_));
	leadSilence_ -= lead;
	const uint32_t wanted = (samplesPerFrame_ - lead) * ringBytesPerSample_;
	uint32_t available = 0;
	uint32_t availableSamples = 0;
	if (convert24_) {
		// The ring carries 24-in-32; the top 16 bits of each sample are the AAC input. A
		// short read leaves the rest of the block silent, as the 16-bit path does.
		available = pcm.ReadUpTo(ringBlock_.data(), wanted, ringBytesPerSample_);
		availableSamples = available / ringBytesPerSample_;
		for (uint32_t frame = 0; frame < availableSamples; ++frame) {
			for (uint16_t channel = 0; channel < params_.channels; ++channel) {
				int32_t sample = 0;
				std::memcpy(&sample, ringBlock_.data() + (frame * params_.channels + channel) * 4, sizeof(sample));
				const int16_t narrow = Pcm::Narrow24To16(sample);
				std::memcpy(pcmBlock_.data() + ((lead + frame) * params_.channels + channel) * 2, &narrow,
							sizeof(narrow));
			}
		}
	} else {
		available = pcm.ReadUpTo(pcmBlock_.data() + static_cast<size_t>(lead) * bytesPerSample_, wanted,
								 bytesPerSample_);
		availableSamples = available / bytesPerSample_;
	}

	stats_.pcmBytes += available;
	// The lead is silence on purpose; only what the ring could not supply after it is a gap.
	const uint32_t silenceSamples = samplesPerFrame_ - lead - availableSamples;
	stats_.silenceSamples += silenceSamples;

	size_t written = 0;
	const HRESULT hr = encoder_->Encode(pcmBlock_.data(), accessUnit_.data(), accessUnit_.size(),
										written);
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
	if (FrameRecord::Write(frames, payload_.data(), static_cast<uint32_t>(payload), samplesPerFrame_,
							silenceSamples, flags)) {
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
