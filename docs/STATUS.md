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
| M6 Codec configuration | Done | — |
| M7 aptX + aptX HD | Done | — |
| M8 LDAC | Done | — |
| M9 Config UI + install/uninstall | Done | — |
| M10 Setup file | Done | — |

## Next

1. **Validate setup on a clean VM**: install, update, uninstall and the restart path, with test
   signing enabled and a sink paired before and after.

## Blockers

- None.

## Where the decisions live

- Which codecs may ship, and what a distributor owes: [Codec licensing](architecture/Codec-Licensing.md).
- Codec math, duplication and the shared timing constants:
  [Protocol and codecs](architecture/Protocol-and-Codecs.md).
