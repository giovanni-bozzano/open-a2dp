# Latency budget and control plan

How much audio sits between Windows' render packet and the sink's speaker, which of those
stages can grow without bound, and what was changed to keep the total under control.
Numbers are for a 44.1 kHz stereo stream unless stated otherwise; 48 kHz scales by 8 %.

## Where the audio waits

| Stage | Set by | Typical |
|---|---|---|
| Windows engine buffer | ACX RT packets; size and count chosen by the engine, bounded by `AudioMaxBufferBytes` (`AudioInternal.h`) | 2 × 10 ms = 20 ms |
| Driver copy granularity | `AudioPeriodMs = 10` in `OnAudioTimer`, plus one copy per released packet in `OnSetRenderPacket` (`AudioPosition.cpp`) | 0–10 ms, shorter buffer only |
| PCM ring backlog | the lead plus `LatencyBudget::kPcmSlackMs`, `Ioctl::kPcmRingBytes` = 64 KB | under 10 ms |
| Encoder pacing lead | `Ioctl::kPrerollMs = 60` as the default, overridable per sink through `Preference.prerollMs` (20–200 ms) | 60 ms, or as configured |
| Packetization | SBC 7 frames × 128 samples at bitpool 53 (`SbcFrame::FramesPerPacket`), or fewer with `Preference.sbcFramesPerPacket`; AAC 1024 samples; aptX HD 10 ms | 10–23 ms |
| Transport | one send in flight per stream (`MediaSendReady`), L2CAP | link dependent |
| Sink buffer | not ours | 50–200 ms |

The driver adds roughly 100–120 ms before the sink's own buffer, and the pacing lead is the
single largest fixed part of it.

## How the budget is held

1. **The rings trim instead of shifting the stream late.** Both were plain FIFOs: a stall
   dropped the *new* audio and kept the old, and the encoder went on draining the backlog at
   real time, so a 500 ms transport stall meant 500 ms (up to 371 ms of PCM, 1.6 s of frames)
   of permanent offset. `OnAudioTimer` now drops the oldest frames past
   `LatencyBudget::RingLimitMs` of backlog, and `FrameRecord::Trim` drops encoded records past
   that same limit; the RTP timestamp moves past the dropped span, so the sink hears one
   gap rather than old audio. Both are counted and reported (`trimmed`: PCM bytes and encoded
   records) and logged once per second, so a stall on the service-encoded codecs is visible in
   the panel too.
2. **The lead is a stored setting.** `Preference.prerollMs` (registry
   `CodecPreferencePrerollMs`, `OpenA2dpCtl codec --preroll`) sets the pacing lead for every
   codec, 20–200 ms or 0 for the default. `MediaStart` resolves it once, the kernel pacer and
   the service's records both use it, the presentation position subtracts the same value, and
   `status.pacingLeadMs` reports it; a low-latency profile asks for 30 ms, the default stays
   60 ms for sinks that need the margin. The whole range is honoured on both paths: the trims
   bound what the rings hold past the lead, and a pacer with less PCM than its lead pads
   silence in front, so 200 ms means 200 ms and not the trim constant.
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
   `status.underruns` counts the SBC audio the media clock asked for that the PCM ring did not
   have - they are sent as silence, so the stream keeps its place on the timeline - and the
   driver logs it at most once a second with the silence duration. `LatencyBudget::UnderrunSamples`
   is that rule, pinned by `LatencyBudget_UnderrunOnlyCountsDueAudio`, and it applies only while
   the endpoint runs (`AudioStreamRunning`): a paused endpoint suspends the stream instead (see
   [driver lifecycle](Driver-Lifecycle.md#acx-audio)), so its silence is a pause, not a gap.
   Only the due window counts:
   the silence a stream pads while it fills its lead, and the frames a packet carries past the
   target, are normal and would otherwise make the number noise. That is the number the exit
   gate below means: without it a lead too short for a sink was audible but invisible on the
   codec the driver uses by default.
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
7. **The trims are a shared rule, not two constants.** `LatencyBudget::RingLimitMs` is the
   lead plus a slack, and both rings use it; `CommonTests` walks the whole 20-200 ms range and
   checks the limit always exceeds the lead, covers a packet or a record at the shortest lead,
   and fits the ring at 48 kHz stereo. A fixed limit sized for the 60 ms default had silently
   capped every longer lead on both paths, which is exactly the drift a shared rule prevents.
8. **Packetization has a setting.** SBC takes a cap (`Preference.sbcFramesPerPacket`,
   `OpenA2dpCtl codec --frames`, the settings window's `SBC frames` field), so a profile can
   trade packets per second for a shorter packet; the default stays MTU-filled. AAC and both
   aptX codecs carry one access unit per packet, so they are already at their shortest.

## What software cannot settle

Every software lever in this document is in place; what is left needs a sink and ears.

1. **The sink's own buffering is invisible.** No A2DP profile reports it; measuring the
   audible delay needs a hardware loopback (a click track against the sink's output), and T30
   is the procedure that derives it from the measured delay minus the driver-side figure.
2. **The trim policy is tested, its effect is not.** The limits and their arithmetic are
   covered in user mode (`LatencyBudget_*`, `FrameRecord_Trim*`, `PcmBacklog_*`); whether the
   sink hears one gap rather than a dropout after a stall needs hardware.
3. **A long lead costs start-up silence.** A stream with a 200 ms lead sends 200 ms of silence
   before the first real sample, because the pacer pads what the ring does not have. That is
   the lead working as configured, not a fault, but it is why the default stays at 60 ms.
4. **The sink's buffer still starts where ours ends.** The delay we report covers the lead we
   hold; how much the sink adds before it plays is unmeasured and unmeasurable in software, so
   Windows' latency figure is a lower bound rather than the audible total.

## Exit gates

Each gate is read from a report line rather than an internal field. The procedure that takes
them is [T29-T33](../HARDWARE-TESTS.md#latency), which also measures the audible delay against
the source - the one number no report line can supply.

- `latency`'s buffered term (`pcmBufferedMs + frameBufferedMs`) stays under 100 ms in steady
  state at the default 60 ms lead. A longer lead makes the rings legitimately hold more, so the
  gate is stated per lead, not once for every configuration.
- After a forced 500 ms transport stall, `trimmed` grows once and then stops, and `latency peak`
  comes back down to the lead plus a packet: the stream recovered to live audio instead of
  playing late.
- `underruns` stays 0 at 44.1 and 48 kHz stereo with the default lead, and rises as the lead is
  shortened past what the sink needs - that curve is the per-sink measurement T31 records.
- A/V offset within ±20 ms, measured on hardware with a loopback. No report line carries it:
  it compares the sink's output with the source, so it belongs to the test, not the driver.
