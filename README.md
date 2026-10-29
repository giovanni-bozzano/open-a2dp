# OpenA2dp

Open-source A2DP source driver for Windows 11, built to replace `BthA2dp.sys` on a per-device basis. SBC is encoded in the kernel; AAC-LC encoding is planned for a user-mode service.

See [docs/A2DP-Driver-Plan.md](docs/A2DP-Driver-Plan.md) for the architecture, protocol notes and roadmap, and [docs/SESSION-CONTEXT.md](docs/SESSION-CONTEXT.md) for the current handoff.

## Status

| Milestone | State |
|---|---|
| M1 Codec library + unit tests | Done |
| M2 Driver skeleton + L2CAP | Done (signaling channel opens to a real headset in the VM) |
| M3 AVDTP negotiation | In progress (SBC stream reaches Streaming; BlueZ sink and sink-initiated configuration not yet validated) |
| M4 ACX endpoint + rings | Done (Speaker0 endpoint; Windows audio plays through the headset) |
| M5 Encoder service + streaming | In progress (audible SBC; AAC selector and the driver side of the service boundary done; AAC SEP + service pending) |
| M5b Codec configuration | Not started |
| M6 Config UI + install/uninstall | Not started |
| M7 System testing | Not started |
| M8 Signing & release | Not started |

Current implementation:

- `OpenA2dp.sys`: KMDF profile driver above `bthport`. L2CAP PSM 0x19 signaling and media channels, AVDTP source (plus acceptor groundwork), ACX render endpoint, kernel fixed-point SBC encoder and RTP send.
- The audio endpoint is named `<headset> [OpenA2DP]` and typed as headphones or speakers from the headset's Class of Device.
- `common/include/OpenA2dp`: kernel-safe AVDTP/RTP/ring-buffer/codec-selection headers, including `SbcConfig.h`, `AacConfig.h` and the driver/service ABI `Ioctl.h`.
- `driver/Service.cpp`: driver side of the AAC boundary: per-device control device with a SYSTEM/Administrators SDDL, IOCTL queue, shared PCM/frame rings mapped into the service process, `WAIT_EVENT` ticked by the media timer.
- `docs/AAC-Service-Boundary.md`: design of the AAC driver/service boundary (rings, IOCTLs, lifecycle); the driver side is implemented, the service is next.
- `codecs/`: the `IA2dpCodec` interface, SBC encoder, AAC IE/negotiation and the Media Foundation/LATM encoder used by tests and the future service.
- `tests/CodecTests`: a self-contained test runner with an independent SBC reference decoder.

## Build

Requires Visual Studio 2026 (v145 toolset) and the WDK NuGet packages pinned in `driver/packages.config`. Use the 64-bit MSBuild.

```
& "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
out\x64\Debug\CodecTests.exe            # 44 tests
out\x64\Debug\CodecTests.exe Sbc_       # tests whose name starts with a filter
```

## Known gaps

- SBC is still tested against our own reference decoder; external validation (ffmpeg / BlueZ `sbcdec`) and more real-device coverage are pending.
- AAC LATM framing must be checked against captures from real sinks; the AAC encoder service (`OpenA2dpSvc.exe`) and the AAC SEP are not implemented yet, so the driver still advertises SBC only (see `docs/AAC-Service-Boundary.md`).
- BlueZ/PipeWire sink validation and the sink-initiated (acceptor) path are deferred until a Linux Bluetooth host is available.
- Codec configuration UI/store, clean install/uninstall, long-run system testing and release signing are not done.
