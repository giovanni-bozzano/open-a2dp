# Latency budget and control plan

How much audio sits between Windows' render packet and the sink's speaker, which of those
stages can grow without bound, and what was changed to keep the total under control.
Numbers are for a 44.1 kHz stereo stream unless stated otherwise; 48 kHz scales by 8 %.

## Where the audio waits

| Stage | Set by | Typical |
|---|---|---|
| Windows engine buffer | ACX RT packets; size and count chosen by the engine, bounded by `AudioMaxBufferBytes` (`AudioInternal.h`) | 2 × 10 ms = 20 ms |
| Driver copy granularity | `AudioPeriodMs = 10` in `OnAudioTimer`, plus one copy per released packet in `OnSetRenderPacket` (`AudioPosition.cpp`) | 0–10 ms, shorter buffer only |
| PCM ring backlog | `AudioPcmBacklogMs = 80`, `Ioctl::kPcmRingBytes` = 64 KB | under 10 ms |
| Encoder pacing lead | `Ioctl::kPrerollMs = 60` as the default, overridable per sink through `Preference.prerollMs` (20–200 ms) | 60 ms, or as configured |
| Packetization | SBC 7 frames × 128 samples at bitpool 53 (`SbcFrame::FramesPerPacket`), or fewer with `Preference.sbcFramesPerPacket`; AAC 1024 samples; aptX HD 10 ms | 10–23 ms |
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
   A2DP profile. Both the CLI status and the settings window's session panel show it, next to
   `underruns` (silence sent where the ring ran dry) and `trimmed` (old audio dropped after a
   stall) - the two ways a lead that is too short for this sink shows up - so a growing backlog
   and the effect of a shorter lead are visible before they are audible.
   Next to it the report shows `latency peak`: the worst lead-plus-rings value sampled on the
   audio copy tick since the stream started (`status.latencyPeakMs`, reset by the next
   `MediaStart`). A stall that trimmed the backlog before anyone polled still leaves that
   number behind, which is what makes a transient spike diagnosable after the fact.
   `status.underruns` counts the SBC packets the PCM ring could not fill - they are sent as
   silence, so the stream keeps its place on the timeline - and the driver logs them at most
   once a second with the silence duration. That is the number the exit gate below means:
   without it a lead too short for a sink was audible but invisible on the codec the driver
   uses by default.
4. **The presentation position matches the ear.** The stream is sent `PacingLeadMs` ahead of
   the media clock, so `OnGetPresentationPosition` subtracts that lead (clamped at the start)
   instead of claiming audio the sink is still buffering. Without it, an app syncing video to
   the position led the audio by 60 ms; the sink's own buffer remains unknown to us.
5. **The reported hardware latency is the lead.** ACX measures `OnGetHwLatency`'s delay from
   `NotifyPacketComplete` to the moment that packet's last sample is heard, which for this
   stream is exactly the pacing lead; it was reported as 0 and a 128-byte FIFO that nothing
   behind it corresponded to. Windows' own latency estimate now agrees with the driver's, and
   `fifoSize` is 0 because the buffered audio is the delay, not a separate FIFO.
6. **The engine's buffer shape is its own choice.** A packet count above the old limit of 2
   was refused, which fails the endpoint for a stream that asks for more (ACX's timer-driven
   mode does); the allocation is bounded by bytes instead, and the copy also runs when the
   engine releases a packet, so a buffer shorter than the 10 ms tick is no longer copied once
   per tick and dropped in between.

## Remaining risks and plan

1. **Packetization still defaults to the MTU.** SBC now takes a stored cap
   (`Preference.sbcFramesPerPacket`, `OpenA2dpCtl codec --frames`, the settings window's
   `SBC frames` field), so a profile can trade packets per second for a shorter packet; the
   default is unchanged at seven frames. AAC and both aptX codecs carry one access unit per
   packet, so they are already at their shortest packet.
2. **The sink's own buffering is invisible.** No A2DP profile reports it; measuring the
   audible delay needs a hardware loopback (a click track against the sink's output).
3. **The trims are verified in user mode only.** Both policies live in shared headers and are
   covered by `FrameRecord_Trim*` and `PcmBacklog_*` in `tests/CodecTests/`, but whether the
   sink hears one gap rather than a dropout needs hardware.
4. **The sink's buffer still starts where ours ends.** The delay we report covers the lead we
   hold; how much the sink adds before it plays is unmeasured and unmeasurable in software, so
   Windows' latency figure is a lower bound rather than the audible total.

Exit gates: steady-state `pcmBufferedMs` + `frameBufferedMs` under 100 ms; no growth after a
forced 500 ms transport stall (one trim, then back to steady state); no underruns logged at
44.1 and 48 kHz stereo; A/V offset within ±20 ms once measured on hardware.
