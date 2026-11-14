#pragma once

// Testable core of OpenA2dpSvc.exe: paces, encodes and frames AAC-LC audio.
//
// The service owns the AAC media clock (docs/architecture/AAC-Service-Boundary.md section 7):
// on every WAIT_EVENT tick it tells this object how much time has elapsed, and
// the object encodes a whole 1024-sample block whenever the clock has made one
// due and its PCM is in the shared PCM ring (zero-filling only a block that is
// overdue and still short), and writes one LATM AudioMuxElement per frame into
// the shared frame ring. Everything here is clock-injected, so the pacing rules
// are unit-tested with fake time.
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
// fills. Ioctl::EffectivePrerollMs(params.prerollMs) is the same distance the
// kernel paces SBC with.

struct AacStreamParams {
	uint32_t sampleRate = 44100;
	uint16_t channels = 2;
	uint32_t bitrate = 0;
	uint32_t objectType = 0; // Aac::kObjectMpeg4Lc
	// Depth the PCM ring carries: 16 (int16) or 24 (24-in-32). AAC takes 16-bit input, so a
	// 24-bit ring is truncated to the top 16 bits per sample here.
	uint16_t bitsPerSample = 16;
	// Real time the pacing sends ahead of the clock, from the stored preference; 0 keeps the
	// default. A shorter lead lowers latency by spending the sink's jitter margin.
	uint32_t prerollMs = 0;
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
	// AAC-LC) and is owned by this object. PCM already buffered in the ring is the
	// beginning of playback - the driver keeps it and drops what really predates
	// the stream - so the first pump sends the preroll as silence in front of it.
	// `startTimeUs` is the clock value that corresponds to stream start.
	bool Start(std::unique_ptr<IA2dpEncoder> encoder, const AacStreamParams& params,
			   uint64_t startTimeUs) noexcept;

	void Stop() noexcept;

	bool Active() const noexcept { return active_; }

	// Total PCM samples (per channel) that must have been encoded by `nowUs`,
	// including the preroll. Exposed for logging and tests.
	uint64_t TargetSamples(uint64_t nowUs) const noexcept;

	// Encodes everything the clock says is due whose PCM is in `pcm`, writing at most
	// `maxRecords` records to `frames`. A block waits for its PCM until it is a block plus the
	// copy slack overdue (LatencyBudget::UnitReady). Returns false on a fatal encoder error; the
	// caller then stops the stream.
	bool Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames, size_t maxRecords) noexcept;

	const AacStreamParams& Params() const noexcept { return params_; }
	const AacStreamStats& Stats() const noexcept { return stats_; }

private:
	// Encodes one whole 1024-sample block: the lead silence still owed, then PCM from the ring,
	// zero-filling only what the ring does not hold. Returns false on a fatal encoder error.
	bool EncodeBlock(RingBufferView& pcm, RingBufferView& frames) noexcept;

	std::unique_ptr<IA2dpEncoder> encoder_;
	AacStreamParams params_;
	AacStreamStats stats_;
	uint64_t startTimeUs_ = 0;
	uint64_t producedSamples_ = 0;
	// Silence still owed in front of the first audio, set when the stream (re)starts its lead.
	uint64_t leadSilence_ = 0;
	bool leadKnown_ = false;
	// Clock skipped at a (re)start that the next record carries for the RTP timestamp.
	uint32_t pendingSkip_ = 0;
	bool active_ = false;
	bool discontinuity_ = false;

	uint32_t samplesPerFrame_ = 0;
	uint32_t bytesPerSample_ = 0; // interleaved 16-bit samples for all channels
	uint32_t ringBytesPerSample_ = 0; // interleaved samples as the ring stores them
	bool convert24_ = false;          // ring is 24-in-32, the encoder input is 16-bit
	std::vector<uint8_t> ringBlock_;  // samplesPerFrame_ * ringBytesPerSample_
	std::vector<uint8_t> pcmBlock_;  // samplesPerFrame_ * bytesPerSample_
	std::vector<uint8_t> accessUnit_; // encoder_->MaxFrameBytes()
	std::vector<uint8_t> payload_;    // Ioctl::kMaxFramePayload
};

} // namespace OpenA2dp
