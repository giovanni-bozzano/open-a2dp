# Hardware test procedure

Manual tests for behaviour host tests cannot cover: the Bluetooth transport, PnP/power,
the encoder service and the event-driven signaling (see
[event model](architecture/Event-Model.md)). Gates and their status live in
[VALIDATION.md](VALIDATION.md); this file only says how to run them. A test passes only
when every "Expect" holds and the [log check](#4-log-check-after-every-test) is clean.

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

### 1.3 Baseline before each session
1. Deploy the build under test and note the commit (`git log -n 1 --oneline`).
2. Pair the sink; `OpenA2dpCtl list` (elevated) shows it.
3. `sc query OpenA2dpSvc` is `RUNNING` unless the test says otherwise.
4. Start log capture; in a second window run `OpenA2dpCtl status --watch`.

## 2. Tools used in the steps
- `OpenA2dpCtl status|list|codec <auto|sbc|aac>` (elevated). `status` shows AVDTP state,
  media state, codec, encoder registration and counters.
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
Update the gate in [VALIDATION.md](VALIDATION.md) only when a gate passes or a
blocker changes.
