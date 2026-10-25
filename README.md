# OpenA2dp

Open-source A2DP source driver for Windows 11, built to replace `BthA2dp.sys` on a per-device basis. It supports SBC (with a configurable bitpool) and AAC-LC.

See [docs/A2DP-Driver-Plan.md](docs/A2DP-Driver-Plan.md) for the architecture and roadmap.

## Status

| Milestone | State |
|---|---|
| M1 Codec library + unit tests | Done (host tests pass) |
| M2 Driver skeleton + L2CAP | Not started (needs WDK) |
| M3-M8 | Not started |

M1 includes:
- `common/include/OpenA2dp`: AVDTP constants/helpers, RTP header, shared SPSC ring buffer, and bit I/O. These are kernel-safe.
- `codecs/`: the `IA2dpCodec` interface, codec negotiation (AAC > SBC, with an optional forced codec), the SBC encoder, and the AAC encoder. AAC uses the Media Foundation encoder with LATM packing.
- `tests/CodecTests`: a self-contained test runner with an independent SBC reference decoder.

## Build

Requires Visual Studio 2026 (v145 toolset) and Windows SDK 10.0.26100.

```
MSBuild open-a2dp.slnx /p:Configuration=Debug /p:Platform=x64
out\x64\Debug\CodecTests.exe            # all tests
out\x64\Debug\CodecTests.exe Sbc_       # tests whose name starts with a filter
```

## Known gaps

- SBC is tested only against our own reference decoder. It still needs testing against an external decoder (ffmpeg / BlueZ `sbcdec`) and real headsets.
- AAC LATM framing must be checked against packet captures from real sinks.
- The driver, service, CLI, INF, and signing are not implemented yet.
