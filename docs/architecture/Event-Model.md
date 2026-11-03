# Event model and timer policy

State changes are driven by transport completions, AVDTP packets, PnP/ACX callbacks and
service IOCTLs. Elapsed time may only drive the cases below.

## Time uses that stay (verified not removable)
| Use | Where | Why no event can replace it |
|---|---|---|
| Media clock | `MediaTimer`, `AudioTimer`, SBC target from `KeQueryInterruptTime`, AAC `kEventTick` | A2DP is isochronous; nothing in the stack signals "next packet due". Paces data, not state. |
| AVDTP response timer (3 s) | `ResponseTimer` -> `SourceSession::OnTimeout` | A connected but silent peer produces no event; AVDTP requires a signaling timeout. Expiry is delivered as a work-item event. |
| Teardown bounds | `BrbOpRun` and send-slot cancel grace, `SignalingStop`, `SignalingAbortSession`, lock acquire bounds, service IOCTL/join bounds | PnP suspend and service stop must return when the Bluetooth stack never completes (no unbounded transport waits). Guards only; never used for steady-state transitions. |

## Time-driven transitions to remove
| Phase | Current timing | Event-driven replacement |
|---|---|---|
| 1 (done, `146abb2`) | 10 ms polling: teardown CLOSE wait, `RejectBusy`, service worker join | `SessionChanged`, `RejectIdle`, thread-handle waits |
| 2 (done, `c7103ef`) | Media send waited up to 500 ms under `MediaLock`; AAC 2 s stall and 3 s pending send -> suspend | Async media send: the completion enqueues `MediaWorkItem`, the timer only raises the sample target and wakes the service. Stall and pending-send checks are diagnostics only; encoder loss (`OnFileCleanup`, ring fault) and link loss (send status, disconnect indication) end the stream. |
| 3a (done, `3f1683c`) | Media open blocked the work item (3 s) under `SessionLock` and retried after 200 ms sleeps | `L2capStartMediaOpen` submits the BRB; its completion sets `MediaOpenCompleted` and enqueues the signaling work item, which collects it (`L2capFinishMediaOpen`) and sends START. Failure -> ABORT, rediscover on the ABORT response (3 attempts), else close. A close reclaims an open in flight: a late success is adopted and closed, an abandoned one is closed by its completion. |
| 3b (done, `2a2ed82`) | Signaling send waited up to 1 s under `SessionLock` (timeout only logged) | `Transmit` submits to the idle slot or appends to a 4-packet FIFO under `SessionLock`; the send completion enqueues the signaling work item, which logs the status and submits the next packet. Close cancels and waits the send-cancel grace; `ResetSession` drops packets of the old session. |
| 4a (done) | Service polled for control devices every 250 ms | The driver enables `kControlArrivalInterface` on its PnP device once the control link exists (disabled in `ServiceStop`, so each resume is a new arrival); the service enumerates on `CM_Register_Notification` arrival, at start and when a worker ends. |
| 4b | Connect waits 8 x 100 ms for the service | Registration enqueues the signaling work item; a late AAC registration renegotiates (ABORT -> rediscover) when the sink offered AAC, instead of waiting. |
| 4c | 700 ms delay before discovery on headset-initiated links | Stay acceptor until the headset's first command or until ACX runs the stream, then discover. |

Phases 3-4 change interoperability behaviour and need PANTONE/AAC-sink validation before
the timed path is deleted.

## Deadlock and ownership rules for every phase
- Completion routines and timers (up to `DISPATCH_LEVEL`) never take `SessionLock` or
  `MediaLock`; they publish an interlocked flag and enqueue the owning work item.
- Work items issue asynchronous requests and return; they never wait on the transport
  while holding a lock. Lock order stays `SessionLock` -> `MediaLock`; the media work item
  never takes `SessionLock` (it uses `SignalingRequestSuspend`).
- Each asynchronous operation owns one preallocated request plus an idle event. Teardown
  sets `StopEvent`, cancels, waits bounded on the idle event with no lock held, then
  flushes work items, never from inside the same work item (`SignalingWorkItemThread`).
- No orphans: requests, BRBs and send buffers are preallocated and device-parented; an
  operation abandoned after its bound stays owned by its slot until the completion
  arrives, and is never reused while busy. A work item a completion enqueues is flushed by
  its stopper (`MediaStop` flushes `MediaWorkItem`) after the active flag is cleared.
- Event waits clear the event before re-checking state under the lock, so a transition
  between check and wait is never lost.
- Validate each phase with Driver Verifier deadlock detection and a teardown during
  streaming, media open and discovery (`windbg-deadlock-check` on any hang).
