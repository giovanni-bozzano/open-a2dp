# Architecture references

[Documentation](../README.md) | [README](../../README.md)

These references explain why OpenA2dp is shaped the way it is. They are not an
API reference: constants, structures and wire layouts live in the headers under
`common/include/OpenA2dp/`, and the source is the final authority.

## Read in this order

| Reference | Answers |
|---|---|
| [Overview](Overview.md) | What replaces what, the components and the audio paths |
| [Kernel driver](Driver-Lifecycle.md) | How the driver binds to a sink, serves the endpoint and tears down |
| [Timing and events](Event-Model.md) | What time is allowed to drive, and why the render clock exists |
| [Encoder service](Encoder-Service.md) | The kernel/user boundary, shared rings, events and failure handling |
| [Protocol and codecs](Protocol-and-Codecs.md) | AVDTP negotiation, codec capabilities, packet formats and PCM depth |
| [Configuration](Codec-Configuration.md) | Endpoint formats, stored settings and how a change applies |
| [Latency](Latency.md) | Where the audio waits and what the driver can control |
| [Codec licensing](Codec-Licensing.md) | Encoders, notices and redistribution |

The implementation map for `driver/` is in [driver/README.md](../../driver/README.md).
Operational material lives one level up: [build](../BUILD.md),
[usage](../USING.md), [diagnostics](../DIAGNOSTICS.md) and
[testing](../TESTING.md).

## Invariants

These hold across every subsystem. A change that breaks one is a design change,
not a local fix.

1. **One sink, one takeover.** OpenA2dp binds the A2DP node of a single remote
   sink. It does not replace the radio, the Bluetooth core, HFP or AVRCP, and it
   never affects a sink that was not switched.
2. **SBC never depends on the service.** The kernel path must work with no
   service installed, running or reachable. When it is absent, the other codecs
   fall back to SBC.
3. **The driver owns transport and session timing.** The service owns encoding and
   paces its own pump from the shared clock. The driver builds RTP packets and
   decides what is sent.
4. **Lock order is `SessionLock` then `MediaLock`.** No lock is held across an
   unbounded transport wait, and completions never take either lock.
5. **Every asynchronous operation owns its resources.** Requests, BRBs and send
   slots are preallocated, device-parented and never reused while busy. Teardown
   leaves no orphaned request, work item, timer, mapping or reference.
6. **Audio is dropped to stay live, not shifted late.** Both rings trim past a
   bounded backlog and the stream resumes at the lead. A stall costs one gap, not
   a permanent offset.
7. **Driver, service and tools are one build.** The ABI is not a compatibility
   surface between versions; a mismatch is refused, not adapted.
8. **The report is produced once.** Both tools render the driver's status through
   the same code, so there is no second source of truth for a field.

## What belongs here

Design rationale, ownership, ordering rules and failure containment. ABI versions,
structure sizes and current field lists belong in headers, not prose. Name the
symbol for internal constants that can change; protocol capabilities and
user-facing ranges are part of the design. Explain the present shape; keep
historical notes only where they state a constraint a future change could
violate.
