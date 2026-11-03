---
name: status
description: Recover current plan and Git state without loading historical handoffs.
---

1. Read `docs/STATUS.md` unless already current in context.
2. Run `git log --oneline -n 5` and `git status --short`; read relevant commit bodies only if needed.
3. Summarize active milestones/blockers, next validation gate, recent SHAs and uncommitted state in ≤8 lines.

Do not load historical plan/action logs at startup or edit files just to record a session. Follow the minimal-file and commit-only change-tracking rules in `AGENTS.md`.
