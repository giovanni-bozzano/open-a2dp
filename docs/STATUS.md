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
| M9 LDAC | In progress | Capability IE, `LdacCodec` and `LdacStreamEncoder` done; service wiring, driver negotiation and UI remain |

## Next

1. **LDAC plumbing**: start the stream from `OpenA2dpSvc.exe`, advertise and negotiate the codec
   in the driver, then the settings window and CLI entry. The IE, codec and stream encoder are
   in place.
2. **A first scripted pass** of the hardware procedures, kept with the build it ran on: it is the
   only run that exercises the pause-then-switch sequence, whose matches are now covered by
   `tests/CodecTests/SinkMatchTests.cpp`.

## Blockers

- None.

## Where the decisions live

- Which codecs may ship, and what a distributor owes: [Codec licensing](architecture/Codec-Licensing.md).
- Codec math, duplication and the shared timing constants:
  [Protocol and codecs](architecture/Protocol-and-Codecs.md).
