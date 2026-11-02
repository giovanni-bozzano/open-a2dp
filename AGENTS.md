# Agent instructions

## General Guidelines
- Project: open-source Windows 11 A2DP Source driver that replaces `BthA2dp.sys`. Read `docs/SESSION-CONTEXT.md` for prior conversation context, and `docs/A2DP-Driver-Plan.md` for the plan.
- Record every action taken and the next steps in `docs/A2DP-Driver-Plan.md` (Action Log and Next Steps sections), and keep the milestone statuses up to date. When updating `docs/A2DP-Driver-Plan.md`, never truncate or cut existing content: edit whole table rows/lines with exact-match replacements (no PowerShell regex rewrites), and re-read the affected section afterward to verify every row is intact.

## Build and Configuration
- Solution: `OpenA2dp.slnx` (modern format). Add every new project to it.
- C++20, x64, toolset v145, warnings as errors. Shared settings are in `Directory.Build.props`.
- Code under `common/include` must stay kernel-safe (no STL or exceptions).

## Testing and Logging
- After every change, build with MSBuild on `OpenA2dp.slnx` and run `out\x64\Debug\CodecTests.exe`.
- After every work session, add the actions taken to the Action Log (section 14) of `docs/A2DP-Driver-Plan.md` and update its Next Steps (section 15) and milestone statuses.
