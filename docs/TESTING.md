# Testing

[README](../README.md) | [Build and deploy](BUILD.md) | [Hardware procedures](HARDWARE-TESTS.md)

Testing is split by what can be checked without a sink. Codec math, packet
layouts, negotiation and the status report run on the host. Transport, PnP,
power, the encoder service and anything audible need a real sink.

## Host tests

Build and run:

```powershell
& MSBuild.exe OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
.\out\x64\Debug\CodecTests.exe
```

The executable exits 0 when every test passes and non-zero on a failure.
`scripts/deploy.ps1 -Build` runs the same tests before installing.

| Suite | Covers |
|---|---|
| SBC | Encoder output, fixed-point path, frame header/CRC/layout, reference decode |
| AAC | Capability parsing, LATM framing, stream encoder records |
| aptX and aptX HD | Reference vectors, vendor capability parsing, record framing and sample carry |
| LDAC | Capability masks, packet framing, quality modes |
| AVDTP source | Discovery, codec selection, sink-initiated configuration, rejected-configuration fallback |
| Common | PCM depth and conversion, latency budget arithmetic, ring helpers |
| Frame records | Headers, validation, trimming and record boundaries |
| PCM backlog | Fill, trim and restart rules shared by the kernel and service paths |
| Configuration | INI parsing, defaults, invalid values, per-sink precedence |
| Sink matching | Address forms used by PnP, the tools and stored settings |
| Status report | Field formatting and the lines both tools show |

The vendored encoders are unmodified from their AOSP drops. Check their hashes
before a release build:

```powershell
.\scripts\check-vendored-encoders.ps1
```

## Hardware tests

`docs/HARDWARE-TESTS.md` is the procedure. `tests/HardwareTests/` holds one
script per test that follows the same steps and asserts the same expectations.

| Level | Meaning |
|---|---|
| `full` | No human step; the script performs the procedure and asserts all of it |
| `assisted` | The script prompts for a physical event or a listening check, then checks the driver's response |
| `manual` | The measurement or judgement is the test; the script records and sanity-checks the values |

Elevated, on the machine under test, with a sink paired and connected:

```powershell
.\tests\HardwareTests\Invoke-HardwareTests.ps1 -List
.\tests\HardwareTests\Invoke-HardwareTests.ps1 -Test T19 -Device <address>
.\tests\HardwareTests\Invoke-HardwareTests.ps1 -Test T19 -Device <address> -DriverLog <file>
```

A single test also runs on its own:

```powershell
.\tests\HardwareTests\T19-CodecSwitch.ps1 -Device <address>
```

| Option | Use |
|---|---|
| `-Device` | Address, list index or partial address; needed when more than one sink is on the driver |
| `-DriverLog` | Kernel log captured with WinDbg or DebugView |
| `-NonInteractive` | Print prompts and continue; for an already prepared rig |
| `-ResultDir` | Where the per-test JSON files go; defaults to an untracked folder under the harness |

Without `-DriverLog`, driver-side lines are reported as **unchecked**, never as
passes. `OPENA2DP_HWTEST_PLAYER` can name a real player for the tests that need
one; those tests narrow or skip when it is unset.

Exit codes are 0 for pass, 1 for fail and 2 for a skipped precondition. The runner
exits 1 if any test failed. A skipped precondition, such as a sink without AAC or
a machine without a recorder, is not a defect.

## Before a hardware session

1. Deploy the build under test and note its commit.
2. Pair and connect the sink; `OpenA2dpCtl list` should show it.
3. Start kernel log capture and, for the power and teardown tests, Driver
   Verifier. The exact commands are in
   [the hardware procedure](HARDWARE-TESTS.md#1-setup-once).
4. Run `OpenA2dpCtl status --watch` in a second window.

Driver Verifier is for a machine you can recover. `/standard` includes deadlock
detection, special pool, pool tracking, IRQL checking, I/O verification and DDI
compliance; a KMDF driver gets the KMDF verifier as well. Turn it off with
`verifier /reset` and remove the extra KMDF values when the session ends.

## What each side cannot test

| Host tests cannot check | Hardware tests cannot check |
|---|---|
| L2CAP transport and link loss | Exact codec output against a numerical reference |
| PnP, power transitions and unload leaks | Every malformed capability combination |
| Sink interoperability and advertised capabilities | INI edge cases and status formatting |
| Audible latency and the sink's own buffer | Verifier diagnostics that need a debugger and dump analysis |
| The service's MMCSS behaviour under real load | A reproducible transport stall shorter than a second |

## When a test fails

- A host test failure names the codec, frame or field it checked. Fix the cause;
  do not weaken the assertion.
- A hardware `FAIL` carries the assertion and the last log lines. The log check
  accepts only the warnings listed for that test; an unexplained warning fails it.
- A hang is a finding. Do not reboot: break in with the kernel debugger, inspect
  the wait and the lock order, and keep a dump.
- A script that disagrees with the procedure is a script bug. The document stays
  the source of truth for what a test means.

Hardware results belong with the build they were made on and stay out of the
repository. They do not move [STATUS](STATUS.md) on their own; a blocker they
expose does.
