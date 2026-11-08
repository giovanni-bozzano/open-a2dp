# Plan status

## Milestones

| ID | State | Remaining exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | - |
| M5b Codec configuration | In progress | Codec preference and the SBC bitpool/blocks/subbands/allocation and AAC bitrate persist and drive selection; 44.1/48 kHz mono/stereo streams negotiate at the format Windows opened, which a stored rate/channel-mode preference narrows in place, and formats or codecs the sink does not advertise are refused rather than listed; named presets remain |
| M6 Config UI + install/uninstall | In progress | The `OpenA2dpConfig` window covers device selection, status and the codec/endpoint settings; presets and the clean restore of `BthA2dp.sys` remain |
| M7 System testing | Not started | System test matrix and long-run validation |
| M8 Signing + release | Not started | Signed release package |
