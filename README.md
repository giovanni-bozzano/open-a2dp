# OpenA2dp

Open-source A2DP source driver for Windows 11, built to replace `BthA2dp.sys` on a per-device basis. SBC is encoded in the kernel; AAC-LC encoding is planned for a user-mode service.

See [plan status](docs/STATUS.md) for milestones, blockers and next steps, and the [documentation map](docs/AGENT-EFFICIENCY.md) for reference locations. Individual changes and validation results are recorded in Git commits.

## Status

The live milestone table and validation backlog are maintained only in [docs/STATUS.md](docs/STATUS.md).

Current implementation:

- `OpenA2dp.sys`: KMDF profile driver above `bthport`. L2CAP PSM 0x19 signaling and media channels, AVDTP source (plus acceptor groundwork; SBC/AAC SEPs with service-gated AAC selection and AAC > SBC fallback), ACX render endpoint, kernel fixed-point SBC encoder and RTP send.
- The audio endpoint is named `<headset> [OpenA2DP]` and typed as headphones or speakers from the headset's Class of Device.
- `common/include/OpenA2dp`: kernel-safe AVDTP/RTP/ring-buffer/codec-selection headers, including `SbcConfig.h`, `AacConfig.h` and the driver/service ABI `Ioctl.h`.
- `driver/service/`: driver side of the AAC boundary: per-device control device with a SYSTEM/Administrators SDDL, IOCTL queue, shared PCM/frame rings mapped into the service process, `WAIT_EVENT` ticked by the media timer. See the [driver map](driver/README.md) for all subsystems.
- [Architecture references](docs/architecture/README.md): system overview, lifecycle, protocols and [AAC service boundary](docs/architecture/AAC-Service-Boundary.md).
- `service/main.cpp`: AAC encoder Windows service (`OpenA2dpSvc.exe`).
- `codecs/`: the `IA2dpCodec` interface, SBC encoder, AAC IE/negotiation and the Media Foundation/LATM encoder used by tests and the service.
- `tests/CodecTests`: a self-contained test runner with an independent SBC reference decoder.
- `tools/OpenA2dpCtl`: diagnostic CLI (`OpenA2dpCtl.exe`) to list devices, show the AVDTP/media state and current codec, and force auto/SBC/AAC; deployed to the test VM by `scripts/deploy-vm.ps1`.

## Build

Requires Visual Studio 2026 (v145 toolset) and the WDK NuGet packages pinned in `driver/packages.config`. Use the 64-bit MSBuild.

```
& "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
out\x64\Debug\CodecTests.exe            # all tests
out\x64\Debug\CodecTests.exe Sbc_       # tests whose name starts with a filter
```

User-mode binaries (`CodecTests.exe`, `OpenA2dpCtl.exe`, `OpenA2dpSvc.exe`) link the CRT statically, so they also run in the test VM without the Visual C++ debug runtime.

## Known gaps

See [plan status](docs/STATUS.md) for the current hardware blockers, deferred validation and remaining release gates.
