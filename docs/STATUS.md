# Plan status

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | - |
| M5b Codec configuration | In progress | No preference narrows the sink-supported PCM list. Blocker: Windows selections opened 44.1 kHz stereo on hardware because ACX answered the preferred-mode-format query from the pin default; verify an applied selection now survives the next playback and a reconnect (the driver answers that query with the format the engine opened last) |
| M6 Config UI + install/uninstall | In progress | The `OpenA2dpConfig` window covers device selection, status and the codec/endpoint settings; the clean restore of `BthA2dp.sys` remains |
| M7 System testing | Not started | System test matrix and long-run validation |
| M8 Signing + release | Not started | Signed release package |
