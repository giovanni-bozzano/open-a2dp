# Configuration

[Architecture index](README.md) | [Using OpenA2dp](../USING.md) | [Protocol and codecs](Protocol-and-Codecs.md)

Configuration has two surfaces: the PCM formats the endpoint offers Windows, and
the per-sink settings the driver stores. Both are per codec, both survive a
reboot, and both reach a running stream without a sink reconnect.

## Endpoint formats

Windows opens the first PCM format the endpoint lists. The format picked in
*Sound settings > the endpoint > Advanced* is not an input to the driver, so the
way to choose a format is to change what the endpoint lists.

The endpoint format setting holds one entry per codec. Each entry is either:

- **Automatic** — offer every format that codec can play.
- **A rate and channel count** — offer that one format while that codec is the one
  in use.

The list is built from what the codecs and the sink can actually run:

- Rates are 16, 32, 44.1, 48, 88.2 and 96 kHz, in both channel counts.
- 44.1 kHz stereo leads, and the first entry is the default of both ACX mode
  lists.
- A rate is listed only while the codec that owns it can run. SBC owns 16 and
  32 kHz, AAC owns 32 kHz, and LDAC owns 88.2 and 96 kHz. Each depends on its
  encoder being registered and the sink advertising the rate.
- 16-bit entries are listed together with 24-in-32 entries while an encoder that
  reads or narrows 24-bit audio can run. The 16-bit set follows the sink's SBC
  capabilities, or its aptX or LDAC capabilities where SBC cannot play the format.
- A forced codec narrows the list to formats that codec can carry. Codec
  parameters never narrow it; only the endpoint format setting pins one.

Discovery edits the list in place with `AcxDataFormatListRemoveDataFormat` and
`AcxDataFormatListAddDataFormat`, and ACX notifies the pin's format change so
Windows re-queries the endpoint. The driver keeps one `ACXDATAFORMAT` object per
entry, shared by both mode lists, so a change adds or removes objects it already
owns. An unchanged list produces no notification and no repeated log line.

A stream's format reaches the driver's pin state only when the stream runs.
Windows creates and closes probe streams for the formats it enumerates before it
opens the one it plays; a probe must not renegotiate the session or move the
encoder's format.

`PROPOSEDATAFORMAT` is not a selection: Windows sweeps rates and channel counts
through it while building the graph, so the driver logs it. The legacy property
also arrives as a bare 64-byte `KSDATAFORMAT` with no selection in it. These
requests are logged once per device start, not per request. ACX does not offer
`EvtAcxPinSetDataFormat` on a software-streaming pin, so it is not a settings
callback.

`OnCreateStream` validates the stream as PCM the endpoint lists: 16-bit, or
24-in-32 when such a format is listed. A format that differs from the negotiated
one renegotiates AVDTP.

Logs report `endpoint formats` with rate, channels and depth: provisional before
discovery, the sink's capabilities after. `endpoint format ... (actual stream)`
is the format the encoder follows. Media and AVDTP records name the negotiated
codec format, not the Windows mix format.

## Storage

One INI file per sink, plus a machine default for a sink that has none:

- `%ProgramData%\OpenA2dp\default.ini` — machine default.
- `%ProgramData%\OpenA2dp\devices\<address>.ini` — that sink's settings.

The machine default is read only when the sink has no file of its own. It is
created by hand, in the same layout as a sink file.

```ini
[general]
codec = ldac
prerollMs = 60

[ldac]
quality = high
rate = 96000
channels = 2
```

The file has one section per codec (`[sbc]`, `[aac]`, `[aptx]`, `[aptxhd]`,
`[ldac]`) plus `[general]`, and one `key = value` line under each. `#` starts a
comment.

| Rule | Behaviour |
|---|---|
| Unknown section or key | Ignored, so a file from another build still loads |
| Invalid value for a known key | The whole file is refused; half a configuration is never applied |
| Missing file | No settings; driver defaults apply |
| Values equal to the default | Not written |
| Sink with no changes | No file at all |
| Reset | The sink's file is removed, not rewritten with defaults |

The driver reads the sink's file in `OnDeviceAdd`, before the endpoint exists. A
stored format the sink does not advertise is refused only for the codec the
endpoint is pinned to, where it would leave a stream with nothing to open.
Another codec's entry is stored as asked and ignored until that codec is
negotiated, so tuning a codec the sink is not running cannot fail.

The tools write `[general]` and codec parameters through `SET_PREFERENCE`, and a
codec's `rate`/`channels` through `SET_ENDPOINT_FORMAT`. The driver rewrites the
sink's file whenever either changes, so the file and the running driver agree.
`OpenA2dpCtl reset` and the window's **Restore defaults** clear both. The
settings themselves are listed in [Using OpenA2dp](../USING.md#settings).

## How a change applies

| Change | Path |
|---|---|
| Codec | The stream closes and rediscovers; the sink keeps its connection |
| A parameter that keeps the codec | SUSPEND to `Open`, RECONFIGURE with the selected SEP, START |
| A parameter of a codec not running | Stored only; the running stream is untouched |
| PCM format | The listed formats are rebuilt, the jack is reported unplugged briefly, Windows rebuilds the endpoint, and the new stream renegotiates |
| Pacing lead | Resolved at the next `MediaStart`; the pacer, the service records and the presentation position all use it |

A refused RECONFIGURE, a refused suspend or an unknown sink configuration falls
back to CLOSE and rediscovery.

The format change needs the jack flap because neither a new list nor a pin
format-change notification moves a running stream. Windows closes the stream at
the old format, rebuilds the endpoint at the new one, and the pin takes the new
stream and renegotiates. RECONFIGURE cannot change rate or channel count. Audio
stops for the length of `AudioJackFlapMs` and a player resumes when it reopens its
stream. The sink's SEP and capabilities are remembered, so there is no discovery
and no reconnect; only an unknown SEP or a refused configuration falls back to
rediscovery.

Configuration the endpoint cannot serve is refused during negotiation with the
AVDTP error that names the field: sampling frequency, channel mode, channels or
bitrate. The media-start format check remains a safety net.

## Selection

SBC and AAC configuration are preference-aware. A stored bitpool caps the
selector's request and a stored bitrate caps AAC, both clamped to the sink's range
inside the shared selector. Each field stands on its own, so no named combination
has to stay in sync with a stored value.

A codec the sink does not advertise, or whose provider is absent, is refused. A
value the sink does not advertise falls back to the selector's own order instead
of failing the negotiation; a bitpool is lifted to the sink's minimum and capped
at its maximum.

| SBC field | Supported | Chosen by |
|---|---|---|
| Sample rate | 16, 32, 44.1, 48 kHz | Endpoint format; must be advertised by the sink |
| Channel mode | Mono, dual, stereo, joint stereo | Endpoint channel count; mono or joint stereo preferred |
| Block length | 4, 8, 12, 16 | Preference, else 16 first |
| Subbands | 4, 8 | Preference, else 8 first |
| Allocation | Loudness, SNR | Preference, else loudness first |
| Bitpool | 2 to 250 | Preference, capped by the sink and mode; 53 when unset |

| AAC field | Supported | Chosen by |
|---|---|---|
| Object type | MPEG-2 LC, MPEG-4 LC | MPEG-2 LC first |
| Sample rate | 32, 44.1, 48 kHz | Endpoint format |
| Channels | 1 or 2 | Endpoint channel count, else stereo |
| Bitrate | 96 to 320 kbps, in the encoder's steps | Preference, capped by the sink and the media MTU |

LDAC negotiates one of 44.1, 48, 88.2 or 96 kHz: the sink's own rate equal to the
endpoint's, stereo only. A 24-in-32 endpoint is narrowed by the service. Quality
mode is local; it travels to the service in the stream start event and the sink is
never told which mode is in use.

The MTU a negotiation is capped against is the one the driver requests, because
discovery runs before the media channel exists. If the sink grants less, the
driver reconfigures once per connection at the highest bitrate the reported MTU
carries. The accepted MTU is kept for the rest of the connection, so a codec switch
is already capped by it.

PCM depth is not a setting: it follows the codec. [Protocol and
codecs](Protocol-and-Codecs.md#pcm-depth) covers the 24-in-32 contract.

SBC frames per packet is the packetization knob. The MTU normally decides how many
frames a packet carries; a stored cap shortens the packet and therefore the audio
buffered per packet. The kernel pacer and the service both read the resolved lead
at stream start.

## Shared and exclusive mode

Shared mode is what ordinary applications use: they hand PCM to the Windows audio
engine, which mixes every stream into one at the endpoint's first listed format.
Everything played through the endpoint is resampled to that format, so a pinned
format is the format the whole system plays on that sink.

Exclusive mode (WASAPI) bypasses the mixer: the application opens the pin itself,
nothing is mixed or converted, and the stream is bit-exact. One application holds
the device, and the format it asks for must be one the endpoint lists. Automatic
therefore keeps every sink format available to such a player; a pinned format
limits it to that one.

## Code map

| Area | Files |
|---|---|
| Endpoint format list and validation | `driver/audio/AudioFormat.cpp`, `AudioEndpoint.cpp` |
| Stream format and position | `driver/audio/AudioStream.cpp`, `AudioPosition.cpp` |
| Settings model and storage | `driver/config/ConfigStore.cpp`, `common/include/OpenA2dp/ConfigFile.h` |
| Preference and endpoint IOCTLs | `driver/service/ServiceIoctl.cpp`, `common/include/OpenA2dp/Ioctl.h` |
| SBC and AAC selectors | `common/include/OpenA2dp/SbcConfig.h`, `AacConfig.h` |
| Tools' settings UI and CLI | `tools/OpenA2dpConfig/main.cpp`, `tools/OpenA2dpCtl/main.cpp` |
