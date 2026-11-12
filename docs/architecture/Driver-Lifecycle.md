# Driver lifecycle and invariants

## PnP and transport
- Bind to remote A2DP sink service UUID `0x110B`; use `BTH_PROFILE_DRIVER_INTERFACE` and L2CAP PSM `0x0019` for signaling/media. Do not replace the core/radio stack.
- Start creates the service control endpoint, registers incoming L2CAP handling and opens signaling. Remote Class of Device determines headphones versus speaker category; endpoint name includes `<sink> [OpenA2DP]`.
- Suspend attempts courtesy AVDTP CLOSE (ABORT where necessary), stops media, sets the stop barrier and drains/cancels work/channels. Preserve the exact ordering in `Driver.cpp`; do not reboot to hide a teardown failure.
- Lock order: `SessionLock` -> `MediaLock`. `FormatLock` is a leaf: the negotiation
  rebuilds the pin's format list under it (`SessionLock` -> `FormatLock`) and the control
  IOCTL takes it alone. No lock is held across an unbounded transport wait.
- Control BRBs and send slots own requests, BRBs and packet copies independently of a waiting caller. Timeout must not leave the transport using stack memory. Late successful opens/accepts close their own channels after teardown.
- Never re-use an in-flight read request. Completion signals read-idle on success and failure; stop also flushes the reader's work item.
- Control-device rundown and an FDO reference protect service handlers; `RingLock` serializes map/unmap ownership. See [service lifetime](Encoder-Service.md#3-shared-memory).

## ACX audio
- Render circuit `Speaker0`: PCM stereo at 44.1 or 48 kHz, 16-bit and 24-in-32 while [aptX HD lists it](Protocol-and-Codecs.md#pcm-depth-and-24-bit-aptx-hd), whichever format the ACX stream opened with.
- Accept one or two RT packets; page-round and zero buffers. Report the pacing lead as the presentation delay with no FIFO in front of the ring, so the audio engine adds no latency of its own and the sink's own buffering is all that is left outside the report.
- The ACX stream drives the AVDTP stream: the first non-silent audio of a run starts it (Windows also runs the stream with nothing to play around an endpoint rebuild), and a pause or an unpaused release suspends it once it has lasted `PauseGraceMs` (3 s; the pacer sends silence meanwhile, so a short pause resumes without a START round trip), so nothing plays into a paused endpoint and its silence is never counted as an underrun. Edges, media-channel opens and every session transition to Open or Streaming are requests (`EndpointStreamPending`), because SUSPEND only exists from Streaming and START only from Open, and because the sink or a media failure can move the session by itself; a request that lands mid-handshake is applied when the response settles the session. At most three STARTs are sent per run or renegotiation, so a sink that refuses them is not retried forever. While it is suspended `status` calls the media `Suspended` rather than `Idle`, because the configured stream is what a resume restarts.
- An endpoint format change unplugs the jack briefly whether or not a stream is open, so Windows drops the format it cached; a change inside that window extends it.
- Windows software volume/mute remains active; ACX/AVRCP absolute-volume integration is deferred.

## Driver binding
- Windows gives each remote sink its own BTHENUM node for the AudioSink service (`0x110B`), and that node's function driver is the A2DP source: the inbox `BthA2dp` or `OpenA2dp`. Both packages match the same id, so a sink moves between them by installing the other package on that one node; the other sinks keep the driver they are on.
- The package to install is the compatible one whose matching id is the service id itself, `BTHENUM\{0000110b-...}` with nothing after the closing brace. A node's list also holds entries matched through `..._GENERIC` (`bth.inf`, the enumerator's catch-all for every profile) and the node's own `..._LOCALMFG&nnnn`, and neither is an A2DP driver. Taking "the first package that is not OpenA2dp" put the catch-all on the node and left it without one.
- `tools/common/SinkDriver.*` reads the binding from the node (`SPDRP_SERVICE`, its instance id, the packages in its compatible list) and switches it with `DiInstallDevice` on that entry, which takes one package for one node rather than a hardware id and an INF. `UpdateDriverForPlugAndPlayDevicesW` cannot be used here: by id it would install on every sink, and with `INSTALLFLAG_FORCE` it installs an INF's first model even when none of that INF's models matches the device. The install needs an elevated prompt and restarts that node, so its endpoints, stream and control device go with it.
- A switch is not trusted from the install call: `ApplyPackage` reads the node's bound service (`SPDRP_SERVICE`) and its problem code back, so a package that installs and loses the bind, or that binds a driver which never starts, is a failed switch. The sink is then put back on the driver it came from - the previous package is installed on the same node after enabling it, in case the restart was what failed - and the error names where the sink ended. A sink left with no A2DP source at all would otherwise need the user to notice and repair it.
- A sink address reaches its node through the instance id, in natural or byte-reversed order (`MatchesAddress`): BTHENUM writes it either way. The same reference names the control device, which is what ties a sink in the tools to a sink in PnP.
- A sink on `BthA2dp` has no control device, so `EnumerateSinkNodes` lists A2DP nodes from PnP alone. `OpenA2dpCtl driver` and the `OpenA2dpConfig` sink list both start from it, which is the only way to reach a sink this driver does not own.

## Diagnostics
- Driver logs use `OA2DP_INFO/WARN/ERROR` with level, device tag and category. Tags are prepared as ANSI before high-IRQL callbacks; do not print `%ws` there.
- Use targeted native WinDbg MCP tools for hangs. Bounded waits do not prove lower-transport cancellation completed.
