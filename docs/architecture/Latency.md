# Latency budget and control plan

How much audio sits between Windows' render packet and the sink's speaker, which of those
stages can grow without bound, and what was changed to keep the total under control.
Numbers are for a 44.1 kHz stereo stream unless stated otherwise; 48 kHz scales by 8 %.

## Where the audio waits

| Stage | Set by | Typical |
|---|---|---|
| Windows engine buffer | ACX RT packets; `AudioMaxPacketCount = 2` (`AudioInternal.h`), packet size chosen by the engine | 2 × 10 ms = 20 ms |
| Driver copy granularity | `AudioPeriodMs = 10`, `OnAudioTimer` (`AudioPosition.cpp`) | 0–10 ms |
| PCM ring backlog | `AudioPcmBacklogMs = 80`, `Ioctl::kPcmRingBytes` = 64 KB | under 10 ms |
| Encoder pacing lead | `Ioctl::kPrerollMs = 60` as the default, overridable per sink through `Preference.prerollMs` (20–200 ms) | 60 ms, or as configured |
| Packetization | SBC 7 frames × 128 samples at bitpool 53 (`SbcFrame::FramesPerPacket`); AAC 1024 samples; aptX HD 10 ms | 10–23 ms |
| Transport | one send in flight per stream (`MediaSendReady`), L2CAP | link dependent |
| Sink buffer | not ours | 50–200 ms |

The driver adds roughly 100–120 ms before the sink's own buffer, and the pacing lead is the
single largest fixed part of it.

## Bounded now

1. **The rings trim instead of shifting the stream late.** Both were plain FIFOs: a stall
   dropped the *new* audio and kept the old, and the encoder went on draining the backlog at
   real time, so a 500 ms transport stall meant 500 ms (up to 371 ms of PCM, 1.6 s of frames)
   of permanent offset. `OnAudioTimer` now drops the oldest frames past
   `AudioPcmBacklogMs` (lead plus a packet), and `FrameRecord::Trim` drops the oldest records
   past `FrameBacklogMs`; the RTP timestamp moves past the dropped span, so the sink hears one
   gap rather than old audio. Both are counted (`status.pcmTrimmedBytes`, `framesTrimmed`
   log) and logged once per second.
2. **The lead is a stored setting.** `Preference.prerollMs` (registry
   `CodecPreferencePrerollMs`, `OpenA2dpCtl codec --preroll`) sets the pacing lead for every
   codec, 20–200 ms or 0 for the default. `MediaStart` resolves it once, the kernel pacer and
   the service's records both use it, the presentation position subtracts the same value, and
   `status.pacingLeadMs` reports it; a low-latency profile asks for 30 ms, the default stays
   60 ms for sinks that need the margin.
3. **The latency is measurable.** `status.pcmBufferedMs` and `status.frameBufferedMs` derive
   the buffered duration from the ring fills (PCM by the depth it carries, frames by the codec
   bitrate). The report's `latency` line adds the two to the pacing lead - the audio the driver
   holds ahead of live - and says so, because the sink's own buffering is not reported by any
   A2DP profile. Both the CLI status and the settings window's session panel show it, so a
   growing backlog and the effect of a shorter lead are visible before they are audible.
4. **The presentation position matches the ear.** The stream is sent `PacingLeadMs` ahead of
   the media clock, so `OnGetPresentationPosition` subtracts that lead (clamped at the start)
   instead of claiming audio the sink is still buffering. Without it, an app syncing video to
   the position led the audio by 60 ms; the sink's own buffer remains unknown to us.

## Remaining risks and plan

1. **Packetization follows the MTU and bitpool**, not a latency target: seven SBC frames is
   20 ms the sink has to buffer, up to 43 ms at a low sink bitpool. A low-latency profile can
   cap frames per packet now that the lead is configurable, since both are decided where the
   stream starts.
2. **The sink's own buffering is invisible.** No A2DP profile reports it; measuring the
   audible delay needs a hardware loopback (a click track against the sink's output).
3. **The trims are verified in user mode only.** Both policies live in shared headers and are
   covered by `FrameRecord_Trim*` and `PcmBacklog_*` in `tests/CodecTests/`, but whether the
   sink hears one gap rather than a dropout needs hardware.
4. **`OnGetHwLatency` claims a 128-byte FIFO with no delay** (`AudioPackets.cpp`). Harmless as
   long as the engine keeps its own packets queued; leave it alone unless glitches appear.

Exit gates: steady-state `pcmBufferedMs` + `frameBufferedMs` under 100 ms; no growth after a
forced 500 ms transport stall (one trim, then back to steady state); no underruns logged at
44.1 and 48 kHz stereo; A/V offset within ±20 ms once measured on hardware.
