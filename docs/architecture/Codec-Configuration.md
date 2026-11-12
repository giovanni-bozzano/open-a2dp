# Codec and endpoint configuration

What can be set, where it is stored, how a change reaches a running stream, and which formats
the endpoint offers Windows. The ABI is `common/include/OpenA2dp/Ioctl.h`.

## What the endpoint offers

Windows plays the first format the endpoint lists. It publishes the formats the ACX render
endpoint supports and opens the shared-mode stream with the leading entry; the format picked in
*Sound settings > the endpoint > Advanced* is remembered but never consulted - under Windows
11's Unified Audio Endpoint the mix format is decided when the format list changes. Hardware
shows both sides: the picked format arrives as a stream that is built and discarded, and the
graph that runs is at the first listed format. Nothing in the audio stack reports the pick, so
the only control is which formats the endpoint offers.

That is the endpoint format setting (`GET`/`SET_ENDPOINT_FORMAT`, stored per sink): automatic
offers every format the sink advertises, a pinned rate and channel count is offered alone. The
driver asserts no format on its own; Windows opens the listed one and its format reaches the
encoder when the stream runs.

The list holds one entry per rate the codecs can play - 16000, 32000, 44100, 48000, 88200 and
96000 Hz - in both channel counts, unless a format is pinned; 44100 stereo leads, and the
first entry is also the assigned default of both mode lists. A rate is listed only while the
codec that owns it can run: SBC's 16 and 32 kHz, AAC's 32 kHz and LDAC's 88.2 and 96 kHz each
follow that codec's registered provider and the sink's advertisement for it. The 16-bit
variants are listed together with the 24-in-32 ones while an encoder that reads or narrows
24-in-32 can run (see [PCM depth](Protocol-and-Codecs.md#pcm-depth-and-24-bit-aptx-hd)); the
16-bit list follows the sink's SBC rate/channel capabilities, or its classic aptX or LDAC ones
where SBC cannot play the format. Codec settings never narrow the list, except that a forced
codec leaves only what that codec can carry; only the endpoint format setting pins it.

Discovery edits the list while the device runs: `AcxDataFormatListRemoveDataFormat` and
`AcxDataFormatListAddDataFormat` change the pin's RAW and DEFAULT-mode lists in place, ACX
fires the pin's format-change event, and Windows re-queries the endpoint instead of needing it
restarted. The driver keeps one `ACXDATAFORMAT` object per entry, shared by both lists, so a
change is an add/remove of objects it already owns. An unchanged list causes no notification
and no repeated log line.

Nothing the graph does moves the list. Windows creates one stream per format it enumerates
before it opens the one it plays (four RAW and four DEFAULT probes per link-up), and every
endpoint format change invalidates the negotiated configuration, so a format taken from a
created stream once closed the session per probe. A stream's format reaches `PinSampleRate`
only when the stream runs, which feeds the encoder and the position math.

`PROPOSEDATAFORMAT` cannot decide either: Windows sweeps every rate and channel count through
it while building the graph, so a proposal is only logged. Windows does not ask this pin for
`KSPROPERTY_PIN_PROPOSEDATAFORMAT2`; the legacy property also arrives as a 64-byte bare
`KSDATAFORMAT` (audio, unspecified) that carries no selection. The requests are logged once per
device start, not per request. `EvtAcxPinSetDataFormat` is not a Sound-settings callback:
[ACX supports it only on non-software-streaming pins](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/acx-data-formats).
The stream format arrives at `OnCreateStream`, is validated as PCM the endpoint lists (16-bit,
or 24-in-32 when such a format is listed), and renegotiates AVDTP when it differs.

The endpoint offers every rate the encoders take and no more: A2DP SBC defines 16, 32, 44.1
and 48 kHz, the Media Foundation AAC encoder accepts 32, 44.1 and 48 kHz (measured - it
refuses 8 to 24 and 64 to 96 kHz with `MF_E_INVALIDMEDIATYPE`), and LDAC runs 44.1, 48, 88.2
and 96 kHz. The native Windows A2DP endpoint stops at 44.1/48, so a higher rate is offered only
while LDAC can run. 16-bit is what SBC, AAC and classic aptX consume; 24-in-32 is listed while
aptX HD or LDAC can run, and the other codecs narrow it with `Pcm::Narrow24To16` (rounded,
saturating).

Logs report `endpoint formats` (what is listed, with rate, channels and depth - provisional
before discovery, the sink's capabilities after) and `endpoint format ... (actual stream)` (the
format the encoder follows). The media and AVDTP logs report the negotiated codec format, not
the Windows mix format.

## Storage

One preference per sink, restored when the device starts; a sink with none uses the global
default:

- `HKLM\SYSTEM\CurrentControlSet\Services\OpenA2dp\Parameters` - global default.
- `...\Parameters\Devices\<12 hex address>` - per-sink override, wins over the global key.

All values are `REG_DWORD`, `0` means "driver default". `OpenA2dpCtl codec` writes the codec
fields through `SET_PREFERENCE` and `format` writes the endpoint format through
`SET_ENDPOINT_FORMAT`; deleting the sink subkey returns it to auto. The driver reads the values
in `OnDeviceAdd`, before the endpoint exists. A value the sink does not advertise is refused
instead of stored, so reading a preference back always shows what the driver will use.

| Setting | Registry value | CLI | Values | Default |
|---|---|---|---|---|
| Codec choice | `CodecPreferenceFlags`, `CodecPreferenceCodec` | `codec auto\|sbc\|aac\|aptx\|aptxhd\|ldac` | auto, SBC, AAC, aptX, aptX HD, LDAC | auto (LDAC, aptX HD, aptX, then AAC, as registered encoders and the sink allow, else SBC) |
| LDAC quality | `CodecPreferenceLdacQuality` | `--ldac-quality high\|standard\|mobile` | 0 (990 kbps), 1 (660), 2 (330) | 0 |
| SBC bitpool | `CodecPreferenceSbcBitpool` | `--bitpool n` | 2..250, clamped to the sink's range | 53 |
| SBC block length | `CodecPreferenceSbcBlocks` | `--blocks n` | 4, 8, 12, 16 | 16 |
| SBC subbands | `CodecPreferenceSbcSubbands` | `--subbands n` | 4, 8 | 8 |
| SBC allocation | `CodecPreferenceSbcAllocation` | `--allocation loudness\|snr` | 1 (loudness), 2 (SNR) | loudness |
| AAC bitrate | `CodecPreferenceAacBitrate` | `--bitrate bps` | 96000, 128000, 160000, 192000, 256000, 320000; clamped to the sink maximum and the media MTU | highest the sink accepts, up to the MTU ceiling |
| Pacing lead | `CodecPreferencePrerollMs` | `--preroll ms` | 20..200 ms, 0 = default | 60 ms |
| SBC frames per packet | `CodecPreferenceSbcFramesPerPacket` | `--frames n` | 1..15, 0 = fill the packet | fill the packet |
| Endpoint format | `SelectedSampleRate`, `SelectedChannels` | `format [auto\|<rate> <channels>]` | automatic, or any rate the endpoint offers (16000, 32000, 44100, 48000, 88200, 96000 Hz) with 1 or 2 channels the sink advertises | automatic (every sink format) |

`CodecPreferenceAacSampleRate`, `CodecPreferenceEndpointRate` and
`CodecPreferenceEndpointChannels` are obsolete and neither read nor written.

The settings window groups these fields by codec - Codec (the choice and the pacing lead every
codec shares), SBC, AAC, LDAC - so a parameter is set beside the codec it shapes; the endpoint
format keeps its own group. Every field's box explains itself when the pointer rests on it, in
the same terms as the row above it; those texts live with the controls in
`tools/OpenA2dpConfig/main.cpp`, so a field and its explanation are reviewed together.

## How a change applies

| Change | Path |
|---|---|
| Codec | The active stream CLOSEs and rediscovers; the sink keeps its connection |
| A parameter that keeps the codec (bitpool, blocks, subbands, allocation, AAC bitrate, frames) | SUSPEND to `Open`, RECONFIGURE with the configuration rebuilt from the selected SEP, START; a refused RECONFIGURE, a refused suspend or an unknown sink configuration falls back to CLOSE and rediscover |
| PCM format | The offered list is rebuilt in place and the jack is reported unplugged for `AudioJackFlapMs` (80 ms); Windows rebuilds the endpoint, the stream comes back at the new format and the session renegotiates without rediscovering |
| Pacing lead | Resolved at the next `MediaStart`; the kernel pacer (SBC) and the service's record pacing both use it, and `OnGetPresentationPosition` subtracts the same distance |

The format change needs the flap because neither a new list nor
`AcxPinNotifyDataFormatChange` moves a running stream: the audio engine closes the stream at the
old format, Windows re-creates the endpoint at the new single format, and with no stream running
the pin takes it and the session renegotiates (RECONFIGURE cannot change rate or channel count).
Audio stops for the length of the flap and a player resumes when it reopens its stream. The sink
SEP and its capabilities are remembered, so there is no discovery and no sink reconnect; only a
SEP that is no longer known, or a configuration the sink refuses, falls back to rediscovery.

Configuration the endpoint cannot serve is refused at negotiation, not at media start:
`NotSupportedSamplingFrequency` (0xC4), `NotSupportedChannelMode` (0xC6),
`NotSupportedChannels` (0xD9) or `NotSupportedBitRate` (0xDD) for a bitrate one packet cannot
carry. A sink that proposes one may retry. The media-start check (`does not match the endpoint
format`) stays as the safety net for a sink that proposes the same one again.

## Selection

`SbcConfig` and `AacConfig` are preference-aware: a stored bitpool caps the selector's request
and a stored bitrate caps the AAC bitrate, both clamped to the sink's range inside the shared
selector. Each field stands on its own - block length, subbands, allocation and frames per
packet are independent - so no named combination has to be kept in sync with a stored value. A
codec the sink does not advertise, or whose encoder provider is not registered, is refused; a
value the sink does not advertise falls back to the selector's own order rather than failing the
negotiation, and a bitpool is lifted to the sink's minimum and capped at its maximum.

| SBC field | Supported | Chosen by |
|---|---|---|
| Sample rate | 16, 32, 44.1, 48 kHz | endpoint format, must be advertised by the sink |
| Channel mode | mono, dual, stereo, joint stereo | endpoint channel count, mono or joint stereo preferred |
| Block length | 4, 8, 12, 16 | preference, else 16 first |
| Subbands | 4, 8 | preference, else 8 first |
| Allocation | loudness, SNR | preference, else loudness first |
| Bitpool | 2..250 | preference, capped by the sink and the mode; 53 when unset |

| AAC field | Supported | Chosen by |
|---|---|---|
| Object type | MPEG-2 LC, MPEG-4 LC | MPEG-2 LC first |
| Sample rate | 32, 44.1, 48 kHz | endpoint format |
| Channels | 1, 2 | endpoint channel count, else stereo |
| Bitrate | 96, 128, 160, 192, 256, 320 kbps | preference, capped at the sink maximum and the MTU |
| VBR | off | - |

AAC runs at 32, 44.1 and 48 kHz because those are the rates the encoder sets types for; it
refuses the rest of the IE's rate field (8 to 24 and 64 to 96 kHz) with
`MF_E_INVALIDMEDIATYPE`, which `AacConfig::EncoderSupportsRate` mirrors. The Media Foundation
encoder accepts exactly those six CBR bitrates (224, 288 and 384 kbps are rejected). A2DP sends one access unit per media packet, so the bitrate is also capped by what one
packet carries: at the 895-byte MTU (883 payload bytes) the ceiling is 256 kbps at 44.1 kHz,
320 kbps at 48 kHz and 192 kbps at 32 kHz. Measured on the encoder, the largest access unit is the average frame plus
about eleven bytes - 753 B at 256 kbps/44.1 kHz, 864 B at 320 kbps/48 kHz - so the selector keeps
16 bytes of headroom. Fragmentation is not used: the AOSP stack also sends one LATM element per
packet and lowers the bitrate to the MTU instead.

LDAC negotiates one of 44.1, 48, 88.2 or 96 kHz - the sink's own rate equal to the endpoint's -
and stereo only, so a 24-in-32 endpoint is narrowed per sample by the service. Its quality
mode is the one local setting here: it is never negotiated, it travels to the service in the
stream start event, and the sink is not told which one is in use.

The MTU a negotiation is capped against is the one the driver asks for, because discovery runs
before the media channel exists. If the sink grants less, the driver suspends and reconfigures in
place once per connection at the highest bitrate the reported MTU carries; a second mismatch is
left silent instead of retried. That MTU is kept for the rest of the connection, so a codec switch
- which rediscovers with no channel open - is already capped by it, and it is dropped when the
connection ends.

PCM depth is not a setting: `PcmFormat` carries it and the endpoint lists 24-in-32 only while
aptX HD can run, because the depth follows the codec. AAC's encoder rejects 24-bit, 32-bit and
float PCM (`0xC00D36B4`), SBC and classic aptX take 16-bit, so a 24-bit stream that negotiates one
of them is narrowed. Forcing classic aptX narrows the 16-bit list to the formats it can run, just
as forcing HD pins 24-in-32; the sequencing is in
[PCM depth](Protocol-and-Codecs.md#pcm-depth-and-24-bit-aptx-hd).

SBC frames per packet is the other latency knob: the MTU normally decides how many frames a packet
carries (seven at bitpool 53 and a 895-byte MTU, about 20 ms), and capping it shortens the packet.
The cap applies when the packet is built from the value resolved at stream start, so a change
rebuilds the stream. One frame is 2.9 ms of audio and the link carries one packet at a time, so it
has to send that often; four frames (11.6 ms) is still shorter than a full packet and leaves the
transport slack. AAC and both aptX codecs carry one access unit per packet and ignore the cap.

The pacing lead is set from the window's `Pacing lead` field or `codec --preroll`: every codec is
sent this far ahead of the media clock so the sink's jitter buffer fills before it runs dry. The
kernel pacer and the service's record pacing read it at stream start - the service through
`WaitEventOut.prerollMs`. A shorter lead lowers audible latency by spending jitter margin; the
60 ms default keeps the headroom a typical sink needs.

## Shared and exclusive mode

Shared mode is what ordinary applications use: they hand PCM to the Windows audio engine, which
mixes all of them into one stream at the endpoint's format - the first listed one, also what
*Sound settings > Advanced* shows as the device format. Everything played through the endpoint is
resampled to it, so a pinned format is the format the whole system plays on this sink.

Exclusive mode (WASAPI) bypasses the mixer: the application opens the pin itself, nothing is mixed
or converted and the stream is bit-exact, but only one application holds the device and the format
it asks for must be one the endpoint lists. Automatic therefore keeps every sink format available
to such a player; a pinned format limits it to that one.

## Two tools, one report

Both tools read `tools/common/StatusReport`, so the window, its **Copy** button and
`OpenA2dpCtl status` cannot show different fields. `StatusView` turns the report's lines into the
window's sectioned list; `--verbose` and the window's Details box add every field of `Status`,
`Preference` and `VersionInfo` with its hex value. `OpenA2dpCtl` remains the script path.
