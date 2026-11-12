# Architecture references

| Document | Scope |
|---|---|
| [Overview](Overview.md) | Component boundaries, stack and audio data flow |
| [Driver lifecycle](Driver-Lifecycle.md) | PnP, transport ownership, locking and ACX constraints |
| [Event model](Event-Model.md) | Which timers stay, what is event-driven, and the deadlock rules |
| [Latency](Latency.md) | Where the audio waits, how the budget is held, and what needs hardware |
| [Protocol and codecs](Protocol-and-Codecs.md) | AVDTP policy, packet formats, codec references and the PCM depth contract |
| [Encoder service boundary](Encoder-Service.md) | Shared rings, IOCTLs, lifetime and pacing for the user-mode encoders (AAC and the aptX codecs) |
| [Codec configuration](Codec-Configuration.md) | The formats the endpoint offers, the settings file and its sections, how a change applies, and the two tools that read them |
| [Codec licensing](Codec-Licensing.md) | The codec licence position: what ships and what each codec obliges a distributor to do |
