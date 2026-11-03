# Plan status

Single source of current plan state. Keep this short; change history and test results belong in Git commits. Historical evidence: [original plan](A2DP-Driver-Plan.md), [old handoff](SESSION-CONTEXT.md). These are not live status sources.

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | In progress | BlueZ/PipeWire interoperability and broader acceptor validation; SBC works on hardware |
| M4 ACX endpoint + rings | Done | —; Windows audio plays over SBC |
| M5 Encoder service + streaming | In progress / AAC hardware blocked | Service implemented; validate audible AAC and counters on an AAC-capable sink, then SBC fallback |
| M5b Codec configuration | Not started | Per-device/global store, preference-aware selection, presets, config IOCTLs/UI, multi-rate ACX |
| M6 Config UI + install/uninstall | Not started | Finish UI and clean restore of `BthA2dp.sys` |
| M7 System testing | Not started | System test matrix and long-run validation |
| M8 Signing + release | Not started | Signed release package |

## Next priorities (ordered)

1. Validate teardown/read/logging fixes on the VM before feature work; deployment unconfirmed. See [hardware validation](validation/Hardware.md#teardown-and-reconnect).
2. Validate AAC end to end, then SBC fallback; blocked on an AAC-capable sink. See [AAC gate](validation/Hardware.md#aac-and-fallback).
3. Implement M5b, then finish M6. See [configuration roadmap](design/Codec-Configuration.md).

## Deferred validation

See [hardware validation](validation/Hardware.md#deferred) for deferred coverage and blockers; load only for validation tasks.
