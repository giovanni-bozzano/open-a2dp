# OpenA2dp – Session Context (handoff from previous chat)

Read this first in a new chat to pick up the previous one. It summarizes that conversation; it is not a full transcript.

## Origin and goal
- It started with questions about the commercial "Alternative A2DP Driver": why it connects audio faster, which codecs it supports, and how it relates to Intel Bluetooth drivers.
  - That driver replaces only Microsoft's A2DP profile driver (`BthA2dp.sys`).
  - It does not replace the Intel radio driver (`ibtusb.sys`), `bthport.sys`/`bthusb.sys`, HFP, or AVRCP.
- **Goal:** build an open-source replacement for `BthA2dp.sys` (A2DP Source) on Windows 11.
  - Codecs: SBC and AAC first, with aptX/LDAC pluggable later.
  - Test the driver safely in a VM first.
  - Ship later with EV/attestation signing.
- The full plan is in [A2DP-Driver-Plan.md](A2DP-Driver-Plan.md). A copy is also on the Desktop.

## Architecture (summary)
- **`OpenA2dp.sys`:** a KMDF profile driver above bthport. It handles L2CAP through BRBs (PSM 0x0019), the AVDTP signaling state machine, the ACX audio endpoint, and RTP media.
- **`OpenA2dpSvc.exe`:** a user-mode encoder service. It talks to the driver through shared SPSC rings (`RingBuffer.h`).
- **Codecs** sit behind `IA2dpCodec`. Negotiation priority is AAC > SBC. A forced codec can be set; if it isn't available, negotiation falls back to the normal order.
- **AAC** uses the Media Foundation AAC encoder (`CLSID_AACMFTEncoder`), packed as LATM.
- **Test environment:** a Windows 11 VM with a USB BT dongle passed through, Secure Boot off, test signing, WinDbg, Driver Verifier, BTP/btvs, and Wireshark.

## Environment
- Visual Studio Community 2026 18.11.1 at `D:\Program Files\Microsoft Visual Studio\18\Community`, toolset v145, C++20, x64.
- Windows SDK 10.0.26100.0. **WDK is not installed**, which blocks M2.
- .NET SDK 10.0.402 is available (it was used to migrate the solution to `.slnx`).
- Repo: `D:\OpenA2dp` (git, branch `master`).

## Build / test
```
& "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64 /m
out\x64\Debug\CodecTests.exe          # 35 tests, all passing
out\x64\Debug\CodecTests.exe Sbc_     # name-prefix filter
```

## Status
| Milestone | State |
|---|---|
| M1 codec library + tests | **Done** (commit 6fc217d) |
| Solution migrated to `OpenA2dp.slnx` with folders | Done (commit 4ec6684) |
| M2 driver skeleton + L2CAP | Not started: install the WDK first |
| M3–M8 (AVDTP, ACX, service, CLI/INF, signing) | Not started |

## Key decisions and lessons
- M1 is user-mode only so it can be unit tested with no risk of a BSOD. Code under `common/` must stay kernel-safe (no STL or exceptions in headers meant for the driver).
- **SBC reference-decoder fix:**
  - Synthesis was rewritten as a direct cosine-modulated filterbank over a 10-block history.
  - Synthesis gain is `-M` because the synthesis filters are the negated time reverse of the analysis filters.
  - Measured delay is 73 samples for 8 subbands and 37 for 4.
  - Filterbank-limited SNR is about 65–67 dB.
- **SBC round-trip SNR thresholds** were lowered to fit the noisy test signal: joint stereo 25 dB, stereo 48 kHz 17 dB, mono 18 dB, dual channel 20 dB, identical L/R 30 dB. These are sanity checks, not quality targets.
- **Open validation gaps:**
  - SBC frames have not been decoded by an external decoder yet (ffmpeg or BlueZ `sbcdec`; ffmpeg was not installed).
  - AAC LATM framing needs checking against captures from real headsets.

## Next steps
1. Install the WDK for SDK 10.0.26100, plus its VS extension.
2. M2: add a KMDF driver project (`driver/OpenA2dp.vcxproj`) and an INF to `OpenA2dp.slnx`. Then open an L2CAP channel through BRBs on PSM 0x0019.
3. Optional: validate SBC with ffmpeg/sbcdec.
