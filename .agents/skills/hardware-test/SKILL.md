# Hardware test

Run a scripted procedure from `tests/HardwareTests/` against a deployed build with a paired,
connected sink. `docs/HARDWARE-TESTS.md` is the procedure; the script is the same steps with the
assertions automated.

## Run
1. Elevated prompt on the machine under test; deploy first (`scripts/deploy.ps1` or `-Vm`).
2. Capture the kernel log (WinDbg `.logopen <file>`, or DbgView saved) so the driver-side lines
   can be checked; without it they are reported as unchecked, not as passing.
3. `powershell -File tests/HardwareTests/Invoke-HardwareTests.ps1 -List` to pick the test.
4. `... -Test T19 -Device <address> -DriverLog <file>`. One test also runs alone:
   `.\tests\HardwareTests\T19-CodecSwitch.ps1 -Device <address>`.
5. Exit 0 pass, 1 fail, 2 skip (precondition). Results: `tests/HardwareTests/results/<time>/*.json`.

## Reading a failure
- `SKIP` names a missing precondition (no AAC SEP, no recorder, no real player): not a defect.
- `FAIL` carries the assertion and the last log lines. The §4 audit is the script's `-AllowWarn`
  list; an unexplained WARN fails the test.
- A hang is a finding: the harness bounds every CLI and PnP call and names the one that did not
  answer. Then use `windbg-deadlock-check`.
- A prompt-driven step that does not line up with the procedure is a script bug: fix the script,
  not the document.

Do not widen `-AllowWarn` or add `-NonInteractive` to make a run pass; both are for a known-good
rig, and a manual confirmation is then assumed rather than checked.
