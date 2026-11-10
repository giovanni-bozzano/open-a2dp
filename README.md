# OpenA2dp

Open-source A2DP source driver for Windows 11, built to replace `BthA2dp.sys` on a per-device basis. SBC is encoded in the kernel; AAC-LC encoding is planned for a user-mode service.

## Status

The live milestone table and validation backlog are maintained only in [docs/STATUS.md](docs/STATUS.md).

Current implementation:

- `OpenA2dp.sys`: KMDF profile driver above `bthport`. L2CAP PSM 0x19 signaling and media channels, AVDTP source (plus acceptor groundwork; SBC/AAC SEPs with service-gated AAC selection and AAC > SBC fallback), ACX render endpoint, kernel fixed-point SBC encoder and RTP send.
- The audio endpoint is named `<sink> [OpenA2DP]` and typed as headphones or speakers from the sink's Class of Device.
- `common/include/OpenA2dp`: kernel-safe AVDTP/RTP/ring-buffer/codec-selection headers, including `SbcConfig.h`, `AacConfig.h` and the driver/service ABI `Ioctl.h`.
- `driver/service/`: driver side of the AAC boundary: per-device control device with a SYSTEM/Administrators SDDL, IOCTL queue, shared PCM/frame rings mapped into the service process, `WAIT_EVENT` ticked by the media timer. See the [driver map](driver/README.md) for all subsystems.
- [Architecture references](docs/architecture/README.md): system overview, lifecycle, protocols and [AAC service boundary](docs/architecture/AAC-Service-Boundary.md).
- `service/main.cpp`: AAC encoder Windows service (`OpenA2dpSvc.exe`).
- `codecs/`: the `IA2dpCodec` interface, SBC encoder, AAC IE/negotiation and the Media Foundation/LATM encoder used by tests and the service.
- `tests/CodecTests`: a self-contained test runner with an independent SBC reference decoder.
- `tools/OpenA2dpCtl`: diagnostic CLI (`OpenA2dpCtl.exe`) to list devices, show the AVDTP/media state and current codec, and force auto/SBC/AAC; deployed to the test VM by `scripts/deploy-vm.ps1`.
- `tools/OpenA2dpConfig`: desktop settings window (`OpenA2dpConfig.exe`) over the same IOCTLs: sink list, live status, and the codec and endpoint preference.
- `tools/common`: the status report both tools print (`StatusReport`), so the window, its **Copy** button and `OpenA2dpCtl status` never show different fields; the window's **Details** box and `status --verbose` add every field with its hex value.

## Licensing

SBC is unencumbered. The in-tree AAC-LC encoder needs a Via LA AAC patent licence for any
product that ships an encoder, whether or not it uses this code. The aptX family is
Qualcomm proprietary and is deliberately not implemented here. Details in
[docs/architecture/Codec-Licensing.md](docs/architecture/Codec-Licensing.md).

## Build

Requires Visual Studio 2026 (v145 toolset) and the WDK NuGet packages pinned in `Directory.Packages.props`. Use the 64-bit MSBuild.

```
& MSBuild.exe OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
out\x64\Debug\CodecTests.exe            # all tests
out\x64\Debug\CodecTests.exe Sbc_       # tests whose name starts with a filter
```

User-mode binaries (`CodecTests.exe`, `OpenA2dpCtl.exe`, `OpenA2dpConfig.exe`, `OpenA2dpSvc.exe`) link the CRT statically, so they also run in the test VM without the Visual C++ debug runtime.
