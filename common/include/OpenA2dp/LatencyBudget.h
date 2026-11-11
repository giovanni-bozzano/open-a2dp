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

constexpr uint32_t RingLimitMs(uint32_t leadMs, uint32_t slackMs) noexcept
{
	return leadMs + slackMs;
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
