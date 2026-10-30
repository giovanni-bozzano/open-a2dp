#pragma once

// Testable core of OpenA2dpSvc.exe: paces, encodes and frames AAC-LC audio.
//
// The service owns the AAC media clock (docs/AAC-Service-Boundary.md section 7):
// on every WAIT_EVENT tick it tells this object how much time has elapsed, and
// the object pulls exactly that much PCM from the shared PCM ring, encodes it in
// 1024-sample blocks (padding the last block with silence) and writes one LATM
// AudioMuxElement per frame into the shared frame ring. Everything here is
// clock-injected, so the pacing rules are unit-tested with fake time.
//
// No OS calls, no threads: the service supplies the encoder, the clock and the
// rings. The driver only adds RTP headers.

#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/RingBuffer.h>

#include <memory>
#include <stdint.h>
#include <vector>

namespace OpenA2dp {

// Samples of real time sent ahead of the clock so the sink's jitter buffer
// fills. Same value as the SBC path (driver/Media.cpp).
constexpr uint32_t kAacPrerollMs = 60;

struct AacStreamParams {
	uint32_t sampleRate = 44100;
	uint16_t channels = 2;
	uint32_t bitrate = 0;
	uint32_t objectType = 0; // Aac::kObjectMpeg4Lc
};

struct AacStreamStats {
	uint64_t records = 0;        // records written to the frame ring
	uint64_t samples = 0;        // PCM samples covered by those records
	uint64_t pcmBytes = 0;       // PCM pulled from the ring
	uint64_t silenceSamples = 0; // samples zero-filled because the ring was short
	uint64_t primed = 0;         // encode calls that produced no output (AAC priming)
	uint64_t droppedRecords = 0; // records the frame ring could not take
	uint64_t encodeErrors = 0;   // Encode/BuildMediaPayload failures
};

class AacStreamEncoder {
public:
	AacStreamEncoder() noexcept = default;
	~AacStreamEncoder();

	AacStreamEncoder(const AacStreamEncoder&) = delete;
	AacStreamEncoder& operator=(const AacStreamEncoder&) = delete;

	// Starts a stream. `encoder` must be created for `params` (44.1 kHz stereo
	// AAC-LC) and is owned by this object. PCM already buffered in `pcm` is
	// discarded: it predates the stream and would only add latency.
	// `startTimeUs` is the clock value that corresponds to stream start.
	bool Start(std::unique_ptr<IA2dpEncoder> encoder, const AacStreamParams& params, RingBufferView& pcm,
			   uint64_t startTimeUs) noexcept;

	void Stop() noexcept;

	bool Active() const noexcept { return active_; }

	// Total PCM samples (per channel) that must have been encoded by `nowUs`,
	// including the preroll. Exposed for logging and tests.
	uint64_t TargetSamples(uint64_t nowUs) const noexcept;

	// Encodes everything the clock says is due, pulling PCM from `pcm` and
	// writing at most `maxRecords` records to `frames`. Returns false on a fatal
	// encoder error; the caller then stops the stream.
	bool Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames, size_t maxRecords) noexcept;

	const AacStreamParams& Params() const noexcept { return params_; }
	const AacStreamStats& Stats() const noexcept { return stats_; }

private:
	// Encodes one 1024-sample block, using at most `dueSamples` real samples and
	// zero-filling the rest. Returns false on a fatal encoder error.
	bool EncodeBlock(RingBufferView& pcm, RingBufferView& frames, uint32_t dueSamples) noexcept;

	std::unique_ptr<IA2dpEncoder> encoder_;
	AacStreamParams params_;
	AacStreamStats stats_;
	uint64_t startTimeUs_ = 0;
	uint64_t producedSamples_ = 0;
	bool active_ = false;
	bool discontinuity_ = false;

	uint32_t samplesPerFrame_ = 0;
	uint32_t bytesPerSample_ = 0; // interleaved 16-bit samples for all channels
	std::vector<uint8_t> pcmBlock_;  // samplesPerFrame_ * bytesPerSample_
	std::vector<uint8_t> accessUnit_; // encoder_->MaxFrameBytes()
	std::vector<uint8_t> payload_;    // Ioctl::kMaxFramePayload
};

} // namespace OpenA2dp
