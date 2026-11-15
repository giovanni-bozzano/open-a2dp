# Agent instructions

## Stable constraints
- Windows 11 KMDF, C++20, x64, toolset v145; warnings as errors. No STL or exceptions in `common/include`.
- Assume the driver, service and tools always use matching versions from the same build. Keep ABI changes consistent across them; do not preserve obsolete fields or add compatibility for mismatched versions.
- Lock order: `SessionLock` -> `MediaLock`. Never hold locks across unbounded transport waits.
- Leave no orphaned resources (handles, work items, timers, pool, references) and create no potential deadlock or lock-order inversion. Keep code as simple and sound as possible and event driven where possible: prefer the smallest correct state and code path over extra layers or speculative abstractions, and do not add polling, timers or new waits.
- Debug through native WinDbg MCP tools; no custom debugger scripts.
- Commit every completed logical change separately after validation; do not batch unrelated work or include pre-existing user changes. Never push automatically. No AI attribution, co-author trailers or "generated with" lines in commit messages.
- No absolute paths, drive letters, machine names or user accounts in code, scripts or docs; take paths from the project root.
- Verification leaves nothing behind: no temporary sample data, forced settings, extra logging, or stray processes and windows, and never change the user's settings to test.

## Context and tracking
- Start with `docs/STATUS.md`, `git status --short`, and `git log -n 5 --oneline`. Read commit bodies only for relevant changes.
- `docs/STATUS.md` is the only live plan: milestone state, blockers, next priorities. A milestone is done when its code and host tests are; a hardware run is evidence for a build, never the gate that closes it. Update it only when this changes; replace stale state rather than append a diary.
- See `docs/README.md` for the documentation map.
- Use Git for historical evidence. Do not put verification status in commit messages: no build or test results, hardware validation, deployment state, or "not yet run" notes.
- Use Conventional Commits: `<type>(<scope>): <summary>` (`feat`, `fix`, `refactor`, `docs`, `test`, `build`, `chore`). Record rationale in the body, not Markdown change logs or verification status.
- Architecture lives in `docs/architecture/`; load only the relevant reference via its index. 
- Keep stable rules here; designs/workflow stay on demand. Locate symbols with grep/glob, then read the relevant file/range. Split by responsibility, not arbitrary line counts.
- Keep `AGENTS.md`, live status and skills minimal: replace or remove redundant text before adding. Never omit safety constraints to meet a target.
- Preserve unchanged wording and section order to support prefix-cache reuse. No cosmetic rewrites, timestamps, session notes, copied tool output or duplicate instructions. Edit only affected lines when rules, plan state or workflows actually change; load only the needed skill.
- Run build-and-test after code changes. If a project cannot build on this host (a missing Windows SDK, for example), build the affected projects, run the tests they feed, and report the gap.
- Keep file responsibilities and private interfaces narrow.
- Prefer standard independently compiled `.cpp` files with declarations in subsystem-private `*Internal.h` headers. Namespace boundaries stay within each file.

## Code map
- Entry/PnP: `driver/Driver.cpp`; `driver/README.md` lists subsystem `.cpp` files under `driver/{audio,config,l2cap,signaling,service,media}/`.
- Encoder service: `service/main.cpp`.
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
- `deploy`: deploy, restore and prepare a test session.
- `hardware-test`: run a scripted procedure from `tests/HardwareTests/`.
