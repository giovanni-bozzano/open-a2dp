# Hardware test scripts

One script per test of [docs/HARDWARE-TESTS.md](../../docs/HARDWARE-TESTS.md). A script drives
its procedure and asserts the procedure's "Expect" lines, so a test that needs a person still
ends in an automated verdict: the person powers the sink or listens where the script asks, and
the script checks what the driver, the service and the log did in response.

## Run

Elevated prompt, on the machine under test, with a sink paired and connected:

```
.\Invoke-HardwareTests.ps1 -List
.\Invoke-HardwareTests.ps1 -Test T1 -Device E8:07:BF:1B:3D:3F
.\Invoke-HardwareTests.ps1 -Device E8:07:BF:1B:3D:3F -DriverLog C:\OpenA2dp\logs\session.txt
```

A single test runs on its own: `.\T19-CodecSwitch.ps1 -Device <address>`.

Options:

- `-Device` — address, list index or partial address; needed when two sinks are on the driver.
- `-DriverLog` — the kernel log file (WinDbg `.logopen`, or DbgView saved). Without it the
  driver-side lines are recorded as **unchecked**, never as passes, and the §4 log check covers
  the service log only.
- `-NonInteractive` — print prompts and continue without waiting, and assume manual
  confirmations. For a rig that is already set up; a real pass uses the prompts.
- `-ResultDir` — where the per-test JSON files go (default `results\<time>` in this folder,
  untracked: a run belongs with the build it was made on).

Exit codes per test: 0 passed, 1 failed, 2 skipped because a precondition was not met (the sink
offers no AAC, the machine has no recorder). The runner exits 1 if any test failed.

`OPENA2DP_HWTEST_PLAYER` may hold a command line for a real player. The tone the scripts play
by default is enough for "audio plays"; T25 and T34 need a player that reopens its stream and
holds a media session, and skip or narrow themselves when the variable is unset.

## What each script can and cannot do

| Level | Meaning |
|---|---|
| `full` | No human step: the script performs the procedure and asserts all of it |
| `assisted` | The script prompts for a physical event or a listening check, then asserts the driver's response |
| `manual` | The measurement or the judgement is the test (T24 window checks, T30 audible latency); the script records and sanity-checks the values |

The prompts are the same steps the document lists, in the same order, so the document stays the
source of truth for what a test means; a script that disagrees with it is a bug in the script.

## Files

- `HwHarness.ps1` — the shared half: one device, one log mark per repetition, the assertions
  (status fields, log lines, counters, responsiveness), the tone player, `sc`/`pnputil`
  helpers, prompts, and the result line.
- `T01-...` to `T34-...` — one procedure each.
- `Invoke-HardwareTests.ps1` — runs a selection and prints the summary of §5.
