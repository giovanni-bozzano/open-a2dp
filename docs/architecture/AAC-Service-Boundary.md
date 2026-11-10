# OpenA2dp — AAC driver/service boundary

## 1. Scope

`OpenA2dpSvc.exe` encodes the service-side codecs: AAC-LC, classic aptX and aptX HD. SBC
stays in the kernel and must keep working with no service installed, running or reachable.

v1 assumptions:

- one service process per machine, one registration per codec per sink device instance;
- AAC-LC, MPEG-2/4, 44.1/48 kHz, stereo, CBR at 96/128/160/192/256/320 kbps (the set the
  Media Foundation encoder accepts; the 895-byte media MTU carries 256 kbps at 44.1 kHz and
  320 kbps at 48 kHz);
- aptX HD at 44.1/48 kHz stereo, 24-in-32 input, one record of ten milliseconds per RTP
  packet (110 frames at 44.1 kHz, 120 at 48 kHz, six bytes each);
- classic aptX at 44.1/48 kHz stereo, 16-bit input, one record of ten milliseconds per RTP
  packet (110 or 111 four-byte frames at 44.1 kHz, 120 at 48 kHz; the odd sample per span is
  carried into the next record so the clock does not drift);
- one AAC LATM `AudioMuxElement` per RTP packet, 1024 PCM samples per record;
- the ACX endpoint lists 16-bit formats, and 24-in-32 while aptX HD can run; the encoders
  follow the rate Windows opened the stream with. AAC keeps 16-bit input: Media Foundation
  rejects 24-bit, 32-bit and float PCM (`0xC00D36B4`), so a 24-in-32 ring is narrowed with
  the rounded downconvert first ([PCM depth](Protocol-and-Codecs.md#pcm-depth-and-24-bit-aptx-hd)).

Invariants:

1. SBC never depends on the service. All three local SEPs remain advertised; AAC and HD
   selection is refused while their provider is absent, so negotiation falls back to a
   codec that can run.
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
| Media work item (`driver/media/`) | PASSIVE | SBC encoding or service frame drain (AAC, aptX, aptX HD), RTP send, `WAIT_EVENT` ticks |
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
records into one packet and never fragments a record (see section 12).

## 5. IOCTLs

All IOCTLs are `METHOD_BUFFERED` with `FILE_ANY_ACCESS`; access control is the control
device's SDDL (`D:P(A;;GA;;;SY)(A;;GA;;;BA)`), and the driver validates every input
struct.

| IOCTL | In | Out | Notes |
|---|---|---|---|
| `GET_VERSION` | – | `VersionInfo` | Works without registration; used by the service and the config UI |
| `REGISTER_ENCODER` | `RegisterEncoderIn` | `RegisterEncoderOut` | One registration per codec (`kCodecAac`, `kCodecAptxHd`) per device; `STATUS_DEVICE_BUSY` if that codec is taken, `STATUS_REVISION_MISMATCH` on ABI mismatch |
| `MAP_RINGS` | – | `SharedRings` | Requires registration on the same handle (`STATUS_INVALID_DEVICE_STATE`); `STATUS_DEVICE_BUSY` if already mapped |
| `WAIT_EVENT` | – | `WaitEventOut` | One pending request; a second returns `STATUS_DEVICE_BUSY`. Completed on lifecycle events or on the next media tick, cancelled via `STATUS_CANCELLED` |
| `GET_STATUS` | – | `Status` | Always available; used by `OpenA2dpCtl` and the config UI (M5b). Reports the negotiated codec, the preference, the media MTU with the AAC bitrate it carries and the counters |
| `GET_PREFERENCE` | – | `Preference` | Always available; returns the preference the next negotiation uses, including the SBC bitpool, block length, subbands and allocation, and the AAC bitrate. The CLI reads it before a change so a codec switch keeps the parameters |
| `SET_PREFERENCE` | `Preference` | – | Stored under the driver's `Parameters\Devices\<addr>` key and restored at device start (`driver/config/`). A forced codec must be SBC, AAC, aptX or aptX HD; an SBC bitpool is 0 or 2..250, blocks 0/4/8/12/16, subbands 0/4/8, allocation 0/loudness/SNR. A forced codec is refused with `STATUS_NOT_SUPPORTED` if the discovered sink lacks it. Changing the codec CLOSEs and rediscovers; changing a parameter keeps the codec and uses SUSPEND/RECONFIGURE/START. There are no rate/channel preference fields; codec settings never narrow Windows' list |
| `GET_ENDPOINT_FORMAT` | – | `EndpointFormat` | Always available; the PCM format the endpoint offers Windows. 0/0 is automatic (every sink format), otherwise the one pinned rate and channel count |
| `SET_ENDPOINT_FORMAT` | `EndpointFormat` | – | Stored per sink (`SelectedSampleRate`/`SelectedChannels`) and applied by rebuilding the listed formats in place, which makes Windows re-query the endpoint. Automatic (`0`/`0`) or 44100/48000 Hz with 1 or 2 channels; a format the discovered sink does not advertise is refused with `STATUS_NOT_SUPPORTED`. Windows plays the first listed format and ignores the Sound-settings pick, so a pinned format is listed alone |

The control queue is parallel, so a parked `WAIT_EVENT` does not block
`GET_STATUS`/`SET_PREFERENCE` on the same device (the CLI and the service both use them).
`Status` carries `negotiatedCodec` (valid before streaming starts), `preferenceFlags` and
`preferenceCodec`; `GET_STATUS` reads published atomic session snapshots rather than
waiting on `SessionLock`. `Preference` carries the SBC bitpool, block length, subbands,
allocation and AAC bitrate. `Status.mediaMtu`/`bitrateLimit` report the AAC bitrate one
media packet carries at the current endpoint rate; `sinkRates`/`sinkChannels` report the
sink's SBC PCM capabilities, `sinkCodecs` its advertised codecs, and
`endpointRate`/`endpointChannels` the endpoint format setting. A forced codec the sink does
not advertise is refused. Negotiation follows the actual Windows stream format.

ABI 2 is the current development baseline, not compatibility with an earlier layout.
Obsolete format preferences and fallback fields are removed, not reserved: `Preference`
is 36 bytes, `EndpointFormat` 16 bytes, `Status` 240 bytes, `RegisterEncoderOut` 32 bytes
and `WaitEventOut` 36 bytes. The endpoint format is its own setting, not a preference
field. Always rebuild and install the driver, tools and service together; older
development layouts are incompatible even when their version number matches.

`REGISTER_ENCODER` enables selection of that codec, not SEP advertisement. `MAP_RINGS` is
a separate call; if mapping fails, the service must close its handle to clear registration.

The registration returns the authoritative `maxFramePayload` (883 bytes), the PCM rate
and channel count, and `pcmValidBits` (16, or 24 for 24-in-32). `WAIT_EVENT` repeats the
depth, because the endpoint format can change between registration and stream start. The
service must reject a bitrate whose frames do not fit and reduce it, or report the failure
to the user.

`WAIT_EVENT` semantics:

- latched events are returned immediately; otherwise the request is parked;
- the media timer completes it with `kEventTick` while a service-encoded codec streams,
  which is the service's clock (no polling, no shared event handle);
- `kEventStreamStart`/`kEventStreamStop` carry the negotiated parameters in the same
  struct (`codec`, rate, channels, bitrate, object type, `pcmValidBits`), so the service
  configures its encoder before the first tick (a reconfigure while streaming stops and
  starts the media path, so it arrives as this pair);
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
4. `REGISTER_ENCODER` once per codec the service provides (AAC, aptX, aptX HD), with the
   service frame limit. AAC is required; each aptX codec is optional.
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
connection selects a codec whose provider is registered, down to SBC (all SEPs remain
advertised).
No driver wait depends on the service.

## 7. Pacing and timestamps

The service owns the media clock for AAC and both aptX codecs. `elapsed * rate + preroll`
(the formula the SBC path uses today) says how much audio must have been encoded by now;
whenever it exceeds
what has been produced, a whole 1024-sample block is encoded and a full block of PCM is
pulled from the ring, zero-filling only what the ring does not hold. A record always
covers 1024 samples: reading just the samples due since the last tick and padding the
rest would insert silence into the middle of the audio at the tick rate. The driver no
longer computes a target for AAC; it only drains the frame ring, one packet per
media-clock tick or send completion, and advances the RTP timestamp by each record's
`samples`. The aptX encoders follow the same clock with ten-millisecond records of whole
four-sample frames; classic aptX carries the 0..3 samples a frame cannot cover into the
next record, so the clock advances by the full span while a record's `samples` is what it
actually encoded.

Consequences:

- no drift between the ACX endpoint and the RTP clock, because both are derived from the
  same PCM;
- the frame ring stays near-empty in steady state (one tick of encoded audio);
- latency is roughly one encode tick plus the driver tick (10-20 ms), on top of the
  existing ~60 ms preroll.

## 8. Negotiation

`Avdtp::SourceSession` now holds a small array of local SEPs:

- SEID 1: SBC, always present (mandatory fallback);
- SEID 2: AAC, always advertised; selectable only while its provider is registered;
- SEID 3: aptX HD (vendor `0x000000D7` / codec `0x0024`), always advertised; selectable
  only while its provider is registered, in 24-bit stereo at the endpoint's rate;
- SEID 4: classic aptX (vendor `0x0000004F` / codec `0x0001`), always advertised; selectable
  only while its provider is registered, in 16-bit stereo at the endpoint's rate.

The session answers DISCOVER with every local SEP, answers GET_CAPABILITIES per SEID,
validates `acpSeid` against the matching local SEP, and keeps the sink-initiated
`SET_CONFIGURATION` check working for every codec (vendor capabilities are matched to a
local SEP by their vendor and codec IDs; SBC is checked inside the session, other codecs
through the `ConfigValidator` callback).

Policy: forced codec (if available) > aptX HD > aptX > AAC > SBC. `driver/signaling/SignalingCodec.cpp`'s
`SelectConfig` has the AAC branch (`AacConfig::Select`, the endpoint rate, highest bitrate the
sink accepts) and one vendor branch that tells the two aptX codecs apart by their IDs, scoring
HD 4, aptX 3, AAC 2 and SBC 1. Every scored sink SEP is kept as a candidate in descending order; when the sink
rejects a `SET_CONFIGURATION`, the session configures the next candidate for the same sink
SEP, so a capable sink that refuses the best codec falls back without a reconnect.
`AvdtpSourceTests` covers the fallback, the vendor SEP selection and the all-rejected failure.

`MediaStart` dispatches on `SelectedCodec()`: SBC keeps the current kernel encoder; AAC
validates the IE with `AacConfig::ParseConfiguredIe` and enters the service path; a vendor
configuration is parsed as aptX HD or classic aptX and checked against the pin's depth in
stereo, then enters the same service path with `ActiveCodec = kCodecAptxHd` or
`kCodecAptx`, so `WAIT_EVENT` names the codec the service must encode.

## 9. Failure handling and counters

| Failure | Driver action |
|---|---|
| No service registered at negotiation | Refuse AAC/aptX/aptX HD selection; retain SBC fallback |
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
  bitpool and frame parameters through
  `Ioctl::ValidPreference`; `maxFramePayload` is clamped to `kMaxFramePayload`.
- The service runs as LocalSystem (it must open the interface), reads PCM from the
  mapping only and never writes the PCM ring; the driver never trusts `samples` beyond
  `kAacSamplesPerFrame`.

## 11. Why the encoder is Windows'

`codecs/aac/AacCodec.cpp` is a client of `CLSID_AACMFTEncoder`, not an AAC implementation.
The service exists because that encoder is a user-mode Media Foundation transform: it
cannot be loaded in the kernel, so the driver cannot call it, and the PCM ring, the frame
ring and the `WAIT_EVENT` handshake (sections 3 to 6) are the price of using it. No AAC
encoder is compiled into this tree; what the tree owns is the IE negotiation and the LATM
framing around the encoder.

What that buys:

- the bitstream is the one Windows itself sends over A2DP AAC, so a sink that works with
  the inbox stack works here, and there is no second encoder to keep in step with the OS
  decoder it will be compared against;
- no encoder code or binary is distributed with this project, which keeps the AAC
  obligation where `Codec-Licensing.md` records it: calling the OS encoder rather than
  shipping one;
- encoder fixes and OS-version behaviour arrive with Windows Update, not with a release
  of this tree.

What it costs:

- the service, its rings, its IOCTLs and the failure modes in section 9; an in-kernel
  encoder would be a shorter data path, but not an available one;
- the OS encoder's parameter surface: 16-bit PCM input only (`0xC00D36B4` otherwise) and
  the rate/bitrate set in section 1, with no quality or afterburner knobs;
- the encoder must exist on the installed OS build. If `CoCreateInstance` fails,
  initialization fails, the service never registers, and negotiation stays on SBC; there
  is deliberately no in-tree fallback.

Bundling an AAC encoder would be a licensing decision before it is a code change: an
encoder this project ships is what the AAC pool licenses per unit (see
`Codec-Licensing.md`). If one is ever wanted — for a quality knob Media Foundation does
not expose, or for an OS build without the encoder — it becomes another `IA2dpEncoder`
created by `AacCodec::CreateEncoder` inside the service. It does not change this boundary.

## 12. Not in v1

- Fragmentation of AAC frames larger than the media MTU: the selector caps the bitrate to
  what one packet carries (256 kbps at 44.1 kHz, 320 kbps at 48 kHz for the 895-byte MTU),
  and oversize records are dropped and counted.
- VBR, mono and rates other than 44.1/48 kHz stereo.
- More than one service or more than one registration per device.
- AVRCP absolute volume and the config UI (D4, M5b/M6).
