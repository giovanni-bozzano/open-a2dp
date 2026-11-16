# Protocol and codecs

[Architecture index](README.md) | [Encoder service](Encoder-Service.md) | [Configuration](Codec-Configuration.md)

Canonical definitions live in `common/include/OpenA2dp/`: `Avdtp.h`,
`AvdtpSource.h`, `Rtp.h`, `SbcFrame.h`, `SbcConfig.h`, `AacConfig.h`,
`VendorAptx.h`, `Aptx.h`, `AptxHd.h` and `Ldac.h`. This document explains how
they are used; the headers are authoritative.

## AVDTP source flow

The source drives:

```mermaid
flowchart LR
    DISCOVER["DISCOVER"] --> CAPS["GET_CAPABILITIES"]
    CAPS --> CONFIG["SET_CONFIGURATION"]
    CONFIG --> OPEN["OPEN"]
    OPEN --> START["START"]
```

`SUSPEND`, `CLOSE` and `ABORT` release stream state. The session also accepts a
sink-initiated configuration: a sink that already knows what it wants can
configure the stream without a discovery from the PC.

| Local SEID | Codec | Advertised | Selectable when |
|---|---|---|---|
| 1 | SBC | Always | Always |
| 2 | AAC | Always | The AAC encoder is registered |
| 3 | aptX HD | Always | The aptX HD encoder is registered and the pin can carry 24-bit stereo |
| 4 | aptX | Always | The aptX encoder is registered and the pin can carry stereo |
| 5 | LDAC | Always | The LDAC encoder is registered, the link carries its payload, and the pin is 44.1 kHz or above in stereo |
| 6 | aptX LL | Always | The aptX LL encoder is registered, the pin is 16-bit stereo, and the codec is forced |

Every SEP stays advertised so the sink can always configure a codec whose
provider is present. Registration controls selection, not advertisement.

## Selection policy

A forced codec wins when it is available. Otherwise the order is:

```text
LDAC > aptX HD > aptX > AAC > SBC
```

aptX LL is deliberately absent from that order: it trades margin for latency, so it is
chosen only when the preference forces it. Its bitstream is classic aptX's, and only the
vendor identity and the buffer levels in its IE differ.

The session scores the sink's candidate SEPs in that order. A sink that rejects a
`SET_CONFIGURATION` moves to the next candidate on the same connection, so a
capable sink that refuses the best codec falls back without a reconnect.

A codec the sink does not advertise, or whose provider is not registered, is not
selectable. A late provider registration closes a streaming lower-quality session
and rediscovers; a forced codec is left alone.

Sink-initiated configurations are validated against the matching local SEP. SBC
is checked inside the session. Vendor capabilities are matched by their vendor and
codec IDs, because AVDTP has one codec type for every vendor codec.

Configuration the endpoint cannot serve is refused during negotiation, not at
media start: `NotSupportedSamplingFrequency`, `NotSupportedChannelMode`,
`NotSupportedChannels` or `NotSupportedBitRate`. The media-start format check is a
safety net, not the normal path.

## Codec capabilities

| Codec | Rates | Channels | Input depth | Packet payload |
|---|---|---|---|---|
| SBC | 16, 32, 44.1, 48 kHz | mono, dual, stereo, joint stereo | 16-bit | RTP + one-byte SBC header + whole frames |
| AAC-LC | 32, 44.1, 48 kHz | 1 or 2 | 16-bit; 24-in-32 narrowed | RTP + one LATM `AudioMuxElement` |
| aptX | 44.1, 48 kHz | stereo | 16-bit; 24-in-32 narrowed | RTP + concatenated 4-byte frames |
| aptX HD | 44.1, 48 kHz | stereo | 24-in-32 | RTP + concatenated 6-byte frames |
| LDAC | 44.1, 48, 88.2, 96 kHz | stereo | 16-bit or 24-in-32 | RTP + the encoder's frame sequence |
| aptX LL | 44.1, 48 kHz | stereo | 16-bit | RTP + concatenated 4-byte frames |

- **SBC** uses 16 blocks, 8 subbands, joint stereo or loudness by default, with
  the bitpool capped at 53 and the sink's range. At 44.1 kHz that is a 119-byte
  frame; seven frames fit a typical media packet.
- **AAC** runs at the three rates the Media Foundation encoder accepts. It sends
  one access unit per packet, so the bitrate is capped by what one packet carries
  at the endpoint rate and the negotiated MTU.
- **aptX** carries ten milliseconds of audio per record: 120 frames at 48 kHz, or
  110 or 111 at 44.1 kHz. The 0 to 3 samples a frame cannot cover are carried into
  the next record, so the clock does not drift. Bitrate is 384 kbps at 48 kHz and
  352.8 kbps at 44.1 kHz.
- **aptX HD** also carries ten milliseconds per record, 120 frames at 48 kHz or
  110 or 111 at 44.1 kHz, at 576 kbps and 529.2 kbps.
- **aptX LL** uses the same encoder and concatenated 4-byte frames as classic aptX,
  but its record covers two milliseconds, matching the buffer target its IE asks
  for: 24 frames at 48 kHz, one packet at a time as completions free the ring.
- **LDAC** runs stereo only. Each encode call consumes 128 samples per channel;
  the encoder returns a whole media packet after two calls at 44.1 or 48 kHz, so
  a record covers 256 samples, about 5.8 ms. Its 2-DH5 framing needs a 679-byte
  media payload, so a link that cannot carry that never selects LDAC. Quality
  mode is local: 990, 660 or 330 kbps at 48 kHz, and 909, 606 or 303 at 44.1 kHz.

## Packetization

Every path sends RTP version 2 with dynamic payload type 96 and a 12-byte header.
The sequence advances per packet; the timestamp advances by the PCM samples the
packet covers. One packet is in flight at a time per stream.

- SBC adds a one-byte payload header carrying the frame count, then whole frames.
  The packet is filled up to the media MTU unless the stored frames-per-packet
  cap is lower.
- AAC carries exactly one LATM `AudioMuxElement` per packet, with the stream
  configuration inline. Records are never fragmented.
- aptX and aptX HD carry concatenated frames with no codec-specific payload
  header.
- LDAC carries the encoder's own frame sequence, headers included.
- A service record becomes one packet. The driver never packs two records into
  one packet and never splits a record.

## Media MTU

The driver requests the largest media MTU and negotiates before the media channel
exists. If the sink grants less, the driver suspends and reconfigures once per
connection at the highest bitrate the reported MTU carries. A second mismatch
leaves the stream silent instead of retrying. The accepted MTU is kept for the
rest of the connection, so a later codec switch is already capped by it, and it
is dropped when the connection ends.

## PCM depth

24-bit audio is native end to end. The endpoint lists 24-in-32 while aptX HD or
LDAC can run, and the PCM ring carries the depth of the stream Windows opened:

- **Endpoint format:** PCM subformat, `wBitsPerSample = 32`,
  `wValidBitsPerSample = 24`, valid bits left-justified with the low byte zero.
  This is the Windows convention and keeps whole bytes in the ring.
- **Ring:** bytes per frame follow the actual stream format, not the channel
  count, and the PCM ring is sized by time at the widest supported format.
- **Encoder seam:** `PcmFormat` carries the depth and `Encode` takes bytes. SBC,
  AAC and classic aptX declare 16-bit input; aptX HD declares 24-in-32.
- **Conversion:** aptX HD consumes four 24-bit samples per channel as
  sign-extended `int32`, and returns 6 bytes per call; from 24-in-32 that is one
  arithmetic shift right by 8 per sample. A 24-bit stream that negotiates SBC,
  AAC or classic aptX is narrowed with a rounded, saturating conversion.

Depth is not a setting. It follows the codec, because the Windows stream format
is an input to codec selection, not its result. A forced aptX HD pins the endpoint
list to 24-in-32; forcing classic aptX narrows it to 16-bit.

Shared mode mixes in float, so a 24-in-32 stream makes `audiodg` convert to 24
bits instead of 16; it does not make delivery bit-exact. aptX HD remains a lossy
576 kbps bitstream.

## Code map

| Area | Files |
|---|---|
| AVDTP types and source session | `common/include/OpenA2dp/Avdtp.h`, `AvdtpSource.h` |
| SBC capabilities, frame layout and selection | `common/include/OpenA2dp/SbcConfig.h`, `SbcFrame.h`, `SbcAllocation.h` |
| AAC capabilities and LATM | `common/include/OpenA2dp/AacConfig.h`, `codecs/aac/` |
| aptX and aptX HD vendor IEs | `common/include/OpenA2dp/VendorAptx.h`, `Aptx.h`, `AptxHd.h` |
| LDAC capabilities | `common/include/OpenA2dp/Ldac.h` |
| RTP header | `common/include/OpenA2dp/Rtp.h` |
| PCM depth conversion | `common/include/OpenA2dp/PcmDepth.h` |
| Negotiation and media dispatch | `driver/signaling/SignalingCodec.cpp`, `driver/media/MediaStream.cpp` |
| Packet assembly | `driver/media/MediaPackets.cpp` |
