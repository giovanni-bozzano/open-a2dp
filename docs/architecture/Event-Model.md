# Timing and events

[Architecture index](README.md) | [Kernel driver](Driver-Lifecycle.md) | [Latency](Latency.md)

State changes are driven by transport completions, AVDTP packets, PnP/ACX
callbacks and control IOCTLs. Elapsed time drives only the cases below; each is
a pacing or a bound, never a substitute for an event that exists.

## What time drives

| Use | Owner | Why it stays |
|---|---|---|
| Media clock | `MediaTimer`, `AudioTimer`, SBC target, service tick | A2DP is isochronous. Nothing in the stack signals "the next packet is due", so the stream's own clock paces data and wakes the service |
| AVDTP response timeout | Signaling session | A connected but silent peer produces no event, and AVDTP requires a signaling timeout. Expiry is delivered as a work-item event |
| Teardown bounds | BRB and send-slot cancellation, signaling stop, lock acquisition, service IOCTL/join | PnP suspend and service stop must return even when the lower transport never completes. These are guards only and never drive steady-state transitions |

## The render clock

`AudioTimer` is a one-shot high-resolution timer armed for the end of the engine
packet that is playing, or 10 ms ahead if that is sooner (`AudioCopyIntervalMs`).
Each wake copies the audio the engine played into the PCM ring, reports the
finished packets with `AcxRtStreamNotifyPacketComplete`, and arms the next wake.
RUN arms it. PAUSE and RELEASE clear `AudioRunning` under `AudioLock` and stop it
with a wait, so nothing runs while no stream is playing.

It exists because this endpoint is virtual:

- Real audio hardware reports a packet complete from the DMA engine's interrupt
  at the packet boundary. Here the end of a packet is a moment in time that
  nothing else signals.
- The stream declares notification support, so the audio engine runs in event
  mode and sleeps until the driver reports a packet complete.
  `EvtAcxStreamSetRenderPacket` arrives only after such a notification has woken
  the engine, and position queries follow the engine's or an application's own
  schedule. Driving completion from those events waits on a message that was
  never sent.
- Before START there are no media wakes, so the first audio, which is what
  triggers START, would go unseen. While streaming, an SBC wake comes once per
  packet, 20 to 43 ms apart, longer than a two-packet engine buffer; completions
  would be late and the copy would read buffers the engine had not refilled.

The 10 ms cap is part of the same contract. With longer engine packets, wakes at
the packet boundary alone would deliver PCM to the ring in bursts longer than the
packet slack the pacing lead assumes. With shorter packets the cap never applies,
so there is still one wake per packet.

The clock can be moved, not removed. Dropping notification support puts the
engine in timer-driven mode: the engine wakes on its own periodic timer, queries
the position, and the copy and first-audio check run inside those queries. The timer
is then inside the audio engine, with a period the driver does not choose.
Pull mode usually runs larger buffers, so latency rises, the copy's granularity
follows the engine's period, and completions do not land on packet boundaries. A
render clock exists either way.

## Event-driven paths

| Path | How it advances |
|---|---|
| Teardown | `SessionChanged` and `RejectIdle` replace a polling wait for CLOSE; the service worker join is a thread-handle wait |
| Media send | The send completion enqueues the media work item; the timer only raises the sample target and wakes the service |
| Media open | `L2capStartMediaOpen` submits the BRB and returns; the completion sets `MediaOpenCompleted` and enqueues the signaling work item, which sends START |
| Signaling send | `Transmit` submits to the idle slot or appends to a bounded FIFO; the completion enqueues the work item, which logs and submits the next packet |
| Service discovery | The driver enables the control arrival interface; the service enumerates on `CM_Register_Notification`, at start and when a worker ends. `REGISTER_ENCODER` enqueues the signaling work item, so a late encoder needs no wait |
| Sink-initiated links | The sink's own commands configure the stream. Discovery starts only when the ACX stream runs, while the session is still Idle |

A stream ends on encoder loss or link loss, never on a stall. Stall and
pending-send checks are diagnostics; they do not change state.

## Completion and wait rules

- Completion routines and timers run up to DISPATCH_LEVEL and never take
  `SessionLock` or `MediaLock`. They publish an interlocked flag and enqueue the
  owning work item.
- Work items issue asynchronous requests and return. They never wait on the
  transport while holding a lock, and they are never flushed from inside
  themselves.
- Each asynchronous operation owns one preallocated request plus an idle event.
  Teardown sets `StopEvent`, cancels, waits bounded on the idle event with no lock
  held, then flushes work items.
- Event waits clear the event before re-checking state under the lock. A
  transition between check and wait is therefore not lost.
- A work item a completion enqueues is flushed by its stopper after the active
  flag is cleared. Nothing is left queued after teardown.

## Validating a change

Exercise the paths that have no steady-state event: a teardown during streaming,
during media open and during discovery, with Driver Verifier deadlock detection
enabled. A hang is a finding; keep the state for the debugger instead of
rebooting.

## Code map

| Area | Files |
|---|---|
| Render clock and PCM copy | `driver/audio/AudioPosition.cpp`, `driver/audio/AudioPackets.cpp`, `driver/audio/AudioRead.cpp` |
| Media timer, RTP send, service tick | `driver/media/MediaPackets.cpp`, `driver/media/MediaPacing.cpp` |
| AVDTP response timer and work item | `driver/signaling/SignalingRead.cpp`, `driver/signaling/SignalingWorkItem.cpp` |
| Teardown barriers | `driver/l2cap/L2capBrb.cpp`, `driver/signaling/SignalingStop.cpp`, `driver/media/MediaStream.cpp` |
| Service waits | `driver/service/ServiceEvents.cpp`, `driver/service/ServiceLifecycle.cpp` |
