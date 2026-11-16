# OpenA2dp

OpenA2dp is an A2DP source driver for Windows 11. It replaces the Bluetooth audio
driver for the headsets you choose, so you can pick the codec and stream settings
and see what the stream is doing while it plays.

OpenA2dp takes over one paired sink at a time and gives you control of its codec
and stream. It brings its own A2DP source, endpoint and settings while leaving
the Bluetooth radio, pairing, HFP and AVRCP to Windows. Handing a headset back is
one command or one button.

## What OpenA2dp gives you

| Capability | What it means in use |
|---|---|
| Codec control | SBC, AAC, aptX, aptX LL, aptX HD and LDAC. Automatic mode picks the best both ends support (LDAC, aptX HD, aptX, AAC, SBC); one codec can be pinned per sink. aptX LL is forced explicitly because it trades margin for latency. |
| Codec settings | Sample rate and channel count per codec, SBC frame shape, block length, subbands and bitpool, AAC bitrate, and LDAC quality. Each is stored per headset. |
| Stream timing | A pacing lead from 20 to 200 ms, and a cap on how many SBC frames one packet carries. |
| Live visibility | Session state, negotiated codec, buffers, latency, counters and the last transport error, refreshed in a window or by `--watch`. |
| Per-sink scope | Only the sinks you switch change driver. Every other headset stays on the path Windows already serves. |
| A way back | `OpenA2dpCtl driver windows` or one window button returns a sink to the driver it came from. |
| One report, two tools | The window, its **Copy** button and `OpenA2dpCtl status` read the same status report, so a script and a person see the same fields. |

The Bluetooth radio, pairing, HFP/HSP and AVRCP stay with Windows. OpenA2dp is a
replacement A2DP source, not a replacement Bluetooth stack.

### The codec you want, on the headset you want it on

Automatic mode picks the best codec both ends support: LDAC, then aptX HD, then
aptX, then AAC, then SBC. Pinning a codec is for the device that advertises LDAC
but cannot hold its highest bitrate, or for one that sounds better on a lower
codec.

### Settings that survive reboots

Each sink has its own stored settings. A parameter for a codec that is not
playing is stored and used the next time that codec runs. A change that can wait
does not restart the stream.

### A window and a command line

`OpenA2dpConfig.exe` lists the sinks, shows the live session and has a tab for
every codec, not only the one playing. `OpenA2dpCtl.exe` prints the same report
and sets the same values, which is what a remote session or a script wants. Both
need an elevated prompt.

### Other headsets are untouched

Windows keeps serving every sink you did not switch. A driver switch moves one
A2DP node, not a radio or a machine-wide stack.

## Requirements and limits

- Windows 11 x64. The driver is not Microsoft-signed, so the machine has to accept
  test-signed drivers.
- The sink has to advertise a codec before OpenA2dp can use it. Pinning a codec the
  sink does not offer is refused.
- Six codecs are supported: SBC, AAC, aptX, aptX LL, aptX HD and LDAC. SBC is
  encoded in the driver; AAC uses the Windows Media Foundation encoder; aptX,
  aptX LL, aptX HD and LDAC are encoded by the service.
- Bluetooth range and interference still apply. OpenA2dp changes the codec and the
  timing, not the radio.
- One A2DP stream per sink.

## Build and install

Building needs Visual Studio 2026 (v145 toolset) and the WDK packages pinned in
`Directory.Packages.props`.

```powershell
& MSBuild.exe OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
.\out\x64\Debug\CodecTests.exe
```

From an elevated prompt, the deploy script builds, tests, installs the driver
package, the tools and the encoder service, and starts the service:

```powershell
.\scripts\deploy.ps1 -Build
```

`-Vm` does the same in the test VM. `-Uninstall` removes the package, the service
and the test certificate, and checks that the sink is back on the inbox driver.
See [Build and deploy](docs/BUILD.md) for the options and the manual checks.

Then open `OpenA2dpConfig.exe`, pick a headset and choose a codec. If the sink is
still on the Windows driver, the window offers to switch it over. The settings
take effect on the next negotiation; a running stream is restarted only when the
change cannot wait.

## Everyday use

The window is the short version: a sink list, a live session pane, and one tab per
codec with its settings under it.

The command line is the same thing as text:

| Command | What it does |
|---|---|
| `OpenA2dpCtl status` | The full report for one sink, refreshed with `--watch`. |
| `OpenA2dpCtl list` | The OpenA2dp devices that are present. |
| `OpenA2dpCtl codec ldac --preroll 40` | Pin a codec, or tune its settings, for this sink. |
| `OpenA2dpCtl format ldac 96000 2` | Choose the sample rate and channel count a codec's streams use. |
| `OpenA2dpCtl driver windows` | Hand the sink back to the Windows driver. |
| `OpenA2dpCtl reset` | Forget this sink's stored settings. |
| `OpenA2dpCtl reset-counters` | Start a fresh measurement window without interrupting playback. |
| `OpenA2dpCtl help` | Every command and option. |

Both tools need an elevated prompt: the control device allows SYSTEM and
Administrators only. The full reference is in [Using OpenA2dp](docs/USING.md).

## How it works

A kernel driver takes over the A2DP connection for the sinks you switch: codec
negotiation, timing, the L2CAP transport and the audio endpoint Windows plays to.
A small user-mode service encodes AAC, aptX, aptX LL, aptX HD and LDAC. SBC is encoded in
the driver, so the mandatory codec keeps working with no service running. The tools talk to a separate control
device, which is also where the status report comes from, so the window, its
**Copy** button and `OpenA2dpCtl status` cannot disagree.

## Documentation

| Guide | What it covers |
|---|---|
| [Build and deploy](docs/BUILD.md) | Prerequisites, build, tests, deploy, VM, uninstall |
| [Using OpenA2dp](docs/USING.md) | Window, command line and per-sink settings |
| [Diagnostics](docs/DIAGNOSTICS.md) | Status report, logs, counters and triage |
| [Testing](docs/TESTING.md) | Host tests and the hardware test harness |
| [Architecture](docs/architecture/README.md) | Components, codec and driver design |
| [Codec licensing](docs/architecture/Codec-Licensing.md) | What ships and the notices it carries |
| [Plan status](docs/STATUS.md) | Milestones, blockers and next work |

The [hardware test procedures](docs/HARDWARE-TESTS.md) describe the behaviour each
test checks on a real sink. The tests in `tests/CodecTests` need no hardware.

## Status

The project is usable and changing quickly. Treat it as beta quality: the driver,
service and tools are always installed from the same build, and uninstalling
returns every sink to the inbox driver. The live plan is
[docs/STATUS.md](docs/STATUS.md).

## Licensing

The repository is Apache-2.0; see [LICENSE](LICENSE). Its patent grant covers
contributors to this repository, not the codecs.

The package includes this repository's SBC encoder, Qualcomm's Apache-2.0 aptX and
aptX HD encoders, and Sony's Apache-2.0 LDAC encoder, all from AOSP. AAC is encoded
by Windows Media Foundation, so no AAC encoder is distributed. The notices a build
carries and the marks it does not claim are listed in
[Codec licensing](docs/architecture/Codec-Licensing.md).

## AI disclosure

This project is developed with extensive use of AI, steered at every step by a
human. Coding agents wrote most of the source, tests and documentation; the
maintainer set the architecture, made the design decisions, directed each change
and validated the result on real hardware. Where a design needs an explanation it
is written down in [docs/architecture](docs/architecture/README.md), next to the
tests and hardware procedures that check it. If AI-developed code is not acceptable
to you, this project is not for you.
