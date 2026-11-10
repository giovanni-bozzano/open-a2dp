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
| M6 Config UI + install/uninstall | In progress | The `OpenA2dpConfig` window covers device selection, status and the codec/endpoint settings; the sink list names every sink `name (address)` like `OpenA2dpCtl list`, with its state refreshed every second, and the layout is measured from the strings with the session panel scrolling itself. The clean restore of `BthA2dp.sys` runs in the VM (`deploy-vm.ps1 -Uninstall`): the packages, both services and the control device are gone and `OpenA2dpCtl list` is empty; the sink's return to `BthA2dp` still needs it paired and connected |
| M7 System testing | Not started | System test matrix and long-run validation, starting with the two checks M5b handed over: a real player resumes after the endpoint flap (T25) and a pinned format survives a sink reconnect (T26) |
| M8 Signing + release | Not started | Signed release package |

## Codec licensing
- aptX and aptX HD use Qualcomm's Apache-2.0 encoders from AOSP, which the project can vendor behind a user-mode provider seam; aptX LL, Adaptive and Lossless need a Qualcomm licence and stay out. AAC runs through the Windows MF encoder (no encoder implementation ships) and a Via LA pool licence may still be owed per shipped product; see [codec licensing](architecture/Codec-Licensing.md).
- Repository licence is Apache-2.0 (`LICENSE`), whose patent grant covers contributors only; codec patent licences are separate from it and only bind distributors of binaries.

## Codec audit
- SBC spec math is single-sourced: `SbcConfig` (IE masks, selector, configured-IE parser), `SbcAllocation` (12.6.3 bit allocation), `SbcFrame` (frame length, header and CRC emission, tail padding, payload header, frames per packet), `SbcFilter` (Q30 prototypes, Q14 cosines, sine). The driver and the codec library only adapt their own parameter types.
- Still duplicated by design: the two encoder front-ends, Q12 in the kernel against `double` in the codec library. The service selects the codec library's SBC encoder nowhere, so only the tests exercise it (see [Protocol and codecs](architecture/Protocol-and-Codecs.md)).
- The service's and the tools' IOCTL wrappers stay separate (different error channels and dependency sides) and share `Ioctl::kIoctlTimeoutMs` and `Ioctl::kCancelGraceMs` (2000 ms, the longer of the two values they used before); the timeout path still needs a sink to be seen end to end.
- Timing constants that must agree are shared (`Ioctl::kIoctlTimeoutMs`, `Ioctl::kCancelGraceMs`, `Ioctl::kPrerollMs`), and the driver's media busy limit is a duration so it follows `MediaPeriodMs`. The audio period, the worker stop wait and the transport timeouts (AVDTP response, BRB control, send cancel grace) stay local: no caller requires them to match.
- Duplicate rescan after the SBC consolidation: no near-duplicate bodies remain. What still matches is thin adapters, the two encoder front-ends kept by design, and the tools' `Upper`/`Lower` pair, which maps opposite case ranges. Every other symbol has a use; `reserved0` is ABI padding.
