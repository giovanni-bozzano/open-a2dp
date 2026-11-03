---
name: log-audit
description: Audit supplied runtime driver logs for warnings, errors and stalls.
---

1. Use the user-supplied log path or current WinDbg output history. If neither is available, ask for a source; do not scan arbitrary logs or treat source strings as runtime evidence.
2. Search `OpenA2dp` records for `WARN|ERROR|stop: still waiting|BRB did not complete|send did not complete|read still pending|no response within`. For files, use `Select-String -LiteralPath <path> -Pattern <pattern>` and inspect nearby lifecycle records only as needed.
3. Report device, category, file/line (or debugger source) and the most relevant matches in ≤5 lines. No matches means no matching evidence in that input, not a clean bill of health. Distinguish normal disconnect/stop from failures; do not change log levels automatically.
