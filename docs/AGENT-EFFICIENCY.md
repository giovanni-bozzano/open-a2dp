# Documentation map

| File | What it answers | When to update |
|---|---|---|
| `README.md` | What the project is, what ships, how to build it | When the build or the shipped surface changes |
| `AGENTS.md` | Stable rules, code map and the on-demand workflow list | Only when constraints or workflows change |
| `docs/STATUS.md` | Where the project stands: milestones, next three tasks, blockers | When the plan changes; rewritten, never appended to |
| `docs/HARDWARE-TESTS.md` | How to verify behaviour on a real sink, test by test (T1-T34) | When tested behaviour or tooling changes |
| `docs/architecture/` | Why the code is shaped the way it is, one document per area | When the design changes |
| `.agents/skills/*/SKILL.md` | Operational workflows, loaded only when needed | When the workflow changes |
| Git commit messages | What changed and why, one logical change per commit | Every commit |

Start with `docs/STATUS.md`, `git status --short` and `git log -n 5 --oneline`. Search before
reading design sections; load one architecture document through its [index](architecture/README.md)
and a skill only when its workflow is needed.

Keep volatile state out of `AGENTS.md` and never duplicate commit history in Markdown: Git is the
record of what changed and why. Stay under the budgets in `AGENTS.md` (40 lines for `AGENTS.md` and
`STATUS.md`, 30 per skill); prefer replacing or removing text over adding it, and keep unchanged
wording and order so a stable prefix stays cache-friendly.
