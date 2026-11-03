# Documentation map

| File | Purpose | Update policy |
|---|---|---|
| `AGENTS.md` | Short, stable agent rules and code map | Only when constraints/workflows change |
| `docs/STATUS.md` | Authoritative milestone states, blockers, priorities and exit gates | Only on plan-state changes; no diary |
| `docs/validation/Hardware.md`, `docs/design/Codec-Configuration.md` | On-demand validation gates and configuration scope | Only when requirements change |
| `driver/README.md` | Map of independently compiled subsystem sources | When source layout changes |
| Git commit messages | Individual changes, rationale, tests and validation limits | One logical change per commit |
| `README.md` | Project introduction and build entry point | Keep status linked, not duplicated |
| `docs/AAC-Service-Boundary.md` | On-demand ABI/lifecycle design | When the design changes; no progress log |
| `.agents/skills/*/SKILL.md` | On-demand operational workflows | When the workflow changes |
| `docs/A2DP-Driver-Plan.md`, `docs/SESSION-CONTEXT.md` | Frozen legacy design/history | Do not maintain; consult only relevant sections |

Startup: read the short live status, Git status and five commit subjects. Search before reading implementation/design sections. Keep volatile state out of auto-loaded agent instructions; do not duplicate commit history in Markdown or create per-session handoffs.

Keep these files minimal under the budgets in `AGENTS.md`. Preserve unchanged text/order, make targeted edits only for substantive changes, and load skills on demand. Stable prefixes support cache reuse; concise files reduce context cost, but neither guarantees cache hits. Do not log token counts or cache statistics here.
