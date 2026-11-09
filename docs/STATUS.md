# Plan status

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | - |
| M5b Codec configuration | In progress | Windows plays the first listed PCM format and ignores the Sound-settings pick, so the endpoint format is a driver setting (`SET_ENDPOINT_FORMAT`, automatic or one pinned format) offered alone when pinned; the proposed-format SET is only logged. A change while streaming rebuilds the endpoint (jack unplugged for `AudioJackFlapMs`) and renegotiates the A2DP stream without rediscovery or a sink reconnect. Verify on hardware that the pinned format is the one the shared-mode stream runs at, survives a reconnect, and that a player resumes after the flap |
| M6 Config UI + install/uninstall | In progress | The `OpenA2dpConfig` window covers device selection, status and the codec/endpoint settings; the sink list names every sink `name (address)` like `OpenA2dpCtl list`, with its state refreshed every second, and the layout is measured from the strings with the session panel scrolling itself. The clean restore of `BthA2dp.sys` remains |
| M7 System testing | Not started | System test matrix and long-run validation |
| M8 Signing + release | Not started | Signed release package |
