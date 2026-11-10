#pragma once

// Paces, encodes and frames aptX HD for OpenA2dpSvc.exe.
//
// The service owns the media clock for service-encoded codecs: on every WAIT_EVENT tick it
// tells this object how much time has elapsed, and the object encodes whole 10 ms records
// whenever the clock has made one due. Each record carries many 6-byte frames, because an
// aptX HD frame covers only four samples per channel; the driver puts one record into one
// RTP packet. A 44.1 kHz span is not a whole number of frames, so the samples a record
// cannot encode carry into the next one and the packet sizes average out exactly. PCM comes
// from the shared ring as 24-in-32 (the endpoint's 24-bit format), so the bytes are handed
// to the encoder unchanged.
//
// No OS calls, no threads: the service supplies the encoder, the clock and the rings.

#include <OpenA2dp/AptxHd.h>
#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/RingBuffer.h>

#include <memory>
#include <stdint.h>
#include <vector>

namespace OpenA2dp {

struct AptxHdStreamParams {
	uint32_t sampleRate = 48000;
	uint16_t channels = 2;
};

struct AptxHdStreamStats {
	uint64_t records = 0;        // records written to the frame ring
	uint64_t samples = 0;        // PCM samples per channel covered by those records
	uint64_t pcmBytes = 0;       // PCM pulled from the ring
	uint64_t silenceSamples = 0; // samples zero-filled because the ring was short
	uint64_t droppedRecords = 0; // records the frame ring could not take
	uint64_t encodeErrors = 0;   // Encode/BuildMediaPayload failures
};

class AptxHdStreamEncoder {
public:
	AptxHdStreamEncoder() noexcept = default;
	~AptxHdStreamEncoder();

	AptxHdStreamEncoder(const AptxHdStreamEncoder&) = delete;
	AptxHdStreamEncoder& operator=(const AptxHdStreamEncoder&) = delete;

	// Starts a stream. `encoder` must take 24-in-32 PCM for `params` and is owned by this
	// object. PCM already buffered in `pcm` is discarded: it predates the stream.
	bool Start(std::unique_ptr<IA2dpEncoder> encoder, const AptxHdStreamParams& params, RingBufferView& pcm,
			   uint64_t startTimeUs) noexcept;

	void Stop() noexcept;

	bool Active() const noexcept { return active_; }

	// Total PCM samples (per channel) that must have been encoded by `nowUs`, including the
	// preroll the kernel paces SBC with.
	uint64_t TargetSamples(uint64_t nowUs) const noexcept;

	// Encodes everything the clock says is due, writing at most `maxRecords` records. Returns
	// false on a fatal encoder error; the caller then stops the stream.
	bool Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames, size_t maxRecords) noexcept;

	const AptxHdStreamParams& Params() const noexcept { return params_; }
	const AptxHdStreamStats& Stats() const noexcept { return stats_; }

private:
	// Encodes one 10 ms record of framesInRecord frames, reading a full span of PCM and
	// zero-filling only what the ring does not hold.
	bool EncodeRecord(RingBufferView& pcm, RingBufferView& frames) noexcept;

	std::unique_ptr<IA2dpEncoder> encoder_;
	AptxHdStreamParams params_;
	AptxHdStreamStats stats_;
	uint64_t startTimeUs_ = 0;
	uint64_t producedSamples_ = 0;
	bool active_ = false;
	bool discontinuity_ = false;

	uint32_t sampleBytes_ = 0;   // interleaved 24-in-32 bytes for all channels
	uint32_t samplesPerFrame_ = 0;
	uint32_t spanSamples_ = 0;   // one 10 ms record's worth of clock
	uint32_t carrySamples_ = 0;  // samples the previous record left for this one
	size_t maxFrames_ = 0;
	std::vector<uint8_t> pcmRecord_;  // (spanSamples_ + samplesPerFrame_ - 1) * sampleBytes_
	std::vector<uint8_t> frameData_;  // maxFrames_ * AptxHd::kFrameBytes
	std::vector<size_t> frameSizes_;
	std::vector<uint8_t> payload_;    // Ioctl::kMaxFramePayload
	std::vector<uint8_t> carryPcm_;   // (samplesPerFrame_ - 1) * sampleBytes_
};

} // namespace OpenA2dp
