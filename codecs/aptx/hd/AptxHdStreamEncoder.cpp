#include <OpenA2dp/AptxHdStreamEncoder.h>

#include <OpenA2dp/AptxHd.h>
#include <OpenA2dp/FrameRecord.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace OpenA2dp {

namespace {

uint64_t PrerollSamples(const AptxHdStreamParams& params) noexcept
{
	return static_cast<uint64_t>(params.sampleRate) * Ioctl::kPrerollMs / 1000;
}

} // namespace

AptxHdStreamEncoder::~AptxHdStreamEncoder() = default;

bool AptxHdStreamEncoder::Start(std::unique_ptr<IA2dpEncoder> encoder, const AptxHdStreamParams& params,
								RingBufferView& pcm, uint64_t startTimeUs) noexcept
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
		encoder->FramesPerPacket(Ioctl::kMaxFramePayload) == 0)
	{
		return false;
	}

	// Ten milliseconds per record, so a record is one media tick: rate/100 samples of four
	// samples per frame. The encoder still caps how many frames fit one packet; a record
	// larger than that would never be sent whole.
	const size_t recordFrames = std::max<size_t>(1, params.sampleRate / 400);
	const size_t framesPerRecord = std::min(recordFrames, encoder->FramesPerPacket(Ioctl::kMaxFramePayload));
	const uint32_t samplesPerRecord = static_cast<uint32_t>(framesPerRecord * samplesPerFrame);
	if (framesPerRecord > 4096 || samplesPerRecord > 4096)
	{
		return false;
	}
	const uint32_t sampleBytes = static_cast<uint32_t>(params.channels) * 4u;

	try {
		pcmRecord_.assign(static_cast<size_t>(samplesPerRecord) * sampleBytes, 0);
		frameData_.assign(framesPerRecord * maxFrameBytes, 0);
		frameSizes_.assign(framesPerRecord, 0);
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
	samplesPerRecord_ = samplesPerRecord;
	framesPerRecord_ = framesPerRecord;
	// The PCM the endpoint queued before MediaStart predates this stream.
	const uint32_t stale = pcm.ReadableBytes();
	if (stale != 0) {
		pcm.Skip(stale);
	}
	discontinuity_ = true;
	active_ = true;
	return true;
}

void AptxHdStreamEncoder::Stop() noexcept
{
	active_ = false;
	encoder_.reset();
	producedSamples_ = 0;
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
	for (size_t i = 0; i < maxRecords && producedSamples_ < target; ++i) {
		if (!EncodeRecord(pcm, frames)) {
			return false;
		}
	}
	return true;
}

bool AptxHdStreamEncoder::EncodeRecord(RingBufferView& pcm, RingBufferView& frames) noexcept
{
	// A record always covers its full span; a short read is zero-filled so a late or empty
	// PCM ring becomes silence instead of a stalled stream.
	std::memset(pcmRecord_.data(), 0, pcmRecord_.size());
	uint32_t available = std::min<uint32_t>(pcm.ReadableBytes(), static_cast<uint32_t>(pcmRecord_.size()));
	available -= available % sampleBytes_;
	if (available != 0 && !pcm.Read(pcmRecord_.data(), available)) {
		available = 0;
	}

	const uint32_t availableSamples = available / sampleBytes_;
	stats_.pcmBytes += available;
	stats_.silenceSamples += samplesPerRecord_ - availableSamples;

	const size_t pcmFrameBytes = static_cast<size_t>(encoder_->SamplesPerFrame()) * sampleBytes_;
	size_t total = 0;
	for (size_t frame = 0; frame < framesPerRecord_; ++frame) {
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
													   framesPerRecord_, payload_.data(), payload_.size());
	if (payload != total) {
		++stats_.encodeErrors;
		return false;
	}

	const uint32_t flags = discontinuity_ ? Ioctl::kFrameFlagDiscontinuity : 0;
	if (FrameRecord::Write(frames, payload_.data(), static_cast<uint32_t>(payload), samplesPerRecord_, flags)) {
		++stats_.records;
		stats_.samples += samplesPerRecord_;
		discontinuity_ = false;
	} else {
		// The frame ring is full: the driver fell behind, so the next record is a
		// discontinuity. The PCM is already consumed, so nothing is retried.
		++stats_.droppedRecords;
		discontinuity_ = true;
	}

	producedSamples_ += samplesPerRecord_;
	return true;
}

} // namespace OpenA2dp
