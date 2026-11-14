#include <OpenA2dp/AptxHdStreamEncoder.h>

#include <OpenA2dp/AptxHd.h>
#include <OpenA2dp/FrameRecord.h>
#include <OpenA2dp/LatencyBudget.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace OpenA2dp {

namespace {

uint64_t PrerollSamples(const AptxHdStreamParams& params) noexcept
{
	return static_cast<uint64_t>(params.sampleRate) * Ioctl::EffectivePrerollMs(params.prerollMs) / 1000;
}

} // namespace

AptxHdStreamEncoder::~AptxHdStreamEncoder() = default;

bool AptxHdStreamEncoder::Start(std::unique_ptr<IA2dpEncoder> encoder, const AptxHdStreamParams& params,
								uint64_t startTimeUs) noexcept
{
	Stop();
	if (encoder == nullptr)
	{
		return false;
	}

	const PcmFormat format = encoder->InputFormat();
	const uint32_t samplesPerFrame = encoder->SamplesPerFrame();
	const size_t maxFrameBytes = encoder->MaxFrameBytes();
	// The frame layout below is the codec's fixed four samples to six bytes; an encoder with
	// another shape would read past the record or write past the frame stride.
	if (format.sampleRate != params.sampleRate || format.channels != params.channels ||
		format.bitsPerSample != AptxHd::kBitsPerSample || params.channels != 2 ||
		samplesPerFrame != AptxHd::kSamplesPerFrame || maxFrameBytes != AptxHd::kFrameBytes ||
		params.sampleRate % 100 != 0 || encoder->FramesPerPacket(Ioctl::kMaxFramePayload) == 0)
	{
		return false;
	}

	// The clock span is a whole number of milliseconds; the carry absorbs the samples a frame
	// cannot cover. A record can need one frame more than the span alone.
	const uint32_t spanSamples = params.sampleRate * Ioctl::kServiceRecordMs / 1000;
	const size_t maxFrames = (static_cast<size_t>(spanSamples) + samplesPerFrame - 1) / samplesPerFrame;
	const uint32_t recordSamples = spanSamples + samplesPerFrame - 1;
	// A record the encoder cannot hold in one packet would be built past the payload buffer.
	if (maxFrames > 4096 || recordSamples > 4096 ||
		maxFrames > encoder->FramesPerPacket(Ioctl::kMaxFramePayload))
	{
		return false;
	}
	const uint32_t sampleBytes = static_cast<uint32_t>(params.channels) * 4u;

	try {
		pcmRecord_.assign(static_cast<size_t>(recordSamples) * sampleBytes, 0);
		frameData_.assign(maxFrames * maxFrameBytes, 0);
		frameSizes_.assign(maxFrames, 0);
		payload_.assign(Ioctl::kMaxFramePayload, 0);
		carryPcm_.assign(static_cast<size_t>(samplesPerFrame - 1) * sampleBytes, 0);
	} catch (const std::bad_alloc&) {
		return false;
	}

	encoder_ = std::move(encoder);
	params_ = params;
	stats_ = {};
	startTimeUs_ = startTimeUs;
	producedSamples_ = 0;
	sampleBytes_ = sampleBytes;
	samplesPerFrame_ = samplesPerFrame;
	spanSamples_ = spanSamples;
	maxFrames_ = maxFrames;
	carrySamples_ = 0;
	pendingSilence_ = 0;
	// Nothing is discarded here: the driver drops the PCM that predates the stream and keeps the
	// beginning of playback, so what the ring holds is the first audio this stream has to encode.
	leadSilence_ = 0;
	leadKnown_ = false;
	discontinuity_ = true;
	active_ = true;
	return true;
}

void AptxHdStreamEncoder::Stop() noexcept
{
	active_ = false;
	encoder_.reset();
	producedSamples_ = 0;
	carrySamples_ = 0;
	pendingSilence_ = 0;
	leadSilence_ = 0;
	leadKnown_ = false;
	discontinuity_ = false;
}

uint64_t AptxHdStreamEncoder::TargetSamples(uint64_t nowUs) const noexcept
{
	const uint64_t elapsedUs = nowUs > startTimeUs_ ? nowUs - startTimeUs_ : 0;
	return elapsedUs * params_.sampleRate / 1000000ull + PrerollSamples(params_);
}

bool AptxHdStreamEncoder::Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames,
							   size_t maxRecords) noexcept
{
	if (!active_) {
		return true;
	}

	const uint64_t target = TargetSamples(nowUs);
	if (!leadKnown_) {
		// The PCM buffered at start is the beginning of playback: the lead goes in front of it.
		leadSilence_ = LatencyBudget::LeadSilenceSamples(target, pcm.ReadableBytes() / sampleBytes_);
		leadKnown_ = true;
	}
	for (size_t i = 0; i < maxRecords && producedSamples_ < target; ++i) {
		const uint64_t available = pcm.ReadableBytes() / sampleBytes_ + leadSilence_;
		if (!LatencyBudget::UnitReady(available, spanSamples_, target - producedSamples_, params_.sampleRate)) {
			break;
		}
		if (!EncodeRecord(pcm, frames)) {
			return false;
		}
	}
	return true;
}

bool AptxHdStreamEncoder::EncodeRecord(RingBufferView& pcm, RingBufferView& frames) noexcept
{
	// Samples the previous record could not fit go first, then the lead silence still owed, then
	// the ring; only a ring still short once the record is overdue (Pump) leaves a zero-filled
	// tail, which is an underrun.
	const uint32_t recordBytes = spanSamples_ * sampleBytes_;
	std::memcpy(pcmRecord_.data(), carryPcm_.data(), static_cast<size_t>(carrySamples_) * sampleBytes_);
	uint8_t* const spanStart = pcmRecord_.data() + static_cast<size_t>(carrySamples_) * sampleBytes_;
	std::memset(spanStart, 0, recordBytes);
	const uint32_t lead = static_cast<uint32_t>(std::min<uint64_t>(leadSilence_, spanSamples_));
	leadSilence_ -= lead;
	const uint32_t available = pcm.ReadUpTo(spanStart + static_cast<size_t>(lead) * sampleBytes_,
											 (spanSamples_ - lead) * sampleBytes_, sampleBytes_);

	const uint32_t readSamples = available / sampleBytes_;
	stats_.pcmBytes += available;
	// Saved for the record these samples end up in, which may be the next call's. The lead is
	// silence on purpose, not a gap.
	pendingSilence_ += spanSamples_ - lead - readSamples;
	stats_.silenceSamples += spanSamples_ - lead - readSamples;

	const uint32_t totalSamples = carrySamples_ + spanSamples_;
	const size_t framesInRecord = totalSamples / samplesPerFrame_;
	const uint32_t encodedSamples = static_cast<uint32_t>(framesInRecord * samplesPerFrame_);
	const uint32_t carried = totalSamples - encodedSamples;
	if (framesInRecord == 0 || framesInRecord > maxFrames_) {
		++stats_.encodeErrors;
		return false;
	}

	const size_t pcmFrameBytes = static_cast<size_t>(samplesPerFrame_) * sampleBytes_;
	size_t total = 0;
	for (size_t frame = 0; frame < framesInRecord; ++frame) {
		size_t written = 0;
		const HRESULT hr = encoder_->Encode(pcmRecord_.data() + frame * pcmFrameBytes,
										   frameData_.data() + frame * AptxHd::kFrameBytes,
										   AptxHd::kFrameBytes, written);
		if (FAILED(hr) || written != AptxHd::kFrameBytes) {
			++stats_.encodeErrors;
			return false;
		}
		frameSizes_[frame] = written;
		total += written;
	}

	const size_t payload = encoder_->BuildMediaPayload(frameData_.data(), frameSizes_.data(),
													   framesInRecord, payload_.data(), payload_.size());
	if (payload != total) {
		++stats_.encodeErrors;
		return false;
	}

	const uint32_t flags = discontinuity_ ? Ioctl::kFrameFlagDiscontinuity : 0;
	const uint32_t recordSilence = pendingSilence_;
	if (FrameRecord::Write(frames, payload_.data(), static_cast<uint32_t>(payload), encodedSamples,
						   recordSilence, flags)) {
		++stats_.records;
		stats_.samples += encodedSamples;
		pendingSilence_ = 0;
		discontinuity_ = false;
	} else {
		// The frame ring is full: the driver fell behind, so the next record is a
		// discontinuity. The PCM is already consumed, so nothing is retried.
		++stats_.droppedRecords;
		discontinuity_ = true;
	}

	std::memcpy(carryPcm_.data(), pcmRecord_.data() + static_cast<size_t>(encodedSamples) * sampleBytes_,
				static_cast<size_t>(carried) * sampleBytes_);
	carrySamples_ = carried;
	// The clock advanced by the full span, whether or not a frame could hold all of it.
	producedSamples_ += spanSamples_;
	return true;
}

} // namespace OpenA2dp
