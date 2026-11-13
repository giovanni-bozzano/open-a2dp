# OpenA2dp

OpenA2dp makes Bluetooth headphones and speakers sound better on Windows 11.

Windows decides the audio codec for a Bluetooth headset and does not tell you which one it picked
or what you could have had instead. OpenA2dp takes a headset over - one device at a time - and
gives that choice back: LDAC, aptX HD, aptX, AAC or SBC, a sample rate you can hold, a bitrate you
can cap, a delay you can shorten, and a window that shows what the stream is doing while you
listen.

Only the devices you switch are affected. Everything else keeps using the driver Windows ships,
and handing a headset back is one click.

## What you get

- **The codec you want, on the headset you want it on.** By default the PC and the headset settle
  on the best codec both support (LDAC, then aptX HD, then aptX, then AAC, then SBC). You can also
  pin one codec for one headset, which is what you do when a device claims LDAC but stumbles at
  its highest bitrate.
- **Sound settings that stay where you put them.** Sample rate and channel count per codec, the
  SBC frame shape and bitpool, the AAC bitrate, LDAC's quality mode and how far ahead of the
  clock the stream is sent are all remembered per headset, and survive reboots.
- **A window that tells you what is happening.** `OpenA2dpConfig.exe` lists your sinks, shows the
  live session (state, codec, buffers, counters) and has a settings pane for every codec - not
  just the one playing - so you can prepare a setting for a device before you use it. Its
  **Copy** button puts the whole report on the clipboard for a bug report.
- **A command line with the same reach.** `OpenA2dpCtl.exe` prints that same report as text and
  sets the same values, which is what you want over a remote session or in a script.
- **Your other headsets are untouched.** The Windows driver stays installed and serving them.

## Good to know

- Anything you set for a codec that is not currently playing is stored and applied the next time
  that codec is used. A stream that is playing is only restarted when a change cannot wait, so
  tuning a setting never costs you the song you were listening to.
- If a headset does not advertise a codec, OpenA2dp cannot force it. The settings window only
  offers what the device reports.
- The counters (packets, drops, underruns, failed sends, trimmed audio) add up over the life of
  the device, so a fault that stopped is still visible. **Reset counters** - in the window, or
  `OpenA2dpCtl reset-counters` - starts a fresh measurement without interrupting playback.

## Requirements and limits

- Windows 11, x64. It is a replacement A2DP source driver, installed per headset, and it is not
  Microsoft-signed: the machine has to accept test-signed drivers.
- aptX Low Latency, aptX Adaptive and aptX Lossless are not implemented. AAC playback uses
  Windows' own encoder.
- Expect the usual Bluetooth range and interference behaviour; OpenA2dp changes the codec and the
  timing, not the radio.

## Try it

Building needs Visual Studio 2026 (v145 toolset) and the WDK packages pinned in
`Directory.Packages.props`.

```
& MSBuild.exe OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
out\x64\Debug\CodecTests.exe            # the codec tests, no hardware needed
```

`scripts\deploy.ps1` installs the driver, the tools and the encoder service on this machine from
an elevated prompt (`-Vm` does the same in the test VM, `-Uninstall` takes it all back out).

Then open `OpenA2dpConfig.exe`, pick your headset, choose a codec, adjust what you care about and
press **Apply**. If the headset is still on Windows' driver, the window offers to switch it over.

## Day to day

The settings window is the short version: a sink list, a live session pane, and one tab per codec
with its own settings under it.

The command line is the same thing as text:

| Command | What it does |
|---|---|
| `OpenA2dpCtl status` | The full report for this sink, refreshed with `--watch`. |
| `OpenA2dpCtl list` | The OpenA2dp devices that are present. |
| `OpenA2dpCtl codec ldac --preroll 40` | Pin a codec, or tune its settings, for this sink. |
| `OpenA2dpCtl format ldac 96000 2` | Choose the sample rate and channel count a codec's streams use. |
| `OpenA2dpCtl driver windows` | Hand the sink back to the Windows driver. |
| `OpenA2dpCtl reset` | Forget this sink's stored settings. |
| `OpenA2dpCtl reset-counters` | Start a fresh measurement window, without interrupting anything. |
| `OpenA2dpCtl help` | Every command and option. |

## How it is built, briefly

A kernel driver takes over the A2DP connection for the sinks you switch (codec negotiation,
timing, the audio endpoint Windows plays to), and a small service encodes the codecs that are too
heavy to run in the kernel: AAC, aptX, aptX HD and LDAC. SBC is encoded in the driver itself. The
two tools talk to the driver over a small control interface, which is also where the status report
comes from, so the window, its **Copy** button and `OpenA2dpCtl status` never disagree.

For the details, the [driver map](driver/README.md) and the
[architecture references](docs/architecture/README.md) cover each subsystem; the
[hardware test procedures](docs/HARDWARE-TESTS.md) say what each behaviour should look like on a
real sink, the [codec tests](tests/CodecTests/) run without hardware, and
[docs/STATUS.md](docs/STATUS.md) is the live milestone list.

## Licensing

Apache-2.0, see [LICENSE](LICENSE); its patent grant covers contributors only. The package ships
this repository's SBC encoder, Qualcomm's Apache-2.0 aptX and aptX HD encoders and Sony's
Apache-2.0 LDAC encoder, all from AOSP, and drives Windows' own encoder for AAC, so no AAC
encoder is distributed. What that obliges a distributor to ship is in
[docs/architecture/Codec-Licensing.md](docs/architecture/Codec-Licensing.md).

## AI disclosure

This project was built with extensive use of AI, steered at every step by a human. Coding
agents wrote most of the source, tests and documentation; the maintainer set the architecture,
made the design decisions, directed each change and validated the result on real hardware. Where
the reasoning behind a design matters it is written down in
[docs/architecture/](docs/architecture/README.md), next to the tests and the hardware procedures
that check it.
