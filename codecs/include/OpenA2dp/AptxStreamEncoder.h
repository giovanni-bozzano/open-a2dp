#pragma once

// Paces, encodes and frames classic aptX for OpenA2dpSvc.exe.
//
// The service owns the media clock for service-encoded codecs: on every WAIT_EVENT tick it
// tells this object how much time has elapsed, and the object encodes one 10 ms record
// whenever the clock has made one due. A frame covers four samples per channel, but 10 ms of
// 44.1 kHz is 441 samples, which is not a multiple of four, so 0..3 samples are carried into
// the next record. The clock still advances by the full span and a record's sample count is
// what it actually encoded, which keeps the RTP timestamp locked to the media clock over
// time. The driver puts one record into one RTP packet.
//
// PCM comes from the shared ring in the endpoint's own depth: 16-bit stereo goes to the
// encoder unchanged, and a 24-in-32 ring (the endpoint runs wide while aptX HD can) is
// narrowed per sample with the same rounding the kernel SBC path uses. No OS calls, no
// threads: the service supplies the encoder, the clock and the rings.

#include <OpenA2dp/Aptx.h>
#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/RingBuffer.h>

#include <memory>
#include <stdint.h>
#include <vector>

namespace OpenA2dp {

struct AptxStreamParams {
	uint32_t sampleRate = 48000;
	uint16_t channels = 2;
	// Depth of the PCM in the ring: 16 for a 16-bit endpoint, 24 for 24-in-32.
	uint16_t bitsPerSample = Aptx::kBitsPerSample;
};

struct AptxStreamStats {
	uint64_t records = 0;        // records written to the frame ring
	uint64_t samples = 0;        // PCM samples per channel covered by those records
	uint64_t pcmBytes = 0;       // PCM pulled from the ring
	uint64_t silenceSamples = 0; // samples zero-filled because the ring was short
	uint64_t droppedRecords = 0; // records the frame ring could not take
	uint64_t encodeErrors = 0;   // Encode/BuildMediaPayload failures
};

class AptxStreamEncoder {
public:
	AptxStreamEncoder() noexcept = default;
	~AptxStreamEncoder();

	AptxStreamEncoder(const AptxStreamEncoder&) = delete;
	AptxStreamEncoder& operator=(const AptxStreamEncoder&) = delete;

	// Starts a stream. `encoder` must take 16-bit stereo PCM for `params.sampleRate` and is
	// owned by this object; `params.bitsPerSample` describes the ring, which may be wider.
	// PCM already buffered in `pcm` is discarded: it predates the stream.
	bool Start(std::unique_ptr<IA2dpEncoder> encoder, const AptxStreamParams& params, RingBufferView& pcm,
			   uint64_t startTimeUs) noexcept;

	void Stop() noexcept;

	bool Active() const noexcept { return active_; }

	// Total PCM samples (per channel) that must have been encoded by `nowUs`, including the
	// preroll the kernel paces SBC with.
	uint64_t TargetSamples(uint64_t nowUs) const noexcept;

	// Encodes everything the clock says is due, writing at most `maxRecords` records. Returns
	// false on a fatal encoder error; the caller then stops the stream.
	bool Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames, size_t maxRecords) noexcept;

	const AptxStreamParams& Params() const noexcept { return params_; }
	const AptxStreamStats& Stats() const noexcept { return stats_; }

private:
	// Encodes one record covering the clock's 10 ms span plus whatever the frame layout
	// carried over, zero-filling only the PCM the ring does not hold.
	bool EncodeRecord(RingBufferView& pcm, RingBufferView& frames) noexcept;

	std::unique_ptr<IA2dpEncoder> encoder_;
	AptxStreamParams params_;
	AptxStreamStats stats_;
	uint64_t startTimeUs_ = 0;
	uint64_t producedSamples_ = 0;
	bool active_ = false;
	bool discontinuity_ = false;

	uint32_t sampleBytes_ = 0;     // interleaved 16-bit bytes for all channels
	uint32_t ringSampleBytes_ = 0; // interleaved ring bytes for all channels (2 or 4 per sample)
	uint32_t samplesPerFrame_ = 0;
	uint32_t spanSamples_ = 0; // 10 ms of the media clock
	size_t maxFrames_ = 0;
	uint32_t carrySamples_ = 0;
	bool convert24_ = false;
	std::vector<uint8_t> carryPcm_;   // carrySamples_ * ringSampleBytes_
	std::vector<uint8_t> pcmRecord_;  // (span + one frame - 1) * ringSampleBytes_
	std::vector<uint8_t> narrowRecord_; // the same record as 16-bit samples, when convert24_
	std::vector<uint8_t> frameData_;  // maxFrames_ * maxFrameBytes_
	std::vector<size_t> frameSizes_;
	std::vector<uint8_t> payload_;    // Ioctl::kMaxFramePayload
};

} // namespace OpenA2dp
