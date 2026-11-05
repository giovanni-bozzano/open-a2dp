# Plan status

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | In progress | BlueZ/PipeWire interoperability and broader acceptor validation; SBC works on hardware |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | In progress / AAC hardware blocked | Service implemented; validate audible AAC and counters on an AAC-capable sink, then SBC fallback |
| M5b Codec configuration | Not started | Per-device/global store, preference-aware selection, presets, config IOCTLs/UI, multi-rate ACX |
| M6 Config UI + install/uninstall | Not started | Finish UI and clean restore of `BthA2dp.sys` |
| M7 System testing | Not started | System test matrix and long-run validation |
| M8 Signing + release | Not started | Signed release package |

## Next priorities (ordered)

1. Event-driven migration done (phases 1-4); validate on hardware: media open retry, AAC renegotiation on late service registration, sink-initiated links discovering only on audio run. Media clock, AVDTP response timer and teardown bounds stay. See [event model](architecture/Event-Model.md).
2. Validate AAC end to end, then SBC fallback; blocked on an AAC-capable sink. See [AAC gate](VALIDATION.md#aac-and-fallback).
3. Implement M5b, then finish M6. See [configuration design](architecture/Codec-Configuration.md).
