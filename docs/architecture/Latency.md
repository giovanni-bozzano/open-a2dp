# Latency

[Architecture index](README.md) | [Using OpenA2DP](../USING.md) | [Testing](../TESTING.md)

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
| Encoder pacing lead | `PrerollMs`, default 60 ms; aptX LL forces its own | 60 ms, or the stored value |
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

### The link is awake before a stream starts

A paused stream leaves the ACL to the stack's power management, which can put the
link in sniff. A start on such a link waits for the next sniff slot before the
AVDTP START and the first packets leave, which is heard as a delay before the
first sound. The driver asks the stack for active mode when the endpoint runs,
which is the earliest sign that sound is coming, and again right before a START
that no run preceded. The work item sends the run's ask before it takes the
session lock, so it is not queued behind whatever the session was doing. The
request is fire and forget, a refusal only means the link keeps its power state,
and only one is out at a time: a second adds nothing to a link that is already
waking.

The request cannot skip the slot it waits for. On a link that already dropped
into sniff the wake completes when the controller leaves it, which is the moment
the START leaves too, so the two finish together and the start costs the sniff
interval either way. Keeping the link out of sniff is the only way to remove
that wait; the request just makes sure nothing else is added to it.

The report's `link` line carries the state the stack reports and how long the
last wake took, so a slow start can be attributed without a log capture. The
state is read when the report is built, with a bound: a status poll never waits
on a link that is gone.

### The lead is a setting with a floor

`PrerollMs` is stored per sink and can be 20 to 200 ms. `MediaStart` resolves it
once; the kernel pacer, the service pacing and the presentation position all use
the same value.

The lead has a floor of one access unit of the running codec plus the copy slack.
Below that, a packet would be requested before the producer had time to fill it,
so every packet would be padded. At 44.1 kHz the floor is about 30 ms for aptX,
41 ms for a full SBC packet and 44 ms for AAC. A stored value below the floor is
raised, and the report says so.

aptX LL is the exception: its record and sink buffer target are 2 ms, so the
stored lead cannot describe the stream. `StartAptx` ignores `PrerollMs` and uses
the floor for a 2 ms record, 22 ms at 44.1 and 48 kHz. The report shows that
value, and `--preroll` is refused while LL is the selected codec.

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
| `latency peak` | Worst lead-plus-rings value since the counters were reset; a new stream keeps it |
| `worst gaps` | Longest interval the render path, the encoder service or a send held audio off since the counters were reset; a send also reports how much of it was transport time and how much was waiting for the media work item |

Both figures accumulate across streams like the loss counters below;
`reset-counters` is the only thing that starts them over. The send split is the
one attribution the driver can make without a debugger.
Transport time is the Bluetooth link and controller; resume time is the passive
media work item not having run. A lead above the total has margin, and which half
is large says what to fix.

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
- **The grace only keeps the link busy while the kernel paces it.** An SBC
  stream keeps sending silence for the whole grace, so a resume inside it pays
  neither START nor the sniff slot; a stream the encoder service paces sends
  nothing while paused, and its resume can still wait for a slot. Pause, resume,
  and read `link` and `worst gaps`: a send gap near a second says the link slept.
- **The reported total is a lower bound.** The sink starts playing when its own
  buffer says so, after the part we report.

## Measuring it on hardware

The procedures in [Hardware tests](../HARDWARE-TESTS.md#latency) cover:

- resetting the counters, then reading `latency`, `latency peak` and `worst gaps`
  at several leads;
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
