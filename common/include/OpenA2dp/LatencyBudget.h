#pragma once

#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/Types.h>

namespace OpenA2dp::LatencyBudget {

// How much audio a ring may hold, given the pacing lead the stream runs with.
//
// The lead is not backlog: it is the audio the sink is supposed to have queued, so the rings
// have to be able to hold it or the trims fight the pacer and silently shorten the configured
// lead. What is excess is anything past the lead, and the slack bounds that: enough for the
// granularity of a record or a copy tick, so a healthy stream is never trimmed, while a stall
// still costs one gap instead of a latency step that lasts until the stream stops.
//
// Both rings had a fixed limit sized for the 60 ms default, which capped every longer lead the
// preference allows; this is the one place the rule lives so it cannot drift that way again.
constexpr uint32_t kPcmSlackMs = 20;   // one packet plus a copy tick
constexpr uint32_t kFrameSlackMs = 40; // a couple of encoded records

// The PCM ring has to hold the longest lead a preference allows plus that slack at the widest
// format the endpoint offers - 96 kHz stereo in 24-in-32, eight bytes a frame - or the trims
// would fight the configured lead and a full ring would drop audio instead of buffering it.
constexpr uint32_t kPcmRingMaxBytesPerFrame = 8;
constexpr uint32_t kPcmRingMaxRate = 96000;
static_assert(Ioctl::kPcmRingBytes * 1000u >=
					  (Ioctl::kMaxPrerollMs + kPcmSlackMs) * kPcmRingMaxRate * kPcmRingMaxBytesPerFrame,
			  "the PCM ring must hold the longest pacing lead at the widest supported format");

constexpr uint32_t RingLimitMs(uint32_t leadMs, uint32_t slackMs) noexcept
{
	return leadMs + slackMs;
}

// A backlog has to persist before audio is dropped for it. One observation over the limit is a
// wobble - a copy tick landing while the pacer sits between packets, a record written just as
// the consumer empties the ring - and trimming it costs a gap the few milliseconds of extra
// latency would have paid for. The excess has to be seen on `kTrimStrikes` consecutive
// observations, which at every supported format is tens of milliseconds; a stall is far longer.
constexpr uint32_t kTrimStrikes = 2;

// Whether this observation should trim, given the caller's running count of consecutive
// over-limit observations. One observation under the limit clears the count, so only a backlog
// that persists reaches the trim.
constexpr bool TrimNow(bool overLimit, uint32_t& strikes) noexcept
{
	if (!overLimit) {
		strikes = 0;
		return false;
	}
	if (strikes < kTrimStrikes) {
		++strikes;
	}
	return strikes >= kTrimStrikes;
}

// What a trim drops back to. The trigger is the lead plus the slack, but trimming to exactly
// that leaves the ring on the edge where the next small burst trims again; dropping the slack
// too buys a full interval of headroom for the one gap it costs. With no lead configured the
// target is the trigger itself, which is the behaviour a stream without a lead had before.
constexpr uint32_t TrimTargetMs(uint32_t leadMs, uint32_t slackMs) noexcept
{
	return leadMs != 0 ? leadMs : leadMs + slackMs;
}

// The least lead a stream can run with: one access unit plus the copy tick that has to fill it.
// Below that the pacer asks for a packet's audio before the producer has had a chance to write
// it, so the deficit is padded with silence on every packet - an underrun that no sink and no
// amount of CPU could avoid. `unitSamples` is what one access unit of the negotiated codec
// carries: the SBC frames the MTU fits, or one record of the encoder service.
constexpr uint32_t LeadFloorMs(uint32_t unitSamples, uint32_t sampleRate) noexcept
{
	if (sampleRate == 0 || unitSamples == 0) {
		return 0;
	}
	return (unitSamples * 1000u + sampleRate - 1) / sampleRate + kPcmSlackMs;
}

// Where a stream picks up when its clock is `dueSamples` ahead of what it sent and the ring holds
// `bufferedSamples`: at its start, and after a stall (Stalled). The sink can hold the lead and no
// more, so the clock skips whatever is due beyond the lead or the buffered audio, whichever is
// longer - padding it would reach the sink as a burst of silence it then plays late for the rest
// of the stream. What the buffered audio cannot fill of the rest is owed as silence in front of
// it, never behind it, so the buffered audio runs straight into the live audio that follows.
struct Restart {
	uint64_t skipSamples;
	uint64_t leadSilenceSamples;
};

constexpr Restart RestartAt(uint64_t dueSamples, uint64_t bufferedSamples, uint64_t leadSamples) noexcept
{
	const uint64_t keep = bufferedSamples > leadSamples ? bufferedSamples : leadSamples;
	const uint64_t resume = dueSamples < keep ? dueSamples : keep;
	return {dueSamples - resume, resume > bufferedSamples ? resume - bufferedSamples : 0};
}

// Whether a running stream fell behind its clock by more than the sink can buffer with the audio
// for it gone - the transport or the encoder was held off, and the copy trimmed the ring back to
// the lead meanwhile. `availableSamples` is the PCM in the ring plus any lead silence still owed.
// A stream that only ran one unit short pads it (UnitReady) and keeps the sink's buffer full; this
// is for the stall a pad cannot help, where the stream restarts its lead instead (RestartAt).
constexpr bool Stalled(uint64_t dueSamples, uint64_t availableSamples, uint64_t leadSamples,
					   uint32_t unitSamples, uint32_t sampleRate) noexcept
{
	const uint64_t slack = static_cast<uint64_t>(sampleRate) * kPcmSlackMs / 1000;
	return dueSamples > leadSamples + slack && dueSamples > availableSamples + unitSamples + slack;
}

// Whether a unit (an SBC packet, an AAC block, a service record) of `unitSamples` is encoded now.
// `availableSamples` is the PCM in the ring plus any lead silence still owed; `dueSamples` is how
// far the clock is past what was sent. A unit whose PCM is there is ready. One whose PCM is not is
// normally a copy tick away, and encoding it now would zero-fill audio that is about to arrive -
// a gap in the middle of the stream. Only once the clock is a whole unit plus the copy slack past
// what was sent is the PCM missing rather than late, and the unit is padded: an underrun.
constexpr bool UnitReady(uint64_t availableSamples, uint32_t unitSamples, uint64_t dueSamples,
						 uint32_t sampleRate) noexcept
{
	return availableSamples >= unitSamples ||
		   dueSamples >= unitSamples + static_cast<uint64_t>(sampleRate) * kPcmSlackMs / 1000;
}

// PCM the media clock asked for that the ring did not have - the gap a listener hears.
//
// A packet carries more samples than the clock has asked for, because it is built whole, and
// the frames past the due point are not a gap: that audio is what the sink is meant to buffer.
// The lead a stream starts with is silence sent on purpose (RestartAt), not a short read,
// so it never reaches this count.
constexpr uint32_t UnderrunSamples(uint32_t silentSamples, uint32_t dueSamples) noexcept
{
	return silentSamples < dueSamples ? silentSamples : dueSamples;
}

// One media send's interval, split into the two waits that make it up. The first is how long the
// packet stayed with the transport - the Bluetooth link and controller - and the second is how
// long the pacer's work item took to run after the completion. The lead has to cover the sum, but
// only the first is the link: a large second figure means the machine did not schedule the driver.
// A completion that never arrived, or that predates the submit, is not a measurement.
struct SendGap {
	uint32_t transportMs;
	uint32_t resumeMs;
};

constexpr SendGap SplitSendGap(uint64_t started, uint64_t completed, uint64_t observed) noexcept
{
	if (started == 0 || completed < started || observed < completed) {
		return {};
	}
	return {static_cast<uint32_t>((completed - started) / 10000),
			static_cast<uint32_t>((observed - completed) / 10000)};
}

} // namespace OpenA2dp::LatencyBudget
