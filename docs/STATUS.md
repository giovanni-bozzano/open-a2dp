# Plan status

## Milestones

| ID | State | Exit gate |
|---|---|---|
| M1 Codec library + tests | Done | — |
| M2 Driver skeleton + L2CAP | Done | — |
| M3 AVDTP negotiation | Done | — |
| M4 ACX endpoint + rings | Done | — |
| M5 Encoder service + streaming | Done | — |
| M5b Codec configuration | Done | — |
| M5c aptX + aptX HD | In progress | T27 and T28 on a sink advertising both codecs, plus a reference codeword vector per encoder |
| M6 Config UI + install/uninstall | In progress | T34 on hardware: one sink moved both ways while a player streams |
| M7 System testing | Not started | [HARDWARE-TESTS](HARDWARE-TESTS.md) T25-T34, and the soak run T20 |
| M8 Signing + release | Not started | Signed release package |

## Next

1. **T34, per-sink driver switch.** The CLI and the window move one sink between `BthA2dp` and
   `OpenA2dp`. The test VM can only exercise the refusal - its build has no media-session broker
   (`0x80070424`) - so the pause-then-switch path needs a normal Windows 11 install.
2. **T27/T28, aptX HD and classic aptX.** Negotiation, the 24-bit path and the in-stream codec
   switch, on a sink that advertises both.
3. **The rest of M7.** The latency gates need a loopback measurement (T30); the other procedures
   are in [HARDWARE-TESTS](HARDWARE-TESTS.md).

## Blockers

- None. Every open gate needs hardware; nothing is waiting on a design decision.

## Where the decisions live

- Which codecs may ship, and their licences: [Codec licensing](architecture/Codec-Licensing.md).
- Codec math, duplication and the shared timing constants:
  [Protocol and codecs](architecture/Protocol-and-Codecs.md).
