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

// PCM the media clock asked for that the ring did not have - the gap a listener hears.
//
// A packet carries more samples than the clock has asked for, because it is built whole, and
// the frames past the due point are normally not in the ring yet: that audio is what the sink
// is meant to buffer, so a short tail is not a fault. Nor is the silence a stream pads while
// it fills its lead at start, which is why `caughtUp` has to be true before anything counts;
// otherwise every healthy stream would report an underrun of one lead.
constexpr uint32_t UnderrunSamples(uint32_t silentSamples, uint32_t dueSamples, bool caughtUp) noexcept
{
	if (!caughtUp || dueSamples == 0) return 0;
	return silentSamples < dueSamples ? silentSamples : dueSamples;
}

} // namespace OpenA2dp::LatencyBudget
