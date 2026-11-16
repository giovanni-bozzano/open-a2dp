# Kernel driver

[Architecture index](README.md) | [Timing and events](Event-Model.md) | [Encoder service](Encoder-Service.md)

`OpenA2dp.sys` is a KMDF driver with an ACX audio endpoint. It is the A2DP source
function driver for one remote sink and owns everything on the host side of that
stream: binding, AVDTP, L2CAP, the render endpoint, PCM handoff, SBC encoding and
RTP.

## Binding

Windows creates a BTHENUM node for each paired sink's AudioSink service
(`0x110B`). Its function driver is the A2DP source. OpenA2DP matches the node and
binds to it with:

- the remote A2DP sink service UUID `0x110B`,
- `BTH_PROFILE_DRIVER_INTERFACE`, and
- L2CAP PSM `0x0019` for signaling and media.

The radio, `bthport`, HFP and AVRCP are untouched. A sink that OpenA2DP does not
bind keeps the inbox source.

The A2DP package is the one whose compatible id is the service id itself,
`BTHENUM\{0000110b-...}` with no suffix. A node's compatible list also contains
entries matched through `..._GENERIC` (the enumerator's catch-all) and the node's
own `..._LOCALMFG&nnnn`; neither is an A2DP source.

## Driver switching

A sink moves between `BthA2dp` and `OpenA2DP` one node at a time.
`tools/common/SinkDriver.*` reads the node's bound service, its instance id and
its compatible list, then calls `DiInstallDevice` on the package entry for that
node. The install restarts the node, so its endpoints, stream and control device
go with it; the sink stays paired.

The switch is verified, not trusted: the tool reads the bound service and the
node's problem code back. A package that installs but loses the bind, or that
binds a driver which never starts, is a failed switch. The previous package is
installed on the same node again, and the error names where the sink ended.

A sink on `BthA2dp` has no control device, so node enumeration comes from PnP
alone. `OpenA2dpCtl driver` and the window's sink list both start there, which is
how a sink the driver does not own can be reached and switched.

The address in the BTHENUM instance id appears in natural or byte-reversed order,
so matching accepts both. The same reference names the control device, which ties
a sink in the tools to a sink in PnP.

## Start and teardown

Start creates the control device, registers incoming L2CAP handling and opens the
signaling channel. The remote Class of Device selects the headphone or speaker
endpoint category; the endpoint name is `<sink> [OpenA2DP]`.

Suspend attempts a courtesy AVDTP CLOSE (ABORT where the session cannot close),
stops media, sets the stop barrier, then drains and cancels the work items and
channels in that order. The order matters: a channel stopped before its work item
can deliver a completion into freed state, and a driver stop must return even when
the Bluetooth stack never completes. Teardown is not allowed to depend on a
reboot.

Late successful opens and accepts close their own channels after teardown. A read
request is never reused while in flight; its completion, success or failure,
signals read-idle, and stop flushes the reader's work item.

## Audio endpoint

The render circuit is a single stereo endpoint:

| Property | Value |
|---|---|
| Rates | 16, 32, 44.1, 48, 88.2 and 96 kHz |
| Depths | 16-bit, and 24-in-32 while aptX HD or LDAC can run |
| Channels | 1 or 2 |
| Buffer shape | One or two RT packets, accepted as the engine offers them |

The endpoint reports the pacing lead as its presentation delay and declares no
FIFO of its own. The audio engine therefore adds no latency the driver does not
account for; the sink's own buffering is the only part outside the report.

The ACX stream drives the AVDTP stream:

- The first non-silent audio of a run starts an idle session. Windows also runs
  the stream briefly around an endpoint rebuild, which must not count as
  playback.
- A pause, or a release that is not followed by a resume, suspends the stream
  once it has lasted the sink's pause grace (`kPauseGraceMs`, 3 s by default).
  The pacer sends silence meanwhile, so a pause shorter than the stored grace
  resumes without a START round trip; a longer grace buys more of those resumes at
  the cost of a silent stream staying up. The run edge asks the link for active
  mode, so a resume does not queue behind a sniff slot.
- Stream edges, media-channel opens and every session transition to `Open` or
  `Streaming` are applied as requests. SUSPEND exists only from `Streaming` and
  START only from `Open`, and the sink or a media failure can move the session on
  its own. A request that lands mid-handshake is applied when the response settles
  the session.
- START is retried a bounded number of times per run or renegotiation. A sink
  that keeps refusing it is not retried forever.
- While suspended, `status` reports the media as `Suspended`, not `Idle`: the
  configured stream is what a resume restarts.

A format change unplugs the jack briefly whether or not a stream is open, so
Windows drops the format it cached. A change inside that window extends it.
Volume and mute use the Windows software volume path.

## Concurrency

| Rule | Reason |
|---|---|
| Lock order is `SessionLock` then `MediaLock` | Session transitions may need the media path; the reverse would invert |
| `FormatLock` is a leaf | Negotiation takes `SessionLock` then `FormatLock`; the format IOCTL takes it alone |
| Completions and timers never take `SessionLock` or `MediaLock` | They run up to DISPATCH_LEVEL; they publish an interlocked flag and enqueue the owning work item |
| Work items never wait on the transport while holding a lock | A stalled lower transport would otherwise hold a session lock through an unbounded wait |
| The media work item never takes `SessionLock` | It requests a suspend through the signaling path instead |
| Every message owns its request and buffer | Timeout cannot leave the transport using stack memory, and a late completion has a home |

Teardown sets `StopEvent`, cancels, waits bounded on the operation's idle event
with no lock held, then flushes work items. Event waits clear the event before
re-checking state under the lock, so a transition between check and wait is not
lost.

[Timing and events](Event-Model.md) covers the timer policy and why the render
clock is not replaceable by an engine callback.

## Control device lifetime

The IOCTL surface lives on a separate control device, not on the PnP/ACX object.
The control device has its own restrictive descriptor and parallel queue, and is
created at the first managed IO start. It is quiesced when the device leaves D0
and deleted only at removal or unload: a copy with open handles can outlive a
power transition, and a name recreated too early loses the race for the rest of
the boot.

The symbolic link follows the sink's A2DP link, not the PnP node state. The tools
and the service find a sink through `\DosDevices\OpenA2dpCtl-<address>`, so the
link is published when the AVDTP signaling channel opens and withdrawn when a
remote disconnect, or a failed open, leaves it closed. A paired sink keeps its
node and control device while the Bluetooth link is down, which is why the node
state alone cannot say whether the sink is connected. The quiesce withdraws the
link before publishing the interface removal, and the next start recreates it
before publishing its arrival; a sink the start cannot reach loses it again when
its open fails. The control device object and its `\Device` name stay put for
the resume path. Its link is created and removed with the object manager,
because a control device can only take framework symbolic links before
`WdfControlFinishInitializing`; the device has long passed that point when a
sink reconnects.

Each disconnect is tagged with the signaling generation that opened its channel,
not the one current when the callback finally runs. The callback carries the
channel handle, so a media-channel disconnect, or one for a channel that has
already been replaced, is not mistaken for a signaling disconnect: the media link
can drop while the sink is still connected, and acting on either would tear down
the channel that took its place. If a newer open attempt or channel exists when
the work item runs, the disconnect is ignored as well. Only the generation that
owns the link withdraws it, so a sink that reconnects while an old disconnect is
still unwinding keeps both its channel and its place in the tools.

Rundown and an FDO reference protect outstanding handlers and handles. Removal
delivers a device-gone event to any parked service wait. `RingLock` serializes
ring map and unmap ownership; the mapping belongs to the file handle and is
released in its cleanup. [Encoder service](Encoder-Service.md) describes the
shared-memory side.

## Diagnostics

Driver records use `OA2DP_INFO`, `OA2DP_WARN` and `OA2DP_ERROR`, with level, device
tag and category. Tags are prepared as ANSI before high-IRQL callbacks. The record
format, log capture and triage are in [Diagnostics](../DIAGNOSTICS.md).

## Code map

| Area | Files |
|---|---|
| Entry, PnP, control-device creation and teardown order | `driver/Driver.cpp` |
| Endpoint, formats, render clock, PCM copy | `driver/audio/` |
| Channel setup, BRBs, sends, incoming accepts | `driver/l2cap/` |
| AVDTP session, codec choice, stream transitions | `driver/signaling/` |
| RTP assembly, SBC and service pacing | `driver/media/` |
| Control device, IOCTLs, rings, service lifetime | `driver/service/` |
| Stored settings | `driver/config/` |
| Per-file responsibilities | `driver/README.md` |
