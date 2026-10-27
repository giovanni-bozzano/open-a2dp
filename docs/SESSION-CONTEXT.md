# OpenA2dp – Session Context (handoff)

Read this first in a new chat. It summarizes the project state; the authoritative plan, action log and next steps are in [A2DP-Driver-Plan.md](A2DP-Driver-Plan.md).

## Origin and goal
- Started from questions about the commercial "Alternative A2DP Driver": why it connects audio faster, which codecs it supports, and how it relates to the Intel Bluetooth driver.
  - That driver replaces only Microsoft's A2DP profile driver (`BthA2dp.sys`).
  - It does not replace the Intel radio driver (`ibtusb.sys`), `bthport.sys`/`bthusb.sys`, HFP, or AVRCP.
- **Goal:** an open-source replacement for `BthA2dp.sys` (A2DP Source) on Windows 11.
  - Codecs: SBC and AAC-LC first, with aptX/LDAC pluggable later.
  - Test the driver safely in a VM first; ship later with EV/attestation signing.

## Status (2026-10-27)
| Milestone | State |
|---|---|
| M1 codec library + tests | **Done** (commit 6fc217d; 44 tests pass) |
| M2 driver skeleton + L2CAP | **Done** (signaling channel opens to a real headset in the VM) |
| M3 AVDTP negotiation | **In progress** (SBC stream reaches Streaming with a real headset; BlueZ sink and sink-initiated configuration not yet validated) |
| M4 ACX endpoint + rings | **Done** (Speaker0 endpoint; Windows audio plays through the headset) |
| M5 encoder service + streaming | **In progress** (audible SBC, 30+ min stable; kernel-safe AAC selector done; AAC service pending) |
| M5b codec configuration | Not started |
| M6 config UI + install/uninstall | Not started |
| M7 system testing | Not started |
| M8 signing & release | Not started |

## Architecture (current)
- **`OpenA2dp.sys`:** a KMDF profile driver above `bthport`. It handles L2CAP through BRBs (PSM 0x0019), the AVDTP source state machine, the ACX audio endpoint, kernel SBC encoding and RTP media.
- **SBC stays in the kernel** using the integer-only `SbcFixed` encoder (no FPU state, proven audible and 30+ min stable), so the mandatory codec works without any user-mode component.
- **`OpenA2dpSvc.exe` (not yet implemented):** AAC only. It will pull PCM from a shared ring, encode with Media Foundation, and push LATM frames back for RTP/L2CAP send. Media Foundation is user-mode only, and a codec fault there cannot BSOD.
- **Driver/service boundary (planned):** device interface plus IOCTLs GET_VERSION, REGISTER_ENCODER, MAP_RINGS, WAIT_EVENT (inverted call), GET_STATUS, SET_PREFERENCE. The driver advertises/selects AAC only while the service is registered; otherwise it falls back to SBC.
- **`common/`** is header-only and kernel-safe (no STL or exceptions): AVDTP/RTP/ring-buffer headers and codec selectors `SbcConfig.h`, `AacConfig.h`.
- **`codecs/`** holds `IA2dpCodec`, the SBC encoder, AAC IE/negotiation and the Media Foundation/LATM encoder used by tests and the future service.
- Negotiation priority is AAC > SBC. A forced codec can be set; if it isn't available, negotiation falls back to the normal order.

## Environment
- Visual Studio Community 2026 18.11.1 at `D:\Program Files\Microsoft Visual Studio\18\Community`, toolset v145, C++20, x64.
- Windows SDK 10.0.26100.0 for user-mode projects.
- **WDK comes from NuGet, not an MSI.** `driver/packages.config` pins `Microsoft.Windows.WDK.x64`, `Microsoft.Windows.SDK.CPP` and `Microsoft.Windows.SDK.CPP.x64` at 10.0.28000.2526; they restore into the global NuGet cache. The driver project pins `WindowsTargetPlatformVersion=10.0.28000.0`.
- .NET SDK 10.0.402 (used for the `.slnx` solution).
- Repo: `D:\repos\open-a2dp` (git, branch `master`).
- Test VM: VMware Workstation Pro, Windows 11 x64, USB Bluetooth dongle passed through, Secure Boot off, test signing on, KDNET debugging, snapshot before install. WinDbg via `WinDbgX -k net:port=50000,key=...`; credentials in ignored `.local/`.

## Build / test
Use the **64-bit** MSBuild (the 32-bit one fails with TRK0005 `stampinf.exe` because the NuGet WDK ships only x64/ARM64 tools).

```
& "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
out\x64\Debug\CodecTests.exe          # 44 tests
out\x64\Debug\CodecTests.exe Sbc_     # name-prefix filter
```

Driver output: `out\x64\Debug\OpenA2dp\` (`.sys`, `.inf`, `.cat`); the driver is test-signed at build time. Deploy with `scripts/deploy-vm.ps1` (vmrun: optional snapshot revert, copy package and exported cert, trust cert, `pnputil /add-driver ... /install`).

## Driver implementation notes
- **`Driver.cpp`:** `EvtDeviceAdd` queries BTHENUM device info early for the Class of Device (endpoint type) and creates the ACX circuit; `OnPrepareHardware` retrieves the address/name again and sets `DEVPKEY_Device_FriendlyName` to `<name> [OpenA2DP]` (address if no name). Self-managed IO init registers the L2CAP server and opens the signaling channel. Suspend calls `MediaStop` first, then unregisters the server, flushes the disconnect work item and closes signaling.
- **`L2cap.cpp`:** queries `BTH_PROFILE_DRIVER_INTERFACE` (v0x0200); opens PSM 0x19 asynchronously with a preallocated request and BRB; signaling MTU 672, media MTU 895. Registers a server for headset-initiated connections (accepts signaling through the shared open request, refuses other channels through a separate preallocated request). Media sends use a 1 s timeout so a stalled ACL transfer cannot block teardown while holding `MediaLock`/`SessionLock`.
- **`Signaling.cpp`:** L2CAP ACL read loop, PASSIVE work item under `SessionLock`, 3 s WDF response timer, SBC selector. Drives `Avdtp::SourceSession` through DISCOVER, GET_CAPABILITIES, SET_CONFIGURATION, OPEN, START/SUSPEND/CLOSE; opens the media channel and sends START; requests SUSPEND after a media send failure.
- **`Media.cpp`:** 10 ms timer plus work item while Streaming; reads endpoint PCM, encodes SBC frames, builds RTP packets within the media MTU, 60 ms preroll, and stops/requests SUSPEND on send failure.
- **`Audio.cpp`:** ACX 1.1 render circuit `Speaker0`, fixed 44.1 kHz/16-bit/stereo; accepts 1 or 2 RT packets; reports a 128-byte FIFO to the audio engine; RT buffer is page-rounded and zeroed. Endpoint category comes from the headset Class of Device: headset/hands-free/headphones -> `KSNODETYPE_HEADPHONES`, otherwise `KSNODETYPE_SPEAKER`. No ACX volume/mute elements: Windows already applies volume and mute in software (revisit only with AVRCP absolute volume).
- **Logs:** `DbgPrintEx(DPFLTR_IHVDRIVER_ID, INFO)` with an "OpenA2dp:" prefix. If symbols don't load, set the Debug Print Filter registry key instead of `nt!Kd_IHVDRIVER_Mask`.

## Validated on VM / real headset
- Signaling channel open, AVDTP discovery/configuration/open/start, media channel open, Streaming.
- Test tone then Windows audio over SBC 44.1 kHz; 30+ min playback without reported dropouts.
- Headset-initiated reconnect; clean remote teardown (AVDTP state 0, media stopped, channels closed, no hang).
- Redeploy no longer hangs after the suspend-order and send-timeout fixes.
- Endpoint appears as `<Headphones|Speakers> (<name> [OpenA2DP])`; Settings shows 2 ch / 16 bit / 44100 Hz for the OpenA2dp render endpoint.
- A "fatal system error" break was triaged as bugcheck 0x80 `NMI_HARDWARE_FAILURE` with all CPUs idle and OpenA2dp absent from every stack: an externally injected NMI (hypervisor), not a driver bug.

## Key decisions and lessons
- **Hybrid encoding decided (Action Log #48):** kernel SBC, user-mode AAC only.
- **SBC reference-decoder fix:** synthesis is a direct cosine-modulated filterbank over a 10-block history; synthesis gain is `-M`; measured delay is 73 samples for 8 subbands and 37 for 4; filterbank-limited SNR is about 65–67 dB.
- **SBC round-trip SNR thresholds** were lowered for the noisy test signal (joint stereo 25 dB, stereo 48 kHz 17 dB, mono 18 dB, dual channel 20 dB, identical L/R 30 dB). They are sanity checks, not quality targets.
- **ACX gotchas:** report a small FIFO (128 bytes, not the 64 KiB ring) or the audio engine rejects the stream; accept 1 or 2 RT packets; page-round and zero the RT buffer; the DEFAULT mode format list is not needed; stale endpoint form factor/name comes from the Windows endpoint cache and is fixed by reinstalling the device.
- **Locking/teardown:** stop media before unregistering the L2CAP server; never hold `MediaLock`/`SessionLock` across an unbounded ACL send.
- **Plan editing:** use exact-match replacements for whole rows/lines; never PowerShell regex rewrites; re-read the section afterward. Do not truncate existing content.

## Open validation gaps
- BlueZ/PipeWire sink and the sink-initiated acceptor path (deferred D3; no Linux Bluetooth host).
- AAC service, AAC SEP registration/fallback, LATM framing against real captures.
- End-to-end latency, pause/resume, switching the default output away from and back to Speaker0 (deferred D1).
- Send-failure path: headset out of range or adapter removed while streaming; confirm SUSPEND and no hang (deferred D2).
- SBC decode with an external decoder (ffmpeg / BlueZ `sbcdec`).
- M5b codec settings store and UI, clean install/uninstall, system testing, release signing.

## Next steps (in order)
1. M5: design the driver/service boundary for AAC (device interface + IOCTLs, shared PCM ring, LATM frame ring back to the driver); SBC path untouched.
2. M3/M5: include `AacConfig.h` in the driver, register an AAC SEP only while the service is registered, policy AAC > SBC with SBC fallback; then implement `OpenA2dpSvc.exe` (MF encoder + `LatmWriter`).
3. M5b: codec settings store (per-headset registry values plus global defaults), read at negotiation time.
4. M5b: preference-aware `SbcConfig`/`AacConfig` selection and presets, checked against media MTU; unit tests.
5. M5b: GET/SET_CODEC_CONFIG and GET_STATUS IOCTLs; desktop configuration UI; apply via SUSPEND + RECONFIGURE + START or reconnect.
6. M5b: multi-rate ACX endpoint (44.1/48 kHz, mono if selected), last because it touches the validated audio path.
7. M6: finish the configuration UI (status, force codec) and clean install/uninstall that restores `BthA2dp.sys`.
