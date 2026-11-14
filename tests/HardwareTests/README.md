# Hardware test scripts

One script per test in [docs/HARDWARE-TESTS.md](../../docs/HARDWARE-TESTS.md).
A script drives the procedure and asserts its "Expect" lines, so a test that
needs a person still ends in an automated verdict: the person powers the sink or
listens where the script asks, and the script checks what the driver, the service
and the log did in response.

## Run

Elevated, on the machine under test, with a sink paired and connected:

```powershell
.\Invoke-HardwareTests.ps1 -List
.\Invoke-HardwareTests.ps1 -Test T1 -Device <address>
.\Invoke-HardwareTests.ps1 -Device <address> -DriverLog <file>
```

One test also runs on its own:

```powershell
.\T19-CodecSwitch.ps1 -Device <address>
```

| Option | Effect |
|---|---|
| `-Device` | Address, list index or partial address; needed when two sinks are on the driver |
| `-DriverLog` | Kernel log file from WinDbg or DebugView. Without it, driver-side lines are **unchecked**, never passes |
| `-NonInteractive` | Print prompts and continue, assuming manual confirmations; for an already prepared rig |
| `-ResultDir` | Where per-test JSON files go; defaults to an untracked `results\<time>` folder here |

Exit codes per test: 0 passed, 1 failed, 2 skipped because a precondition was not
met (the sink offers no AAC, the machine has no recorder). The runner exits 1 if
any test failed.

`OPENA2DP_HWTEST_PLAYER` may hold a command line for a real player. The default
tone player is enough for "audio plays"; the tests that need a player to reopen
its stream or hold a media session narrow or skip themselves when the variable is
unset.

## Levels

| Level | Meaning |
|---|---|
| `full` | No human step: the script performs the procedure and asserts all of it |
| `assisted` | The script prompts for a physical event or a listening check, then asserts the driver's response |
| `manual` | The measurement or judgement is the test; the script records and sanity-checks the values |

The prompts follow the document step by step. The document is the source of truth
for what a test means; a script that disagrees with it is a script bug.

## Files

- `HwHarness.ps1` — the shared half: one device, a log mark per repetition, the
  assertions (status fields, log lines, counters, responsiveness), the tone
  player, `sc`/`pnputil` helpers, prompts and the result line.
- `T01-...` to `T34-...` — one procedure each.
- `Invoke-HardwareTests.ps1` — runs a selection and prints the summary.

See [Testing](../../docs/TESTING.md) for the host suites, setup and what the
hardware tests cannot cover.
