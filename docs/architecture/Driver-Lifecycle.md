# Driver lifecycle and invariants

## PnP and transport
- Bind to remote A2DP sink service UUID `0x110B`; use `BTH_PROFILE_DRIVER_INTERFACE` and L2CAP PSM `0x0019` for signaling/media. Do not replace the core/radio stack.
- Start creates the service control endpoint, registers incoming L2CAP handling and opens signaling. Remote Class of Device determines headphones versus speaker category; endpoint name includes `<headset> [OpenA2DP]`.
- Suspend attempts courtesy AVDTP CLOSE (ABORT where necessary), stops media, sets the stop barrier and drains/cancels work/channels. Preserve the exact ordering in `Driver.cpp`; do not reboot to hide a teardown failure.
- Lock order: `SessionLock` -> `MediaLock`. No lock is held across an unbounded transport wait.
- Control BRBs and send slots own requests, BRBs and packet copies independently of a waiting caller. Timeout must not leave the transport using stack memory. Late successful opens/accepts close their own channels after teardown.
- Never re-use an in-flight read request. Completion signals read-idle on success and failure; stop also flushes the reader's work item.
- Control-device rundown and an FDO reference protect service handlers; `RingLock` serializes map/unmap ownership. See [AAC lifetime](AAC-Service-Boundary.md#3-shared-memory).

## ACX audio
- Render circuit `Speaker0`: fixed 44.1 kHz, 16-bit stereo until planned multi-rate support.
- Accept one or two RT packets; page-round and zero buffers. Report a small hardware FIFO (128 bytes), not PCM ring capacity, to avoid audio-engine rejection.
- Windows software volume/mute remains active; ACX/AVRCP absolute-volume integration is deferred.

## Diagnostics
- Driver logs use `OA2DP_INFO/WARN/ERROR` with level, device tag and category. Tags are prepared as ANSI before high-IRQL callbacks; do not print `%ws` there.
- Use targeted native WinDbg MCP tools for hangs. Bounded waits do not prove lower-transport cancellation completed.
