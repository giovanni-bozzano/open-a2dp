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
| M6 Config UI + install/uninstall | In progress | Automated coverage of the window's driver switch; today only the CLI path is exercised |
| M7 System testing | In progress | The procedures are scripted; their runs and logs stay outside the repository |
| M8 Signing + release | Not started | A signed package and its install path |

## Next

1. **Coverage for the pause-then-switch path in `tools/common`**, which only a real player
   exercises today.
2. **A first scripted pass** of the hardware procedures, kept with the build it ran on.

## Blockers

- None.

## Where the decisions live

- Which codecs may ship, and what a distributor owes: [Codec licensing](architecture/Codec-Licensing.md).
- Codec math, duplication and the shared timing constants:
  [Protocol and codecs](architecture/Protocol-and-Codecs.md).
