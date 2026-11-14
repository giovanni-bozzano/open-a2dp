# Event model and timer policy

State changes are driven by transport completions, AVDTP packets, PnP/ACX callbacks and service
IOCTLs. Elapsed time may only drive the cases below.

## Time uses that stay (verified not removable)
| Use | Where | Why no event can replace it |
|---|---|---|
| Media clock | `MediaTimer`, `AudioTimer`, SBC target from `KeQueryInterruptTime`, service `kEventTick` (AAC, aptX, aptX HD) | A2DP is isochronous; nothing in the stack signals "next packet due". Paces data, not state. `AudioTimer` is the virtual device's own clock: armed one shot for the end of each engine packet (or 10 ms, if sooner) and re-armed while the stream runs, it copies what was played and reports each packet complete. The engine waits on that notification, so no engine callback can stand in for it. |
| AVDTP response timer (3 s) | `ResponseTimer` -> `SourceSession::OnTimeout` | A connected but silent peer produces no event; AVDTP requires a signaling timeout. Expiry is delivered as a work-item event. |
| Teardown bounds | `BrbOpRun` and send-slot cancel grace, `SignalingStop`, `SignalingAbortSession`, lock acquire bounds, service IOCTL/join bounds | PnP suspend and service stop must return when the Bluetooth stack never completes (no unbounded transport waits). Guards only; never used for steady-state transitions. |

## What is event-driven

- **Teardown.** `SessionChanged` and `RejectIdle` replace the polling loop that waited for a
  CLOSE, and the service worker join is a thread-handle wait.
- **Media send.** The completion enqueues `MediaWorkItem`; the timer only raises the sample target
  and wakes the service. A stream ends on encoder loss (`OnFileCleanup`, ring fault) or link loss
  (send status, disconnect indication), never on a stall - the stall and pending-send checks are
  diagnostics only.
- **Media open.** `L2capStartMediaOpen` submits the BRB and returns; the completion sets
  `MediaOpenCompleted` and enqueues the signaling work item, which collects it and sends START.
  A failure ABORTs, and rediscovery follows the ABORT response (three attempts, then close). A
  close reclaims an open in flight: a late success is adopted and closed.
- **Signaling send.** `Transmit` submits to the idle slot or appends to a 4-packet FIFO under
  `SessionLock`; the send completion enqueues the signaling work item, which logs the status and
  submits the next packet. Close cancels and waits the send-cancel grace; `ResetSession` drops the
  packets of an old session.
- **Service discovery.** The driver enables `kControlArrivalInterface` once the control link
  exists (disabled in `ServiceStop`, so each resume is a new arrival); the service enumerates on
  `CM_Register_Notification` arrival, at start, and when a worker ends. `REGISTER_ENCODER`
  enqueues the signaling work item, so a late encoder is picked up without a wait.
- **Sink-initiated links.** The sink's own commands configure the stream. Discovery starts only
  when the ACX stream runs (`SignalingOnAudioRun`, or already running at connect) while the
  session is still Idle.

## Deadlock and ownership rules
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
- Validate a change with Driver Verifier deadlock detection and a teardown during
  streaming, media open and discovery (`windbg-deadlock-check` on any hang).
