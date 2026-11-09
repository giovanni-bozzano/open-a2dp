# Plan status

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | - |
| M5b Codec configuration | Done | — |
| M6 Config UI + install/uninstall | In progress | The `OpenA2dpConfig` window covers device selection, status and the codec/endpoint settings; the sink list names every sink `name (address)` like `OpenA2dpCtl list`, with its state refreshed every second, and the layout is measured from the strings with the session panel scrolling itself. The clean restore of `BthA2dp.sys` remains |
| M7 System testing | Not started | System test matrix and long-run validation, starting with the two checks M5b handed over: a real player resumes after the endpoint flap (T25) and a pinned format survives a sink reconnect (T26) |
| M8 Signing + release | Not started | Signed release package |

## Codec audit
- SBC spec math is single-sourced: `SbcConfig` (IE masks, selector, configured-IE parser), `SbcAllocation` (12.6.3 bit allocation), `SbcFrame` (frame length, CRC, payload header, frames per packet). The driver and the codec library only adapt their own parameter types.
- Still duplicated by design: the two encoder front-ends, Q12 in the kernel against `double` in the codec library. The service selects the codec library's SBC encoder nowhere, so only the tests exercise it (see [Protocol and codecs](architecture/Protocol-and-Codecs.md)).
