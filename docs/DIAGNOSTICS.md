# Diagnostics

[README](../README.md) | [Using OpenA2dp](USING.md) | [Testing](TESTING.md)

## Start with the report

`OpenA2dpCtl status` is the first source of truth. The settings window shows the
same fields in sections, and **Copy** copies the full report. The two tools share
one report implementation, so a difference between them is a bug, not a clue.

Use `--verbose` when a value is missing from the formatted lines: it appends every
field with its decimal and hex value.

### Report sections

| Lines | What they tell you |
|---|---|
| `AVDTP`, `media` | Where the A2DP session is: discovery, configuration, open, streaming, suspended |
| `endpoint` | Whether Windows is running a render stream to the endpoint |
| `sink jack` | The endpoint is reported unplugged during a format rebuild; nothing can play until it returns |
| `codec` | The codec the stream runs, or the one negotiated but not streaming |
| `media MTU` | What one media packet carries and the AAC bitrate that fits |
| `sink name`, `sink formats` | The sink's name, rates, channel counts and advertised codecs |
| `endpoint format`, `listed formats`, `refused streams` | What Windows can open; a pin, an empty list or refused formats explains silence |
| `preference` | The stored codec choice and its parameters |
| `encoder` | Whether the service has registered the encoders |
| `latency`, `latency peak`, `worst gaps` | What the driver holds ahead of live audio and what that cost |
| `pacing lead`, `buffered`, `capacity` | The lead in use and the two rings' fill |
| `underruns`, `trimmed`, `not copied`, `service silence` | Audio lost or invented on the way to the sink |
| `counters`, `last error` | Packet totals and the last transport failure |
| `driver` | The driver build and its ABI; a mismatch means the tools and driver are from different builds |

Within `worst gaps`, `send` is split into the time the packet spent in the
transport and the time from its completion until the media work item ran. Only
the first is the Bluetooth link and controller; a large second value means the
machine did not schedule the driver's passive work item, not that the link
stalled.

[Latency](architecture/Latency.md) explains how the latency numbers are formed.
The sink's own buffering is not reported by any A2DP profile, so the report only
covers the driver side.

## Counters

The counters, including `latency peak` and `worst gaps`, accumulate across
streams until `OpenA2dpCtl reset-counters` or a driver reload. A new stream
cannot hide an earlier fault.

| Counter | Meaning |
|---|---|
| `packets` | Packets handed to the transport |
| `dropped` | Service frames skipped or rejected |
| `underruns` | Packets that had to be filled with silence because PCM was late |
| `send failures` | Media sends that failed |
| `trimmed` | PCM bytes and encoded records dropped to stay near live after a stall |
| `not copied` | Endpoint PCM the copy missed because it ran late or the ring was full |
| `service silence` | Samples the service had to zero-fill |
| `refused streams` | Windows stream formats the endpoint could not serve |
| `jack flaps` | Endpoint rebuilds caused by a format change |
| `latency peak` | Worst lead-plus-rings value since the last reset |
| `worst gaps` | Longest render, service or send gap since the last reset |
| `last error` | NTSTATUS of the last media failure, or `none` |

`reset-counters` leaves the stream and its settings alone, so it can run during
playback; every counter, the peaks included, starts from zero.

Boundary cleanup is not a loss. PCM left from a previous run, or captured at a
format that is ending, is discarded on purpose and does not move `not copied`;
the pre-stream trim that bounds the opening lead is not counted in `trimmed`.
Those boundary events can still produce an `INFO` line when the discarded amount
is nonzero, but they should not make an otherwise clean stream look faulty.

## Driver and service logs

The driver logs through `DbgPrintEx` with the `IHVDRIVER` facility at info level:

```text
OpenA2dp <LEVEL> [<sink> (<address>)] <Category>: <message>
```

The service writes `[svc]` lines to the same debugger output. There is no service
log file.

The audio path reports a run's missed PCM once when it pauses or is released, and
only if that run lost any; a plain `stream paused` is the normal lifecycle line.
Normal start and stop do not add a loss line.

To capture:

1. Enable the `IHVDRIVER` mask: the `Debug Print Filter` registry value, or
   `ed nt!Kd_IHVDRIVER_Mask 0xF` with WinDbg attached. A registry change needs a
   reboot.
2. Capture with WinDbg (`.logopen <file>` before the run, `.logclose` after) or
   with Sysinternals DebugView running as administrator, with kernel capture and
   verbose kernel output enabled.
3. Reproduce the problem, then keep the capture with the test or bug report.

`INFO` records the normal lifecycle. `WARN` and `ERROR` name a failure the driver
handled or could not handle; a `WARN` is not automatically a defect. Lines that
mean a wait did not finish include `stop: still waiting`, `BRB did not complete`,
`send did not complete`, `read still pending` and `no response within`.

## Triage

| Symptom | Check first | Usual meaning |
|---|---|---|
| The sink is not in the window or `list` | `OpenA2dpCtl driver` | The sink is on `BthA2dp`, or its node has a problem |
| A codec is refused | `sink formats` and `encoder` | The sink does not advertise it, or its encoder is not registered |
| `encoder Not registered` | The encoder service state | The service is stopped, crashed, or from another build |
| No sound, session looks configured | `listed formats`, `refused streams`, `sink jack` | The endpoint has no format Windows can open, or is rebuilding |
| Occasional gaps | `underruns`, `pacing lead`, `worst gaps` | The lead is below what this sink or link needs |
| Growing `trimmed` | `latency peak`, `worst gaps` | A transport or media work-item stall; old audio was dropped to return to live |
| `send failures` or `last error` | The link and the sink | The transport failed; the session will suspend or close |
| Stream stays `Suspended` | `endpoint` | The endpoint is paused; a paused stream is suspended by design |
| Format change stops audio briefly | `jack flaps` | Windows is rebuilding the endpoint; the player reopens its stream |
| Driver switch refused | Open players | A program holds the node and cannot be paused |
| `status` cannot open the driver | The prompt | The control device needs elevation |

A service that stops while streaming leaves SBC available: all codecs remain
advertised, and the next negotiation chooses one whose encoder is registered. A
service restart is a stream interruption, not a driver failure.

## A useful bug report

Include:

- The output of `OpenA2dpCtl status --verbose` for the affected sink, or the
  window's **Copy** text.
- The sink model, the codec and rate in use, and the pacing lead.
- The driver and service log segment around the failure, with the repro steps.
- The build's commit and whether the driver was under Driver Verifier.
- Whether the sink was shared mode or exclusive mode, and what the player was.

Do not paste a log without the matching report: the report names the sink and the
session state the log lines belong to.
