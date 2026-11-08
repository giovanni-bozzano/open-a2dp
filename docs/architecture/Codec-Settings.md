# Codec settings reference

Which fields exist, who chooses them, and what a change does. The design notes are in
[Codec configuration](Codec-Configuration.md); the ABI is `common/include/OpenA2dp/Ioctl.h`.

## Who chooses the PCM format

Windows does. The ACX render endpoint publishes the formats it supports, the audio engine
opens a stream with one of them (shared mode uses the endpoint's default format, changeable
in *Sound settings > the endpoint > Advanced*), and the driver receives that
`ACXDATAFORMAT` when the stream is created. The driver never asserts a format on its own.

The endpoint publishes 16-bit PCM at 44.1 and 48 kHz, mono and stereo: the rates every A2DP
sink must support and the channel counts both codecs can encode. The selected format is
recorded as the device's `PinSampleRate`/`PinChannels`, drives the position and packet math
and the PCM buffers, and AVDTP then negotiates a configuration that matches it.

Restricting that list is therefore the only way to make a stream run at a chosen rate or
channel count, which is what the endpoint settings below do: a pinned rate or channel mode
removes the other formats from the pin, so the engine can only open the pinned one and AVDTP
negotiates it. The list cannot be empty - the validator only accepts 44.1/48 kHz and 1/2
channels, and a preference that failed validation is discarded as a whole.

The list is filtered by what the sink advertises, too: after a discovery the driver keeps
only the formats the sink's SBC capabilities cover, so Sound settings never offers a rate or
channel count the sink cannot play. The sink's capabilities describe its PCM support for both
codecs, since SBC is mandatory. A forced codec the sink does not advertise is refused - AAC
is the only one that can be missing - and `OpenA2dpCtl codec` checks the same masks before it
sends anything, so an unusable choice is named instead of stored.

The list is also edited while the device runs: `AcxDataFormatListRemoveDataFormat` and
`AcxDataFormatListAddDataFormat` change the pin's raw format list in place, ACX fires the
pin's format-change event after that, and Windows re-queries the endpoint instead of needing
it restarted. The driver keeps one `ACXDATAFORMAT` object per entry so a change is an
add/remove of objects it already owns, and it assigns the first listed format as the
endpoint default so the pinned one is what Windows reports and opens.

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
through `SET_PREFERENCE` (`kAbiVersion` 6). Deleting the sink subkey returns it to auto.
The driver reads it in `OnDeviceAdd`, before the endpoint exists, because the endpoint rate
and channel mode decide the render formats.

A pin is refused by `SET_PREFERENCE` (`STATUS_NOT_SUPPORTED`) once the sink's advertised SBC
capabilities are known and do not cover it, rather than stored and left to fail later; the
`status` line `sink formats` lists the same capabilities, with the sink's codecs, for the
user. Before the first discovery nothing is known, so the pin is stored and the negotiation
check refuses it. `OpenA2dpCtl codec` applies the same checks against that line before it
sends the request, and refuses `codec aac` (or a forced AAC via the driver) on a sink that
does not advertise AAC.

| Setting | Registry value | CLI | Values | Default |
|---|---|---|---|---|
| Codec choice | `CodecPreferenceFlags`, `CodecPreferenceCodec` | `codec auto\|sbc\|aac` | auto, SBC, AAC | auto (AAC when an encoder service is registered, else SBC) |
| SBC bitpool | `CodecPreferenceSbcBitpool` | `--bitpool n` | 2..250, clamped to the sink's range | 53 |
| SBC block length | `CodecPreferenceSbcBlocks` | `--blocks n` | 4, 8, 12, 16 | 16 |
| SBC subbands | `CodecPreferenceSbcSubbands` | `--subbands n` | 4, 8 | 8 |
| SBC allocation | `CodecPreferenceSbcAllocation` | `--allocation loudness\|snr` | 1 (loudness), 2 (SNR) | loudness |
| AAC bitrate | `CodecPreferenceAacBitrate` | `--bitrate bps` | 96000, 128000, 160000, 192000, 256000, 320000; clamped to the sink maximum and to the media MTU | highest the sink accepts, up to the MTU ceiling |
| AAC sample rate | `CodecPreferenceAacSampleRate` | - | 0, 44100, 48000 | stored, not honoured: the stream follows the endpoint rate |
| Endpoint rate | `CodecPreferenceEndpointRate` | `--rate hz` | 44100, 48000 | both served rates |
| Endpoint channels | `CodecPreferenceEndpointChannels` | `--channels n` | 1, 2 | mono and stereo |

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
silent instead of retried.

PCM depth is 16-bit everywhere: the endpoint publishes 16-bit formats, SBC carries 16-bit
samples by definition, and the Media Foundation AAC encoder rejects 24-bit, 32-bit and
float input (`0xC00D36B4`), so a 24-bit endpoint would only add a truncation step.

A value the sink does not advertise falls back to the selector's own order instead of
failing the negotiation; a bitpool is lifted to the sink's minimum and capped at its
maximum for the chosen mode.

## SBC

| Field | Supported | Chosen by |
|---|---|---|
| Sample rate | 44.1, 48 kHz | endpoint format (mandatory sink rates) |
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
- Endpoint rate or channel mode change: the pin's format list is rebuilt in place, so
  Windows offers the new set without the device restarting. A stream configured for another
  format is closed and negotiated again (RECONFIGURE cannot change the sampling frequency or
  channel count); a stream that already runs at the new format is left alone. A pinned format
  the sink does not advertise leaves no usable configuration: the driver refuses its own
  selection and logs `the sink cannot serve the endpoint format`, so the stream fails instead
  of running at a rate the endpoint cannot produce. A sink that only supports 44.1 kHz
  therefore needs `--rate 44100`.
- Configuration the endpoint format cannot serve, whichever side proposed it: refused at
  negotiation, not at media start - `NotSupportedSamplingFrequency` (0xC4),
  `NotSupportedChannelMode` (0xC6), `NotSupportedChannels` (0xD9) or `NotSupportedBitRate`
  (0xDD) for a bitrate one media packet cannot carry. A sink that proposes one can retry
  with another configuration. The media-start check (`does not match the endpoint format`)
  remains as a safety net for a sink that re-proposes the same one.
