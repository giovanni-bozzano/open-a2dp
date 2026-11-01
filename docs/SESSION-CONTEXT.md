# OpenA2dp – Session Context (handoff)

Read this first in a new chat. It summarizes the project state; the authoritative plan, action log and next steps are in [A2DP-Driver-Plan.md](A2DP-Driver-Plan.md).

## Origin and goal
- Started from questions about the commercial "Alternative A2DP Driver": why it connects audio faster, which codecs it supports, and how it relates to the Intel Bluetooth driver.
  - That driver replaces only Microsoft's A2DP profile driver (`BthA2dp.sys`).
  - It does not replace the Intel radio driver (`ibtusb.sys`), `bthport.sys`/`bthusb.sys`, HFP, or AVRCP.
- **Goal:** an open-source replacement for `BthA2dp.sys` (A2DP Source) on Windows 11.
  - Codecs: SBC and AAC-LC first, with aptX/LDAC pluggable later.
  - Test the driver safely in a VM first; ship later with EV/attestation signing.

## Status (2026-10-31)
| Milestone | State |
|---|---|
| M1 codec library + tests | **Done** (commit 6fc217d; 73 tests pass) |
| M2 driver skeleton + L2CAP | **Done** (signaling channel opens to a real headset in the VM) |
| M3 AVDTP negotiation | **In progress** (SBC stream reaches Streaming with a real headset; BlueZ sink and sink-initiated configuration not yet validated) |
| M4 ACX endpoint + rings | **Done** (Speaker0 endpoint; Windows audio plays through the headset) |
| M5 encoder service + streaming | **In progress** (audible SBC, 30+ min stable; `OpenA2dpSvc.exe` implemented and stable on the VM; the AAC negotiation race is fixed (#72) but AAC has not yet been validated end-to-end on a real sink) |
| M5b codec configuration | Not started |
| M6 config UI + install/uninstall | Not started |
| M7 system testing | Not started |
| M8 signing & release | Not started |

## Architecture (current)
- **`OpenA2dp.sys`:** a KMDF profile driver above `bthport`. It handles L2CAP through BRBs (PSM 0x0019), the AVDTP source state machine, the ACX audio endpoint, kernel SBC encoding and RTP media.
- **SBC stays in the kernel** using the integer-only `SbcFixed` encoder (no FPU state, proven audible and 30+ min stable), so the mandatory codec works without any user-mode component.
- **`OpenA2dpSvc.exe`:** AAC only. It pulls PCM from a shared ring, encodes with Media Foundation, and pushes LATM frames back for RTP/L2CAP send. Media Foundation is user-mode only, and a codec fault there cannot BSOD. It enumerates the per-device control devices, registers the encoder, maps the rings and then parks in `WAIT_EVENT`; if the driver disappears it logs `device gone`, closes and waits for the device to come back. Built and running stably on the VM; AAC itself is not yet validated on hardware.
- **Driver/service boundary (implemented in the driver, Action Log #66):** `common/include/OpenA2dp/Ioctl.h` holds the ABI (named control-device link prefix, six IOCTLs, shared-ring layout, frame record, status/event structs), [AAC-Service-Boundary.md](AAC-Service-Boundary.md) the lifecycle rules and `driver/Service.cpp` the driver side. The interface lives on a per-device control device (SYSTEM + Administrators) so the ACX device object keeps its default security and KS request path. One non-paged block per device holds the PCM ring (driver -> service) and the frame ring (service -> driver), mapped once into the service process; file cleanup clears the registration and unmaps, which is how a crashed service is detected. `WAIT_EVENT` is tick-driven by the media timer; the service owns pacing and the driver only adds RTP headers. The driver always advertises the AAC SEP and refuses AAC in `SelectConfig` while no service is registered, so a late registration still negotiates AAC and a device without the service still gets SBC.
- **`common/`** is header-only and kernel-safe (no STL or exceptions): AVDTP/RTP/ring-buffer headers, the driver/service ABI (`Ioctl.h`) and codec selectors `SbcConfig.h`, `AacConfig.h`.
- **`codecs/`** holds `IA2dpCodec`, the SBC encoder, AAC IE/negotiation and the Media Foundation/LATM encoder used by tests and the future service.
- Negotiation priority is AAC > SBC. The source always advertises SEID 1 (SBC) and SEID 2 (AAC); the AAC SEP stays present even without a service because `SelectConfig` already refuses AAC while no encoder is registered, and adding the SEP at registration time lost the race against the headset's signaling connect. The session keeps the scored candidates and retries SBC when the sink rejects the AAC configuration. A forced codec can be set; if it isn't available, negotiation falls back to the normal order.

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
out\x64\Debug\CodecTests.exe          # 73 tests
out\x64\Debug\CodecTests.exe Sbc_     # name-prefix filter
```

Driver output: `out\x64\Debug\OpenA2dp\` (`.sys`, `.inf`, `.cat`); the driver is test-signed at build time. `out\x64\Debug\OpenA2dpCtl.exe` is the diagnostic CLI. Deploy with `scripts/deploy-vm.ps1` (vmrun: optional snapshot revert, copy package, CLI and exported cert, trust cert, `pnputil /add-driver ... /install`). Every non-driver project links the CRT statically (`Directory.Build.props`), because the test VM has no Visual C++ debug runtime: `OpenA2dpCtl.exe` then depends only on `KERNEL32.dll` and starts in the guest.

## Driver implementation notes
- **`Driver.cpp`:** `EvtDeviceAdd` queries BTHENUM device info early for the Class of Device (endpoint type) and creates the ACX circuit; `OnPrepareHardware` retrieves the address/name again and sets `DEVPKEY_Device_FriendlyName` to `<name> [OpenA2DP]` (address if no name). Self-managed IO init registers the L2CAP server and opens the signaling channel. Suspend calls `MediaStop` first, then unregisters the server, flushes the disconnect work item and closes signaling.
- **`L2cap.cpp`:** queries `BTH_PROFILE_DRIVER_INTERFACE` (v0x0200); opens PSM 0x19 asynchronously with a preallocated request and BRB; signaling MTU 672, media MTU 895. Registers a server for headset-initiated connections (accepts signaling through the shared open request, refuses other channels through a separate preallocated request). Media sends use a 1 s timeout so a stalled ACL transfer cannot block teardown while holding `MediaLock`/`SessionLock`.
- **`Signaling.cpp`:** L2CAP ACL read loop, PASSIVE work item under `SessionLock`, 3 s WDF response timer, SBC/AAC selector (SBC 1, AAC 2; AAC only while a service is registered) and `AacConfig::Validate` for sink-proposed AAC. `ResetSession` always registers both local SEPs; sink-proposed AAC is rejected while no encoder exists. Drives `Avdtp::SourceSession` through DISCOVER, GET_CAPABILITIES, SET_CONFIGURATION, OPEN, START/SUSPEND/CLOSE; retries SBC if the sink rejects AAC; opens the media channel and sends START; requests SUSPEND after a media send failure.
- **`Media.cpp`:** 10 ms timer plus work item while Streaming. SBC: reads endpoint PCM, encodes in the kernel, builds RTP packets within the media MTU, 60 ms preroll. AAC: drains the frame ring (one record = one RTP packet, timestamp from the record's `samples`), completes `WAIT_EVENT` every tick, counts an underrun after 200 ms and stops/SUSPENDs after 2 s or on a corrupt record. Stops/requests SUSPEND on send failure in both modes.
- **`Service.cpp`:** per-device named control device (`\Device\OpenA2dpCtl<address>`, link `\DosDevices\OpenA2dpCtl-<address>`) with the service SDDL, `WdfControlFinishInitializing`, and a parallel queue (a parked `WAIT_EVENT` must not block status/set) at PASSIVE level; `REGISTER_ENCODER`/`MAP_RINGS`/`WAIT_EVENT`/`GET_STATUS`/`SET_PREFERENCE`/`GET_VERSION`; one non-paged block with both rings mapped once per handle (process referenced so teardown can attach and unmap in the right context); `WAIT_EVENT` parks one request and is completed by stream events or the media tick, with the cancel race handled. ABI 2 `Status` reports the negotiated codec and the current preference under `SessionLock`. The control device's parent must stay the driver object, so `ServiceStop` clears its context pointer before deleting it.
- **`OpenA2dpCtl.exe` (`tools/OpenA2dpCtl`):** enumerates the control devices by scanning `QueryDosDevice` for `OpenA2dpCtl-<12 hex address>` and opens `\\.\<link>`. `status`/`--watch`, `list`, `version`, and `codec auto|sbc|aac` via `SET_PREFERENCE`; prints codec, preference, encoder registration, rings and counters, and warns that a codec change applies at the next negotiation (reconnect) or that AAC needs a registered encoder. `--device` takes a MAC address (with or without separators), a 1-based index or a partial address. Must run elevated.
- **`Audio.cpp`:** ACX 1.1 render circuit `Speaker0`, fixed 44.1 kHz/16-bit/stereo; accepts 1 or 2 RT packets; reports a 128-byte FIFO to the audio engine; RT buffer is page-rounded and zeroed. Endpoint category comes from the headset Class of Device: headset/hands-free/headphones -> `KSNODETYPE_HEADPHONES`, otherwise `KSNODETYPE_SPEAKER`. No ACX volume/mute elements: Windows already applies volume and mute in software (revisit only with AVRCP absolute volume).
- **Logs:** `DbgPrintEx(DPFLTR_IHVDRIVER_ID, INFO)` with an "OpenA2dp:" prefix. If symbols don't load, set the Debug Print Filter registry key instead of `nt!Kd_IHVDRIVER_Mask`.

## Validated on VM / real headset
- Signaling channel open, AVDTP discovery/configuration/open/start, media channel open, Streaming.
- Test tone then Windows audio over SBC 44.1 kHz; 30+ min playback without reported dropouts.
- Headset-initiated reconnect; clean remote teardown (AVDTP state 0, media stopped, channels closed, no hang).
- Redeploy no longer hangs after the suspend-order and send-timeout fixes.
- Endpoint appears as `<Headphones|Speakers> (<name> [OpenA2DP])`; Settings shows 2 ch / 16 bit / 44100 Hz for the OpenA2dp render endpoint.
- A "fatal system error" break was triaged as bugcheck 0x80 `NMI_HARDWARE_FAILURE` with all CPUs idle and OpenA2dp absent from every stack: an externally injected NMI (hypervisor), not a driver bug.
- `OpenA2dpSvc.exe` starts, opens the control device, registers (`max frame 883, 44100 Hz, 2 ch`), maps both rings and parks in `WAIT_EVENT` without spurious events; a PnP disable/enable produces `device gone` -> `done` -> `no control devices found` and then a clean reopen/register/map.
- The service now serves **multiple headsets at once**: it keeps a worker table instead of joining device threads, so a second headset that connects while the first is streaming is still registered (verified: `opening ...E807BF1B3D3F` + `opening ...41424632D42A`, both `registered` + `rings mapped`). It re-registers within the same second after a devnode restart.
- SBC still streams while the service is registered (`AVDTP Streaming`, `media Streaming`, `codec SBC`, counters advancing, 0 dropped/underruns/send failures), so the service does not disturb the kernel path.

## Key decisions and lessons
- **Hybrid encoding decided (Action Log #48):** kernel SBC, user-mode AAC only.
- **Service boundary (Action Log #65/#66):** SPSC rings in one driver-allocated non-paged block, mapped once into the service process; one LATM frame per RTP packet; the service owns AAC pacing; registration is cleared by file cleanup so a crashed service cannot wedge the driver; `WAIT_EVENT` is ticked by the media timer instead of polling or sharing an event handle. A frame record's `size` is exact (no padding) so the payload length is unambiguous, and only the frame ring is flushed by the driver because the service owns the PCM consumer index.
- **Multi-SEP negotiation (Action Log #67, revised in #72):** SEID 1 is SBC and always present; SEID 2 is AAC and also always advertised, because the service can register after the headset opened the signaling channel. Whether AAC is *selectable* is decided per negotiation by `SelectConfig`, which checks `ServiceRegistered` at the moment the capabilities arrive. The session scores every sink SEP and every Media Codec entry in a SEP, keeps the candidates in descending order, and on a rejected SET_CONFIGURATION configures the next one, so an AAC-capable sink that refuses AAC still gets SBC without a reconnect.
- **Manual codec control (Action Log #68):** `OpenA2dpCtl.exe` is the interim way to see the current codec and force auto/SBC/AAC. A preference change applies at the next AVDTP negotiation (reconnect the headset); in-driver renegotiation is deferred. The control queue is parallel so status works while the service has a `WAIT_EVENT` parked.
- **SBC reference-decoder fix:** synthesis is a direct cosine-modulated filterbank over a 10-block history; synthesis gain is `-M`; measured delay is 73 samples for 8 subbands and 37 for 4; filterbank-limited SNR is about 65–67 dB.
- **SBC round-trip SNR thresholds** were lowered for the noisy test signal (joint stereo 25 dB, stereo 48 kHz 17 dB, mono 18 dB, dual channel 20 dB, identical L/R 30 dB). They are sanity checks, not quality targets.
- **ACX gotchas:** report a small FIFO (128 bytes, not the 64 KiB ring) or the audio engine rejects the stream; accept 1 or 2 RT packets; page-round and zero the RT buffer; the DEFAULT mode format list is not needed; stale endpoint form factor/name comes from the Windows endpoint cache and is fixed by reinstalling the device.
- **Locking/teardown:** stop media before unregistering the L2CAP server; never hold `MediaLock`/`SessionLock` across an unbounded ACL send.
- **Plan editing:** use exact-match replacements for whole rows/lines; never PowerShell regex rewrites; re-read the section afterward. Do not truncate existing content.

## Open validation gaps
- BlueZ/PipeWire sink and the sink-initiated acceptor path (deferred D3; no Linux Bluetooth host).
- AAC end to end: the negotiation race is fixed (#72), the service now runs as an auto-start Windows service, and the driver logs the sink's codec offer (`AVDTP sink offers codec N`). The test headset (Positive Vibration 2 BT) advertises **SBC only** (`sink offers codec 0, 4 IE bytes`), so AAC cannot be negotiated with it; validation needs a headset that advertises AAC. The first check on such a device is `AVDTP configured codec 2` + `media: AAC ...` + audible output. A stream already negotiated as SBC stays SBC until the headset reconnects (logged on registration).
- LATM framing against real captures.
- OpenA2dpCtl hang: fixed. Root cause was `SubmitBrbSynchronously` (L2CAP open/close/register/unregister) using NULL send options, so a BRB that the BT stack never completed blocked forever; during pairing that held `SessionLock` and wedged every control-device IOCTL (no Ctrl+C recovery). All BRBs now time out after 5 s, `SignalingStop`'s `ReadIdle`/`OpenIdle` waits are bounded to 5 s, a self-deadlock in `SignalingStop` (called from the media-open failure path) is guarded by `InSignalingWorkItem`, and the CLI uses overlapped I/O with a 5 s timeout.
- Media channel open failure (`media channel open failed 0xC00000D0 (L2CAP response 4/0)`): PANTONE refuses every outbound media channel. The Wireshark capture (Action Log #76) shows it drives AVDTP itself in parallel with us (own Discover/GetCapabilities/SET_CONFIGURATION at 48 kHz, plus HFP over RFCOMM), so the driver now lets a headset-initiated connection configure first (700 ms), advertises only 44.1 kHz SBC, and resets the session when the link closes. To confirm on the headset: the log should show `signaling: headset-initiated; waiting ...`, then `AVDTP state 4` (Configured), `incoming media connection` and `AVDTP streaming`; if it stays silent you get `signaling: headset did not configure the stream; starting discovery` and the old outbound path. Still open: the Windows endpoint stays listed after the headset disconnects (an ACX jack plug-state element would mark it unplugged); the three-retry outbound path still tears the link down on persistent refusal.
- The diagnostic CLI is deployed and starts in the VM (`help`, `list`/`status` reporting no device while the headset is disconnected); it has not yet been exercised against a bound headset.
- End-to-end latency, pause/resume, switching the default output away from and back to Speaker0 (deferred D1).
- Send-failure path: headset out of range or adapter removed while streaming; confirm SUSPEND and no hang (deferred D2).
- SBC decode with an external decoder (ffmpeg / BlueZ `sbcdec`).
- M5b codec settings store and UI, clean install/uninstall, system testing, release signing.

## Next steps (in order)
0. Endpoint fix (#77, deployed as 1.26.1101.1120): power-cycle or disconnect a headset and confirm its Windows output endpoint shows as unplugged. Then reconnect and confirm it shows as plugged. Report which one happens.
1. M5: validate AAC end to end. `OpenA2dpSvc.exe` is implemented and stable; deploy the #72 negotiation fix (needs a reboot to activate), start the service before the headset connects, and confirm `AVDTP configured codec 2`, `media: AAC ...`, audible output and `OpenA2dpCtl status` counters. Then re-check that SBC still negotiates with the service stopped.
2. M5b: codec settings store (per-headset registry values plus global defaults), read at negotiation time.
3. M5b: preference-aware `SbcConfig`/`AacConfig` selection and presets, checked against media MTU; unit tests.
4. M5b: GET/SET_CODEC_CONFIG IOCTLs (GET_STATUS and SET_PREFERENCE already exist for `OpenA2dpCtl`) and the desktop configuration UI; apply via SUSPEND + RECONFIGURE + START or reconnect.
5. M5b: multi-rate ACX endpoint (44.1/48 kHz, mono if selected), last because it touches the validated audio path.
6. M6: finish the configuration UI (status, force codec) and clean install/uninstall that restores `BthA2dp.sys`.
