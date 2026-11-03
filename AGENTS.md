# Agent instructions

## Stable constraints
- Windows 11 KMDF, C++20, x64, toolset v145; warnings as errors. No STL or exceptions in `common/include`.
- Lock order: `SessionLock` -> `MediaLock`. Never hold locks across unbounded transport waits.
- Debug through native WinDbg MCP tools; no custom debugger scripts.
- Commit every completed logical change separately after validation; do not batch unrelated work or include pre-existing user changes. Never push automatically.

## Context and tracking
- Start with `docs/STATUS.md`, `git status --short`, and `git log -n 5 --oneline`. Read commit bodies only for relevant changes.
- `docs/STATUS.md` is the only live plan: milestone state, blockers, next priorities and validation gates. Update it only when these change; replace stale state rather than append a diary.
- Use Conventional Commits: `<type>(<scope>): <summary>` (`feat`, `fix`, `refactor`, `docs`, `test`, `build`, `chore`). Record rationale, tests/results and deployment/hardware limits in the body, not Markdown change logs. Legacy action-log numbers are not required.
- Architecture lives in `docs/architecture/`; load only the relevant reference via its index. Legacy plan/handoff files are retired; use Git for historical evidence, never recreate action logs.
- Keep stable rules here; designs/workflows stay on demand. Locate symbols with grep/glob, then read the relevant file/range. Split by responsibility, not arbitrary line counts.
- Keep `AGENTS.md`, live status and skills minimal (targets: ≤40 lines here/status, ≤30 per skill). Replace or remove redundant text before adding; never omit safety constraints to meet a target.
- Preserve unchanged wording and section order to support prefix-cache reuse. No cosmetic rewrites, timestamps, session notes, copied tool output or duplicate instructions. Edit only affected lines when rules, plan state or workflows actually change; load only the needed skill.

## Code map
- Entry/PnP: `driver/Driver.cpp`; [driver map](driver/README.md) lists subsystem `.cpp` files under `driver/{audio,l2cap,signaling,service,media}/`.
- Prefer standard independently compiled `.cpp` files with declarations in subsystem-private `*Internal.h` headers; never include `.cpp` implementations or introduce `.inc` fragments. Namespace boundaries stay within each file.
- AAC service: `service/main.cpp`; ABI: `common/include/OpenA2dp/Ioctl.h`.
- Codecs/tests: `codecs/`, `tests/CodecTests/`; diagnostics: `tools/OpenA2dpCtl/`.

## On-demand workflows
Read `.agents/skills/<name>/SKILL.md` when needed:
- `status`: compact plan + Git handoff.
- `build-and-test`: MSBuild and CodecTests after code changes; docs-only changes need link/diff checks, not a driver build.
- `windbg-deadlock-check`: diagnose hangs.
- `log-audit`: inspect driver warnings/errors.
