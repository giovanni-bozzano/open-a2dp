# Codec and endpoint configuration

How a stored setting reaches the driver, a running stream and the endpoint Windows offers. The
fields, their defaults and the registry names are in [Codec settings](Codec-Settings.md).

## Storage

- Per sink under `Parameters\Devices\<address>`, restored when the device starts; a global default
  applies to a sink that has none. `Preference` carries the codec choice, the SBC frame shape, the
  AAC bitrate and the pacing lead. `EndpointFormat` carries the PCM format the endpoint offers.
- The endpoint format is its own setting, not a narrowing of the codec: `automatic` offers every
  format the sink supports, and a rate plus channel count pin the endpoint to that one format,
  which is then the only entry in the Windows format list. VBR stays off.
- A value the sink does not advertise is refused instead of stored, so reading a preference back
  always shows what the driver will use.

## How a change applies

| Change | Path |
|---|---|
| Codec | The active stream CLOSEs and rediscovers; the sink keeps its connection |
| Parameter that keeps the codec (bitpool, bitrate, blocks, subbands, allocation, frames) | SUSPEND to Open, RECONFIGURE with the configuration rebuilt from the selected SEP, START; a refused RECONFIGURE falls back to CLOSE and rediscover |
| PCM format | The jack is unplugged for 80 ms so Windows drops its cached format; the endpoint and the stream come back at the new format, without rediscovering |
| Pacing lead | Resolved at the next `MediaStart`; the kernel pacer and the service's records both use it |

## Selection

`SbcConfig` and `AacConfig` are preference-aware: a stored bitpool caps the selector's request and
a stored bitrate caps the AAC bitrate, both clamped to the sink's advertised range inside the
shared selector. Each field stands on its own - block length, subband count, allocation and frames
per packet are independent - so there is no named combination to keep in sync with a stored value.
A codec the sink does not advertise, or one whose encoder provider is not registered, is refused.

## Two tools, one report

Both tools read `tools/common/StatusReport`, so the window, its **Copy** button and
`OpenA2dpCtl status` cannot show different fields. `StatusView` turns the report's lines into the
window's sectioned list; `--verbose` and the window's Details box add every field of `Status`,
`Preference` and `VersionInfo` with its hex value. `OpenA2dpCtl` remains the script path.
