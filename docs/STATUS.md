# Plan status

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | - |
| M5b Codec configuration | In progress | No preference narrows the sink-supported PCM list. Blocker: the engine opens the shared-mode stream with the first listed format, not the assigned default; the driver now leads both lists with the format Windows proposes and stores it per sink. Verify an applied selection is used by the next stream and survives a reconnect |
| M6 Config UI + install/uninstall | In progress | The `OpenA2dpConfig` window covers device selection, status and the codec/endpoint settings; the clean restore of `BthA2dp.sys` remains |
| M7 System testing | Not started | System test matrix and long-run validation |
| M8 Signing + release | Not started | Signed release package |
