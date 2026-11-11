# Codec settings reference

Which fields exist, who chooses them, and what a change does. The design notes are in
[Codec configuration](Codec-Configuration.md); the ABI is `common/include/OpenA2dp/Ioctl.h`.

## Who chooses the PCM format

Windows plays the first format the endpoint lists. It publishes the formats the ACX render
endpoint supports and opens the shared-mode stream with the leading entry; the format picked
in *Sound settings > the endpoint > Advanced* is remembered by Windows but never consulted
for this endpoint - under Windows 11's Unified Audio Endpoint the mix format is decided when
the endpoint's format list changes. Hardware shows both sides: the picked format arrives as
a stream that is built and discarded, and the graph that runs is at the first listed format.
Nothing in the audio stack reports the pick, so the only way to control it is which formats
the endpoint offers.

The driver therefore decides which formats it offers, through the endpoint format setting
(`GET`/`SET_ENDPOINT_FORMAT`, stored per sink): automatic offers every format the sink
advertises, a pinned rate and channel count is offered alone. The driver still asserts no
format on its own - Windows opens the listed one and its format reaches the encoder when the
stream runs.

The driver can publish PCM at 44.1 and 48 kHz, mono and stereo. It lists the 16-bit
variants together with the 24-in-32 ones while aptX HD can run (see [PCM
depth](Protocol-and-Codecs.md#pcm-depth-and-24-bit-aptx-hd)); the 16-bit list follows the
sink's SBC rate/channel capabilities, or its classic aptX ones where SBC cannot play the
format. The selected format is
recorded as the device's `PinSampleRate`/`PinChannels`/`PinValidBits`, drives the position
and packet math and the PCM buffers, and AVDTP then negotiates a configuration that matches
it.

Codec settings never narrow this list; only the endpoint format setting does. The old
`--rate`/`--channels` flags are gone (use `format`), and the registry values they wrote are
ignored, including global defaults. SBC is the baseline
because it is mandatory; AAC has separate capabilities and may not support every listed
PCM format. Automatic selection can use SBC in that case; forced AAC can fail rather than
silently encode a different format. A forced codec the sink does not advertise is refused.
Before discovery the list is provisional and contains all four driver formats, not proof
that the sink supports them. Rates above 48 kHz, SBC 16/32 kHz and wider PCM depths are not
implemented, so Windows does not enumerate every theoretical Bluetooth codec format.

Discovery edits the list while the device runs: `AcxDataFormatListRemoveDataFormat` and
`AcxDataFormatListAddDataFormat` change the pin's RAW list and its DEFAULT-mode list in place,
ACX fires the pin's format-change event after that, and Windows re-queries the endpoint
instead of needing it restarted. The driver keeps one `ACXDATAFORMAT` object per entry, shared
by both lists, so a change is an add/remove of objects it already owns and both modes always
list the same formats with the same default. Unchanged lists cause no notification or
repeated log.
The list order is 44100 stereo, 44100 mono, 48000 stereo, 48000 mono unless a format is
pinned (below); the first listed entry is also the assigned default of both mode lists.

Windows ignores the format picked in Sound settings for this endpoint: the decision happens
when the format list changes, so Windows plays the first format the endpoint lists and the
latter selection is never consulted. Hardware shows it from both sides - the request arrives
as a stream at the picked format that is built and then discarded, and the graph that runs is
at the first listed format. That is platform behaviour under Windows 11's Unified Audio
Endpoint, not something this driver can influence.

An endpoint format is therefore selected in the driver's own settings (`SET_ENDPOINT_FORMAT`,
stored per sink as `SelectedSampleRate`/`SelectedChannels`) and offered as the only entry of
both mode lists, so the shared-mode mix cannot land on anything else. `0`/`0` is automatic and
lists every format the sink supports, in the order above, which also keeps every format
available to a WASAPI exclusive-mode player; pinning one format limits those players to it.
A selection the sink does not advertise is refused by the IOCTL and, if a hand-edited store
produces one, `AudioApplyEndpointFormats` logs it and offers every supported format instead.

Nothing the graph does may change that list. Windows creates one stream per format it
enumerates before it opens the one it plays (four RAW and four DEFAULT probes per link-up),
and every endpoint format change invalidates the negotiated codec configuration, so an
endpoint format taken from a created stream closed the session once per probe. The format of
a stream only reaches `PinSampleRate` when the stream runs, which feeds the encoder and the
position math; it never moves the list.

The `PROPOSEDATAFORMAT` SET cannot decide either: Windows sweeps every rate and channel count
it knows through it while building and validating the graph (8000 Hz to 192000 Hz, 1 to 8
channels, one value per listed format and many that are not listed), so a proposal is only
logged. Adopting one moves the default data format for a few milliseconds until the next
proposal replaces it, and each move makes audiodg rebuild the graph.

Windows does not ask this pin for `KSPROPERTY_PIN_PROPOSEDATAFORMAT2`; a device-start log
shows only the enumeration queries, then `MODEDATAFORMATS` as a size probe. The legacy
`KSPROPERTY_PIN_PROPOSEDATAFORMAT` **SET** carries the graph's probe traffic and is only
logged. The same property also arrives as a
64-byte bare `KSDATAFORMAT` (`KSDATAFORMAT_TYPE_AUDIO`, no `WAVEFORMATEX`): that is
"audio, unspecified" and carries no selection. The property requests are logged once per
device start, not per request, and the stream processing mode is logged once per stream.
`EvtAcxPinSetDataFormat` is not a Sound-settings callback:
[ACX supports it only on non-software-streaming pins](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/acx-data-formats).
The actual stream format arrives at `OnCreateStream`, is validated as PCM the endpoint lists
(16-bit, or 24-in-32 when such a format is listed), and triggers AVDTP renegotiation when it
differs. Missing/invalid formats are refused, not reported as a fabricated 44100 stereo
fallback.

Logs report `endpoint formats` (what is listed with its rate, channels and depth,
provisional before discovery or the sink's SBC capabilities after) and
`endpoint format ... (actual stream)` (the format the encoder follows). The media/AVDTP logs
report the negotiated codec format, not the Windows mix format. The list line is printed only
when the list actually changes, and ACX's own format-change notification is not duplicated
explicitly. The property traffic Windows uses to build the graph is not logged: it is
hundreds of requests per graph and none of them can decide the format.

It goes no further than that on purpose. A2DP SBC defines no rate above 48 kHz, and the
Media Foundation AAC encoder this driver feeds is limited to 44.1/48 kHz, so 88.2/96/192 kHz
would leave SBC and AAC without an encoder (the native Windows A2DP endpoint stops at the
same two rates). 16-bit is the sample format SBC, AAC and classic aptX consume; 24-in-32 is
listed when aptX HD can run, and SBC, AAC or classic aptX fed from such a stream narrow to
16-bit through `Pcm::Narrow24To16` (rounded, saturating) on the PCM path.

## Stored preference

One preference per sink, kept in the registry and restored when the device starts:

- `HKLM\SYSTEM\CurrentControlSet\Services\OpenA2dp\Parameters` - global default.
- `...\Parameters\Devices\<12 hex address>` - per-sink override, wins over the global key.

All values are `REG_DWORD`, `0` means "driver default", `OpenA2dpCtl codec` writes the codec
fields through `SET_PREFERENCE` and `OpenA2dpCtl format` writes the endpoint format through
`SET_ENDPOINT_FORMAT`. Deleting the sink subkey returns it to auto. The driver reads it in
`OnDeviceAdd`, before the endpoint exists. The development ABI baseline (2) removes the
obsolete format preference and fallback fields:`Preference` is 40 bytes, `EndpointFormat` 16 bytes, `Status` 260 bytes, `RegisterEncoderOut` 32 bytes and `WaitEventOut` 36 bytes.
Rebuild the tools, driver and service together; earlier development layouts are
incompatible, even if they reported the same version.
`status` lists the sink's SBC PCM capability masks and discovered codecs, not a Windows
mix-format query. `codec aac` and `codec aptxhd` are refused on a sink that does not
advertise them.

| Setting | Registry value | CLI | Values | Default |
|---|---|---|---|---|
| Codec choice | `CodecPreferenceFlags`, `CodecPreferenceCodec` | `codec auto\|sbc\|aac\|aptx\|aptxhd` | auto, SBC, AAC, aptX, aptX HD | auto (aptX HD, aptX, then AAC, as far as registered encoders and the sink allow, else SBC) |
| SBC bitpool | `CodecPreferenceSbcBitpool` | `--bitpool n` | 2..250, clamped to the sink's range | 53 |
| SBC block length | `CodecPreferenceSbcBlocks` | `--blocks n` | 4, 8, 12, 16 | 16 |
| SBC subbands | `CodecPreferenceSbcSubbands` | `--subbands n` | 4, 8 | 8 |
| SBC allocation | `CodecPreferenceSbcAllocation` | `--allocation loudness\|snr` | 1 (loudness), 2 (SNR) | loudness |
| AAC bitrate | `CodecPreferenceAacBitrate` | `--bitrate bps` | 96000, 128000, 160000, 192000, 256000, 320000; clamped to the sink maximum and to the media MTU | highest the sink accepts, up to the MTU ceiling |
| Pacing lead | `CodecPreferencePrerollMs` | `--preroll ms` | 20..200 ms, 0 = default | 60 ms |
| SBC frames per packet | `CodecPreferenceSbcFramesPerPacket` | `--frames n` | 1..15, 0 = fill the packet | fill the packet |
| Endpoint format | `SelectedSampleRate`, `SelectedChannels` | `format [auto\|<rate> <channels>]` | automatic, or 44100/48000 Hz with 1 or 2 channels the sink advertises | automatic (every sink format) |

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

PCM depth is not a setting and has no preference field: `PcmFormat` carries it and the
endpoint lists 24-in-32 only while aptX HD can run, because the depth follows the codec.
SBC, classic aptX and the Media Foundation encoder take 16-bit input (AAC's encoder rejects
24-bit, 32-bit and float PCM with `0xC00D36B4`), so a 24-bit endpoint stream that negotiates
one of them is narrowed with the rounded downconvert. Forcing classic aptX narrows the
16-bit list to the formats it can run, just as forcing HD pins 24-in-32. The decision and
its sequencing are in [PCM depth](Protocol-and-Codecs.md#pcm-depth-and-24-bit-aptx-hd).

SBC frames per packet is the other latency knob: the MTU normally decides how many frames a
packet carries (seven at bitpool 53 and a 895-byte MTU, about 20 ms of audio), and capping it
shortens the packet. The cap is applied when the packet is built from the value resolved at
stream start, so a change rebuilds the stream like the other parameters. The cap trades
latency for packet rate: one frame is 2.9 ms of audio, so the link has to carry a packet that
often (one packet is in flight at a time), while four frames - 11.6 ms - is still shorter than
the 20 ms a full packet carries and leaves the transport slack. AAC and both aptX codecs carry
one access unit per packet by design and ignore the cap.

The pacing lead is set from the settings window's `Pacing lead` field or
`OpenA2dpCtl codec --preroll`: every codec is sent this far ahead of the media clock so the
sink's jitter buffer fills before it runs dry (the frames-per-packet field sits beside it in
the same dialog). The kernel pacer (SBC)
and the service's record pacing both read it at stream start - the service through
`WaitEventOut.prerollMs` - and `OnGetPresentationPosition` subtracts the same distance, so the
reported position stays what the listener hears. A shorter lead lowers audible latency by
spending that jitter margin; the 60 ms default keeps the headroom a typical sink needs.

A value the sink does not advertise falls back to the selector's own order instead of
failing the negotiation; a bitpool is lifted to the sink's minimum and capped at its
maximum for the chosen mode.

## Shared and exclusive mode

Shared mode is what every ordinary application uses: applications hand PCM to the Windows
audio engine (`audiodg.exe`), which mixes all of them into one shared-mode stream at the
endpoint's format - the first listed one, which is also what *Sound settings > Advanced*
shows as the device format. Everything played through the endpoint is resampled to it, so a
pinned format is the format the whole system plays on this sink.

Exclusive mode (WASAPI exclusive) bypasses the mixer: the application opens the pin itself,
no mixing or conversion happens and the stream is bit-exact, but only one application holds
the device and the format it asks for must be one the endpoint lists. Automatic therefore
keeps every sink format available to such a player; a pinned format limits it to that one.

Neither mode overrides the endpoint setting: the Sound-settings picker only ever recorded
the shared-mode default, which this endpoint ignores.

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
- Parameter change while streaming (bitpool, blocks, subbands, allocation, AAC bitrate,
  pacing lead, SBC frames per packet):
  the encoder stops, SUSPEND brings the stream to `Open`, the session rebuilds the
  configuration from the sink's advertised capabilities and sends RECONFIGURE, then START.
  A refused suspend, a rejected command, or a sink-configured stream whose capabilities are
  unknown falls back to close-and-rediscover.
- Endpoint format change (`format`): the offered list is rebuilt in place and the jack is
  reported unplugged for `AudioJackFlapMs` and plugged back in, because Windows only
  re-reads an endpoint's formats when it rebuilds the endpoint - neither the new list nor
  `AcxPinNotifyDataFormatChange` moves a running stream. The audio engine closes the stream
  it had at the old format, Windows re-creates the endpoint at the new (single) format, and
  with no stream running the pin takes that format and the session renegotiates the A2DP
  stream to it (RECONFIGURE cannot change the sampling frequency or channel count). The
  sink SEP and its capabilities are remembered, so there is no discovery and no sink
  reconnect; only a SEP that is no longer known, or a configuration the sink refuses, falls
  back to a rediscovery. Audio stops for the length of the flap, and a player resumes only
  when it reopens its stream. Codec/parameter settings do not change the available formats.
- A stream Windows opens: its format reaches `PinSampleRate` and the encoder when the stream
  runs. It never changes the offered list - Windows creates one stream per format it
  enumerates before it plays one, and those probes must not redefine the endpoint.
- Configuration the endpoint format cannot serve, whichever side proposed it: refused at
  negotiation, not at media start - `NotSupportedSamplingFrequency` (0xC4),
  `NotSupportedChannelMode` (0xC6), `NotSupportedChannels` (0xD9) or `NotSupportedBitRate`
  (0xDD) for a bitrate one media packet cannot carry. A sink that proposes one can retry
  with another configuration. The media-start check (`does not match the endpoint format`)
  remains as a safety net for a sink that re-proposes the same one.
