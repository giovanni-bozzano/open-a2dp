# Plan status

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | - |
| M5b Codec configuration | In progress | Windows plays the first listed PCM format and ignores the Sound-settings pick, so the endpoint format is now a driver setting (`SET_ENDPOINT_FORMAT`, automatic or one pinned format) and a pinned format is offered alone; the proposed-format SET is only logged. Verify on hardware that a pinned format is the one the shared-mode stream runs at and survives a reconnect |
| M6 Config UI + install/uninstall | In progress | The `OpenA2dpConfig` window covers device selection, status and the codec/endpoint settings; the clean restore of `BthA2dp.sys` remains |
| M7 System testing | Not started | System test matrix and long-run validation |
| M8 Signing + release | Not started | Signed release package |
