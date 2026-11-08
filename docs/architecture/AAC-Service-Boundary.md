# OpenA2dp — AAC driver/service boundary

## 1. Scope

`OpenA2dpSvc.exe` encodes AAC-LC only. SBC stays in the kernel and must keep working
with no service installed, running or reachable.

v1 assumptions:

- one service process per machine, one registration per sink device instance;
- AAC-LC, MPEG-2/4, 44.1/48 kHz, stereo, CBR at 96/128/160/192/256/320 kbps (the set the
  Media Foundation encoder accepts; the 895-byte media MTU carries 256 kbps at 44.1 kHz and
  320 kbps at 48 kHz);
- one LATM `AudioMuxElement` per RTP packet, 1024 PCM samples per record;
- the ACX endpoint offers 16-bit stereo at 44.1 and 48 kHz; the encoder follows the rate
  Windows opened the stream with. 16-bit is also the encoder's only input depth: Media
  Foundation rejects 24-bit, 32-bit and float PCM (`0xC00D36B4`).

Invariants:

1. SBC never depends on the service. Both local SEPs remain advertised; AAC selection
   is refused while the service is absent, so negotiation falls back to SBC.
2. The service never receives kernel pointers. The driver allocates the rings and maps
   them into the caller; the service only sees one user-mode base address.
3. Losing the service (crash, kill, hang) must not hang the driver, leak a mapping or
   block teardown. The worst case is a suspended stream that reconnects as SBC.
4. Each ring has exactly one producer and one consumer (SPSC), so the existing
   `RingBufferView` needs no kernel lock beyond the single-consumer rule.

## 2. Components and threads

| Component | IRQL | Responsibility |
|---|---|---|
| Signaling work item (`driver/signaling/`) | PASSIVE | SEP list, selection, stream lifecycle, AAC fallback |
| ACX audio timer (`driver/audio/`) | DISPATCH | Produces PCM into the PCM ring |
| Media work item (`driver/media/`) | PASSIVE | SBC encoding or AAC frame drain, RTP send, `WAIT_EVENT` ticks |
| Control device (`driver/service/`) | PASSIVE | Parallel IOCTL queue; one parked `WAIT_EVENT`; separate registration/mapping protection |
| Service main loop | user mode | `WAIT_EVENT` -> pull PCM -> encode -> write frames |

Rules:

- The IOCTLs live on a per-device **control device**, not on the PnP/ACX device object:
  ACX owns the KS request path there, and the PnP object must keep its default security
  descriptor so the audio engine can open it. The control device carries the restrictive
  SDDL and its own queue. It is created at self-managed IO start with an address-based
  named link (section 6) and deleted at suspend/removal. Rundown and an FDO reference
  protect outstanding handlers/handles; removal delivers device-gone state.
- `MediaLock` still serializes `MediaStart`/`MediaStop`/the media work item. The frame
  ring is only touched by the media work item (consumer) and the service (producer), so
  no additional kernel lock is needed.
- `WAIT_EVENT` ticks come from the passive media work item; lifecycle events also
  release waits. `WaitLock` protects the parked request against cancellation.
- Registration uses an interlocked claim; `RingLock` serializes map/unmap and records
  the owning file. Control-device rundown protects IOCTL handlers against teardown.

## 3. Shared memory

The driver allocates one contiguous non-paged block per device and maps it once, into
the process that called `MAP_RINGS`:

```
offset 0                    RingHeader PCM      (192 bytes, 64-byte aligned)
offset 192                  PCM data            (64 KiB, power of two)
offset 192 + 64 KiB         RingHeader frames   (192 bytes)
offset 192 + 64 KiB + 192   Frame data          (64 KiB, power of two)
```

`SharedRings` (the `MAP_RINGS` output) carries the offsets, capacities and the user-mode
base address, so the service builds two `RingBufferView`s without hard-coding the layout.
`pcmHeaderOffset`/`frameHeaderOffset` point at the `RingHeader`; the data offset is the
byte ring base. All offsets are 64-byte aligned.

Lifetime:

- The block is a child of the device (`WdfMemoryCreate`), so it survives as long as the
  device instance and is freed with it.
- The mapping (`IoAllocateMdl` + `MmBuildMdlForNonPagedPool` +
  `MmMapLockedPagesSpecifyCache(..., UserMode, ...)`) belongs to the file handle. It is
  unmapped with `MmUnmapLockedPages` and the MDL freed in `EvtFileCleanup`, which also
  clears the registration. A crashed service is therefore detected by the IO manager,
  not by a timeout. The mapping keeps a reference to the process that called
  `MAP_RINGS`; if teardown runs in a system thread, the driver attaches to that process
  before unmapping (a user mapping must be released in its own process context) and skips
  the unmap if the process already exited.
- Only one handle may hold a mapping per device; a second `MAP_RINGS` returns
  `STATUS_DEVICE_BUSY`.
- The mapping is cached and the rings are not paged, so the service may touch them at
  any time while the handle is open.

At AAC stream start the driver flushes the frame ring before it latches
`kEventStreamStart`, so the service never sends a stale encoded frame. The service is the
PCM ring's consumer, so it discards buffered PCM itself when it sees the event (the
driver, as producer, never moves the PCM tail).

## 4. Frame records

The service writes variable-length records into the frame ring. A record is
`FrameRecordHeader` followed by the LATM payload; `size` is exact, so the payload length
is `size - sizeof(FrameRecordHeader)` and records are never padded:

| Field | Meaning |
|---|---|
| `magic` | `kFrameRecordMagic` |
| `size` | whole record including the header |
| `samples` | PCM samples the payload covers (1024 for AAC-LC) |
| `flags` | `kFrameFlagDiscontinuity` after a gap |

Write and read are atomic per record:

- the service builds header + payload in a 2 KiB stack buffer and calls
  `RingBufferView::Write` once; if it does not fit, the record is dropped and counted
  (never a partial record);
- the driver peeks the 12-byte header and validates `magic` and `size`. If the payload
  (`size - sizeof(FrameRecordHeader)`) is larger than `kMaxFramePayload` the driver skips
  the record (`Skip(size)`) and counts it (`framesDropped`); otherwise it peeks the whole
  record into its scratch buffer and sends the payload;
- a corrupt header (bad magic or size) is not repairable from the consumer side: the
  driver stops consuming, counts an error, stops media and requests SUSPEND. The stream
  reconnects (as SBC if the service is still unhealthy); the service must reset its own
  producer index when it sees `kEventStreamStart`.

The driver adds the RTP header itself: one record = one RTP packet, `payloadType` 96,
sequence incremented per packet, timestamp advanced by `samples`. It never packs two
records into one packet and never fragments a record (see section 11).

## 5. IOCTLs

All IOCTLs are `METHOD_BUFFERED` with `FILE_ANY_ACCESS`; access control is the control
device's SDDL (`D:P(A;;GA;;;SY)(A;;GA;;;BA)`), and the driver validates every input
struct.

| IOCTL | In | Out | Notes |
|---|---|---|---|
| `GET_VERSION` | – | `VersionInfo` | Works without registration; used by the service and the config UI |
| `REGISTER_ENCODER` | `RegisterEncoderIn` | `RegisterEncoderOut` | One registration per device; `STATUS_DEVICE_BUSY` if taken, `STATUS_REVISION_MISMATCH` on ABI mismatch |
| `MAP_RINGS` | – | `SharedRings` | Requires registration on the same handle (`STATUS_INVALID_DEVICE_STATE`); `STATUS_DEVICE_BUSY` if already mapped |
| `WAIT_EVENT` | – | `WaitEventOut` | One pending request; a second returns `STATUS_DEVICE_BUSY`. Completed on lifecycle events or on the next media tick, cancelled via `STATUS_CANCELLED` |
| `GET_STATUS` | – | `Status` | Always available; used by `OpenA2dpCtl` and the config UI (M5b). Reports the negotiated codec, the preference, the media MTU with the AAC bitrate it carries and the counters |
| `GET_PREFERENCE` | – | `Preference` | Always available; returns the preference the next negotiation uses, including the SBC bitpool, block length, subbands and allocation, and the AAC bitrate. The CLI reads it before a change so a codec switch keeps the parameters |
| `SET_PREFERENCE` | `Preference` | – | Stored under the driver's `Parameters\Devices\<addr>` key and restored at device start (`driver/config/`). A forced codec must be SBC or AAC; an SBC bitpool is 0 or 2..250, blocks 0/4/8/12/16, subbands 0/4/8, allocation 0/loudness/SNR, endpoint rate 0/44100/48000 and endpoint channels 0/1/2. The endpoint pair and a forced AAC are refused with `STATUS_NOT_SUPPORTED` once the sink's advertised capabilities are known and do not cover them. Changing the codec CLOSEs and rediscovers the stream; changing a parameter keeps the codec and applies it in place through SUSPEND/RECONFIGURE/START, while an endpoint change rebuilds the pin's format list in place, so it needs no device restart |

The control queue is parallel, so a parked `WAIT_EVENT` does not block
`GET_STATUS`/`SET_PREFERENCE` on the same device (the CLI and the service both use them).
`Status` carries `negotiatedCodec` (valid before streaming starts), `preferenceFlags` and
`preferenceCodec` (both added in ABI 2); `GET_STATUS` reads published atomic session
snapshots rather than waiting on `SessionLock`. `Preference` (ABI 4) adds the SBC bitpool,
block length, subbands and allocation; its AAC sample-rate field is stored but not honoured
while negotiation follows the rate Windows opened the endpoint with. `Status` (ABI 5) adds
`mediaMtu` and `bitrateLimit`, the AAC bitrate one media packet carries at the current
endpoint rate, which is what `OpenA2dpCtl status` shows as the ceiling. ABI 6 adds
`Preference.endpointRate`/`endpointChannels`, the render formats that pin narrows, and
`Status.sinkRates`/`sinkChannels` (the formats the sink advertised), which is what the CLI
prints as `sink formats` and what a pin is checked against; the whole check is
`Ioctl::ValidPreference`. ABI 7 adds `Status.sinkCodecs`, the codecs the sink advertised:
the endpoint only offers formats the sink can play and `SET_PREFERENCE` refuses a forced
codec that is not among them, so neither Windows nor `OpenA2dpCtl` shows a choice the sink
cannot serve. ABI 8 adds `Status.pinNotServed`: the endpoint lists the sink's own formats
when the last discovery does not serve the stored rate/channel pin, and the CLI reports it
on a `fallback` line.

`REGISTER_ENCODER` enables AAC selection, not SEP advertisement. `MAP_RINGS` is a
separate call; if mapping fails, the service must close its handle to clear registration.

The registration returns the authoritative `maxFramePayload` (883 bytes) and the PCM
format the service must request from Media Foundation. The service must reject a
bitrate whose frames do not fit and reduce it, or report the failure to the user.

`WAIT_EVENT` semantics:

- latched events are returned immediately; otherwise the request is parked;
- the media timer completes it with `kEventTick` while AAC is streaming, which is the
  service's clock (no polling, no shared event handle);
- `kEventStreamStart`/`kEventStreamStop`/`kEventCodecChange` carry the negotiated AAC
  parameters in the same struct, so the service reconfigures MF before the first tick;
- `kEventDeviceGone` is delivered from `EvtFileCleanup`/device removal; the service then
  closes the handle and retries;
- the service waits with a 500 ms timeout and re-checks `GET_STATUS`, so a lost event
  cannot wedge it.

## 6. Lifecycle

Service start (once per control device; device interfaces are not supported on
control devices, so each one is named `\Device\OpenA2dpCtl<address>` with the
symbolic link `\DosDevices\OpenA2dpCtl-<address>`):

1. `QueryDosDevice(NULL, ...)` to enumerate the `OpenA2dpCtl-<12 hex address>` links,
   at start, on every `kControlArrivalInterface` arrival (`CM_Register_Notification`;
   the driver enables that interface on its PnP device only while the control device
   exists) and when a worker ends. There is no polling.
2. `CreateFile` on `\\.\OpenA2dpCtl-<address>`.
3. `GET_VERSION`; refuse to continue on an ABI mismatch.
4. `REGISTER_ENCODER` (codec AAC, service frame limit).
5. `MAP_RINGS`; build the two `RingBufferView`s.
6. `WAIT_EVENT` loop.

Stream start (driver, from the signaling work item):

1. `AacConfig::Select` produced the configured IE and the service is registered, so the
   session reaches `Configured` with `SelectedCodec() == Aac`.
2. `MediaStart` sets `MediaState::WaitingForEncoder`, flushes the frame ring, latches
   `kEventStreamStart` with the parameters, and starts the media timer.
3. The service receives the event, configures MF (rate, channels, bitrate, object type),
   prerolls ~60 ms of PCM (same target-sample formula as SBC) and starts writing records.
4. The driver sends whatever complete records exist on each tick. If the frame ring is
   empty it sends nothing; that is a jitter-buffer underrun, not an error.

Stream stop: `MediaStop` stops the timer, latches `kEventStreamStop`, resets the frame
ring, and leaves the PCM ring to the ACX endpoint.

Service crash or `CloseHandle` while streaming: the file cleanup clears the registration
and mapping, stops media, requests AVDTP SUSPEND, and closes the stream. The next
connection selects SBC if no encoder is registered (both SEPs remain advertised).
No driver wait depends on the service.

## 7. Pacing and timestamps

The service owns the media clock for AAC. `elapsed * rate + preroll` (the formula the SBC
path uses today) says how much audio must have been encoded by now; whenever it exceeds
what has been produced, a whole 1024-sample block is encoded and a full block of PCM is
pulled from the ring, zero-filling only what the ring does not hold. A record always
covers 1024 samples: reading just the samples due since the last tick and padding the
rest would insert silence into the middle of the audio at the tick rate. The driver no
longer computes a target for AAC; it only drains the frame ring, one packet per
media-clock tick or send completion, and advances the RTP timestamp by each record's
`samples`.

Consequences:

- no drift between the ACX endpoint and the RTP clock, because both are derived from the
  same PCM;
- the frame ring stays near-empty in steady state (one tick of encoded audio);
- latency is roughly one encode tick plus the driver tick (10-20 ms), on top of the
  existing ~60 ms preroll.

## 8. Negotiation

`Avdtp::SourceSession` now holds a small array of local SEPs:

- SEID 1: SBC, always present (mandatory fallback);
- SEID 2: AAC, always advertised; selectable only while an encoder is registered.

The session answers DISCOVER with every local SEP, answers GET_CAPABILITIES per SEID,
validates `acpSeid` against the matching local SEP, and keeps the sink-initiated
`SET_CONFIGURATION` check working for either codec (SBC is checked inside the session,
other codecs through the `ConfigValidator` callback).

Policy: forced codec (if available) > AAC > SBC. `driver/signaling/SignalingCodec.cpp`'s `SelectConfig` has
the AAC branch (`AacConfig::Select`, the endpoint rate, highest bitrate the sink accepts) and
scores AAC 2 / SBC 1. Every scored sink SEP is kept as a candidate in descending order;
when the sink rejects a `SET_CONFIGURATION`, the session configures the next candidate
for the same sink SEP, so an AAC-capable sink that refuses AAC falls back to SBC in the
same connection. `AvdtpSourceTests` covers the fallback and the all-rejected failure.

`MediaStart` dispatches on `SelectedCodec()`: SBC keeps the current kernel encoder;
AAC validates the IE with `AacConfig::ParseConfiguredIe`, rejects a bitrate whose frames
exceed `kMaxFramePayload`, and enters the service path.

## 9. Failure handling and counters

| Failure | Driver action |
|---|---|
| No service registered at negotiation | Refuse AAC selection; retain SBC fallback |
| Service closes the handle while streaming | Stop media, request SUSPEND, close the stream |
| No frame for 200 ms while streaming | Count `underruns`, log once per second; no state change (encoder loss is the file-cleanup event) |
| Oversize record | Skip and count `framesDropped`; suspend after 100 consecutive |
| Corrupt record header (bad magic/size) | Stop media and request SUSPEND immediately; count an error |
| Media send failure | Reported by the send completion: stop media, request SUSPEND |
| PCM ring full | Existing `AudioDroppedBytes` counter; the service is behind |

`GET_STATUS` exposes `packetsSent`, `framesDropped`, `underruns`, `sendFailures` and
`lastError` so the config UI and `dbgprint` triage agree.

## 10. Security

- The control device is SYSTEM + Administrators only; the PnP/ACX device object keeps its
  default security descriptor because the audio engine opens it, so service access is
  restricted without touching the validated audio path.
- The driver never accepts a user-mode pointer for shared memory: it allocates, locks and
  maps its own pages, so there is no `ProbeForRead`/`ProbeForWrite` surface and no
  double-mapping of arbitrary user memory.
- Input structs are validated for size, `abiVersion` and value ranges: the codec, the SBC
  bitpool and frame parameters, and the endpoint rate and channel count through
  `Ioctl::ValidPreference`; `maxFramePayload` is clamped to `kMaxFramePayload`.
- The service runs as LocalSystem (it must open the interface), reads PCM from the
  mapping only and never writes the PCM ring; the driver never trusts `samples` beyond
  `kAacSamplesPerFrame`.

## 11. Not in v1

- Fragmentation of AAC frames larger than the media MTU: the selector caps the bitrate to
  what one packet carries (256 kbps at 44.1 kHz, 320 kbps at 48 kHz for the 895-byte MTU),
  and oversize records are dropped and counted.
- VBR, mono and rates other than 44.1/48 kHz stereo.
- More than one service or more than one registration per device.
- AVRCP absolute volume and the config UI (D4, M5b/M6).
