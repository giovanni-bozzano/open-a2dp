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

} // namespace OpenA2dp::LatencyBudget
