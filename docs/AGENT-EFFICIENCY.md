# Documentation map

## For a human

| File | What it answers |
|---|---|
| `README.md` | What the project is, what ships, how to build it |
| `docs/STATUS.md` | Where the project stands: milestones, next three tasks, blockers |
| `docs/HARDWARE-TESTS.md` | How to verify behaviour on a real sink, test by test (T1-T34) |
| `docs/architecture/` | Why the code is shaped the way it is, one document per area ([index](architecture/README.md)) |

## For an agent

| File | Use |
|---|---|
| `AGENTS.md` | Stable rules, code map, on-demand workflow list |
| `docs/STATUS.md` | The only live plan. Read it first, with `git status --short` and `git log -n 5 --oneline` |
| `.agents/skills/*/SKILL.md` | Operational workflows, loaded only when needed |
| `docs/architecture/` | Design detail; load one document through its index, not the whole tree |

Keep volatile state out of `AGENTS.md`, and never duplicate commit history in Markdown: Git is the
record of what changed and why. `docs/STATUS.md` is rewritten when the plan changes, not appended
to.

Stay under the budgets in `AGENTS.md` (40 lines for `AGENTS.md` and `STATUS.md`, 30 per skill).
Prefer replacing or removing text over adding it, and keep unchanged wording and section order so
a stable prefix stays cache-friendly.
