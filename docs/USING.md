# Using OpenA2dp

[README](../README.md) | [Build and deploy](BUILD.md) | [Diagnostics](DIAGNOSTICS.md)

Both tools need an elevated prompt. The control device accepts SYSTEM and
Administrators only; the settings window asks for elevation itself.

## Switch a sink over

A sink stays on the Windows driver until it is switched. Both tools do the switch
per sink, and neither needs the sink to be on OpenA2dp first:

```powershell
OpenA2dpCtl driver                  # one line per A2DP node and its driver
OpenA2dpCtl driver opena2dp         # take the selected sink over
OpenA2dpCtl driver windows         # hand it back
```

Only that sink's A2DP node is rebound. The other sinks keep the driver they are
on. The node restarts, so an open stream on that sink stops; the window pauses a
player that holds the node and tells you which program it paused. A program that
cannot be paused refuses the switch and leaves the binding alone.

After a switch, Windows creates a new endpoint for the sink. The sink stays
paired and reconnects through the new driver.

## The settings window

`OpenA2dpConfig.exe` is the short version of the command line:

| Part | Use |
|---|---|
| Sink picker | Select the headset. Each entry shows its address and whether it is connected, playing or on the Windows driver. |
| Session, Sink, Endpoint, Service, Driver | The same fields as `OpenA2dpCtl status`, split into sections. |
| Codec | Automatic, or one forced codec. |
| SBC, AAC, aptX, aptX HD, LDAC tabs | Every codec's settings, whether or not that codec is playing. |
| **Apply** | Store the selected values for this sink. |
| **Restore defaults** | Clear this sink's stored settings, the same as `OpenA2dpCtl reset`. |
| **Reset counters** | Start a fresh measurement window without interrupting playback. |
| **Details** | Show every status and preference field with its decimal and hex value. |
| **Copy** | Put the report on the clipboard, identical to `OpenA2dpCtl status --verbose`. |
| **Use Windows driver** / **Use OpenA2dp driver** | Move this sink between the two drivers. |

The window refreshes itself, including sinks that start or stop playing while it
is open. A field's tooltip explains the setting it changes.

## The command line

```text
OpenA2dpCtl [status] [--device <mac|index>] [--verbose]
                     [--watch [--interval <ms>]]
OpenA2dpCtl list
OpenA2dpCtl codec [<auto|sbc|aac|aptx|aptxhd|ldac>] [options]
OpenA2dpCtl format [<codec> [<auto|rate> [<1|2>]]]
OpenA2dpCtl driver [<opena2dp|windows>]
OpenA2dpCtl reset
OpenA2dpCtl reset-counters
OpenA2dpCtl version
OpenA2dpCtl help
```

| Command | What it does |
|---|---|
| `status` | AVDTP and media state, codec, sink formats, endpoint format, preference, buffers and counters. The default command. |
| `list` | The OpenA2dp devices that are present, with name and address. |
| `codec` | Set the codec preference and codec parameters for this sink. Without a codec name, keep the selection and apply only the flags given. |
| `format` | Show or set the PCM format each codec's streams are played at. |
| `driver` | Show the A2DP driver for each node, or switch this sink. |
| `reset` | Clear the stored codec preference and endpoint format for this sink. |
| `reset-counters` | Zero the status counters without stopping the stream or clearing per-stream peaks. |
| `version` | The driver's version information. |
| `help` | The full command and option text. |

### Selecting a sink

`--device` takes a Bluetooth address (`AA:BB:CC:DD:EE:FF`), a 1-based list index,
or a partial address. One OpenA2dp device is the default when only one is present.

### Codec options

These apply to `codec`:

| Option | Values | Meaning |
|---|---|---|
| `--bitrate <bps>` | 0, or a rate the encoder accepts | AAC bitrate cap; the sink and the media packet still cap it |
| `--bitpool <n>` | 0, or 2..250 | SBC bitpool cap, clamped to the sink's range |
| `--blocks <n>` | 4, 8, 12, 16 | SBC block length |
| `--subbands <n>` | 4, 8 | SBC subbands |
| `--allocation <mode>` | `loudness`, `snr`, `default` | SBC bit allocation |
| `--frames <n>` | 0, or 1..15 | SBC frames per packet; 0 fills the packet |
| `--preroll <ms>` | 0, or 20..200 | Pacing lead; 0 is the 60 ms default |
| `--ldac-quality <q>` | `high`, `standard`, `mobile` | 990, 660 or 330 kbps; local to the encoder |

A value of 0, or `default`, restores the driver default. The sink's advertised
limits still win; a stored value outside them is clamped.

### Status options

| Option | Meaning |
|---|---|
| `--watch` | Refresh the report until interrupted |
| `--interval <ms>` | Refresh period for `--watch` (default 1000) |
| `--verbose` | Append every status and preference field in decimal and hex |

## Settings

Each sink has one text file. The machine default is used only when the sink has no
file of its own:

- `%ProgramData%\OpenA2dp\default.ini` — machine default.
- `%ProgramData%\OpenA2dp\devices\<address>.ini` — settings for that sink.

The file has one section per codec (`[sbc]`, `[aac]`, `[aptx]`, `[aptxhd]`,
`[ldac]`) plus `[general]`, and one `key = value` line under each. `#` starts a
comment. Only values that differ from the driver default are written, so an
untouched sink has no file. A section or key this build does not know is ignored;
a value the driver cannot serve refuses the whole file. `default.ini` is created
by hand and shares the same layout.

| Setting | Section and key | CLI | Values | Default |
|---|---|---|---|---|
| Codec | `[general] codec` | `codec ...` | `auto`, `sbc`, `aac`, `aptx`, `aptxhd`, `ldac` | `auto` |
| Pacing lead | `[general] prerollMs` | `--preroll` | 0, or 20..200 ms | 60 ms |
| SBC bitpool | `[sbc] bitpool` | `--bitpool` | 0, or 2..250 | sink-selected, capped at 53 |
| SBC block length | `[sbc] blocks` | `--blocks` | 4, 8, 12, 16 | 16 |
| SBC subbands | `[sbc] subbands` | `--subbands` | 4, 8 | 8 |
| SBC allocation | `[sbc] allocation` | `--allocation` | `loudness`, `snr` | `loudness` |
| SBC frames per packet | `[sbc] framesPerPacket` | `--frames` | 0, or 1..15 | 0 (fill the packet) |
| AAC bitrate | `[aac] bitrate` | `--bitrate` | 96000 to 320000, in the encoder's steps | highest the sink and packet accept |
| LDAC quality | `[ldac] quality` | `--ldac-quality` | `high`, `standard`, `mobile` | `high` |
| Rate and channels | `[<codec>] rate`, `[<codec>] channels` | `format` | `auto`, or an offered rate with 1 or 2 channels | auto |

Automatic codec order is LDAC, aptX HD, aptX, AAC, SBC, skipping any codec the
sink does not advertise or whose encoder is not registered.

## Endpoint formats

Windows plays the first PCM format the endpoint lists. The format picked in
*Sound settings > the endpoint > Advanced* does not reach the driver, so the
endpoint format setting is the only way to choose one.

- **Automatic** offers every rate and channel count the codec can play.
- **A rate and channel count** pins that codec to the one format. While that codec
  runs, it is the only entry in the Windows list.

Pinning also limits a WASAPI exclusive-mode player to that format. Automatic
keeps every sink format available to it. A rate or channel count the sink cannot
serve for the codec in use is refused and nothing is stored; another codec's
entry is stored and used once that codec can play it.

Pinning a format is not the same as forcing a codec: codec parameters never
narrow the endpoint list, while forcing a codec narrows it to what that codec
carries.

## How a change applies

| Change | What happens |
|---|---|
| Codec | The stream closes and rediscovers; the sink stays connected |
| A parameter that keeps the codec | Suspend, reconfigure, start; a refused reconfigure falls back to close and rediscover |
| A parameter of a codec not playing | Stored only; the running stream is untouched |
| PCM format | The listed formats are rebuilt and the jack is reported unplugged briefly; Windows rebuilds the endpoint and the stream renegotiates |
| Pacing lead | Used at the next stream start; the presentation position subtracts the same value |

A format change costs a short silence while Windows rebuilds the endpoint and the
player reopens its stream. A codec or parameter change costs whatever the
renegotiation takes. Tuning a codec the sink is not running costs nothing.

## Recipes

**Best quality on a capable sink**

```powershell
OpenA2dpCtl codec auto
OpenA2dpCtl format ldac 96000 2
```

Use `codec ldac` instead of `auto` to refuse every other codec. If the sink claims
LDAC but stutters at its top rate, try `--ldac-quality standard` or `mobile`.

**Lower latency**

```powershell
OpenA2dpCtl codec sbc --preroll 30 --frames 4
```

The lead is floored at one access unit of the running codec: below that, every
packet would be padded. A lead the sink cannot absorb shows up as rising
`underruns`, not as stable audio.

**One fixed format**

```powershell
OpenA2dpCtl format aac 48000 2
```

**Back to the Windows driver**

```powershell
OpenA2dpCtl driver windows
```

**Start a fresh measurement**

```powershell
OpenA2dpCtl reset-counters
```

Counters are cumulative until this command or a driver reload. The stream, its
settings and the per-stream latency peak are untouched.

## Reading the report

`OpenA2dpCtl status` and the window show the same report. It starts with the
session state, then the codec and sink, then the endpoint and service, then the
latency and counters. [Diagnostics](DIAGNOSTICS.md) explains each line and what a
change in it means.
