---
name: windbg-deadlock-check
description: Inspect an OpenA2dp hang using native WinDbg MCP tools.
---

1. Use native `tools.windbg.*` only; discover the command tool/schema before use. No custom scripts or shell debugger commands.
2. Check sessions/connection and `get_target_state` before debugger commands. If running, request a break and confirm the stopped state; if no target, report the blocker.
3. Begin with `!wdfkd.wdfdriverinfo OpenA2dp 0x1f`; inspect identified devices using `!wdfkd.wdfdevice <address>`. Follow specific blocked threads, locks and pending IRPs with targeted commands (`!thread <address>`, `!irp <address>`) as evidence permits.
4. Avoid global `!stacks`/`!pnptriage` scans (previously stalled). Do not reproduce overlapping installer/PnP operations, reboot, or change driver state as part of diagnosis.
5. Report observed wait chain/owner, relevant frames and device state in ≤5 lines. Mark missing evidence and hypotheses explicitly; a wait alone does not prove deadlock. State whether the target remains stopped; do not silently resume it.
