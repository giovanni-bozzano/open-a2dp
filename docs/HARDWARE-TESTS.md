# Hardware test procedure

Manual tests for behaviour host tests cannot cover: the Bluetooth transport, PnP/power,
the encoder service and the event-driven signaling (see
[event model](architecture/Event-Model.md)). A test passes only when every "Expect"
holds and the [log check](#4-log-check-after-every-test) is clean.

## 1. Setup (once)

Use the test VM (`scripts/deploy-vm.ps1 -Build`) or a test-signing machine with a kernel
debugger attached. Never enable Verifier on a machine you cannot recover: a Verifier
bugcheck repeats on every boot until it is turned off.

### 1.1 Kernel log capture
The driver logs through `DbgPrintEx` (`IHVDRIVER`, info level), one line per record:
`OpenA2dp <LEVEL> [<sink> (<address>)] <Category>: <message>`.

1. Elevated prompt: `reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v IHVDRIVER /t REG_DWORD /d 0xF /f`, then reboot.
   With WinDbg attached you can instead run `ed nt!Kd_IHVDRIVER_Mask 0xF` (no reboot).
2. Capture with either:
   - WinDbg: `.logopen C:\OpenA2dp\logs\<test-id>.txt` before the test, `.logclose` after; or
   - Sysinternals DebugView as administrator: enable *Capture Kernel* and *Enable Verbose
     Kernel Output*, clear, run the test, save as `<test-id>.log`.
3. Service log: `%ProgramData%\OpenA2dp\OpenA2dpSvc.log` (lines prefixed `[svc]`).

### 1.2 Driver Verifier
1. Elevated prompt:
   ```
   verifier /standard /driver OpenA2dp.sys
   ```
   `/standard` includes deadlock detection, special pool, pool tracking (leaks at
   unload), forced IRQL checking, I/O verification and DDI compliance checking. When
   Verifier is on for a KMDF driver, the KMDF verifier is enabled too.
2. Optional, stronger KMDF handle checks (diagnoses leaked or over-released WDF objects):
   ```
   reg add HKLM\SYSTEM\CurrentControlSet\Services\OpenA2dp\Parameters\Wdf /v VerifierOn /t REG_DWORD /d 1 /f
   reg add HKLM\SYSTEM\CurrentControlSet\Services\OpenA2dp\Parameters\Wdf /v TrackHandles /t REG_MULTI_SZ /d "*" /f
   reg add HKLM\SYSTEM\CurrentControlSet\Services\OpenA2dp\Parameters\Wdf /v DbgBreakOnError /t REG_DWORD /d 1 /f
   ```
   `DbgBreakOnError` breaks into the debugger on a framework error; only set it with a
   debugger attached.
3. Reboot, then confirm: `verifier /querysettings` lists `OpenA2dp.sys`, and
   `verifier /query` shows it as loaded once a sink is paired.
4. During and after tests: `verifier /query` shows counters; any violation is a
   bugcheck (typically `0xC4`, `0xC9`, `0x10D` for KMDF). In WinDbg run `!analyze -v`,
   then `!verifier 0x3`, `!deadlock` (lock-order report) and
   `!wdfkd.wdflogdump OpenA2dp`. Keep the dump with the test id.
5. Turn it off when finished: `verifier /reset`, delete the `Parameters\Wdf` values,
   reboot.

### 1.3 Restore check (M6 exit gate)
The deploy script doubles as the restore check and needs no package, so it can run against
whatever the VM has installed:

```
scripts/deploy-vm.ps1 -Uninstall
```

It stops the service and the tools, removes every `opena2dp.inf` package with
`pnputil /delete-driver oem<N>.inf /uninstall`, then deletes both `OpenA2dpSvc` and the
kernel `OpenA2dp` service entry - `pnputil` leaves that one behind - and waits up to 30 s
for a node bound to `BthA2dp`. The guest log is streamed as it runs.

The `BthA2dp` half needs the sink paired **and connected**: a paired but powered-off sink
has no node, and the check cannot tell that apart from a failed restore, so it fails with
`pair and connect the sink, then rerun`.

Expect `removing driver package oem<N>.inf` (or `no OpenA2dp package in the driver store`),
a `BthA2dp: <name> status=...` line, `OpenA2dpCtl list: No OpenA2dp devices found`, then
`RESTORED` and `Restore check passed` on the host.

The check fails while any of these is still true: an `OpenA2dp` device node exists;
`sc query OpenA2dp` or `sc query OpenA2dpSvc` does not answer 1060; `Win32_SystemDriver`
lists `OpenA2dp`; `pnputil /enum-drivers` still shows `opena2dp.inf`;
`C:\Windows\System32\drivers\OpenA2dp.sys` exists; nothing is bound to `BthA2dp`;
`OpenA2dpCtl list` still finds a control device. Rerunning it on an already restored VM
repeats the checks without removing anything.

Then play audio to the sink: the Microsoft A2DP path must work again, without a reboot. If
the sink has not reconnected yet, the script says so - reconnect it and rerun.

### 1.4 Baseline before each session
1. Deploy the build under test and note the commit (`git log -n 1 --oneline`).
2. Pair the sink; `OpenA2dpCtl list` (elevated) shows it.
3. `sc query OpenA2dpSvc` is `RUNNING` unless the test says otherwise.
4. Start log capture; in a second window run `OpenA2dpCtl status --watch`.

## 2. Tools used in the steps
- `OpenA2dpCtl status|list|codec <auto|sbc|aac|aptx|aptxhd> [--bitpool n] [--bitrate bps]
  [--blocks n] [--subbands n] [--allocation loudness|snr]` (elevated). `status` shows
  AVDTP state, media state, codec, encoder registration and counters.
- `OpenA2dpConfig` (elevated): the settings window over the same IOCTLs. It lists the
  sinks, refreshes the status every second and stores the codec preference.
- Service: `sc stop OpenA2dpSvc`, `sc start OpenA2dpSvc`.
- Device node: Device Manager, or `pnputil /enum-devices /class MEDIA` /
  `pnputil /disable-device <id>` / `pnputil /enable-device <id>`.
- Sleep: `rundll32 powrprof.dll,SetSuspendState 0,1,0` (or the Start menu) and wake.
- Pool leaks: `poolmon -b` and watch tag `OAdp` (driver pool tag `'pdAO'`).

## 3. Tests

Sink ids: **S** = an SBC only sink, **A** = an AAC-capable sink. Run each test at
least 3 times; teardown tests 10 times.

### Connection

**T1 PC-initiated connect (S)**
1. Sink on, not connected. In Windows Bluetooth settings click *Connect*.
2. Play audio to `<sink> [OpenA2DP]`.
- Expect: `signaling channel open`, AVDTP states `Discovering` -> ... -> `Streaming`,
  `media channel open`, `streaming, media MTU`; audible; `packets` advancing,
  `send failures` 0.

**T2 Sink-initiated connect, sink configures (S)**
1. Disconnect from Windows; power-cycle the sink so it reconnects by itself.
2. Do not play audio for 10 s.
- Expect: `sink-initiated; waiting for it to configure the stream or for audio`; if
  the sink configures, the session reaches `Open`/`Streaming` through
  `incoming media channel open` with no `Discovering` from us. Then play: audible.

**T3 Sink-initiated connect, silent sink, then audio (S)**
1. As T2. If the sink stays `Idle` in `status`, start playback.
- Expect: `audio started on a silent sink-initiated link; starting discovery`, then
  `Streaming` and audio. No discovery before playback (no timer starts it any more).

**T4 Sink-initiated connect with audio started at once (S)**
1. Have a player ready with the OpenA2DP endpoint as default output. Power the sink
   on and start playback as soon as the endpoint appears (before AVDTP leaves `Idle`).
   Repeat with playback already running on another endpoint that Windows moves over.
- Expect: one of `sink-initiated with audio running; starting discovery`,
  `audio started on a silent sink-initiated link; starting discovery` or the sink's
  own configuration, then `Streaming`; no `SEP in use` / `no resources` errors and no
  double configuration.

**T5 Pause/resume and output switching (S)**
1. While streaming: pause 30 s, resume; switch default output to speakers and back.
- Expect: `stream paused` / `stream run`; audio resumes; no WARN/ERROR.

### Teardown and recovery (deadlock and orphan checks)

**T6 Disconnect during discovery (S)** — connect (T1) and power the sink off within
1 s of `signaling channel open`.

**T7 Disconnect during media channel open (S)** — power off right after AVDTP `Open`.

**T8 Disconnect while streaming (S)** — power off during playback.
- Expect for T6-T8: `signaling channel remote disconnect` or link-down messages, then
  `signaling channel closed`; no hang (the Bluetooth settings page and `status` stay
  responsive), no Verifier bugcheck; the endpoint becomes unplugged; reconnecting works.
  Acceptable WARNs: `still pending at close; abandoning it`, `closed a channel whose open
  finished after its waiter gave up`.

**T9 Rapid reconnect, media open retry (S)**
1. 10 times: connect, wait for `Streaming` or 3 s, disconnect from Windows.
- Expect: every cycle ends in `Streaming` or a clean close. If
  `media channel open attempt N failed; aborting the stream to retry` appears, it is
  followed by `stream aborted; rediscovering` and `Streaming`, or after 3 attempts by
  `media channel open failed after 3 attempts` and a clean close.

**T10 Out of range (S)** — walk away while streaming until the link drops, come back.
- Expect: as T8; automatic or manual reconnect works.

### Power and PnP

**T11 Sleep/resume (S)**
1. Sleep while connected and idle; wake. Repeat while streaming.
- Expect: no hang on sleep or wake; after wake `OpenA2dpCtl list` shows the device, the
  service log shows a new `[svc] opening ... registered`, and reconnect plays audio.

**T12 Disable/enable the device (S)**
1. While streaming, disable the OpenA2dp device node, wait 5 s, enable it.
- Expect: disable completes within a few seconds (no hang); `control device removed`;
  after enable the service re-attaches and playback works.

**T13 Unload leak check (S)**
1. Note `OAdp` in `poolmon`. Run T1, T8, T12, then disable the device (driver unloads).
- Expect: `OAdp` allocations return to 0; Verifier reports no leaked pool (an unload
  with outstanding allocations is bugcheck `0xC4`); `!wdfkd.wdfdriverinfo OpenA2dp` in
  the debugger shows no leaked handles with `TrackHandles`.

**T14 Bluetooth adapter removal** — while streaming, unplug the USB adapter (VM: detach
it), reattach.
- Expect: no hang or bugcheck; the device returns and reconnects.

### Encoder service

**T15 Service started after the driver**
1. `sc stop OpenA2dpSvc`; connect (S) so the control device exists; `sc start OpenA2dpSvc`.
- Expect: the service log shows `[svc] opening ...` within ~1 s of start and no
  periodic polling lines; `status` shows `encoder registered`. Repeat after T11 (wake):
  the service attaches by interface arrival.

**T16 Service stop/restart while streaming**
1. Stream SBC (S), `sc stop OpenA2dpSvc`, `sc start OpenA2dpSvc`.
- Expect: SBC continues uninterrupted; `encoder registered` toggles; S does not offer
  AAC so no renegotiation is attempted (no `preference wants AAC` close).
2. Repeat on A while streaming AAC.
- Expect: `encoder lost while streaming AAC; suspending`; no hang; the service stop
  completes within its bound (`OpenA2dpSvc stopping`).

### AAC (sink A)

**T17 AAC with the service running before connect**
1. Service running; `OpenA2dpCtl codec auto`; connect A and play.
- Expect: `sink offers codec AAC`, `configured codec AAC`, `[svc] stream start`, audible,
  `status` codec AAC with packets advancing and few `underruns`.

**T18 Late registration renegotiates to AAC**
1. `sc stop OpenA2dpSvc`; connect A and play (SBC).
2. `sc start OpenA2dpSvc` while playing.
- Expect: `stream is SBC; renegotiating if the sink offered AAC`,
  `SBC stream selected but the preference wants AAC; closing to renegotiate`,
  `stream closed; rediscovering to negotiate AAC`, then `configured codec AAC`; a short
  audio gap, then AAC audio.
3. With `OpenA2dpCtl codec sbc` set before step 1, repeat: no renegotiation, SBC continues.

**T19 Live codec switch and SBC fallback**
1. Service running; `OpenA2dpCtl codec auto`; connect A and play: AAC.
2. `OpenA2dpCtl codec sbc` while playing. Expect
   `AAC stream selected but the preference wants SBC; closing to renegotiate`,
   `stream closed; rediscovering to negotiate SBC`, then `configured codec SBC`; a short
   audio gap, SBC audio, and no sink reconnect. If the sink drops the A2DP connection on
   that CLOSE (`signaling channel remote disconnect`), expect
   `signaling channel closed during the codec switch; reconnecting (1/3)` and the same
   renegotiation without touching the sink; only a sink that keeps dropping the channel
   reaches `(3/3)` and stays disconnected.
3. `OpenA2dpCtl codec aac` while playing. Expect the reverse switch to AAC the same way.
4. With SBC playing, `OpenA2dpCtl codec aac`/`auto` when AAC cannot be selected (no
   encoder service, or the sink does not offer AAC): no renegotiation, SBC continues.
   `codec aac` reports the missing encoder service when that is the reason.

**T21 Codec preference persistence (S/A)**
1. `OpenA2dpCtl codec sbc`, then disable and enable the OpenA2dp device (or reboot).
   Connect and play.
- Expect: `codec preference SBC restored from the registry` when the device starts, then
  `configured codec SBC` with no further `codec` command.
2. `OpenA2dpCtl codec auto`; repeat: AAC where an encoder is registered, SBC otherwise.
- The value lives under
  `HKLM\SYSTEM\CurrentControlSet\Services\OpenA2dp\Parameters\Devices\<address>`
  (`CodecPreferenceFlags`, `CodecPreferenceCodec`, `CodecPreferenceSbcBitpool`); deleting
  that subkey returns the sink to auto.

**T22 Codec parameters (S/A)**
1. `OpenA2dpCtl status` lists `SBC bitpool` or `AAC bitrate` once one is stored.
2. While SBC streams, `OpenA2dpCtl codec sbc --bitpool 35`. Expect
   `SBC parameters changed; suspending to reconfigure`, then
   `SBC parameters changed; sending RECONFIGURE`, then `Streaming` with the media start
   line showing `bitpool 35`: no sink disconnect, no `stream closed; rediscovering`.
   A sink that advertises a lower maximum, or a minimum above 35, still configures its
   own range. The `codec` line of `status` reports the resulting SBC bitrate.
   A sink that refuses the RECONFIGURE logs `sink rejected the new parameters; closing to
   renegotiate` and continues through the T19 close/rediscovery path. A stream that is
   already `Open` (paused) skips the SUSPEND and only logs the RECONFIGURE line.
3. While SBC streams, `OpenA2dpCtl codec sbc --blocks 8 --subbands 4 --allocation snr`:
   the same in-place path. Expect `status` to show `SBC frame 8 blocks, 4 subbands, SNR`
   and the media start line to report `blocks 8 subbands 4`. A sink that offers only 16
   blocks or 8 subbands keeps its own values and still reconfigures.
4. `OpenA2dpCtl codec auto --bitpool 0 --blocks 0 --subbands 0 --allocation default`
   restores the driver defaults (bitpool 53, the selector's own frame order).
5. While AAC streams, `OpenA2dpCtl codec aac --bitrate 128000`: same in-place path, and
   `status` reports 128000 bps; a sink advertising less gets its maximum.
6. `OpenA2dpCtl status`: the media line reads `media MTU 895 B, 883 B payload; AAC up to
   <ceiling> bps`, where the ceiling is 256000 at a 44.1 kHz endpoint and 320000 at
   48 kHz. That is the MTU-derived limit the driver enforces on every negotiation.
7. At a 48 kHz endpoint, `OpenA2dpCtl codec aac --bitrate 320000`: expect `AAC ... 320000
   bps` in the media line, no `does not fit the media MTU`, and no dropped frames in
   `packets`/`underruns`. At 44.1 kHz the same request settles on 256000, because one
   895-byte packet cannot carry a 320 kbps 44.1 kHz access unit.
8. On a sink whose media channel grants less than the 895 bytes the driver asks for:
   expect `AAC bitrate <n> does not fit the media MTU (max <m>)`, then
   `the AAC bitrate does not fit the media MTU; reconfiguring at a bitrate it carries`,
   `sending RECONFIGURE at a bitrate the media MTU carries`, and `Streaming` with the
   media line showing the lower bitrate - without a sink reconnect. A sink that refuses
   the RECONFIGURE goes through `sink rejected the new parameters; closing to renegotiate`
   and a rediscovery; if that stream still does not fit, the log ends with
   `leaving the stream silent` and the stream stays silent instead of retrying.
   Switching codec on that connection (`codec sbc`, then `codec aac`) negotiates again
   without another `does not fit` warning or RECONFIGURE: the connection keeps the MTU
   its media channel reported until it ends.

**T23 Endpoint format selection (S/A)**
1. `OpenA2dpCtl status`: `endpoint format automatic (every sink format)`, and
   *Sound settings > the endpoint > Advanced* offers every format the sink advertises.
   Pick another one there and play: the stream still runs at the first listed format
   (`endpoint format ... (actual stream)`), because the picker does not reach the driver.
2. While streaming, `OpenA2dpCtl format 48000 2`.
- Expect `endpoint format set to 48000 Hz 2 ch`, `endpoint formats 48000 Hz 2 ch 16-bit`,
  `jack unplugged for 80 ms so Windows rebuilds the endpoint`, then within a second
  `status` reporting `codec SBC, 48000 Hz, 2 ch`. No `rediscovering`, no sink reconnect;
  the sink's own display follows to 48 kHz. Audio pauses for the length of the flap and
  resumes when the player reopens its stream.
3. *Advanced* now offers that one format only. Choose `16 bit, 44100 Hz` there and play:
   the stream keeps running at 48 kHz.
4. Reconnect the sink: `Config: endpoint format 48000 Hz 2 ch restored from the registry`
   and `endpoint formats 48000 Hz 2 ch 16-bit` appear before Windows opens anything.
5. `format 48000 1`, play (S).
- Expect `endpoint format 48000 Hz 1 ch 16-bit`, `configured codec SBC` with `mode 0` and audible
  mono audio; `status` shows one channel. Repeat on A: AAC negotiates mono.
6. On A with AAC: the service log shows `[svc] stream start: 48000 Hz, 1 ch`, `status`
   reports the 48 kHz bitrate, audio is audible.
7. `format auto`: every sink format is listed again, `status` reads
   `endpoint format automatic (every sink format)`, and the *Advanced* list grows back.
   A WASAPI exclusive-mode player can open any of them; with a pinned format it is limited
   to that one.
8. `format 48000 2` on a 44.1-only sink is refused with the advertised masks, nothing is
   stored, and the endpoint keeps its previous list.
9. `OpenA2dpCtl reset`: `codec preference reset to auto (aptX HD > aptX > AAC > SBC), endpoint format reset
   to automatic (every format the sink supports)`, and both settings stay there after a
   reconnect.
10. A sink that configures the stream itself with another rate or channel count
   (non-compliant, one format only) is rejected: our SET_CONFIGURATION response carries
   0xC4 (sampling frequency), 0xC6 (channel mode) or 0xD9 (channels) and the sink should
   retry. If it insists, the stream never opens; `does not match the endpoint format` in
   the media log is the safety net, not the normal path.
11. Switch codec and SBC/AAC parameters while playing: the offered list does not change
    (a pinned format stays the only entry). The window has the same setting; CLI
    `--rate`/`--channels` explain their removal.
12. With old global/per-device `CodecPreferenceEndpointRate`/`EndpointChannels` values
   already present, load the current ABI 1 development build: `GET_PREFERENCE` has no
   rate/channel fields, those values are not read as an endpoint format, and Windows
   still offers all sink-supported driver formats.
   Even invalid old format values must not
   discard valid codec parameters. On a stereo-only/44.1-only sink only those supported
   choices appear after discovery; the initial four-format log is labelled provisional.
13. On a sink without an AAC SEP, `OpenA2dpCtl codec aac` is refused with `the sink does not
   advertise AAC (it offers SBC)`, and `status` lists `sink formats 44100 Hz, 48000 Hz, mono,
   stereo (SBC)`; Windows offers only the sink's formats.
14. Discovery with an unchanged format set does not repeat `endpoint formats` or emit an
   explicit format-change notification, and the log carries no pin-property traffic.
   `endpoint format ... (actual stream)` is the encoder input, not proof of the Windows
   mix value.

**T24 Config UI (S/A)**
1. Start `OpenA2dpConfig` elevated: the sink list shows each sink as `name (address)` -
   the same name and address `OpenA2dpCtl list` prints - suffixed with `- connected` or
   `- playing`, and with no sink opened yet the one that is playing is the one selected. The
   status list shows the same fields as `OpenA2dpCtl status` under its Session, Sink,
   Endpoint, Service and Driver sections, and the controls show the stored codec preference
   and endpoint format. A sink that starts or
   stops streaming is relabelled within a second, including one that is not the selected
   sink and with no Refresh press.
2. Play, then choose `SBC only` and press Apply: the window reports `Stored. The active
   stream renegotiates to apply it.` and the status list follows the renegotiation within a
   few seconds, with no sink reconnect.
3. Choose `SBC bitpool` 35 and apply: the same in-place path, and both the status list and
   `status` report the resulting SBC bitrate. A stored bitpool the list does not offer is
   shown as its own entry instead of jumping to `Automatic`.
4. Choose another `PCM format` while streaming and apply: the endpoint is rebuilt (one
   `jack unplugged for 80 ms so Windows rebuilds the endpoint`) and the stream comes back
   at the new format without a sink reconnect - never `rediscovering`. The same on a
   format the stream already runs at. On a 1024x768 screen the whole window fits: nothing
   is off-screen and the session panel scrolls to its last row.
5. Choose `AAC only` on an SBC-only sink (S): the window reports that the sink cannot serve
   it and the stored preference stays as it was.
6. Connect a second sink while the window is open: both are listed within a second without
   pressing Refresh, and each loads its own preference. Unplug the selected sink: the window
   reports it as gone instead of retrying forever.
7. Choose `44100 Hz, mono` in `PCM format`, apply, then check Sound settings: the endpoint
   offers only that format. `Automatic (every sink format)` restores every served format,
   and so does `OpenA2dpCtl reset`. The status list has no selectable rows and no row
   highlights under the cursor.
8. Tick **Details**, then press **Copy** and paste into an editor: the text matches
   `OpenA2dpCtl status --verbose` for the same sink, one field per line, every value with
   its decimal and hex form. Use this dump for any failure found below it.

### Release gate

The checks the codec configuration and aptX HD work handed to system testing. All need a
real player and a real sink; the scripted helpers in `.local/` cannot stand in for either.

**T25 Player resumes after the endpoint flap (S/A)** — while a media player streams, change
`PCM format`.
- Expect: `jack unplugged for 80 ms so Windows rebuilds the endpoint`, the stream comes back
  at the new format, the sink's display follows it and the player keeps playing. The 80 ms
  gap is all it hears; no reconnect, no `rediscovering`.

**T26 Pinned format survives a reconnect (S/A)** — pin a format, power the sink off and on.
- Expect: `Config: endpoint format <rate> Hz <ch> ch restored from the registry` and
  `endpoint formats ...` appear before Windows opens anything, and the stream runs at the
  pinned format with no Apply and no format change from the window.

**T27 aptX HD negotiation and 24-bit path (S/A)** — service running, sink advertises aptX HD.
1. `OpenA2dpCtl codec auto`, connect and play. `OpenA2dpCtl status` lists aptX HD under
   `sink codecs` and reports `codec aptX HD, <rate> Hz, 2 ch, 576000 bps` (529200 at 44.1 kHz).
2. Driver log shows `sink offers aptX HD`, `endpoint formats ... 24-bit`,
   `configured codec aptX HD IE D7 00 00 00` and `aptX HD <rate> Hz stereo 24-bit, ... frames
   from service`; after a minute `send failures` and `underruns` stay at 0.
3. `OpenA2dpCtl codec sbc` while streaming: it renegotiates in the same connection and the
   endpoint keeps playing (no silence) through the rounded 24->16 downconvert.
4. `OpenA2dpCtl codec aptxhd` on a sink that does not advertise it: refused, stored
   preference unchanged.

**T28 classic aptX negotiation and 16-bit path (S/A)** — service running, sink advertises
classic aptX but not aptX HD.
1. `OpenA2dpCtl codec auto`, connect and play. `OpenA2dpCtl status` lists aptX under `sink
   codecs` and reports `codec aptX, <rate> Hz, 2 ch, 384000 bps` (352800 at 44.1 kHz).
2. Driver log shows `sink offers aptX`, `configured codec aptX IE 4F 00 00 00`, and `aptX
   <rate> Hz stereo 16-bit, ... frames from service`; after a minute `send failures` and
   `underruns` stay at 0. At 44.1 kHz also let it play for several minutes and check the
   sink stays in sync against the PC: that is what the sample carry protects, and underruns
   alone would not show it.
3. A sink advertising both aptX codecs selects aptX HD; forcing `codec aptx` moves the same
   connection to classic aptX and back.
4. `OpenA2dpCtl codec aptx` on a sink that does not advertise it: refused, stored
   preference unchanged.

### Soak

**T20 Long run** — stream (A if available, else S) for 8 hours with Verifier on.
- Expect: no bugcheck, `send failures` and `dropped` stay near 0, memory (`OAdp`) flat.

## 4. Log check after every test
Search the captured log for
`WARN|ERROR|stop: still waiting|BRB did not complete|send did not complete|read still pending|no response within`
(for example with the `log-audit` skill or
`Select-String -Path <log> -Pattern '<pattern>'`). WARNs listed as acceptable for the
test are fine; any other match fails the test.

On a hang: do not reboot. Break in with the kernel debugger and follow the
`windbg-deadlock-check` skill (`!locks`, `!deadlock`, the signaling and media work item
stacks), then save a dump (`.dump /ka`).

## 5. Reporting
Per test: id, sink, commit, Verifier on/off, pass/fail, log file and dump if any.
Update the exit gate in [STATUS.md](STATUS.md) only when a gate passes or a blocker
changes.
