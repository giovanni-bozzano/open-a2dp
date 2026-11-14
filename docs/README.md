# Documentation

OpenA2dp's documentation is split by what a reader is trying to do. Read the
entry that matches the task, then follow its links instead of reading everything.

## Start here

| I want to... | Read |
|---|---|
| See what OpenA2dp adds over the Windows driver | [README](../README.md) |
| Build it, deploy it, or remove it | [Build and deploy](BUILD.md) |
| Use the window or the command line | [Using OpenA2dp](USING.md) |
| Work out why a stream is misbehaving | [Diagnostics](DIAGNOSTICS.md) |
| Run the tests | [Testing](TESTING.md) |
| Understand the design | [Architecture](architecture/README.md) |
| Know what a build may ship | [Codec licensing](architecture/Codec-Licensing.md) |
| See the live plan | [STATUS](STATUS.md) |
| Follow the rules for changing code | [AGENTS](../AGENTS.md) |

## Guides

| Guide | Scope |
|---|---|
| `BUILD.md` | Toolchain, build outputs, host tests, deploy and restore |
| `USING.md` | The settings window, the CLI, the per-sink settings file, recipes |
| `DIAGNOSTICS.md` | The status report, driver and service logs, counters, triage |
| `TESTING.md` | Host test suite, hardware harness, what needs a real sink |
| `HARDWARE-TESTS.md` | The hardware procedure, test by test |
| `STATUS.md` | Milestone state, blockers, next priorities |

## Architecture

`architecture/` explains why the code is shaped the way it is. It is not an API
reference: constants, structures and wire layouts live in the headers under
`common/include/OpenA2dp/`, and the code is the final authority.

| Reference | Answers |
|---|---|
| [Overview](architecture/Overview.md) | What replaces what, the components and the audio paths |
| [Kernel driver](architecture/Driver-Lifecycle.md) | Binding, PnP, the ACX endpoint, teardown and locking |
| [Timing and events](architecture/Event-Model.md) | What time is allowed to drive, and why |
| [Encoder service](architecture/Encoder-Service.md) | The kernel/user boundary, rings, events and failure handling |
| [Protocol and codecs](architecture/Protocol-and-Codecs.md) | AVDTP negotiation, codec capabilities, packet formats, PCM depth |
| [Configuration](architecture/Codec-Configuration.md) | Endpoint formats, storage, and how a change reaches a stream |
| [Latency](architecture/Latency.md) | Where the audio waits, what the driver controls, what it reports |
| [Codec licensing](architecture/Codec-Licensing.md) | Encoders, notices and redistribution obligations |

## How the docs are kept

- **The live state lives in one place.** `STATUS.md` owns milestones, blockers and
  next priorities. Git owns history; do not write change logs into these files.
- **One owner per fact.** Usage belongs in `USING.md`, operational diagnosis in
  `DIAGNOSTICS.md`, and design rationale in `architecture/`. Link instead of
  copying.
- **No build-specific numbers in prose.** Do not pin ABI versions, structure
  sizes, current field lists or tunable internal constants. Protocol capabilities
  and user-facing ranges are fine; name the header or symbol for anything that
  can change between builds.
- **Explain the present.** Describe how the system works, not the bugs it fixed
  or the options that were considered. Keep historical rationale only where it
  states a constraint a future change could otherwise violate.
- **Update the owner, not every copy.** When behaviour changes, change the guide
  or reference that owns it and fix the links that point at it.

Coding agents should start with the repository root, then `architecture/README.md`
for design context, and load only the reference a task needs. Read `STATUS.md`,
`git status --short` and recent commits before planning work.
