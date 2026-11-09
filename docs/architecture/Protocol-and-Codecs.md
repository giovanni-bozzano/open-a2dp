# Protocol and codec contracts

Canonical definitions: `common/include/OpenA2dp/{Avdtp,AvdtpSource,Rtp,SbcConfig,AacConfig}.h`. Consult these instead of duplicating wire constants here.

## Negotiation
- Source flow: DISCOVER -> GET_CAPABILITIES -> SET_CONFIGURATION -> OPEN -> START; SUSPEND, CLOSE and ABORT release stream state. The state machine also accepts sink-initiated configuration.
- Local SEID 1 (SBC) and SEID 2 (AAC) are always advertised. AAC is selected/accepted only with a registered encoder; a late registration CLOSEs a streaming SBC session and rediscovers when the sink advertised AAC (initiator role only).
- Prefer a forced codec when available, otherwise AAC > SBC. Keep scored sink candidates and fall back after rejected configuration rather than requiring reconnect.
- Driver SBC capabilities match ACX: 44.1/48 kHz stereo-family modes. Default selection uses 16 blocks, 8 subbands, joint stereo/loudness and bitpool capped at 53, subject to sink support.
- AAC format/payload limits: [service boundary](AAC-Service-Boundary.md). Broader rates and parameter overrides are [planned](Codec-Configuration.md), not current capabilities.

## Packetization
- RTP: 12-byte header, version 2, payload type 96; sequence advances per packet and timestamps in PCM samples.
- SBC adds a one-byte payload header with frame count; pack whole frames within media MTU. Example: 44.1 kHz joint stereo, 16 blocks, 8 subbands, bitpool 53 yields 119-byte frames.
- AAC uses one LATM AudioMuxElement per record/RTP packet with inline stream configuration. No oversized-frame fragmentation; the negotiated bitrate is capped to what one packet carries (256 kbps at 44.1 kHz, 320 kbps at 48 kHz for the 895-byte MTU).
- Kernel SBC uses integer-only `SbcFixed`; it and the codec library's floating-point `SbcEncoder` allocate bits with the shared `SbcAllocation` solver. No production path selects the codec library's SBC encoder (the driver encodes SBC in the kernel, the service encodes AAC only): it is the reference implementation the tests pin frame layout against. The independent test decoder is a sanity reference, not external interoperability proof; synthesis gain is `-M`, with 73/37-sample delay for 8/4 subbands.
