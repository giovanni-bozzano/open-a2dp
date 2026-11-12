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
| M9 LDAC | In progress | Encoder vendored and compiling; capability IE, codec wrapper, service wiring and UI remain |

## Next

1. **LDAC negotiation and encoding**: the A2DP capability IE, `LdacCodec`, the encoder wrapper
   over the vendored `ldacBT_*` API, then service wiring and the settings window entry.
2. **A first scripted pass** of the hardware procedures, kept with the build it ran on: it is the
   only run that exercises the pause-then-switch sequence, whose matches are now covered by
   `tests/CodecTests/SinkMatchTests.cpp`.

## Blockers

- None.

## Where the decisions live

- Which codecs may ship, and what a distributor owes: [Codec licensing](architecture/Codec-Licensing.md).
- Codec math, duplication and the shared timing constants:
  [Protocol and codecs](architecture/Protocol-and-Codecs.md).
