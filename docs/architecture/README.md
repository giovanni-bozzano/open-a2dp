# Architecture references

Load only the relevant document; these describe responsibilities and invariants, not change history.

| Document | Scope |
|---|---|
| [Overview](Overview.md) | Component boundaries, stack and audio data flow |
| [Driver lifecycle](Driver-Lifecycle.md) | PnP, transport ownership, locking and ACX constraints |
| [Protocol and codecs](Protocol-and-Codecs.md) | AVDTP policy, packet formats and codec references |
| [AAC service boundary](AAC-Service-Boundary.md) | Shared rings, IOCTLs, lifetime and encoder pacing |
| [Codec configuration](Codec-Configuration.md) | Planned settings/UI design; not yet implemented |

Live milestones/blockers: [STATUS.md](../STATUS.md). Hardware gates: [validation](../validation/Hardware.md). Source navigation: [driver map](../../driver/README.md). Historical decisions and individual changes belong in Git, not replacement handoffs/action logs.
