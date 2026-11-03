# Agent Instructions (v3 - Ultra-Lean & Cache-Optimized)

## 1. Immutable Rules
- **KMDF Kernel Environment**: Windows 11 KMDF (C++20, x64, toolset v145, warnings as errors). Zero STL/exceptions in `common/include`.
- **Locking Order**: Always `SessionLock` -> `MediaLock`. Never hold locks across unbounded transport waits.
- **MCP Debugging**: Use native WinDbg MCP tools (`tools.windbg.*`). No custom scripts.
- **Commits**: One logical change per commit (`feat(scope): #id description [state, tests]`). Never push automatically.

## 2. Zero-Cost State Tracking Protocol
- **DO NOT** edit or rewrite `docs/A2DP-Driver-Plan.md` or `docs/SESSION-CONTEXT.md` during regular work.
- Check current context in 1 turn via `skills/status` or `git log -n 5 --oneline`.
- Document work in **Git Commit Messages**. If text logging is required, append a single line to `CHANGELOG.log` via `Add-Content`.

## 3. Modular Code Map (Read ONLY the Target Sub-File)
- Entry / PnP: `driver/Driver.cpp`
- L2CAP: `driver/l2cap/L2cap*.cpp` (BRB, Channel, Server, Send)
- Signaling: `driver/signaling/Signaling*.cpp` (Session, WorkItem, Abort)
- Service: `driver/service/Service*.cpp` (Control, Ioctl, Rings)
- Media: `driver/media/Media*.cpp` (Stream, Pacing)
- Codecs / Tests: `codecs/` & `tests/CodecTests/`

## 4. Skills (Global Standard: `.agents/skills/`)
- `build-and-test` → MSBuild + CodecTests in one turn (≤3 lines output).
- `windbg-deadlock-check` → WinDbg deadlock diagnosis in 3 lines (≤5 lines output).
- `status` → git log/status one-turn handoff (≤6 lines output).
- `log-audit` → grep driver logs for WARN/ERROR patterns (≤5 lines output).
- Each skill file is a single markdown file (`SKILL.md`) with description, instructions, and output format.

## 5. Operational Protocol
1. **Locate**: Use `grep` or `glob`. Read ONLY exact lines via `offset` and `limit`.
2. **Build & Test**: Execute `skills/build-and-test`.
3. **Debug**: On hangs, execute `skills/windbg-deadlock-check`.
