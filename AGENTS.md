# Agent Instructions (v2 - Cache Optimized)

## 1. Immutable Rules
- **OS/Kernel**: Windows 11 KMDF (C++20, x64, toolset v145, warnings as errors). No STL/exceptions in `common/include`.
- **Concurrency**: Lock order is always `SessionLock` -> `MediaLock`. Never hold locks across unbounded waits.
- **Tools**: Use native WinDbg MCP tools (`tools.windbg.*` via `execute`) for live debugging. Never use custom debugger scripts.
- **Commits**: One logical change per commit. Never push automatically.
- **Docs**: Never truncate or cut existing content in `docs/A2DP-Driver-Plan.md`. Edit whole table rows/lines with exact-match replacements. Re-read the affected section afterward to verify every row is intact.

## 2. File Location Map (Read ONLY the relevant file)
- PnP / Driver Entry: `driver/Driver.cpp`
- L2CAP / BRBs / Sends: `driver/L2cap*.cpp`
- AVDTP / Signaling / State: `driver/Signaling*.cpp`
- PCM/AAC Pacing / Media: `driver/Media*.cpp`
- Encoder Service / Rings / IOCTLs: `driver/Service*.cpp`
- Codecs / Tests: `codecs/` & `tests/CodecTests/`

## 3. Documentation Approach (Token-Saving)
Instead of rewriting `docs/A2DP-Driver-Plan.md` and `docs/SESSION-CONTEXT.md` in long blocks (which destroys KV-cache hits), the approach is:
- **Commit after every logical change** with a self-describing message (`git commit -m "#94: structured log format"`).
- **Keep docs append-only**: only add new rows to the Action Log (section 14) and update Next Steps (section 15) using exact-match replacements. Never truncate existing content.
- **Avoid rewriting session context**: `docs/SESSION-CONTEXT.md` is read-only reference; do not edit it after initial creation.
- **Log tag tracking**: `docs/A2DP-Driver-Plan.md` milestone rows and Action Log entries use exact line replacements (`edit` tool) to preserve every other row intact.
- If a document must change, edit a single table row or single paragraph; never rewrite an entire file in one turn.

## 4. Skills (Global Standard: `.agents/skills/`)
- `build-and-test` → MSBuild + CodecTests in one turn (≤3 lines output).
- `windbg-deadlock-check` → WinDbg deadlock diagnosis in 3 lines (≤5 lines output).
- Skills live in `.agents/skills/` (global, not `.opencode`-specific). Each skill file is a single markdown file (`SKILL.md`) with description, instructions, and output format.

## 5. Operational Protocol
1. **Find**: Use `grep` or `glob` to locate the exact symbol/file. Read *only* the specific function/lines needed (use `offset` and `limit`).
2. **Build**: Run MSBuild via `execute`:
   `& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1 | ForEach-Object { & $_ OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo }`
3. **Test**: Run `out\x64\Debug\CodecTests.exe`.
4. **Docs**: After every work session, add the actions taken to the Action Log (section 14) of `docs/A2DP-Driver-Plan.md` and update its Next Steps (section 15) and milestone statuses. Use `docs/SESSION-CONTEXT.md` for prior conversation context.