# Hardware validation gates

On-demand checklists; live priority/state stays in [STATUS.md](../STATUS.md). Record run results in Git commits, not here.

## AAC and fallback
- Requires an AAC-capable sink; the documented PANTONE and Positive Vibration 2 BT headsets offer SBC only.
- Start the service before negotiation; confirm AAC offered/selected, audible output and advancing CLI counters.
- Reconnect with the service stopped and confirm SBC. An existing SBC session does not switch codecs on service registration.

## Deferred
- BlueZ/PipeWire + `btmon` negotiation/acceptor coverage: needs a Linux Bluetooth host.
- External SBC decoder comparison; AAC LATM captures from real sinks.
- Latency, pause/resume, default-output switching, out-of-range/adapter-removal handling and long-run system tests.
- AVRCP absolute volume: deferred; Windows software volume/mute already works.

## Test layers
- Host: codec IE/frame/CRC/round-trip tests, negotiation/state machine and ring stress; external decoder checks remain separate evidence.
- VM: install/uninstall/restore, suspend/resume and Driver Verifier; use a Windows 11 guest with USB Bluetooth passthrough, test signing and kernel debugging configured before deployment.
- Protocol: Bluetooth captures with Wireshark or BlueZ/`btmon`.
- Devices: SBC-only and AAC sinks, connection timing, codec switching, dropouts, latency and an 8-hour soak. These are exit gates, not claims that testing passed.
