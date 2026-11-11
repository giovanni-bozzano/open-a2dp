# Documentation

The map of this folder. Load only what a task needs.

## Start here

| Task | File |
|---|---|
| What the project is, how to build and deploy it | [README](../README.md) |
| Where it stands: milestones, next tasks, blockers | [STATUS](STATUS.md) - the only live plan |
| Verify behaviour on a real sink | [HARDWARE-TESTS](HARDWARE-TESTS.md), scripted per test in [`tests/HardwareTests/`](../tests/HardwareTests/README.md) |
| Why the code is shaped the way it is | [Architecture references](architecture/README.md) |
| What a build may ship, licence-wise | [Codec licensing](architecture/Codec-Licensing.md) |

## Files

| File | What it answers | When to update |
|---|---|---|
| `../README.md` | The project, its parts, build and deploy | When the shipped surface changes |
| `../AGENTS.md` | Stable rules, code map, on-demand workflows | Only when constraints or workflows change |
| `STATUS.md` | Milestone state, next tasks, blockers | When the plan changes; rewritten, never appended to |
| `HARDWARE-TESTS.md` | The hardware procedure, test by test (T1-T34) | When tested behaviour or tooling changes |
| `architecture/` | Why the code is shaped this way, one document per area | When the design changes |
| `../.agents/skills/*/SKILL.md` | Operational workflows, loaded on demand | When the workflow changes |
| Git commit messages | What changed and why | Every commit |

Start with `STATUS.md`, `git status --short` and `git log -n 5 --oneline`. Search before reading
design sections, and load one architecture document through its index.

Keep volatile state out of `AGENTS.md` and never duplicate commit history in Markdown: Git is the
record. Stay under the budgets in `AGENTS.md` (40 lines for `AGENTS.md` and `STATUS.md`, 30 per
skill); prefer replacing text over adding it, and keep unchanged wording and order so a stable
prefix stays cache-friendly.
