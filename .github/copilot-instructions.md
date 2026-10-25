# Copilot instructions – OpenA2dp

- Project: open-source Windows 11 A2DP Source driver that replaces `BthA2dp.sys`. Read `docs/SESSION-CONTEXT.md` for prior conversation context, and `docs/A2DP-Driver-Plan.md` for the plan.
- Solution: `OpenA2dp.slnx` (modern format). Add every new project to it.
- C++20, x64, toolset v145, warnings as errors. Shared settings are in `Directory.Build.props`.
- Code under `common/include` must stay kernel-safe (no STL or exceptions).
- After every change, build with MSBuild on `OpenA2dp.slnx` and run `out\x64\Debug\CodecTests.exe`.
