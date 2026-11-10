# Architecture references

| Document | Scope |
|---|---|
| [Overview](Overview.md) | Component boundaries, stack and audio data flow |
| [Driver lifecycle](Driver-Lifecycle.md) | PnP, transport ownership, locking and ACX constraints |
| [Event model](Event-Model.md) | Allowed timers, event-driven migration phases and deadlock rules |
| [Protocol and codecs](Protocol-and-Codecs.md) | AVDTP policy, packet formats and codec references |
| [AAC service boundary](AAC-Service-Boundary.md) | Shared rings, IOCTLs, lifetime and encoder pacing |
| [Codec configuration](Codec-Configuration.md) | Stored codec/endpoint settings, how a change applies, and the settings window |
| [Codec settings](Codec-Settings.md) | Configurable fields per codec, defaults, registry names and what a change does |
| [Codec licensing](Codec-Licensing.md) | What may live in-tree per codec, and the aptX family and AAC obligations of a shipped product |
