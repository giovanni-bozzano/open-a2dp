# Event model and timer policy

State changes are driven by transport completions, AVDTP packets, PnP/ACX callbacks and
service IOCTLs. Elapsed time may only drive the cases below.

## Time uses that stay (verified not removable)
| Use | Where | Why no event can replace it |
|---|---|---|
| Media clock | `MediaTimer`, `AudioTimer`, SBC target from `KeQueryInterruptTime`, AAC `kEventTick` | A2DP is isochronous; nothing in the stack signals "next packet due". Paces data, not state. |
| AVDTP response timer (3 s) | `ResponseTimer` -> `SourceSession::OnTimeout` | A connected but silent peer produces no event; AVDTP requires a signaling timeout. Expiry is delivered as a work-item event. |
| Teardown bounds | `BrbOpRun`/`L2capSend` cancel grace, `SignalingStop`, `SignalingAbortSession`, lock acquire bounds, service IOCTL/join bounds | PnP suspend and service stop must return when the Bluetooth stack never completes (no unbounded transport waits). Guards only; never used for steady-state transitions. |

## Time-driven transitions to remove
| Phase | Current timing | Event-driven replacement |
|---|---|---|
| 1 (done, `146abb2`) | 10 ms polling: teardown CLOSE wait, `RejectBusy`, service worker join | `SessionChanged`, `RejectIdle`, thread-handle waits |
| 2 | Media send waits up to 500 ms under `MediaLock`; AAC 2 s stall -> suspend | Async media send: completion enqueues `MediaWorkItem`, timer only raises the sample target. Encoder loss is already an event (`OnFileCleanup`, ring fault, `kEventStreamStop`); the stall check becomes an underrun counter only. |
| 3 | Signaling send waits 1 s; media open blocks the work item (3 s) and retries after 200 ms sleeps | Async send queue and async media-open BRB; completions set a flag and enqueue the signaling work item, which sends START. "No resources" -> ABORT, retry configuration on the ABORT response, else close. |
| 4 | Connect waits 8 x 100 ms for the service; 700 ms delay before discovery on headset-initiated links | Service discovers the device by PnP interface notification instead of 250 ms polling; registration enqueues the signaling work item. Late AAC registration renegotiates (CLOSE -> SET_CONFIGURATION) instead of waiting. Headset-initiated links stay acceptor until its first command or until ACX requests a stream, then discover. |

Phases 3-4 change interoperability behaviour and need PANTONE/AAC-sink validation before
the timed path is deleted.

## Deadlock rules for every phase
- Completion routines and timers (up to `DISPATCH_LEVEL`) never take `SessionLock` or
  `MediaLock`; they publish an interlocked flag and enqueue the owning work item.
- Work items issue asynchronous requests and return; they never wait on the transport
  while holding a lock. Lock order stays `SessionLock` -> `MediaLock`; the media work item
  never takes `SessionLock` (it uses `SignalingRequestSuspend`).
- Each asynchronous operation owns one preallocated request plus an idle event. Teardown
  sets `StopEvent`, cancels, waits bounded on the idle event with no lock held, then
  flushes work items, never from inside the same work item (`SignalingWorkItemThread`).
- Event waits clear the event before re-checking state under the lock, so a transition
  between check and wait is never lost.
- Validate each phase with Driver Verifier deadlock detection and a teardown during
  streaming, media open and discovery (`windbg-deadlock-check` on any hang).
