#include <OpenA2dp/LdacStreamEncoder.h>

#include <OpenA2dp/FrameRecord.h>
#include <OpenA2dp/LatencyBudget.h>
#include <OpenA2dp/PcmDepth.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace OpenA2dp {

namespace {

uint64_t PrerollSamples(const LdacStreamParams& params) noexcept
{
	return static_cast<uint64_t>(params.sampleRate) * Ioctl::EffectivePrerollMs(params.prerollMs) / 1000;
}

} // namespace

LdacStreamEncoder::~LdacStreamEncoder() = default;

bool LdacStreamEncoder::Start(std::unique_ptr<IA2dpEncoder> encoder, const LdacStreamParams& params,
							  uint64_t startTimeUs) noexcept
{
	Stop();
	if (encoder == nullptr) {
		return false;
	}

	const PcmFormat format = encoder->InputFormat();
	const uint32_t samplesPerFrame = encoder->SamplesPerFrame();
	const size_t maxFrameBytes = encoder->MaxFrameBytes();
	// The record layout below is fixed to the codec's 128 samples into one variable-length
	// transport frame; another encoder shape would write past the frame buffer.
	if (format.sampleRate != params.sampleRate || format.channels != params.channels ||
		format.bitsPerSample != Ldac::kBitsPerSample || params.channels != 2 ||
		samplesPerFrame != Ldac::kSamplesPerFrame || maxFrameBytes > Ldac::kMaxFrameBytes ||
		params.sampleRate % 100 != 0 || encoder->FramesPerPacket(Ioctl::kMaxFramePayload) == 0) {
		return false;
	}
	// The ring depth is the endpoint's; the encoder above is always the 16-bit one.
	if (params.bitsPerSample != Ldac::kBitsPerSample && params.bitsPerSample != 24) {
		return false;
	}
	const bool convert24 = params.bitsPerSample == 24;
	const uint32_t sampleBytes = static_cast<uint32_t>(params.channels) * 2u;
	const uint32_t ringSampleBytes = static_cast<uint32_t>(params.channels) * (convert24 ? 4u : 2u);

	try {
		pcmRecord_.assign(static_cast<size_t>(samplesPerFrame) * ringSampleBytes, 0);
		if (convert24) {
			narrowRecord_.assign(static_cast<size_t>(samplesPerFrame) * sampleBytes, 0);
		}
		frameData_.assign(maxFrameBytes, 0);
		payload_.assign(Ioctl::kMaxFramePayload, 0);
	} catch (const std::bad_alloc&) {
		return false;
	}

	encoder_ = std::move(encoder);
	params_ = params;
	stats_ = {};
	startTimeUs_ = startTimeUs;
	producedSamples_ = 0;
	pendingSamples_ = 0;
	pendingSilence_ = 0;
	sampleBytes_ = sampleBytes;
	ringSampleBytes_ = ringSampleBytes;
	samplesPerFrame_ = samplesPerFrame;
	convert24_ = convert24;
	// Nothing is discarded here: the driver drops the PCM that predates the stream and keeps the
	// beginning of playback, so what the ring holds is the first audio this stream has to encode.
	leadSilence_ = 0;
	leadKnown_ = false;
	discontinuity_ = true;
	active_ = true;
	return true;
}

void LdacStreamEncoder::Stop() noexcept
{
	active_ = false;
	encoder_.reset();
	producedSamples_ = 0;
	pendingSamples_ = 0;
	pendingSilence_ = 0;
	leadSilence_ = 0;
	leadKnown_ = false;
	discontinuity_ = false;
}

uint64_t LdacStreamEncoder::TargetSamples(uint64_t nowUs) const noexcept
{
	const uint64_t elapsedUs = nowUs > startTimeUs_ ? nowUs - startTimeUs_ : 0;
	return elapsedUs * params_.sampleRate / 1000000ull + PrerollSamples(params_);
}

bool LdacStreamEncoder::Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames,
							 size_t maxRecords) noexcept
{
	if (!active_) {
		return true;
	}

	const uint64_t target = TargetSamples(nowUs);
	if (!leadKnown_) {
		// The PCM buffered at start is the beginning of playback: the lead goes in front of it.
		leadSilence_ = LatencyBudget::LeadSilenceSamples(target, pcm.ReadableBytes() / ringSampleBytes_);
		leadKnown_ = true;
	}
	for (size_t i = 0; i < maxRecords && producedSamples_ < target; ++i) {
		const uint64_t available = pcm.ReadableBytes() / ringSampleBytes_ + leadSilence_;
		if (!LatencyBudget::UnitReady(available, samplesPerFrame_, target - producedSamples_, params_.sampleRate)) {
			break;
		}
		if (!EncodeRecord(pcm, frames)) {
			return false;
		}
	}
	return true;
}

bool LdacStreamEncoder::EncodeRecord(RingBufferView& pcm, RingBufferView& frames) noexcept
{
	// Lead silence still owed goes first, then the ring; only a ring still short once the unit is
	// overdue (Pump) leaves a zero-filled tail, which is an underrun.
	const uint32_t recordBytes = samplesPerFrame_ * ringSampleBytes_;
	std::memset(pcmRecord_.data(), 0, recordBytes);
	const uint32_t lead = static_cast<uint32_t>(std::min<uint64_t>(leadSilence_, samplesPerFrame_));
	leadSilence_ -= lead;
	const uint32_t available = pcm.ReadUpTo(pcmRecord_.data() + static_cast<size_t>(lead) * ringSampleBytes_,
											 (samplesPerFrame_ - lead) * ringSampleBytes_, ringSampleBytes_);
	stats_.pcmBytes += available;
	// Saved for the record this unit ends up in, which may be a later call's output. The lead is
	// silence on purpose, not a gap.
	const uint32_t silence = samplesPerFrame_ - lead - available / ringSampleBytes_;
	pendingSilence_ += silence;
	stats_.silenceSamples += silence;

	// A wide ring holds 24-in-32 words; the encoder takes 16-bit samples, narrowed the way the
	// kernel SBC path narrows the same ring.
	const uint8_t* encodeBase = pcmRecord_.data();
	if (convert24_) {
		const uint32_t words = samplesPerFrame_ * params_.channels;
		for (uint32_t word = 0; word < words; ++word) {
			int32_t sample = 0;
			std::memcpy(&sample, pcmRecord_.data() + static_cast<size_t>(word) * 4, sizeof(sample));
			const int16_t narrow = Pcm::Narrow24To16(sample);
			std::memcpy(narrowRecord_.data() + static_cast<size_t>(word) * 2, &narrow, sizeof(narrow));
		}
		encodeBase = narrowRecord_.data();
	}

	size_t frameBytes = 0;
	const HRESULT hr = encoder_->Encode(encodeBase, frameData_.data(), frameData_.size(), frameBytes);
	if (FAILED(hr)) {
		++stats_.encodeErrors;
		return false;
	}
	// The encoder fills a whole media packet before returning anything, so a call that writes
	// nothing is not an error: its samples belong to the record that follows.
	pendingSamples_ += samplesPerFrame_;
	if (frameBytes == 0) {
		producedSamples_ += samplesPerFrame_;
		return true;
	}

	const size_t payload = encoder_->BuildMediaPayload(frameData_.data(), &frameBytes, 1,
													   payload_.data(), payload_.size());
	if (payload != frameBytes) {
		++stats_.encodeErrors;
		return false;
	}

	const uint32_t recordSamples = pendingSamples_;
	const uint32_t recordSilence = pendingSilence_;
	const uint32_t flags = discontinuity_ ? Ioctl::kFrameFlagDiscontinuity : 0;
	if (FrameRecord::Write(frames, payload_.data(), static_cast<uint32_t>(payload), recordSamples,
						   recordSilence, flags)) {
		++stats_.records;
		stats_.samples += recordSamples;
		pendingSamples_ = 0;
		pendingSilence_ = 0;
		stats_.payloadBytes += payload;
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
