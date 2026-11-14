# Encoder service

[Architecture index](README.md) | [Protocol and codecs](Protocol-and-Codecs.md) | [Latency](Latency.md)

## Role

`OpenA2dpSvc.exe` encodes the codecs that do not run in the kernel: AAC-LC,
classic aptX, aptX HD and LDAC. SBC stays in the driver and works with no service
installed, running or reachable.

The split is deliberate:

- Media Foundation's AAC encoder is a user-mode transform and cannot be loaded in
  the kernel. The tree drives `CLSID_AACMFTEncoder` and owns the LATM framing, so
  no AAC encoder is distributed or maintained here.
- The aptX, aptX HD and LDAC encoders are user-mode AOSP code. Keeping them out
  of the kernel keeps CPU-heavy code and codec implementations out of the driver.
- A service crash is contained. The driver loses the encoder, not the transport,
  and SBC remains available.

The cost is the shared-memory boundary below, a control interface and the failure
modes at the end of this document. The driver keeps ownership of the A2DP
transport and RTP; the service turns PCM into encoded records on its own pump
clock.

## Shared memory

The driver allocates one contiguous non-paged block per device and maps it once
into the process that calls `MAP_RINGS`:

```text
PCM ring header
PCM data
Frame ring header
Frame data
```

`SharedRings` carries the offsets, capacities and user-mode base address, so the
service builds its two ring views without hard-coding the layout. Headers and
data are aligned for direct access.

| Property | Contract |
|---|---|
| Mapping | One handle per device; a second `MAP_RINGS` returns `STATUS_DEVICE_BUSY` |
| Lifetime | The block is a device child; the mapping belongs to the file handle and is released in file cleanup |
| Process context | Teardown releases a user mapping in the process that created it, and skips it if that process has exited |
| Paging | The rings are non-paged and cached while the handle is open |
| Producers | PCM: driver. Frames: service. Each ring has exactly one consumer |

The service never writes the PCM ring. The driver may drop the oldest unread PCM
after a stall, so a read commits only if no drop happened under its copy; no lock
spans the two processes.

## Control interface

The interface is an IOCTL surface on the per-device control device, not on the
PnP/ACX object. The control device allows SYSTEM and Administrators, has a
parallel queue and validates every input; the audio engine never opens it.

| IOCTL | Purpose |
|---|---|
| `GET_VERSION` | Driver version and limits; works without registration |
| `REGISTER_ENCODER` | Claim one codec for this handle; returns frame and PCM limits |
| `MAP_RINGS` | Map the shared rings into the calling process |
| `WAIT_EVENT` | Park until a stream event or the next media tick |
| `GET_STATUS` | Session, counters and negotiated parameters |
| `GET_PREFERENCE` | The stored codec parameters the next negotiation uses |
| `SET_PREFERENCE` | Store codec choice and parameters for this sink |
| `GET_ENDPOINT_FORMAT` | The PCM format each codec's streams use |
| `SET_ENDPOINT_FORMAT` | Pin or clear one codec's format |

Registration enables selection of a codec; it does not advertise its SEP. All
SEPs stay advertised, so a sink can always fall back to a codec whose provider is
present.

The registration reply carries the authoritative frame payload limit, PCM rate,
channel count and depth. `WAIT_EVENT` repeats the depth because the endpoint
format can change between registration and stream start. The service must reject
or reduce a bitrate whose frames do not fit.

`WAIT_EVENT` semantics:

- A latched event returns immediately; otherwise the request parks.
- While a service-encoded codec streams, the media timer completes it with
  `kEventTick`. It is raised only when a whole access unit is due beyond what the
  service has already produced, so a wake always has audio to encode.
- `kEventStreamStart` and `kEventStreamStop` carry the negotiated codec, rate,
  channels, bitrate and depth. The service configures its encoder before the
  first tick.
- A reconfigure while streaming stops and starts the media path, so it arrives as
  a start/stop pair.
- `kEventDeviceGone` is delivered on file cleanup or device removal; the service
  closes the handle and retries.
- The service waits with a timeout and re-checks `GET_STATUS`, so a lost event
  cannot wedge it.

The queue is parallel, so a parked wait does not block `GET_STATUS` or a
preference query on the same device.

## Lifecycle

Service start, once per control device:

1. Enumerate the address-named control links at start, on control-interface
   arrival and when a worker ends. No polling.
2. Open the device.
3. Check the driver version and refuse a different build.
4. Register each codec the service provides.
5. Map the rings and build the two views.
6. Enter the `WAIT_EVENT` loop.

Stream start, from the driver's signaling path:

1. Media start sets `WaitingForEncoder`, flushes the frame ring, latches the
   start event with the stream parameters and starts the media timer.
2. The service configures its encoder, sends the pacing lead as silence in front
   of the first audio as needed, and begins writing records.
3. The driver sends whatever complete records exist on each tick. An empty frame
   ring sends nothing; that is a jitter-buffer underrun, not an error.

Stream stop stops the timer, latches the stop event, resets the frame ring and
leaves the PCM ring to the ACX endpoint.

When the service releases the handle while streaming, file cleanup clears the
registration and mapping, stops media, requests a suspend and closes the stream.
The next connection selects a codec whose provider is registered, down to SBC. No
driver wait depends on the service.

The service worker registers with MMCSS (Pro Audio) for its lifetime, falling
back to highest thread priority and logging which is in force. The driver stops
waking the service while the frame ring is over its trim limit: a record written
then would only be trimmed away.

## Frame records

The service writes variable-length records into the frame ring. A record is a
fixed header followed by the codec payload; `size` is exact, and records are
never padded.

| Field | Meaning |
|---|---|
| `magic` | Record marker |
| `size` | Whole record, header included |
| `samples` | PCM samples the payload covers |
| `silenceSamples` | Samples inside `samples` that were zero-filled because the ring was short |
| `skippedSamples` | Clock skipped just before this record when the lead was restarted |
| `flags` | Discontinuity after a gap |

A record is written in one call. If it does not fit, it is dropped and counted;
a partial record is never written.

The driver validates the header before using it. A payload above the negotiated
frame limit is skipped and counted; a corrupt header stops consumption, stops
media and requests a suspend, because the producer cannot be repaired from the
consumer side. The service resets its producer index on the next stream start.

One record becomes one RTP packet. The driver adds the 12-byte RTP header, keeps
one packet in flight, advances the sequence per packet and advances the timestamp
by the record's `samples`. It never packs two records into one packet and never
fragments a record.

## Pacing

The service paces its own pump for its codecs. For each pump it computes how much
audio must have been encoded from elapsed time, the PCM rate and the lead, then
encodes whole access units:

- AAC: one 1024-sample block per record.
- aptX HD: a ten-millisecond record of four-sample frames.
- aptX: the same record length, carrying the 0 to 3 samples a frame cannot cover
  into the next record, so the clock does not drift.
- LDAC: the encoder consumes 128 samples per channel and returns a whole media
  packet; a record covers the samples those transport frames hold.

A block is encoded once its PCM is in the ring. Encoding as soon as one sample is
due would zero-fill audio that is a copy tick away and play it as a gap. Only a
block that is a whole block plus the copy slack overdue is padded, and that is
reported. A stream that falls further behind than the lead with its audio gone
restarts its lead: the clock skips the gap and the next record's
`skippedSamples` carries it, so the sink hears one gap instead of a burst of
silence that plays late for the rest of the stream.

The driver only drains the frame ring, one packet per media-clock tick or send
completion. Both sides derive the clock from the same PCM, so the endpoint and
RTP do not drift. In steady state the frame ring holds about one tick of encoded
audio. [Latency](Latency.md) covers the resulting budget and the trim policy.

## Failure handling

| Failure | Driver response |
|---|---|
| No service registered at negotiation | Refuse its codecs; SBC remains available |
| Service releases the handle while streaming | Stop media, request SUSPEND, close the stream |
| No frame for the underrun interval while the endpoint runs | Count `underruns` and log; no state change |
| Oversize record | Skip and count; suspend after a sustained run |
| Corrupt record header | Stop media and request SUSPEND immediately |
| Media send failure | Stop media and request SUSPEND |
| PCM ring full | Count the dropped endpoint PCM; the service is behind |

`GET_STATUS` exposes the counters, the last media error and the losses that cause
no state change: trimmed PCM, trimmed records, PCM the copy missed, and service
zero-fill. [Diagnostics](../DIAGNOSTICS.md) explains them.

## Security

- The control device is SYSTEM and Administrators only. The PnP/ACX object keeps
  its default descriptor because the audio engine opens it.
- The driver allocates, locks and maps its own pages. It accepts no user-mode
  pointer for shared memory, so there is no probe surface and no double mapping.
- Input structures are checked for size, version and range. The frame payload
  limit is clamped to the driver's maximum.
- The service runs as LocalSystem, reads PCM and writes frames only, and the
  driver does not trust a record's sample count beyond the codec's frame size.

## Limits

- One service process per machine and one registration per codec per device.
- AAC sends one access unit per packet, with no fragmentation. The bitrate is
  capped to what one media packet carries.
- AAC runs where the Media Foundation encoder is available; without it the
  service does not register AAC and the sink stays on a codec that works.
- Codec payloads are not split across packets and packets are not aggregated.

## Code map

| Area | Files |
|---|---|
| Service host, enumeration and workers | `service/main.cpp` |
| Control device and IOCTL dispatch | `driver/service/ServiceControl.cpp`, `ServiceIoctl.cpp` |
| Rings, mapping and file cleanup | `driver/service/ServiceRings.cpp`, `ServiceFiles.cpp` |
| Event waits and cancellation | `driver/service/ServiceEvents.cpp`, `ServiceRundown.cpp` |
| Frame records and draining | `common/include/OpenA2dp/FrameRecord.h`, `driver/service/ServiceStream.cpp`, `driver/media/MediaAac.cpp` |
| Service codecs | `codecs/aac/`, `codecs/aptx/`, `codecs/ldac/` |
| ABI | `common/include/OpenA2dp/Ioctl.h` |
