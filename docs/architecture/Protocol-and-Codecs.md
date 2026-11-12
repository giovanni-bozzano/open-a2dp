# Protocol and codec contracts

Canonical definitions: `common/include/OpenA2dp/{Avdtp,AvdtpSource,Rtp,SbcConfig,AacConfig}.h`. Consult these instead of duplicating wire constants here.

## Negotiation
- Source flow: DISCOVER -> GET_CAPABILITIES -> SET_CONFIGURATION -> OPEN -> START; SUSPEND, CLOSE and ABORT release stream state. The state machine also accepts sink-initiated configuration.
- Local SEID 1 (SBC), SEID 2 (AAC), SEID 3 (aptX HD), SEID 4 (classic aptX) and SEID 5 (LDAC) are always advertised. AAC, the aptX codecs and LDAC are selected/accepted only while their encoder provider is registered; a late registration closes a streaming lower-quality session and rediscovers (initiator role only).
- Prefer a forced codec when available, otherwise LDAC > aptX HD > aptX > AAC > SBC. Keep scored sink candidates and fall back after rejected configuration rather than requiring reconnect.
- Both aptX codecs are vendor codecs: SEP 3 carries vendor `0x000000D7` / codec `0x0024`, SEP 4 carries vendor `0x0000004F` / codec `0x0001`, and a sink capability is matched to a SEP by those IDs in its IE, because AVDTP has one codec type for every vendor codec. HD is selectable only as 24-bit stereo and classic only as 16-bit stereo, at 44.1 or 48 kHz. HD needs the pin at 24-in-32 (a 16-bit pin falls back to a codec that fits instead of converting); classic also takes a 24-in-32 pin, because its stream encoder narrows it the way the kernel SBC path narrows the same ring. Their IEs share the layout in `VendorAptx.h`: four-byte vendor ID, two-byte codec ID and the rate/mode byte; HD appends four reserved bytes.
- LDAC is the third vendor codec: SEP 5, vendor `0x0000012D` / codec `0x00AA`, and an eight-octet IE that carries a rate mask and a channel-mode mask in separate octets. It plays 44.1, 48, 88.2 and 96 kHz stereo and is selected only when the endpoint's rate is one of those and the link's media payload reaches the 679 bytes its 2-DH5 framing needs; its quality mode is local rather than negotiated.
- Driver SBC capabilities cover 16, 32, 44.1 and 48 kHz with every channel mode, and the endpoint offers a 16-bit format at each of those rates. Default selection uses 16 blocks, 8 subbands, joint stereo/loudness and bitpool capped at 53, subject to sink support.
- AAC runs at 32, 44.1 and 48 kHz: those are the rates the Media Foundation encoder sets types for, and it refuses the rest of the IE's rate field (8 to 24 and 64 to 96 kHz) with `MF_E_INVALIDMEDIATYPE`. Format/payload limits: [service boundary](Encoder-Service.md). Endpoint format pinning and the per-codec parameter overrides are described in [codec configuration](Codec-Configuration.md).

## Packetization
- RTP: 12-byte header, version 2, payload type 96; sequence advances per packet and timestamps in PCM samples.
- SBC adds a one-byte payload header with frame count; pack whole frames within media MTU. Example: 44.1 kHz joint stereo, 16 blocks, 8 subbands, bitpool 53 yields 119-byte frames.
- AAC uses one LATM AudioMuxElement per record/RTP packet with inline stream configuration. No oversized-frame fragmentation; the negotiated bitrate is capped to what one packet carries (256 kbps at 44.1 kHz, 320 kbps at 48 kHz for the 895-byte MTU).
- Both aptX codecs keep the RTP header (12 bytes, payload type 96) and carry concatenated frames with no codec-specific payload header. HD frames are six bytes (three per channel) for four samples, so a 10 ms record is 120 frames at 48 kHz and 110 at 44.1 kHz, 576 kbps at 48 kHz. Classic frames are four bytes (two per channel) for four samples, 384 kbps at 48 kHz and 352.8 kbps at 44.1 kHz; 10 ms of 44.1 kHz is 441 samples, so the service carries 0..3 samples into the next record and a record holds 110 or 111 frames. The driver sends a record whole, as it does an AAC access unit.
- Kernel SBC uses integer-only `SbcFixed`; it and the codec library's floating-point `SbcEncoder` allocate bits with the shared `SbcAllocation` solver and write the same `SbcFrame` header, CRC and tail padding; only the filterbank, scale-factor search and quantizer stay per-encoder. No production path selects the codec library's SBC encoder (the driver encodes SBC in the kernel, the service encodes AAC, aptX and aptX HD): it is the reference implementation the tests pin frame layout against. The independent test decoder is a sanity reference, not external interoperability proof; synthesis gain is `-M`, with 73/37-sample delay for 8/4 subbands.

## PCM depth and 24-bit aptX HD

**Decision: 24-bit is native end to end.** No 16->24 expansion adapter goes into a codec
wrapper. The HD encoder already consumes 24-bit samples, so a pad would feed zeroed low
bits to its predictor and be deleted again when the real path lands, after the interface,
both encoders, both test fakes and every call site had been touched twice. SBC, AAC and
classic aptX keep their own 16-bit input; the endpoint tells the service which depth the PCM ring
carries (`RegisterEncoderOut`/`WaitEventOut.pcmValidBits`).

- Endpoint: 24-in-32, PCM subformat, `wBitsPerSample = 32`, `wValidBitsPerSample = 24`,
  valid bits left-justified with the low byte zero. That is the Windows convention and it
  keeps whole bytes in the ring; packed 3-byte 24 is legal but rarely accepted.
- Ring: the depth comes from the actual stream format, so `PinBytesPerFrame` is channels
  times the container size and `AudioReadPcm`, `ctx->PcmBuffer` and the render format table
  become depth aware. At 48 kHz stereo, 24-in-32 halves the 64 KiB ring to about 170 ms,
  still above the preroll.
- Encoder seam: `PcmFormat` carries the depth and `Encode` takes bytes. SBC, AAC and
  classic aptX declare 16 and reject anything else; HD declares 24-in-32.
- HD input: `aptxhdbtenc_encodestereo(state, pcmL[4], pcmR[4], out[2])` wants four 24-bit
  samples per channel as `int32`, sign-extended in the low 24 bits, and returns 6 bytes (3
  per channel) per call. AOSP's own glue expands packed 24-bit into that layout; from
  24-in-32 the conversion is one arithmetic shift right by 8 per sample.
- Depth is not a setting and adds no `Preference`/`EndpointFormat` field: it follows the
  codec, because the Windows stream format is an input to `SelectConfig`, not its result.
  24-in-32 is listed only while aptX HD can actually run: its provider is registered, the
  current discovery found the codec, the user has not forced another codec, and the sink
  advertises that rate. A forced HD pins the list to 24-bit. Otherwise 16-bit entries stay,
  and they follow either the sink's SBC formats or its classic aptX formats, because either
  codec can play them; forcing classic aptX narrows them to the formats it can run.

Sequencing, together with the aptX HD milestone rather than before it:

1. Vendor `encoder_for_aptxhd` and build the wrapper against the encoder's native input,
   with tests for 24-in-32 to `int32`. Done.
2. Add the depth dimension to the render format table and `AudioValidatePinFormat`, and
   take `PinBytesPerFrame` from the stream instead of the channel count. Done.
3. Pick 24-in-32 in `AudioApplyEndpointFormats` from the codec preference and the sink's
   advertised HD capabilities. Done: `DesiredFormats` follows the HD capability byte and
   the registered provider, and the 24-bit list leads the endpoint.
4. Widen `IA2dpEncoder`/`PcmFormat`, then add the 24->16 rounded downconvert on the 16-bit
   codec path only. Done: `Pcm::Narrow24To16` rounds and saturates, used by the kernel
   SBC reader and by `AacStreamEncoder` when the ring is 24-in-32.

Two limits stay, and neither is in the HD path: a configuration that falls back from HD
may leave a 24-bit stream for SBC or AAC, which are 16-bit by definition and get the
rounded downconvert; and shared mode is a float mix, so 24-in-32 makes `audiodg` convert
to 24 bits instead of 16 rather than making delivery bit-exact - aptX HD is still a lossy
576 kbps bitstream.
