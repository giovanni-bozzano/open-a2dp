# Codec settings reference

Which fields exist, who chooses them, and what a change does. The design notes are in
[Codec configuration](Codec-Configuration.md); the ABI is `common/include/OpenA2dp/Ioctl.h`.

## Who chooses the PCM format

Windows does. The ACX render endpoint publishes the formats it supports, the audio engine
opens a stream with one of them (shared mode uses Windows' persisted mix format, changeable
in *Sound settings > the endpoint > Advanced*), and the driver receives that
`ACXDATAFORMAT` when the stream is created. The driver never asserts a format on its own.

The driver can publish 16-bit PCM at 44.1 and 48 kHz, mono and stereo. After discovery it
lists the intersection with the sink's SBC rate/channel capabilities. The selected format is
recorded as the device's `PinSampleRate`/`PinChannels`, drives the position and packet math
and the PCM buffers, and AVDTP then negotiates a configuration that matches it.

No preference narrows this list. Rate/channel controls and `--rate`/`--channels` have been
removed; old registry values are ignored, including global defaults. SBC is the baseline
because it is mandatory; AAC has separate capabilities and may not support every listed
PCM format. Automatic selection can use SBC in that case; forced AAC can fail rather than
silently encode a different format. A forced codec the sink does not advertise is refused.
Before discovery the list is provisional and contains all four driver formats, not proof
that the sink supports them. Rates above 48 kHz, SBC 16/32 kHz and wider PCM depths are not
implemented, so Windows does not enumerate every theoretical Bluetooth codec format.

Discovery edits the list while the device runs: `AcxDataFormatListRemoveDataFormat` and
`AcxDataFormatListAddDataFormat` change the pin's raw format list in place, ACX fires the
pin's format-change event after that, and Windows re-queries the endpoint instead of needing
it restarted. The driver keeps one `ACXDATAFORMAT` object per entry so a change is an
add/remove of objects it already owns. Unchanged lists cause no notification or repeated log.
The advertised initial default is the first available format in this order: 44100 stereo,
44100 mono, 48000 stereo, 48000 mono. A still-listed default survives discovery. Windows
persists its own mix format; that value and this advertised default are not the same state.
An active stream is never relabelled with the list default.

`KSPROPERTY_PIN_PROPOSEDATAFORMAT2` is deliberately not supported on the streaming pin:
[Windows always uses a preferred processing-mode format when this property succeeds](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/ksproperty-pin-proposedataformat2).
ACX otherwise supplies the list default, explaining a fixed 44100 stereo hardware stream
despite changing the mix format. The normal format list and initial-default queries remain
ACX-managed. `EvtAcxPinSetDataFormat` is not a Sound-settings callback:
[ACX supports it only on non-software-streaming pins](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/acx-data-formats).
The actual stream format arrives at `OnCreateStream`, is validated as supported 16-bit PCM,
and triggers AVDTP renegotiation when it differs. Missing/invalid formats are refused, not
reported as a fabricated 44100 stereo fallback.

Logs distinguish `endpoint formats` (availability, 16-bit and provisional/discovered source),
`advertised default` (initial suggestion), `stream format` (Windows' raw request), and
`endpoint format ... (actual stream)` (encoder input). The media/AVDTP logs report the
negotiated codec format, not the Windows mix format. Previously the same availability line
was printed at initialization and discovery even if nothing changed, with an extra explicit
notification despite ACX already notifying; neither is needed.

It goes no further than that on purpose. A2DP SBC defines no rate above 48 kHz, and the
Media Foundation AAC encoder this driver feeds is limited to 44.1/48 kHz, so 88.2/96/192 kHz
would leave both codecs without an encoder (the native Windows A2DP endpoint stops at the
same two rates). 16-bit is the sample format both encoders consume; wider formats would
need a sample-format conversion stage on the PCM path.

## Stored preference

One preference per sink, kept in the registry and restored when the device starts:

- `HKLM\SYSTEM\CurrentControlSet\Services\OpenA2dp\Parameters` - global default.
- `...\Parameters\Devices\<12 hex address>` - per-sink override, wins over the global key.

All values are `REG_DWORD`, `0` means "driver default", and `OpenA2dpCtl codec` writes them
through `SET_PREFERENCE` (`kAbiVersion` 1). Deleting the sink subkey returns it to auto.
The driver reads it in `OnDeviceAdd`, before the endpoint exists. The development ABI
baseline removes the obsolete format preference and fallback fields: `Preference` is
36 bytes and `Status` is 104 bytes. Rebuild the tools and AAC service with the driver;
earlier development layouts are incompatible, even if they also reported ABI 1.
`status` lists the sink's SBC PCM capability masks and discovered codecs, not a Windows
mix-format query. `codec aac` is refused on a sink that does not advertise AAC.

| Setting | Registry value | CLI | Values | Default |
|---|---|---|---|---|
| Codec choice | `CodecPreferenceFlags`, `CodecPreferenceCodec` | `codec auto\|sbc\|aac` | auto, SBC, AAC | auto (AAC when an encoder service is registered, else SBC) |
| SBC bitpool | `CodecPreferenceSbcBitpool` | `--bitpool n` | 2..250, clamped to the sink's range | 53 |
| SBC block length | `CodecPreferenceSbcBlocks` | `--blocks n` | 4, 8, 12, 16 | 16 |
| SBC subbands | `CodecPreferenceSbcSubbands` | `--subbands n` | 4, 8 | 8 |
| SBC allocation | `CodecPreferenceSbcAllocation` | `--allocation loudness\|snr` | 1 (loudness), 2 (SNR) | loudness |
| AAC bitrate | `CodecPreferenceAacBitrate` | `--bitrate bps` | 96000, 128000, 160000, 192000, 256000, 320000; clamped to the sink maximum and to the media MTU | highest the sink accepts, up to the MTU ceiling |

`CodecPreferenceAacSampleRate`, `CodecPreferenceEndpointRate` and
`CodecPreferenceEndpointChannels` are obsolete and no longer read or written.

The Media Foundation encoder accepts exactly those six CBR bitrates (224, 288 and 384 kbps
are rejected). A2DP sends one access unit per media packet, so the bitrate is also capped
by what one packet carries: at the 895-byte MTU (883 payload bytes) the ceiling is
256 kbps at 44.1 kHz and 320 kbps at 48 kHz. Measured on the encoder, the largest access
unit is the average frame plus about eleven bytes - 753 B at 256 kbps/44.1 kHz, 864 B at
320 kbps/48 kHz - so the selector keeps 16 bytes of headroom and `OpenA2dpCtl status`
prints the MTU and the ceiling it enforces. Fragmentation is not used: sources such as the
AOSP stack also send one LATM element per packet and lower the bitrate to the MTU instead.

The MTU a negotiation is capped against is the one the driver asks for, because discovery
runs before the media channel exists. If the sink grants less, the stream would start into
a bitrate one packet cannot carry, so the driver suspends and reconfigures it in place once
per connection at the highest bitrate the reported MTU carries; a second mismatch is left
silent instead of retried. The MTU the channel reported is kept for the rest of the
connection, so a codec switch - which rediscovers and negotiates again with no channel open
- is already capped by it, and it is dropped when the connection ends.

PCM depth is 16-bit everywhere: the endpoint publishes 16-bit formats, SBC carries 16-bit
samples by definition, and the Media Foundation AAC encoder rejects 24-bit, 32-bit and
float input (`0xC00D36B4`), so a 24-bit endpoint would only add a truncation step.

A value the sink does not advertise falls back to the selector's own order instead of
failing the negotiation; a bitpool is lifted to the sink's minimum and capped at its
maximum for the chosen mode.

## SBC

| Field | Supported | Chosen by |
|---|---|---|
| Sample rate | 44.1, 48 kHz | endpoint format, must be advertised by the sink |
| Channel mode | mono, dual, stereo, joint stereo | endpoint channel count, mono or joint stereo preferred; must be advertised by the sink |
| Block length | 4, 8, 12, 16 | preference, else 16 first |
| Subbands | 4, 8 | preference, else 8 first |
| Allocation | loudness, SNR | preference, else loudness first |
| Bitpool | 2..250 | preference, capped by the sink and by the mode; 53 when unset |

## AAC

| Field | Supported | Chosen by |
|---|---|---|
| Object type | MPEG-2 LC, MPEG-4 LC | MPEG-2 LC first |
| Sample rate | 44.1, 48 kHz | endpoint format |
| Channels | 1, 2 | endpoint channel count, else stereo |
| Bitrate | 96, 128, 160, 192, 256, 320 kbps | preference, capped at the sink maximum and the MTU (256 kbps at 44.1 kHz, 320 kbps at 48 kHz) |
| VBR | off | - |

## What a change does

- Codec change while streaming: CLOSE, then rediscover and configure the other codec; the
  sink connection is kept.
- Parameter change while streaming (bitpool, blocks, subbands, allocation, AAC bitrate):
  the encoder stops, SUSPEND brings the stream to `Open`, the session rebuilds the
  configuration from the sink's advertised capabilities and sends RECONFIGURE, then START.
  A refused suspend, a rejected command, or a sink-configured stream whose capabilities are
  unknown falls back to close-and-rediscover.
- Format change in Windows: applies the next time a stream is opened; the endpoint rate is
  preferred for the new negotiation.
  A stream configured for another PCM format is closed and negotiated again (RECONFIGURE
  cannot change the sampling frequency or channel count). A stream already matching it is
  left alone. Codec/parameter settings do not change the available PCM formats.
- Configuration the endpoint format cannot serve, whichever side proposed it: refused at
  negotiation, not at media start - `NotSupportedSamplingFrequency` (0xC4),
  `NotSupportedChannelMode` (0xC6), `NotSupportedChannels` (0xD9) or `NotSupportedBitRate`
  (0xDD) for a bitrate one media packet cannot carry. A sink that proposes one can retry
  with another configuration. The media-start check (`does not match the endpoint format`)
  remains as a safety net for a sink that re-proposes the same one.
