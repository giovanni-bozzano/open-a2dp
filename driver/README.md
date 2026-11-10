# Driver implementation map

`Driver.cpp` owns entry/PnP. Subsystem folders contain independently compiled `.cpp` files and a private `<Subsystem>Internal.h` for shared types/declarations. Helpers use `OpenA2dpDriver::<Subsystem>` namespaces; public interfaces remain in `Driver.h`/`Service.h`.

| Folder / prefix | `.cpp` suffix -> responsibility |
|---|---|
| `config/Config` | `Registry`: stored codec preference (per-sink key, then the global default) |
| `l2cap/L2cap` | `Brb`: bounded control BRBs/orphan cleanup; `Callbacks`: sends/open callbacks; `Server`: incoming accepts; `Initialize`: setup/server registration/address; `Signaling`: signaling channel lifecycle; `Send`: send slots/link loss; `Media`: media channel lifecycle |
| `signaling/Signaling` | `Codec`: selection/session reset; `Transmit`: transitions/transmit; `Read`: reads/timers; `WorkItem`: media open/work item; `Lifecycle`: initialization/channel events; `Stop`: close/abort/stop |
| `media/Media` | `Packets`: timer/readiness/RTP assembly; `Pacing`: SBC/service ticks and the work item; `Aac`/`AptxHd`: private per-codec setup; `Stream`: initialize/start/stop |
| `audio/Audio` | `Helpers`: endpoint category/shared render format; `Position`: clock/timer; `Stream`: create/run/pause; `Packets`: RT allocation/position callbacks; `Format`: jack/format helpers; `Endpoint`: link notification/circuit setup; `Read`: PCM consumption |
| `service/Service` | `Rundown`: control-device access protection; `Events`: WAIT_EVENT/cancel; `Rings`: map/unmap; `Control`: rundown/IOCTL entry; `Ioctl`: dispatch; `Files`: handle cleanup; `Lifecycle`: initialize/start/stop/service detection; `Stream`: stream events/frame reads/flush |
