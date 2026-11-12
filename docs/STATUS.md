# Plan status

Milestones track code. A hardware run is evidence for a build, not what closes a milestone:
the procedures are in [HARDWARE-TESTS](HARDWARE-TESTS.md) and scripted per test in
`tests/HardwareTests/`.

## Milestones

| ID | State | What is left |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | — |
| M5b Codec configuration | Done | — |
| M5c aptX + aptX HD | Done | — |
| M6 Config UI + install/uninstall | Done | — |
| M7 System testing | In progress | The procedures are scripted; their runs and logs stay outside the repository |
| M8 Signing + release | Not started | A signed package and its install path |
| M9 LDAC | In progress | Wired end to end (SEP 5, service, preference, CLI and window); a pass on a real LDAC sink remains |

## Next

1. **LDAC on hardware**: a pass with a real LDAC sink - the 990 kbps stream, forcing the codec
   from the settings window, and the quality modes through `OpenA2dpCtl --ldac-quality`.
2. **A first scripted pass** of the hardware procedures, kept with the build it ran on: it is the
   only run that exercises the pause-then-switch sequence, whose matches are now covered by
   `tests/CodecTests/SinkMatchTests.cpp`.

## Blockers

- None.

## Where the decisions live

- Which codecs may ship, and what a distributor owes: [Codec licensing](architecture/Codec-Licensing.md).
- Codec math, duplication and the shared timing constants:
  [Protocol and codecs](architecture/Protocol-and-Codecs.md).
