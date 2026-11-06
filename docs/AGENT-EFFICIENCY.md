# Documentation map

| File | Purpose | Update policy |
|---|---|---|
| `AGENTS.md` | Short, stable agent rules and code map | Only when constraints/workflows change |
| `docs/STATUS.md` | Authoritative milestone states, blockers, priorities and exit gates | Only on plan-state changes; no diary |
| `docs/HARDWARE-TESTS.md` | Manual hardware/Verifier test procedures | When tested behaviour or tooling changes |
| `driver/README.md` | Map of independently compiled subsystem sources | When source layout changes |
| Git commit messages | Individual changes, rationale, tests and validation limits | One logical change per commit |
| `README.md` | Project introduction and build entry point | Keep status linked, not duplicated |
| `docs/architecture/` | Indexed component, lifecycle, protocol and ABI designs | When the design changes |
| `.agents/skills/*/SKILL.md` | On-demand operational workflows | When the workflow changes |

Startup: read the short live status, Git status and five commit subjects. Search before reading implementation/design sections. Keep volatile state out of auto-loaded agent instructions; do not duplicate commit history in Markdown or create per-session handoffs.

Keep these files minimal under the budgets in `AGENTS.md`. Preserve unchanged text/order, make targeted edits only for substantive changes, and load skills on demand. Stable prefixes support cache reuse; concise files reduce context cost, but neither guarantees cache hits. Do not log token counts or cache statistics here.
