# Plan status

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | - |
| M5b Codec configuration | In progress | Codec preference and the SBC bitpool/blocks/subbands/allocation and AAC bitrate persist and drive selection; 44.1/48 kHz mono/stereo streams negotiate at the format Windows opened, which a stored rate/channel-mode preference narrows in place, and formats or codecs the sink does not advertise are refused; presets and the config UI remain |
| M6 Config UI + install/uninstall | Not started | Finish UI and clean restore of `BthA2dp.sys` |
| M7 System testing | Not started | System test matrix and long-run validation |
| M8 Signing + release | Not started | Signed release package |
