#pragma once

// Backlog trim for the PCM ring shared by the kernel SBC path and the encoder service.
//
// The ring is a FIFO with no other trim: when the consumer falls behind (a stalled transport,
// a stalled service, DPC starvation) the producer's writes fail and the newest audio is lost,
// while the old audio stays. The consumer then keeps draining that backlog at real time, so the
// stream stays behind live for as long as it runs. Dropping the oldest whole frames instead
// costs one skip and returns the stream to live audio.
//
// Kernel-safe and user-mode safe: no STL, no exceptions, no allocation.

#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/Types.h>

namespace OpenA2dp::PcmBacklog {

// Bytes `backlogMs` of PCM occupies at this format. The caller picks the allowance: it has to
// cover the encoder's pacing lead plus a packet, because both are read ahead of the clock.
constexpr uint32_t LimitBytes(uint32_t sampleRate, uint32_t frameBytes, uint32_t backlogMs) noexcept
{
	return static_cast<uint32_t>(static_cast<uint64_t>(sampleRate) * frameBytes * backlogMs / 1000);
}

// The test Trim applies before it drops anything: more than the allowance plus one frame, so a
// ring that is only marginally over is left alone. Exposed separately so a caller can require
// the excess to persist across several observations (LatencyBudget::TrimNow) before it pays for
// a gap by dropping audio.
inline bool OverLimit(const RingBufferView& ring, uint32_t limitBytes, uint32_t frameBytes) noexcept
{
	if (frameBytes == 0) {
		return false;
	}
	return ring.ReadableBytes() > limitBytes + frameBytes;
}

// Drops the oldest whole frames while the ring holds more than `limitBytes`, and returns the
// number of bytes dropped (always a whole number of frames). Frames are never split, so the
// consumer still reads aligned audio, and the newest frame is always kept.
inline uint32_t Trim(RingBufferView& ring, uint32_t limitBytes, uint32_t frameBytes) noexcept
{
	if (frameBytes == 0) {
		return 0;
	}
	const uint32_t readable = ring.ReadableBytes();
	// One frame of slack: a ring that is only marginally over the allowance is left alone, so
	// trimming cannot turn a small timing wobble into a skip.
	if (readable <= limitBytes + frameBytes) {
		return 0;
	}
	uint32_t trim = readable - limitBytes;
	trim = ((trim + frameBytes - 1) / frameBytes) * frameBytes;
	if (trim > readable - frameBytes) {
		trim = readable - frameBytes;
	}
	ring.Skip(trim);
	return trim;
}

} // namespace OpenA2dp::PcmBacklog
