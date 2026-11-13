# OpenA2dp

An open-source A2DP **source** driver for Windows 11: it replaces the inbox `BthA2dp.sys` on a
per-device basis, so a Bluetooth headset or speaker plays from this PC with the codec, bitrate
and pacing you choose instead of what the inbox driver happens to negotiate.

SBC is encoded in the kernel. AAC-LC, classic aptX, aptX HD and LDAC are encoded by a user-mode
service over shared rings, with aptX HD and LDAC taking native 24-bit PCM. Every setting is
stored per sink, and two small tools read the same IOCTL surface the audio path does.

## Why it exists

The inbox A2DP source works, but it is closed: it picks its own codec, exposes no way to pin the
endpoint format, keeps its own idea of latency, and cannot be extended toward another codec. This
project is the same job done where it can be read, measured and changed - a KMDF profile driver
above `bthport` that owns L2CAP, AVDTP and the ACX render endpoint, with a service for the codecs
that are too heavy for the kernel. It is a replacement, not a wrapper: `BthA2dp.sys` stays
installed and keeps serving every other sink, and the switch is a per-device decision.

## What it does

- **Codecs.** SBC (kernel, fixed-point encoder), AAC-LC (Windows' own encoder through Media
  Foundation), classic aptX, aptX HD and LDAC. The automatic choice runs the best both ends
  support - LDAC, then aptX HD, then aptX, then AAC, then SBC - and a codec can be forced per
  sink.
- **An audio endpoint that behaves.** An ACX render endpoint named `<sink> [OpenA2DP]`, typed as
  headphones or speakers from the sink's Class of Device, offering 16-bit and 24-in-32 formats.
  Changing the sample rate rebuilds the listing and renegotiates the running stream instead of
  waiting for the next one.
- **Per-sink settings.** Codec, sample rate and channel count per codec, SBC frame shape and
  bitpool, AAC bitrate, LDAC quality, pacing lead, all stored per sink in one file. A setting for
  a codec the stream is not running is stored and applied at the next negotiation, never at the
  cost of a stream that is playing.
- **Two front ends.** `OpenA2dpConfig.exe` is a dark-mode settings window with a live session
  pane; `OpenA2dpCtl.exe` prints the same status as text and sets the same values.
- **Tests you can run without hardware.** A self-contained codec test runner with an independent
  SBC reference decoder, and a scripted hardware procedure for everything that needs a real sink.

## Status

The live milestone table, next tasks and blockers are maintained only in
[docs/STATUS.md](docs/STATUS.md). In short: driver, negotiation, endpoint, encoder service, all
five codecs, the configuration tools and install/uninstall are done; what remains is a single
setup file. Behaviour is validated against a real sink with the procedures in
[docs/HARDWARE-TESTS.md](docs/HARDWARE-TESTS.md), which state what each test expects.

## Try it

Requires Windows 11 x64, Visual Studio 2026 (v145 toolset) and the WDK NuGet packages pinned in
`Directory.Packages.props`. Use the 64-bit MSBuild.

```
& MSBuild.exe OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
out\x64\Debug\CodecTests.exe            # all tests
out\x64\Debug\CodecTests.exe Sbc_       # tests whose name starts with a filter
```

`scripts\deploy.ps1` installs the test-signed package, the tools and the encoder service on this
machine without a reboot (elevated prompt); `scripts\deploy.ps1 -Vm` runs the same installer in
the test VM, so the two targets cannot drift apart, and `-Uninstall` takes it all back out.
User-mode binaries (`CodecTests.exe`, `OpenA2dpCtl.exe`, `OpenA2dpConfig.exe`,
`OpenA2dpSvc.exe`) link the CRT statically, so they also run in the test VM without the Visual
C++ debug runtime.

## Repository layout

| Path | What it is |
|---|---|
| `driver/` | The KMDF driver: entry/PnP, L2CAP, AVDTP, media path, ACX endpoint, service boundary. Its [map](driver/README.md) lists every subsystem file. |
| `service/` | `OpenA2dpSvc.exe`, the encoder service: AAC-LC, classic aptX, aptX HD and LDAC. |
| `codecs/` | The `IA2dpCodec` seam, the SBC encoder and AAC negotiation, and the vendored AOSP aptX, aptX HD and LDAC encoders. |
| `common/include/OpenA2dp/` | Kernel-safe AVDTP/RTP/ring-buffer/codec headers and the driver-service-tools ABI (`Ioctl.h`). |
| `tools/OpenA2dpConfig/` | The settings window: sink list, live session pane, codec strip and the fields below it. |
| `tools/OpenA2dpCtl/` | The diagnostic CLI: devices, AVDTP/media state, codec, and the same settings from a console. |
| `tools/common/` | The status report both tools print, so the window, its **Copy** button and `OpenA2dpCtl status` never disagree. |
| `tests/CodecTests/` | The codec test runner, with an independent SBC reference decoder. |
| `tests/HardwareTests/` | The hardware procedure scripted per test, with its "Expect" lines asserted. |
| `docs/` | [Status](docs/STATUS.md), [hardware tests](docs/HARDWARE-TESTS.md) and the [architecture references](docs/architecture/README.md). |

## Licensing and limits

Apache-2.0, see [LICENSE](LICENSE); its patent grant covers contributors only. The package ships
this repository's SBC encoder, Qualcomm's Apache-2.0 aptX and aptX HD encoders and Sony's
Apache-2.0 LDAC encoder, all from AOSP, and drives Windows' own encoder for AAC, so no AAC
encoder is distributed. aptX LL, aptX Adaptive and aptX Lossless are not implemented. What that
obliges a distributor to ship is in
[docs/architecture/Codec-Licensing.md](docs/architecture/Codec-Licensing.md).

The driver is Windows 11 x64 only, is not WHQL-signed, and is meant to be installed per sink on a
machine where a test-signed driver is acceptable. It replaces the inbox A2DP source for the sinks
it claims; everything else keeps using `BthA2dp.sys`.

## AI disclosure

This project was built with extensive use of AI. Coding agents wrote the large majority of the
source, tests and documentation under human direction: the human maintainer set the
architecture, made the design decisions, reviewed every change, ran the builds and the hardware
tests, and is the only person responsible for what is here. The AI was steered rather than
autonomous - it did not choose the scope, the codec policy or the safety rules in
[AGENTS.md](AGENTS.md), and it does not appear as an author of any commit.

Two consequences are worth stating plainly:

- **Review it like any other driver.** Machine-written code can be confidently wrong, and a
  kernel driver is a bad place to find out. The reasoning behind each design lives in
  [docs/architecture/](docs/architecture/README.md), and the tests are there to be re-run rather
  than trusted.
- **The evidence is the build and the hardware runs**, not the prose. `docs/STATUS.md` records
  what is done; `docs/HARDWARE-TESTS.md` records what was checked against a real sink and what
  each test expects. Where something has not been validated on hardware, it says so.
