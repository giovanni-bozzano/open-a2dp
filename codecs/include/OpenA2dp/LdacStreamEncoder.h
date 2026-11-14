#pragma once

// Paces, encodes and frames LDAC for OpenA2dpSvc.exe.
//
// LDAC's input unit is 128 samples per channel. The encoder takes them one unit per call and
// returns bytes only once it has a whole media packet of transport frames - two at 44.1 kHz
// high quality, so a record covers 256 samples, about 5.8 ms. The encoder's own output is the
// A2DP payload, frame headers included, so a record carries it unchanged and the RTP timestamp
// advances by exactly the samples those frames cover. Nothing is carried between records.
//
// PCM comes
// from the shared ring in the endpoint's own depth: 16-bit stereo goes to the encoder
// unchanged, and a 24-in-32 ring is narrowed per sample with the rounding the kernel SBC path
// uses. No OS calls, no threads: the service supplies the encoder, the clock and the rings.

#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/Ldac.h>
#include <OpenA2dp/RingBuffer.h>

#include <memory>
#include <stdint.h>
#include <vector>

namespace OpenA2dp {

struct LdacStreamParams {
	uint32_t sampleRate = 48000;
	uint16_t channels = 2;
	// Depth of the PCM in the ring: 16 for a 16-bit endpoint, 24 for 24-in-32.
	uint16_t bitsPerSample = Ldac::kBitsPerSample;
	// Encode quality mode the stream runs at, for the status report; the encoder itself
	// already holds it.
	uint8_t quality = Ldac::kQualityHigh;
	// Real time the pacing sends ahead of the clock, from the stored preference; 0 keeps the
	// default.
	uint32_t prerollMs = 0;
};

struct LdacStreamStats {
	uint64_t records = 0;        // records written to the frame ring
	uint64_t samples = 0;        // PCM samples per channel covered by those records
	uint64_t pcmBytes = 0;       // PCM pulled from the ring
	uint64_t silenceSamples = 0; // samples zero-filled because the ring was short
	uint64_t payloadBytes = 0;   // encoded bytes handed to the frame ring
	uint64_t droppedRecords = 0; // records the frame ring could not take
	uint64_t encodeErrors = 0;   // Encode/BuildMediaPayload failures
};

class LdacStreamEncoder {
public:
	LdacStreamEncoder() noexcept = default;
	~LdacStreamEncoder();

	LdacStreamEncoder(const LdacStreamEncoder&) = delete;
	LdacStreamEncoder& operator=(const LdacStreamEncoder&) = delete;

	// Starts a stream. `encoder` must take 16-bit stereo PCM for `params.sampleRate` and is
	// owned by this object; `params.bitsPerSample` describes the ring, which may be wider.
	// PCM already buffered in the ring is the beginning of playback, kept by the driver; the
	// first pump sends the preroll as silence in front of it.
	bool Start(std::unique_ptr<IA2dpEncoder> encoder, const LdacStreamParams& params,
			   uint64_t startTimeUs) noexcept;

	void Stop() noexcept;

	bool Active() const noexcept { return active_; }

	// Total PCM samples (per channel) that must have been encoded by `nowUs`.
	uint64_t TargetSamples(uint64_t nowUs) const noexcept;

	// Encodes everything the clock says is due whose PCM is in the ring, writing at most
	// `maxRecords` records; a unit waits for its PCM until it is overdue
	// (LatencyBudget::UnitReady). Returns false on a fatal encoder error; the caller then stops
	// the stream.
	bool Pump(uint64_t nowUs, RingBufferView& pcm, RingBufferView& frames, size_t maxRecords) noexcept;

	const LdacStreamParams& Params() const noexcept { return params_; }
	const LdacStreamStats& Stats() const noexcept { return stats_; }

private:
	// Encodes one record of SamplesPerFrame() samples: lead silence still owed first, then PCM,
	// zero-filling only what the ring does not hold.
	bool EncodeRecord(RingBufferView& pcm, RingBufferView& frames) noexcept;

	std::unique_ptr<IA2dpEncoder> encoder_;
	LdacStreamParams params_;
	LdacStreamStats stats_;
	uint64_t startTimeUs_ = 0;
	uint64_t producedSamples_ = 0;
	// Silence still owed in front of the first audio, set when the stream (re)starts its lead.
	uint64_t leadSilence_ = 0;
	bool leadKnown_ = false;
	// Clock skipped at a (re)start that the next record carries for the RTP timestamp.
	uint32_t pendingSkip_ = 0;
	// Samples encoded since the last record was written: the encoder holds them until a packet
	// is full, and the record that follows covers them all.
	uint32_t pendingSamples_ = 0;
	// Silence zero-filled for the samples the record that follows will carry, so the count
	// reaches the driver with that record instead of staying in the service's statistics.
	uint32_t pendingSilence_ = 0;
	bool active_ = false;
	bool discontinuity_ = false;

	uint32_t sampleBytes_ = 0;     // interleaved 16-bit bytes for all channels
	uint32_t ringSampleBytes_ = 0; // interleaved ring bytes for all channels (2 or 4 per sample)
	uint32_t samplesPerFrame_ = 0;
	bool convert24_ = false;
	std::vector<uint8_t> pcmRecord_;    // samplesPerFrame_ * ringSampleBytes_
	std::vector<uint8_t> narrowRecord_; // the same record as 16-bit samples, when convert24_
	std::vector<uint8_t> frameData_;    // up to kMaxFrameBytes
	std::vector<uint8_t> payload_;      // Ioctl::kMaxFramePayload
};

} // namespace OpenA2dp
