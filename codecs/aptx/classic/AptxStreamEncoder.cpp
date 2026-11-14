#include <OpenA2dp/AptxStreamEncoder.h>

#include <OpenA2dp/Aptx.h>
#include <OpenA2dp/FrameRecord.h>
#include <OpenA2dp/LatencyBudget.h>
#include <OpenA2dp/PcmDepth.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace OpenA2dp {

namespace {

uint64_t PrerollSamples(const AptxStreamParams& params) noexcept
{
	return static_cast<uint64_t>(params.sampleRate) * Ioctl::EffectivePrerollMs(params.prerollMs) / 1000;
}

} // namespace

AptxStreamEncoder::~AptxStreamEncoder() = default;

bool AptxStreamEncoder::Start(std::unique_ptr<IA2dpEncoder> encoder, const AptxStreamParams& params,
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
	// The record layout below is the codec's fixed four samples to four bytes; an encoder
	// with another shape would read past the record or write past the frame stride.
	if (format.sampleRate != params.sampleRate || format.channels != params.channels ||
		format.bitsPerSample != Aptx::kBitsPerSample || params.channels != 2 ||
		samplesPerFrame != Aptx::kSamplesPerFrame || maxFrameBytes != Aptx::kFrameBytes ||
		params.sampleRate % 100 != 0 || encoder->FramesPerPacket(Ioctl::kMaxFramePayload) == 0)
	{
		return false;
	}
	// The ring depth is the endpoint's; the encoder above is always the 16-bit one.
	if (params.bitsPerSample != Aptx::kBitsPerSample && params.bitsPerSample != 24)
	{
		return false;
	}
	const bool convert24 = params.bitsPerSample == 24;

	// The clock span is a whole number of milliseconds; the carry absorbs the samples a frame
	// cannot cover. A record can need one frame more than the span alone.
	const uint32_t spanSamples = params.sampleRate * Ioctl::kServiceRecordMs / 1000;
	const size_t maxFrames = (static_cast<size_t>(spanSamples) + samplesPerFrame - 1) / samplesPerFrame;
	const uint32_t recordSamples = spanSamples + samplesPerFrame - 1;
	if (maxFrames > 4096 || recordSamples > 4096)
	{
		return false;
	}
	const uint32_t sampleBytes = static_cast<uint32_t>(params.channels) * 2u;
	const uint32_t ringSampleBytes = static_cast<uint32_t>(params.channels) * (convert24 ? 4u : 2u);

	try {
		pcmRecord_.assign(static_cast<size_t>(recordSamples) * ringSampleBytes, 0);
		carryPcm_.assign(static_cast<size_t>(samplesPerFrame - 1) * ringSampleBytes, 0);
		if (convert24) {
			narrowRecord_.assign(static_cast<size_t>(recordSamples) * sampleBytes, 0);
		}
		frameData_.assign(maxFrames * maxFrameBytes, 0);
		frameSizes_.assign(maxFrames, 0);
		payload_.assign(Ioctl::kMaxFramePayload, 0);
	} catch (const std::bad_alloc&) {
		return false;
	}

	encoder_ = std::move(encoder);
	params_ = params;
	stats_ = {};
	startTimeUs_ = startTimeUs;
	producedSamples_ = 0;
	sampleBytes_ = sampleBytes;
	ringSampleBytes_ = ringSampleBytes;
	samplesPerFrame_ = samplesPerFrame;
	spanSamples_ = spanSamples;
	maxFrames_ = maxFrames;
	carrySamples_ = 0;
	pendingSilence_ = 0;
	convert24_ = convert24;
	// Nothing is discarded here: the driver drops the PCM that predates the stream and keeps the
	// beginning of playback, so what the ring holds is the first audio this stream has to encode.
	leadSilence_ = 0;
	leadKnown_ = false;
	pendingSkip_ = 0;
	discontinuity_ = true;
	active_ = true;
	return true;
}

void AptxStreamEncoder::Stop() noexcept
{
	active_ = false;
	encoder_.reset();
	producedSamples_ = 0;
	carrySamples_ = 0;
	pendingSilence_ = 0;
	leadSilence_ = 0;
	leadKnown_ = false;
	pendingSkip_ = 0;
	discontinuity_ = false;
}

uint64_t AptxStreamEncoder::TargetSamples(uint64_t nowUs) const noexcept
{
	const uint64_t elapsedUs = nowUs > startTimeUs_ ? nowUs - startTimeUs_ : 0;
	return elapsedUs * params_.sampleRate / 1000000ull + PrerollSamples(params_);
}

bool AptxStreamEncoder::Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames,
							 size_t maxRecords) noexcept
{
	if (!active_) {
		return true;
	}

	const uint64_t target = TargetSamples(nowUs);
	const uint64_t due = target > producedSamples_ ? target - producedSamples_ : 0;
	const uint64_t buffered = pcm.ReadableBytes() / ringSampleBytes_;
	const uint64_t lead = PrerollSamples(params_);
	if (!leadKnown_ || LatencyBudget::Stalled(due, buffered + leadSilence_, lead, spanSamples_, params_.sampleRate)) {
		// The stream picks up at the lead - at its start, and after a stall in which the driver
		// trimmed what this one could not read - instead of padding the gap back with silence. The
		// next record carries the skip, so the driver moves the RTP timestamp past it.
		const LatencyBudget::Restart restart = LatencyBudget::RestartAt(due, buffered, lead);
		producedSamples_ += restart.skipSamples;
		pendingSkip_ += static_cast<uint32_t>(restart.skipSamples);
		leadSilence_ = restart.leadSilenceSamples;
		leadKnown_ = true;
	}
	for (size_t i = 0; i < maxRecords && producedSamples_ < target; ++i) {
		const uint64_t available = pcm.ReadableBytes() / ringSampleBytes_ + leadSilence_;
		if (!LatencyBudget::UnitReady(available, spanSamples_, target - producedSamples_, params_.sampleRate)) {
			break;
		}
		if (!EncodeRecord(pcm, frames)) {
			return false;
		}
	}
	return true;
}

bool AptxStreamEncoder::EncodeRecord(RingBufferView& pcm, RingBufferView& frames) noexcept
{
	// Samples the previous record could not fit go first, then the lead silence still owed, then
	// the ring; only a ring still short once the record is overdue (Pump) leaves a zero-filled
	// tail, which is an underrun.
	const uint32_t recordBytes = spanSamples_ * ringSampleBytes_;
	std::memcpy(pcmRecord_.data(), carryPcm_.data(), static_cast<size_t>(carrySamples_) * ringSampleBytes_);
	uint8_t* const spanStart = pcmRecord_.data() + static_cast<size_t>(carrySamples_) * ringSampleBytes_;
	std::memset(spanStart, 0, recordBytes);
	const uint32_t lead = static_cast<uint32_t>(std::min<uint64_t>(leadSilence_, spanSamples_));
	leadSilence_ -= lead;
	const uint32_t available = pcm.ReadUpTo(spanStart + static_cast<size_t>(lead) * ringSampleBytes_,
											 (spanSamples_ - lead) * ringSampleBytes_, ringSampleBytes_);

	const uint32_t readSamples = available / ringSampleBytes_;
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

	// A wide ring holds 24-in-32 words; the encoder takes the top 16 bits of each, rounded the
	// way the kernel SBC path narrows the same ring.
	const uint8_t* encodeBase = pcmRecord_.data();
	if (convert24_) {
		const uint32_t words = totalSamples * params_.channels;
		for (uint32_t word = 0; word < words; ++word) {
			int32_t sample = 0;
			std::memcpy(&sample, pcmRecord_.data() + static_cast<size_t>(word) * 4, sizeof(sample));
			const int16_t narrow = Pcm::Narrow24To16(sample);
			std::memcpy(narrowRecord_.data() + static_cast<size_t>(word) * 2, &narrow, sizeof(narrow));
		}
		encodeBase = narrowRecord_.data();
	}

	const size_t pcmFrameBytes = static_cast<size_t>(samplesPerFrame_) * sampleBytes_;
	size_t total = 0;
	for (size_t frame = 0; frame < framesInRecord; ++frame) {
		size_t written = 0;
		const HRESULT hr = encoder_->Encode(encodeBase + frame * pcmFrameBytes,
											frameData_.data() + frame * Aptx::kFrameBytes,
											Aptx::kFrameBytes, written);
		if (FAILED(hr) || written != Aptx::kFrameBytes) {
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
						   recordSilence, pendingSkip_, flags)) {
		++stats_.records;
		stats_.samples += encodedSamples;
		pendingSilence_ = 0;
		pendingSkip_ = 0;
		discontinuity_ = false;
	} else {
		// The frame ring is full: the driver fell behind, so the next record is a
		// discontinuity. The PCM is already consumed, so nothing is retried.
		++stats_.droppedRecords;
		discontinuity_ = true;
	}

	std::memcpy(carryPcm_.data(), pcmRecord_.data() + static_cast<size_t>(encodedSamples) * ringSampleBytes_,
				static_cast<size_t>(carried) * ringSampleBytes_);
	carrySamples_ = carried;
	// The clock advanced by the full span, whether or not a frame could hold all of it.
	producedSamples_ += spanSamples_;
	return true;
}

} // namespace OpenA2dp
