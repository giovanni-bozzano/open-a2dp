# Latency

[Architecture index](README.md) | [Using OpenA2dp](../USING.md) | [Testing](../TESTING.md)

The driver adds a bounded, reported delay between the Windows render packet and
the signal leaving the PC. The sink's own buffering is outside that number and
outside software control.

Typical values below are for a 44.1 kHz stereo stream; 48 kHz scales by about
8 percent.

## The budget

| Stage | Controlled by | Typical |
|---|---|---|
| Windows engine buffer | The engine; bounded by bytes, not a packet count | 2 x 10 ms |
| Driver copy granularity | `AudioTimer`, at the packet boundary or 10 ms, whichever is sooner | one packet, at most 10 ms |
| PCM ring backlog | The lead plus the copy slack; the ring is sized by time at the widest format | under 10 ms |
| Encoder pacing lead | `PrerollMs`, default 60 ms | 60 ms, or the stored value |
| Packetization | Codec record size, SBC frames per packet | 10 to 23 ms |
| Transport | One send in flight per stream | link dependent |
| Sink buffer | Not ours | 50 to 200 ms or more |

The driver-side total is roughly 100 to 120 ms at the default lead. The pacing
lead is the single largest fixed part of it.

## Design rules

### Trim to stay live

Both rings drop old audio past a bounded backlog instead of letting the stream
run permanently late. The limit is the lead plus the copy slack, shared by the
kernel and service paths.

- The render copy drops the oldest PCM, and the frame path drops the oldest
  records. The RTP timestamp moves past the dropped span, so the sink hears one
  gap rather than stale audio followed by a permanent offset.
- A consumer that finds itself further behind the lead with its audio gone
  restarts at the lead instead of padding the gap. The service carries the skipped
  samples in the next record so the timestamp follows.
- The excess has to persist across two observations before it is dropped, because
  a copy tick landing between packets is a wobble the lead absorbs.
- A trim drops back to the lead, not to the limit, so the one gap buys an interval
  of headroom.

### The lead is a setting with a floor

`PrerollMs` is stored per sink and can be 20 to 200 ms. `MediaStart` resolves it
once; the kernel pacer, the service pacing and the presentation position all use
the same value.

The lead has a floor of one access unit of the running codec plus the copy slack.
Below that, a packet would be requested before the producer had time to fill it,
so every packet would be padded. At 44.1 kHz the floor is about 30 ms for aptX,
41 ms for a full SBC packet and 44 ms for AAC. A stored value below the floor is
raised, and the report says so.

### Report the driver side, not the whole path

`status` derives the latency from the actual ring fills:

```text
latency = pacing lead + PCM buffered + frames buffered
```

The report labels it as driver-side and states that the sink adds its own. The
presentation position subtracts the same lead, so an application syncing to the
position is not told the audio is 60 ms earlier than it is. The hardware latency
reported to ACX is the lead, and the declared FIFO is zero: the buffered audio is
the delay.

Two extra figures make a transient visible after it ended:

| Line | Meaning |
|---|---|
| `latency peak` | Worst lead-plus-rings value sampled during this stream; resets at the next `MediaStart` |
| `worst gaps` | Longest interval the render path, the encoder service or a send held audio off during this stream |

`underruns` counts packets that had to be filled with silence because PCM was
late. Only the due window counts; the intentional lead at stream start is not an
underrun, and a paused endpoint asks for no audio. `trimmed` counts the audio
dropped from a running stream to return to live; the pre-stream trim that bounds
the opening lead is not counted. `not copied` covers PCM the copy missed because
it ran past the engine buffer or the ring was full, and `service silence` covers
PCM the service had to zero-fill. Stale PCM discarded at a run or format boundary
is not a copy loss.

### Protect the producer

The service worker runs at MMCSS Pro Audio priority, so ordinary CPU load cannot
hold a record past the underrun interval. The driver stops waking the service
while the frame ring is over its trim limit: a record written then would be
trimmed away, so the stall costs one gap instead of a gap plus an encode.

### Let packetization be a choice

SBC normally fills the media MTU. `--frames` caps how many frames one packet
carries: one frame is 2.9 ms, four frames 11.6 ms, a full packet about 20 ms.
Fewer frames means more packets per second, which trades transport headroom for
latency. AAC and both aptX codecs carry one access unit per packet and are already
at their shortest. LDAC's record is its encoder packet.

## What needs a sink

- **The sink's buffer is measured, not reported.** A loopback recording against
  the source yields the audible delay; subtracting the driver-side figure leaves
  the sink's own buffering.
- **A trim is judged by ear.** The arithmetic is covered on the host; whether a
  sink hears one gap rather than a dropout belongs to a hardware run.
- **A long lead sends start-up silence.** A 200 ms lead sends up to 200 ms of
  silence before the first real sample. That is the lead working as configured,
  and why the default stays at 60 ms.
- **The reported total is a lower bound.** The sink starts playing when its own
  buffer says so, after the part we report.

## Measuring it on hardware

The procedures in [Hardware tests](../HARDWARE-TESTS.md#latency) cover:

- reading `latency`, `latency peak` and `worst gaps` at several leads;
- measuring audible delay against a loopback and subtracting the driver figure,
  which yields the sink's buffering;
- sweeping the lead down to the lowest value this sink tolerates without
  `underruns` or `trimmed` growth;
- recovering from a forced stall;
- comparing the short-packet SBC profile with the default.

A/V offset is measured in the same loopback procedure; no report line carries it.

## Code map

| Area | Files |
|---|---|
| Shared budget arithmetic | `common/include/OpenA2dp/LatencyBudget.h` |
| PCM backlog and trimming | `common/include/OpenA2dp/PcmBacklog.h`, `RingBuffer.h` |
| Record trimming | `common/include/OpenA2dp/FrameRecord.h` |
| Render copy and clock | `driver/audio/AudioPosition.cpp`, `AudioRead.cpp` |
| Kernel pacer | `driver/media/MediaPacing.cpp` |
| Service pacing | `codecs/*/` stream encoders, `service/main.cpp` |
| Presentation position and hardware latency | `driver/audio/AudioStream.cpp` |
| Report lines | `tools/common/StatusReport.cpp` |
