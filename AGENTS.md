# Agent instructions

## Stable constraints
- Windows 11 KMDF, C++20, x64, toolset v145; warnings as errors. No STL or exceptions in `common/include`.
- Lock order: `SessionLock` -> `MediaLock`. Never hold locks across unbounded transport waits.
- Leave no orphaned resources (handles, work items, timers, pool, references) and create no potential deadlock or lock-order inversion. Keep code as event driven as possible instead of adding polling, timers or new waits.
- Debug through native WinDbg MCP tools; no custom debugger scripts.
- Commit every completed logical change separately after validation; do not batch unrelated work or include pre-existing user changes. Never push automatically. No AI attribution, co-author trailers or "generated with" lines in commit messages.

## Context and tracking
- Start with `docs/STATUS.md`, `git status --short`, and `git log -n 5 --oneline`. Read commit bodies only for relevant changes.
- `docs/STATUS.md` is the only live plan: milestone state, blockers, next priorities and exit gates. Update it only when these change; replace stale state rather than append a diary.
- See `docs/AGENT-EFFICIENCY.md` for reference locations.
- Use Git for historical evidence. Do not log validation, build, and test status in Git commit messages.
- Use Conventional Commits: `<type>(<scope>): <summary>` (`feat`, `fix`, `refactor`, `docs`, `test`, `build`, `chore`). Record rationale, tests/results and deployment/hardware limits in the body, not Markdown change logs.
- Architecture lives in `docs/architecture/`; load only the relevant reference via its index. 
- Keep stable rules here; designs/workflow stay on demand. Locate symbols with grep/glob, then read the relevant file/range. Split by responsibility, not arbitrary line counts.
- Keep `AGENTS.md`, live status and skills minimal (targets: ≤40 lines here/status, ≤30 per skill). Replace or remove redundant text before adding; never omit safety constraints to meet a target.
- Preserve unchanged wording and section order to support prefix-cache reuse. No cosmetic rewrites, timestamps, session notes, copied tool output or duplicate instructions. Edit only affected lines when rules, plan state or workflows actually change; load only the needed skill.
- Run build-and-test after code changes.
- Keep file responsibilities and private interfaces narrow.
- Prefer standard independently compiled `.cpp` files with declarations in subsystem-private `*Internal.h` headers. Namespace boundaries stay within each file.

## Code map
- Entry/PnP: `driver/Driver.cpp`; `driver/README.md` lists subsystem `.cpp` files under `driver/{audio,config,l2cap,signaling,service,media}/`.
- AAC service: `service/main.cpp`.
- Diagnostics: `tools/OpenA2dpCtl/`.
- Codecs: `codecs/`.
- Tests: `tests/CodecTests/`.
- ABI: `common/include/OpenA2dp/Ioctl.h`.

## On-demand workflows
Read `.agents/skills/<name>/SKILL.md` when needed:
- `status`: compact plan + Git handoff.
- `build-and-test`: MSBuild and CodecTests after code changes; docs-only changes need link/diff checks, not a driver build.
- `windbg-deadlock-check`: diagnose hangs.
- `log-audit`: inspect driver warnings/errors.
