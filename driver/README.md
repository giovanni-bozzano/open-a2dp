# Driver implementation map

`Driver.cpp` owns entry, PnP and the device lifecycle. Each subsystem folder
contains independently compiled `.cpp` files and a private `<Subsystem>Internal.h`
for shared types and declarations. Helpers use the `OpenA2dpDriver::<Subsystem>`
namespace; the interfaces other subsystems use stay in `Driver.h` and `Service.h`.

| Folder | Files | Responsibility |
|---|---|---|
| `config/` | `ConfigStore` | Per-sink settings: load, validate and save |
| `l2cap/` | `Brb`, `Callbacks`, `Server`, `Initialize`, `Signaling`, `Send`, `Media` | Channel setup, BRBs, send slots, incoming accepts and link loss |
| `signaling/` | `Codec`, `Transmit`, `Read`, `WorkItem`, `Lifecycle`, `Stop` | AVDTP session, codec selection, stream transitions, response timer and teardown |
| `media/` | `Packets`, `Pacing`, `Aac`, `Aptx`, `AptxHd`, `Ldac`, `Stream` | RTP assembly, SBC and service pacing, per-codec media setup |
| `audio/` | `Helpers`, `Position`, `Stream`, `Packets`, `Format`, `Endpoint`, `Read` | ACX endpoint and circuit, render clock, PCM copy and format handling |
| `service/` | `Rundown`, `Events`, `Rings`, `Control`, `Ioctl`, `Files`, `Lifecycle`, `Stream` | Control device, IOCTLs, shared rings, service lifetime and frame reads |

Design context is in [docs/architecture/Driver-Lifecycle.md](../docs/architecture/Driver-Lifecycle.md)
and [Event-Model.md](../docs/architecture/Event-Model.md). The ABI is
`common/include/OpenA2dp/Ioctl.h`; the packet and codec contracts are in the
sibling headers there.
