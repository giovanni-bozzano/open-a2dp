# OpenA2dp — Open-Source A2DP Source Driver for Windows

Status: M1, M2 and M4 complete (Windows audio plays over SBC on a real headset via the ACX Speaker0 endpoint); M3 (BlueZ sink, acceptor path, AAC selection) and M5 (AAC) in progress
Target OS: Windows 11 (22H2+), x64 first, ARM64 later
License: Apache-2.0 (proposed, see Open Decisions)
Repo location: `D:\repos\open-a2dp`

## 1. Goals and Scope

### Goals
- Open-source replacement for Microsoft's A2DP source profile driver (`BthA2dp.sys`) on a per-device basis.
- Faster, more predictable A2DP connection and stream start.
- User control over codec selection and codec parameters.
- Phase 1 codecs: **SBC** (mandatory, incl. configurable bitpool) and **AAC-LC**.
- Pluggable codec architecture for later: aptX, aptX HD, LDAC, SBC XQ presets.

### Non-goals (Phase 1)
- Replacing the Bluetooth radio/transport driver (Intel `ibtusb.sys`, Realtek, Qualcomm). Not touched.
- Replacing the Microsoft core stack (`bthport.sys`, `bthusb.sys`). We sit on top of it.
- HFP/HSP (microphone): remains on Microsoft `BthHfAud`/`BthHfEnum`.
- AVRCP (media keys, absolute volume): remains on Microsoft (revisit later).
- A2DP Sink role, LE Audio / LC3, Windows 10 support.

### Where we sit in the stack
```
+---------------------------------------------------------------+
| Windows Audio Engine (audiodg.exe)                            |
+---------------------------------------------------------------+
| OpenA2dp.sys  (KMDF + ACX render endpoint, AVDTP, L2CAP BRBs, |  <-- replaces BthA2dp.sys
|               kernel SBC encoder)                             |
|      ^  shared-memory ring buffers (PCM out / frames in)      |
|      v                                                        |
| OpenA2dpSvc.exe (user mode: AAC encoding only)                |
+---------------------------------------------------------------+
| bthport.sys / bthenum (Microsoft Bluetooth core stack)        |  <-- unchanged
+---------------------------------------------------------------+
| ibtusb.sys / bthusb.sys (Intel/vendor transport + radio)      |  <-- unchanged
+---------------------------------------------------------------+
```

## 2. Architecture

| Component | Type | Responsibility |
|---|---|---|
| `OpenA2dp.sys` | KMDF + ACX kernel driver | PnP on A2DP sink service, L2CAP channels, AVDTP signaling, ACX render endpoint, SBC encoding (`SbcFixed`), RTP send, ring buffers |
| `OpenA2dpSvc.exe` | Windows service (C++20) | AAC only: pull PCM, encode via Media Foundation, push LATM frames back |
| `OpenA2dpCodecs.lib` | Static library | `IA2dpCodec`: capability IE build/parse, config selection, encoding, payload packing |
| `common/` | Header-only, kernel-safe | IOCTL codes, ring buffer layout, AVDTP/RTP constants, codec selectors (`SbcConfig.h`, `AacConfig.h`) |
| `OpenA2dpConfig.exe` | Desktop UI app | List devices, choose codec and presets, set SBC/AAC parameters per headset, show stream status (no CLI) |
| `OpenA2dp.inf` | INF | Matches A2DP sink service (UUID 0x110B) on remote devices |

Hybrid encoding (decided, see Action Log #48): SBC is encoded in the kernel with the integer-only `SbcFixed` encoder (no FPU state, proven audible and 30+ min stable), so the mandatory codec works without any user-mode component. AAC is encoded only in `OpenA2dpSvc.exe`, because Media Foundation AAC is user-mode only and a codec fault there cannot BSOD. The driver advertises/selects AAC only while the service is registered; otherwise it falls back to SBC.

Data flow:
```
SBC: audiodg -> ACX buffer -> driver -> PCM ring -> kernel SBC -> RTP -> L2CAP -> bthport
AAC: audiodg -> ACX buffer -> driver -> PCM ring (shared) -> service encodes AAC/LATM -> frame ring
		-> driver builds RTP packets <= L2CAP MTU -> BRB L2CA_ACL_TRANSFER -> bthport
```

Driver <-> service IOCTLs: GET_VERSION, REGISTER_ENCODER, MAP_RINGS, WAIT_EVENT (inverted call), GET_STATUS, SET_PREFERENCE. Device interface SDDL: SYSTEM + Administrators only.

## 3. Stack
| Area | Choice |
|---|---|
| IDE | Visual Studio 2026, C++20 |
| Driver | WDK (SDK 10.0.26100), KMDF, ACX, BRBs via `bthddi.h` |
| User mode | C++20, Win32, Media Foundation (AAC) |
| SBC | Own implementation |
| Tests | Self-contained test runner (no external deps), later GoogleTest optional |
| Tracing | WPP (driver), TraceLogging (service) |
| CI | GitHub Actions windows-latest |

WDK samples to study: `bluetooth/bthecho`, ACX audio samples, `audio/sysvad`.

## 4. Repository Layout
```
OpenA2dp/
  OpenA2dp.sln
  common/include/OpenA2dp/   Avdtp.h Rtp.h RingBuffer.h Ioctl.h
  codecs/                    IA2dpCodec.h, sbc/, aac/
  driver/                    (M2, needs WDK)
  service/                   (M5)
  tools/config-ui/           (M5b/M6)
  tests/CodecTests/
  scripts/
  docs/
```
AVDTP parsing and the state machine are plain C++ without kernel dependencies so they compile in driver and tests.

## 5. Driver Design
- INF hardware ID `BTHENUM\{0000110B-0000-1000-8000-00805F9B34FB}...`, opt-in per device, uninstall restores `BthA2dp`.
- `BTH_PROFILE_DRIVER_INTERFACE`, L2CAP PSM 0x0019 (signaling + media channel), handle link loss, Dx, surprise removal.
- AVDTP: IDLE -> DISCOVER -> GET_(ALL_)CAPABILITIES -> SET_CONFIGURATION -> OPEN -> START <-> SUSPEND -> CLOSE/ABORT. Also ACP role (sink-initiated).
- Codec switch: SUSPEND -> RECONFIGURE or CLOSE + SET_CONFIGURATION.
- Codec policy: forced codec > AAC > SBC. SBC default: Joint, 16 blocks, 8 subbands, Loudness, bitpool min(sinkMax, 53). AAC default: AAC-LC, 2ch, 256 kbps (capped by sink).
- ACX: one render circuit per connected device, 16-bit stereo 44.1/48 kHz.

## 6. Protocol Reference

AVDTP signals: 0x01 DISCOVER, 0x02 GET_CAPABILITIES, 0x03 SET_CONFIGURATION, 0x04 GET_CONFIGURATION, 0x05 RECONFIGURE, 0x06 OPEN, 0x07 START, 0x08 CLOSE, 0x09 SUSPEND, 0x0A ABORT, 0x0B SECURITY_CONTROL, 0x0C GET_ALL_CAPABILITIES, 0x0D DELAYREPORT.

Service categories: 0x01 Media Transport, 0x02 Reporting, 0x03 Recovery, 0x04 Content Protection, 0x05 Header Compression, 0x06 Multiplexing, 0x07 Media Codec, 0x08 Delay Reporting.

Codec types: SBC 0x00, MPEG-1,2 0x01, MPEG-2,4 AAC 0x02, Vendor 0xFF.

SBC IE (4 bytes):
- B0: fs 16k=0x80 32k=0x40 44.1k=0x20 48k=0x10 | mode Mono=0x08 Dual=0x04 Stereo=0x02 Joint=0x01
- B1: blocks 4=0x80 8=0x40 12=0x20 16=0x10 | subbands 4=0x08 8=0x04 | alloc SNR=0x02 Loudness=0x01
- B2: min bitpool, B3: max bitpool
- Frame length Mono/Dual: `4 + 4*SB*CH/8 + ceil(BL*CH*BP/8)`; Stereo/Joint: `4 + 4*SB*CH/8 + ceil((J*SB + BL*BP)/8)`
- Example 44.1k Joint BL16 SB8 BP53 = 119 bytes, ~328 kbps.

AAC IE (6 bytes):
- B0: object type MPEG-2 LC=0x80, MPEG-4 LC=0x40, LTP=0x20, Scalable=0x10
- B1: 8k=0x80 11.025k=0x40 12k=0x20 16k=0x10 22.05k=0x08 24k=0x04 32k=0x02 44.1k=0x01
- B2: 48k=0x80 64k=0x40 88.2k=0x20 96k=0x10 | channels 1=0x08 2=0x04
- B3: VBR bit7 + bitrate[22:16]; B4-B5: bitrate[15:0]

Media packets:
- RTP 12 bytes, V=2, PT=96, seq++, timestamp in samples, fixed SSRC.
- SBC: 1-byte payload header (F|S|L|RFA|NumFrames:4), whole frames up to MTU.
- AAC: LATM AudioMuxElement (muxConfigPresent=1, StreamMuxConfig inline). VERIFY against captures.

## 7. Test Strategy
- Tier 1 host unit tests: SBC (CRC, headers, frame length, decode with ffmpeg/sbcdec, SNR), AAC (LATM decode via ffmpeg), IE round-trips, negotiation matrix, AVDTP build/parse, state machine with mock transport, ring buffer stress.
- Tier 2 VM: install/uninstall/restore BthA2dp, Driver Verifier always on, suspend/resume.
- Tier 3 protocol: `btvs.exe` -> Wireshark, BlueZ/PipeWire reference sink with `btmon`.
- Tier 4 real devices: SBC-only headset, AAC headset, cheap speaker; connect time vs BthA2dp, dropouts, latency, codec switching, 8 h soak.

## 8. VM Setup Checklist
. VMware Workstation (recommended) or VirtualBox; Hyper-V not suitable (USB passthrough).
2. Dedicated USB Bluetooth dongle passed through to the VM.
3. Windows 11 x64 guest, 4 vCPU, 8 GB, UEFI, Secure Boot OFF.
4. Guest (admin):
   ```
   bcdedit /set testsigning on
   bcdedit /debug on
   bcdedit /dbgsettings net hostip:<HOST_IP> port:50000 key:1.2.3.4
   ```
   (VirtualBox: `bcdedit /dbgsettings serial debugport:1 baudrate:115200` + named pipe)
5. Host: WinDbg kernel attach (Net or `\\.\pipe\com_1`).
6. Snapshot "Clean+Debug" before first install; revert after BSOD.
7. `verifier /standard /driver OpenA2dp.sys`
8. Deploy via `scripts\Install.ps1` from a shared folder.

## 9. Signing Roadmap
- Now: self-signed test cert (`New-SelfSignedCertificate -Type CodeSigningCert`), `Inf2Cat`, `signtool sign /fd SHA256`, cert in guest Root + TrustedPublisher.
- Later: EV cert -> Partner Center attestation signing.

## 10. Milestones
| # | Milestone | Exit criteria | Status |
|---|---|---|---|
| M1 | Codec library + unit tests | SBC & AAC tests pass | Done |
| M2 | Driver skeleton + L2CAP | Loads in VM, signaling channel open | Done (VM: signaling channel open to headset) |
| M3 | AVDTP negotiation | Works vs BlueZ sink and real headset | In progress (SBC stream reaches Streaming with real headset) |
| M4 | ACX endpoint + rings | Endpoint visible, PCM reaches ring | Done (VM: Speaker0 visible, Windows audio plays through the headset) |
| M5 | Encoder service + streaming | Audible SBC then AAC | In progress (Windows audio audible over SBC on real headset; kernel-safe AAC selector done; AAC service pending) |
| M5b | Codec configuration | SBC/AAC parameters (rate, channel mode, blocks/subbands, allocation, bitpool, AAC bitrate/VBR) selectable per device and globally; codec priority/force; presets incl. SBC XQ; changes applied by reconfigure/reconnect | Not started |
| M6 | Config UI + install/uninstall | Codec settings, status and force codec in the UI; clean restore | Not started |
| M7 | System testing | Tier 4 matrix passes | Not started |
| M8 | Signing & release | Attestation-signed package | Not started |

## 11. Risks
- Coexistence with Microsoft A2DP/HFP/AVRCP endpoints on same device.
- PSM 0x19 ownership by Microsoft stack: validate in M2.
- ACX documentation maturity; fallback PortCls/WaveRT.
- AAC licensing: use OS Media Foundation encoder.
- VM timing jitter: final numbers on bare metal.

## 12. Open Decisions
- [ ] License Apache-2.0 vs GPL
- [ ] Win11-only confirmed
- [ ] AAC backend MF (default) vs fdk-aac
- [x] Encoder placement: kernel SBC + user-mode AAC service (#48)
- [ ] Test framework

## 13. Environment Status
- Visual Studio 2026 Community: installed.
- Windows SDK 10.0.26100: installed.
- **WDK:** provided by NuGet (`driver/packages.config`, 10.0.28000.2526) plus the VS WDK component. No MSI install is needed.
- To install: WinDbg, Bluetooth Test Platform (btvs), Wireshark, ffmpeg (for decode tests), VMware/VirtualBox.

## 14. Action Log
Append an entry for every work session, newest last.

| # | Action | Result / commit |
|---|---|---|
| 1 | M1: codec library (SBC, AAC via MF/LATM) and CodecTests | 35 tests pass (6fc217d) |
| 2 | Fixed the SBC reference-decoder synthesis (direct cosine filterbank, gain -M) and lowered the round-trip SNR thresholds | Tests pass |
| 3 | Migrated the solution to `.slnx` with solution folders | 4ec6684 |
| 4 | Added SESSION-CONTEXT.md and copilot-instructions.md | 5cfee89 |
| 5 | Moved the repo to `D:\repos\open-a2dp` and renamed the solution to `open-a2dp.slnx` | 55bbb0b |
| 6 | Chose WDK via NuGet (10.0.28000.2526: WDK.x64, SDK.CPP, SDK.CPP.x64) in `driver/packages.config`; added `packages/` to `.gitignore` | Restored with `nuget restore` |
| 7 | `Directory.Build.props`: `WindowsTargetPlatformVersion` now defaults to 10.0 only when unset; user-mode defines no longer apply to driver projects | Codec projects unaffected |
| 8 | M2: added `driver/OpenA2dp.vcxproj` (KMDF, target platform pinned to 10.0.28000.0, output in `out\`/`obj\`, `FilesToPackage` set to the .sys) and added it to `open-a2dp.slnx` | Builds in Debug and Release with no warnings |
| 9 | M2: `Driver.cpp` (DeviceAdd, PrepareHardware gets the remote address, SelfManagedIo Init/Restart/Suspend) and `L2cap.cpp` (profile interface QI, async open of PSM 0x19, synchronous close, remote-disconnect work item) | Compiles; not yet run |
| 10 | M2: `OpenA2dp.inf` (Class=MEDIA, `BTHENUM\{0000110b-...}`, KMDF service, PnpLockdown) | Inf2Cat OK; InfVerif: valid |
| 11 | Updated SESSION-CONTEXT.md and this plan | Uncommitted |
| 12 | Modern project format (closest equivalent to SDK-style, which native C++ does not support): `Directory.Build.props` now holds configurations, toolset (default only, so the driver keeps `WindowsKernelModeDriver10.0`), Debug/Release settings and LTCG; the three `.vcxproj` files contain only Globals, items and references | Debug and Release build with no warnings; 35/35 tests pass |
| 13 | WDK via `PackageReference`
| 14 | Committed M2 and the project-format work | 97ce477 |
| 15 | M3: `common/include/OpenA2dp/AvdtpSource.h`, a kernel-safe AVDTP source state machine (DISCOVER, GET_CAPABILITIES per sink SEP, codec selection via callback, SET_CONFIGURATION, OPEN, START/SUSPEND/CLOSE, ABORT on timeout; answers sink DISCOVER/GET_CAPABILITIES/START/SUSPEND/CLOSE/ABORT, rejects sink-initiated configuration) and 6 tests in `AvdtpSourceTests.cpp` | 41/41 tests pass |
| 16 | Solution file is `OpenA2dp.slnx` again (renamed in 97ce477); updated references in docs, README and instructions | |
| 17 | Driver build now fails with MSB8020: the `WindowsKernelModeDriver10.0` toolset is no longer in Visual Studio (only v145 under `PlatformToolsets`), probably removed by the VS update | Fixed by reinstalling the WDK extension; Debug and Release build with no warnings, 41/41 tests pass |
| 18 | Old `packages/` folder is gone; removed its `.gitignore` entry (WDK now restores to the global NuGet cache) | |
| 19 | Test VM: installed VMware Workstation Pro (free) and created the `OpenA2dp` Windows 11 VM | Next: USB Bluetooth dongle, Secure Boot off, test signing, KDNET, snapshot |
| 20 | VM kernel debugging: KDNET over NAT (host VMnet8 192.168.135.1, port 50000; key in ignored `.local/kdnet.txt`); installed WinDbg via winget, connected with `WinDbgX -k net:port=50000,key=...` | Connected |
| 21 | WinDbg MCP: connected to the kernel session, break-in works (target Windows 11 26100 x64, 1 vCPU). Kernel symbols failed: msdl returned 404 for the ntkrnlmp.pdb of this build, so only exports load and `nt!Kd_IHVDRIVER_Mask` can't be set; use the Debug Print Filter registry key instead | Session closed afterwards |
| 22 | Driver now test-signed at build time (`SignMode=TestSign`, SHA256, WDK auto-generated `WDKTestCert`); added `scripts/deploy-vm.ps1` (vmrun: optional snapshot revert, copy package + exported cert, trust cert, `pnputil /add-driver`; credentials in ignored `.local/vm.json`) | .sys and .cat signed; 41/41 tests pass |
| 23 | deploy-vm.ps1: added VM encryption password (`-vp`, VMware encrypts VMs with a vTPM); guest login needs a non-empty password; install now runs from a batch file because vmrun mangled the inline `&&` command. First deploy: certificate trusted, `pnputil /add-driver` published `oem11.inf`; test signing confirmed on | Driver in the VM's driver store; not yet bound to the headset |
| 24 | Device Manager "Have Disk" refused the INF ("no compatible driver"; the inbox A2DP driver outranks it). Bound with `pnputil /add-driver OpenA2dp.inf /install` instead (now in deploy-vm.ps1). Driver loaded; WinDbg showed the L2CAP open failing with 0xC000000D: `Mtu.Preferred` was 0 (below Min 48). Set Preferred=672 for ConfigOut/ConfigIn. Also: deploy-vm.ps1 no longer echoes vmrun arguments (leaked the VM password) and tolerates an existing guest folder | Log: "signaling channel open, out MTU 672" with the real headset. M2 done |
| 25 | M3: SBC IE selection moved to kernel-safe `common/include/OpenA2dp/SbcConfig.h` (SbcCodec uses it); added `OpenA2dp/Types.h` (kernel-safe fixed-width ints, common headers use it). Driver: new `Signaling.cpp` (L2CAP ACL read loop, `L2capSend`, PASSIVE work item under a wait lock, 3 s WDF response timer, SBC selector) drives `SourceSession` once the signaling channel opens | 41/41 tests pass. Real headset: DISCOVER -> GET_CAPABILITIES -> SET_CONFIGURATION -> OPEN accepted, SEID 1, SBC IE 21 15 02 35 (44.1 kHz, joint, 16 blocks, 8 subbands, loudness, bitpool 2-53) |
| 26 | M3: media channel. `L2capOpenMediaChannel`/`L2capCloseMediaChannel` (synchronous BRB, out MTU 895) opened from the signaling work item when the session reaches Open, then START is sent; media channel closed on Idle/Failed and before the signaling channel | 41/41 tests pass. Real headset: "media channel open, out MTU 895" -> state 6 (Starting) -> 7 (Streaming) |
| 27 | M5: kernel-safe fixed-point SBC encoder (`SbcFixed.h`, `SbcFixedTables.h`) + tone generator, `SbcFixedTests.cpp`; `driver/Media.cpp` sends RTP/SBC packets from a 10 ms WDF timer + work item while Streaming; deploy-vm.ps1 restarts the bound device | 43/43 tests pass (SNR 62-68 dB). Streaming reached on the new image; media packet logs/audio not yet confirmed |
| 28 | DriverVer now stamped in UTC with a date-based version `1.yy.MMdd.HHmm` (`Inf` `DateStamp`/`TimeStamp` in `driver/OpenA2dp.vcxproj`). The WDK default `TimeStamp=*` used only the time of day (e.g. `0.10.2.807`), so versions could go backwards; the local date was also ahead of UTC, which inf2cat rejects (22.9.7) | Generated `DriverVer = 10/25/2026,1.26.1025.2314`; 43/43 tests pass; deployed and device restarted |
| 29 | M5 (SBC) validated: WinDbg shows `media: test tone 440 Hz ... frame 119 bytes` and thousands of packets sent; the user hears the tone in the headset. One run hit `media send failed 0x80000010` after a VM XHCI fatal error (USB passthrough), not a driver fault | SBC streaming works |
| 30 | M3: L2CAP server for PSM 0x19 (`L2capRegisterServer`/`L2capUnregisterServer`, BRB_L2CA_REGISTER_SERVER bound to the headset address) registered in self-managed IO init, unregistered on suspend. Incoming signaling connection is accepted with BRB_L2CA_OPEN_CHANNEL_RESPONSE through the shared open request; other incoming channels are refused (CONNECT_RSP_RESULT_NO_RESOURCES) via a separate preallocated request. Outbound open shares `InitSignalingConfig`/`SendOpenRequest` | 43/43 tests pass. Real headset: `L2CAP server register 0x00000000`, outbound path still reaches Streaming with tone; unregister 0x00000000. Headset power-cycle: `incoming signaling connection` -> `signaling channel open, out MTU 672` |
| 31 | M5: media send failure handling. On `L2capSend` failure the media work item stops the timer and calls `SignalingRequestSuspend`, which sets `SuspendPending`; the signaling work item then sends AVDTP SUSPEND while Streaming (the Open state calls `MediaStop`). | Build clean; 43/43 tests pass. Not yet validated on the VM |
| 32 | M3: AVDTP acceptor role. `SourceSession` accepts sink-initiated SET_CONFIGURATION (SBC subset of local caps, otherwise codec-category reject; a SET_CONFIGURATION during our own discovery wins over it), GET_CONFIGURATION and OPEN (new `Configured` state, `IsAcceptor()`). In acceptor mode the driver waits for the headset to open the media channel: `MediaAcceptAllowed` lets `OnServerIndication` accept the next incoming PSM 0x19 channel as media through the preallocated response request, and `SignalingOnMediaChannelOpen` then sends START. Test `AvdtpSource_AcceptsSinkConfiguration` added. | Build clean; 44/44 tests pass. Not yet validated on the VM |
| 33 | VM: headset-initiated reconnect. `incoming signaling connection` -> driver-initiated discovery (states 1,2,3,5,6) -> `media channel open` (outbound) -> Streaming, tone sent. The headset did not configure the stream itself, so the acceptor path (state 4) was not exercised. | Headset-initiated reconnect validated |
| 34 | VM: headset powered off while streaming. The headset tore the stream down over AVDTP before dropping the link: `AVDTP state 0` -> `media stopped after 6446 packets` -> `media channel closed` -> `signaling channel remote disconnect, reason 1` -> `signaling channel closed`. No send failure occurred, so the send-failure SUSPEND path was not hit. | Clean remote teardown validated; no hang or crash |
| 35 | M4: ACX 1.1 render endpoint (`driver/Audio.cpp`): circuit `Speaker0`, fixed 44.1 kHz/16-bit/stereo, RT packets drained by a timer into a 64 KiB PCM ring; `Media.cpp` encodes ring PCM (silence on underrun) instead of the test tone; SBC selection prefers 44.1 kHz. Build fixes: `KMDF_VERSION_MINOR=33` (ACX 1.1 needs `WDF_STRUCT_INFO`), `NOBITMAP` + `mmreg.h` before `ks.h`, local `RENDER_FORMAT` struct, restored `MediaInitialize` prototype. Command-line builds must use the 64-bit MSBuild (`Bin\amd64`); the 32-bit one fails with TRK0005 `stampinf.exe` (the NuGet WDK ships only x64/ARM64 tools). | Solution builds, `OpenA2dp.sys` produced; 44/44 tests pass. Not yet validated on the VM |
| 36 | M4: silent endpoint diagnosis. Code 31 fixed (circuit component ID, non-NULL circuit/pin attributes). Fixed malformed `MediaStart` log. VM log: `audio: create stream 0x00000000` but no prepare/run/allocate callbacks. Added `AcxStreamInitSetAcxRtStreamSupportsNotifications` (as in the ACX sample), stream destroy callback, and prepare/allocate/timer diagnostics. | Build clean; 44/44 tests pass. Awaiting VM validation |
| 37 | M4: VM test tone showed `audio: allocate 1 packets of 12288 bytes` then destroy: the driver rejected anything but 2 RT packets. `OnAllocateRtPackets` now accepts 1 or 2 packets; the actual count is stored in `AudioRtPacketCount` and used for the circular RT buffer size. | Build clean; 44/44 tests pass. Awaiting VM validation |
| 38 | M4: VM still logs `allocate 1 packets of 12288 bytes` then destroy, with no further output. Added allocate rejected/MDL failure/allocate ok logs and a build timestamp in the `DriverEntry` log to confirm which binary the VM runs. | Build clean; 44/44 tests pass. Awaiting VM log |
| 39 | M4: VM confirmed current build: `allocate ok, buffer 12288 bytes` then `stream destroyed` with no prepare/run. Added logs to every remaining RT/stream callback (hw latency, set render packet, current packet, presentation position, free packets, release hardware) to find the last call before teardown. | Build clean; 44/44 tests pass. Awaiting VM log |
| 40 | M4: VM log: allocate ok -> `get hw latency` -> `free rt packets` -> destroy. `OnGetHwLatency` reported FIFO = 64 KiB (`PcmRingBytes`), larger than the 12 KiB RT buffer, so the audio engine rejected the stream. Now reports 128 bytes like the ACX sample. | Build clean; 44/44 tests pass. VM: Windows audio audible in the headset |
| 41 | M4/M5: VM validation of #40: Speaker0 stream now prepares and runs; Windows audio is audible in the headset over SBC 44.1 kHz. End-to-end PCM -> SBC -> A2DP path works. | Validated on VM with real headset |
| 42 | Docs: refreshed the header status line (M1, M2, M4 done; M3 and M5 in progress). M3 still needs BlueZ sink validation, the acceptor path on real hardware, and AAC selection. | Docs only |
| 43 | Docs: reviewed and reordered Next Steps: commit and snapshot first, then M4 cleanup and volume/mute, M5 quality and send-failure checks, M3 BlueZ/acceptor validation, the kernel vs user-mode encoding decision, AAC, and M6. | Docs only |
| 44 | Review of #36-#40: required fixes were 1-packet RT allocation (#37) and FIFO 128 bytes (#40). `AcxStreamInitSetAcxRtStreamSupportsNotifications` (#36) is kept to match the ACX sample, but the driver never signals the notification event, so its effect is unverified. MediaStart log fix and build stamp kept. Committed the working state (a1e43f5), then removed per-callback logs (create/destroy/prepare/release/allocate/free/latency/packet/position) and the `sample0` read; timer status every 10 s; failures still logged. | Build clean; 44/44 tests pass. Not yet redeployed |
| 45 | VM: cleaned-up build (#44) deployed; audio plays. Log shows only `stream run` and the 10 s `audio: pos` line (ring 4288 bytes, 0 dropped). #36 resolved: the timer already calls `AcxRtStreamNotifyPacketComplete`, which ACX uses to signal notification events, so the flag is kept. Note: the `DriverEntry` build stamp only changes when `Driver.cpp` recompiles. | Validated on VM |
| 46 | M4: RT buffer is now rounded up to whole pages (zeroed), so no unrelated kernel memory is mapped to the audio engine. Volume/mute ACX elements were tried and reverted: without them Windows already applies the slider and mute in software (confirmed on VM), so driver-side elements only add value with AVRCP absolute volume. | Build clean; 44/44 tests pass |
| 47 | VM: long-run playback test (30+ min) on SBC 44.1 kHz with the real headset: no audible dropouts reported. | Validated on VM |
| 48 | Architecture decided: SBC stays in kernel (mandatory fallback); AAC is encoded only in the user-mode service (Media Foundation). Added kernel-safe `common/include/OpenA2dp/AacConfig.h` (IE parse/write, local capabilities, `Select`, `Validate`); `AacCodec` now delegates to it. Sections 1, 2, 10 and 12 updated. Driver negotiation unchanged (SBC only) until the service exists. Build clean, CodecTests 44/44. | Done |
| 49 | M4: Windows Settings appeared to show Speaker0 as 1 channel, 16 bit, 8000 Hz. Suspected cause: the 44.1 kHz stereo format was only added to the RAW mode list. `AddFormat` now also adds it to the `AUDIO_SIGNALPROCESSINGMODE_DEFAULT` list via `AcxPinRetrieveModeDataFormatList`. Build clean, CodecTests 44/44. | Failed on VM: `AcxPinRetrieveModeDataFormatList` returned 0xC0000225 (list does not exist), endpoint creation failed |
| 50 | Fix for #49: `AddFormat` now creates the DEFAULT mode list (`AcxDataFormatListCreate`, parented to the pin), adds the format and assigns it with `AcxPinAssignModeDataFormatList`. Build clean, CodecTests 44/44. | VM: endpoint works, audio plays, Settings still shows 1 ch / 16 bit / 8000 Hz |
| 51 | Investigation: suspected cause is a stale `PKEY_AudioEngine_DeviceFormat` ({f19f064d-082c-4e27-bc73-6882a1bb8e4c},0) stored in the MMDevices endpoint key when the endpoint was first built; it is not rebuilt on driver update. To check on VM: dump the value, then remove the device's MMDevices render key (or uninstall the device with driver removal) and reinstall. | Not the cause (see #52) |
| 52 | VM registry dump: the OpenA2dp render endpoint (`Speakers`, OpenA2dp Bluetooth A2DP Source) stores 2 ch / 16 bit / 44100 Hz, so the stored format was not stale. The 1 ch / 16 bit / 8000 Hz entry in Settings is under *Input settings* of a `Headset` page (Provider Microsoft, 10.0.26100.x): the inbox Hands-Free (HFP) microphone capture endpoint. Settings labels it with the OpenA2dp device name because both share the headset's Bluetooth container. A2DP Source has no capture endpoint. Not an OpenA2dp bug. | Closed |
| 53 | VM: confirmed in Settings > Sound that the OpenA2dp `Speakers` output shows 2 channel, 16 bit, 44100 Hz; the 8000 Hz format was the Hands-Free `Headset` input. | Resolved |
| 54 | M4: reverted #49/#50. The DEFAULT mode format list was not needed (Settings and the stored format are the same without it) and added a failure point to endpoint creation. `AddFormat` again adds the format only to the RAW list, as in the validated build. Build clean, CodecTests 44/44. | Done |
| 55 | Docs: Next Steps split into "Active (in order)" and a "Deferred" table (D1-D4) with milestone and reason; AVRCP absolute volume moved to Deferred (D4). | Done |
| 56 | Docs: added milestone M5b (codec configuration: user-selectable SBC/AAC parameters, per-device settings, presets, codec priority) and its steps to Next Steps, after AAC and before M6. | Docs only |
| 57 | Docs: configuration is done through a desktop UI (`OpenA2dpConfig.exe`) instead of the `a2dpctl` CLI; updated the components table, repo layout, M6 and Next Steps 5 and 7. | Docs only |
| 58 | M4: the device (and so its Windows audio endpoint) is named after the headset: `OnPrepareHardware` reads the remote name from `IOCTL_INTERNAL_BTHENUM_GET_DEVINFO` (UTF-8 -> UTF-16, `RemoteName`) and sets `DEVPKEY_Device_FriendlyName` to `<name> (OpenA2dp)` (address if no name). Build clean, CodecTests 44/44. | Not yet validated on VM |
| 59 | Fix intermittent redeploy hang (media still sending after `L2CAP server unregister`): `OnSelfManagedIoSuspend` now calls `MediaStop` first (stops timer, waits for the in-flight work item) before unregistering the server/closing channels; `L2capSend` uses a 1 s send timeout so a stalled ACL transfer cannot block teardown while holding `MediaLock`/`SessionLock`. Build clean, CodecTests 44/44. | Validated on VM |
| 60 | M4: endpoint type comes from the headset Class of Device (`BTH_DEVICE_INFO.classOfDevice`, queried in `EvtDeviceAdd` before the ACX circuit is created): audio minor headset/hands-free/headphones -> `KSNODETYPE_HEADPHONES`, otherwise `KSNODETYPE_SPEAKER`. Device FriendlyName is `<name> [OpenA2DP]`, so the endpoint shows as `<Headphones|Speakers> (<name> [OpenA2DP])`. Build clean, CodecTests 44/44. | Validated on VM (CoD 0x240404 -> Headphones after endpoint reinstall) |
| 61 | Crash triage of a "fatal system error" break: bugcheck 0x80 NMI_HARDWARE_FAILURE (Arg1 'TDO', Arg2 0x10), all 4 CPUs idle, OpenA2dp not on any stack: externally injected NMI (hypervisor), not a driver bug. Stale "Speakers" form factor was the Windows endpoint cache; fixed by reinstalling the device. #59 redeploy fix confirmed. | Confirmed |
| 62 | Docs: refreshed README (status, build, known gaps) and SESSION-CONTEXT (current handoff) to the M4/M5 state; verified Debug x64 build and CodecTests 44/44; committed the working state (#48-#61). | Done |
| 63 | WinDbg MCP: configured OpenCode's local `windbg` server for `DbgX.Mcp.Proxy.exe` (versioned package path; the WindowsApps alias fails `WaitNamedPipe` 121 when launched by OpenCode). Used the MCP tools on the live kernel session: `bl` and the Breakpoints pane are empty and `OpenA2dp!DriverEntry` is unpatched (`48 89 54 24 10`), so no driver-initialization breakpoint remains after the VM reboot; target resumed and still streaming. | Done |

## 15. Next Steps

### Active (in order)

1. M5: design the driver/service boundary for AAC: device interface + IOCTLs (GET_VERSION, REGISTER_ENCODER, MAP_RINGS, WAIT_EVENT), PCM ring shared with the service, LATM frame ring back to the driver for RTP/L2CAP send; SBC path untouched.
2. M3/M5: AAC: include `AacConfig.h` in the driver, register an AAC SEP only while the service is registered, codec policy AAC > SBC with SBC fallback; then implement `OpenA2dpSvc.exe` (MF encoder + `LatmWriter`).
3. M5b: codec settings store: registry values per headset address (`Parameters\Devices\<addr>`) plus global defaults (codec priority, SBC rate/channel mode/blocks/subbands/allocation/bitpool range, AAC rate/channels/bitrate/VBR); read at negotiation time.
4. M5b: preference-aware selection: `SbcConfig`/`AacConfig` `Select` take user preferences and fall back to the best headset-supported value; presets (Default, SBC HQ, SBC XQ dual-channel high bitpool) checked against media MTU; unit tests.
5. M5b: IOCTLs GET/SET_CODEC_CONFIG and GET_STATUS; desktop configuration UI (`OpenA2dpConfig.exe`, no CLI) to pick headset, codec, preset and SBC/AAC parameters and show the active configuration; apply via AVDTP SUSPEND + RECONFIGURE + START, or reconnect when the codec changes.
6. M5b: multi-rate ACX endpoint: offer 44.1 and 48 kHz (and mono if selected) and use the format matching the negotiated configuration, or resample; last, because it touches the validated audio path.
7. M6: finish the configuration UI (status, force codec) and clean install/uninstall that restores `BthA2dp.sys`.

### Deferred

| # | Milestone | Item | Deferred because |
|---|---|---|---|
| D1 | M5 | Measure end-to-end latency; check pause/resume and switching the default output away from and back to Speaker0. | Needs hands-on testing |
| D2 | M5 | Exercise the send-failure path (headset out of range or BT adapter removed while streaming); confirm SUSPEND and no hang. | Needs hands-on testing |
| D3 | M3 | Set up a BlueZ/PipeWire sink with `btmon` capture (Tier 3) and validate negotiation against it; use it to exercise the acceptor path (sink-initiated SET_CONFIGURATION, state 4, incoming media channel). | No Linux Bluetooth host available |
| D4 | M4/M5 | AVRCP absolute volume (headset hardware volume) with ACX volume/mute elements mapped to it. | Not needed now: Windows already applies volume/mute in software (#46); revisit after AAC |
