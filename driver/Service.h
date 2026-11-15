#pragma once

// Encoder-service interface (AAC): per-device control device, IOCTLs and the shared
// PCM/frame rings. The ABI and the lifecycle rules are in
// common/include/OpenA2dp/Ioctl.h and docs/architecture/AAC-Service-Boundary.md.

#include "Driver.h"

// Allocates the shared block, initializes both rings and the WAIT_EVENT state.
// Must run before AudioInitialize (which starts producing into the PCM ring).
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ServiceInitialize(_In_ DEVICE_CONTEXT* ctx);

// Creates the per-device control device that exposes the interface, or re-enables the one a
// power transition left in place. Called from self-managed IO start: the object and its \Device
// name exist from the first start until the device is removed, while the link the tools
// enumerate exists only while the sink's A2DP link is up.
_IRQL_requires_(PASSIVE_LEVEL)
void ServiceStart(_In_ DEVICE_CONTEXT* ctx);

// Stops serving, removes the link and keeps the control device for the next start. Called when
// the device leaves D0: the object is not recreated on the way back, which is what keeps its
// name from colliding with a copy of itself that is still being destroyed.
_IRQL_requires_(PASSIVE_LEVEL)
void ServiceQuiesce(_In_ DEVICE_CONTEXT* ctx);

// Publishes or withdraws the link the tools enumerate. A paired sink keeps its PnP node while
// the Bluetooth link is down, so the link follows the AVDTP signaling channel instead: it is
// withdrawn when the channel closes or its open fails, and published when it opens. A disconnect
// is tagged with its signaling generation, so a reconnect that overtakes a failed open keeps its
// channel and link.
_IRQL_requires_(PASSIVE_LEVEL)
void ServicePublishLink(_In_ DEVICE_CONTEXT* ctx, bool present);

// Final teardown for device removal and unload: quiesces, then deletes the control device.
_IRQL_requires_(PASSIVE_LEVEL)
void ServiceStop(_In_ DEVICE_CONTEXT* ctx);

// Device cleanup: completes a parked WAIT_EVENT, unmaps the rings and clears the
// registration.
_IRQL_requires_(PASSIVE_LEVEL)
void ServiceCleanup(_In_ DEVICE_CONTEXT* ctx);

// True while any encoder service is registered for this device.
_IRQL_requires_max_(DISPATCH_LEVEL)
bool ServiceRegistered(_In_ DEVICE_CONTEXT* ctx);

// True while an encoder provider for this codec (Ioctl::kCodec*) is registered.
_IRQL_requires_max_(DISPATCH_LEVEL)
bool ServiceCodecRegistered(_In_ DEVICE_CONTEXT* ctx, uint32_t codec);

// Latches stream lifecycle events (start/stop) for WAIT_EVENT.
_IRQL_requires_max_(DISPATCH_LEVEL)
void ServiceStreamStart(_In_ DEVICE_CONTEXT* ctx);

_IRQL_requires_max_(DISPATCH_LEVEL)
void ServiceStreamStop(_In_ DEVICE_CONTEXT* ctx);

// Completes a parked WAIT_EVENT with kEventTick (called from the media work item).
_IRQL_requires_max_(DISPATCH_LEVEL)
void ServiceTick(_In_ DEVICE_CONTEXT* ctx);

// Consumes one frame record. Returns the payload length and points *payload into the
// device scratch buffer (valid until the next call), or 0 when no record is available.
// `silenceSamples` is how much of the record's PCM the service had to zero-fill, and
// `skippedSamples` how far its clock skipped before the record over PCM the driver discarded.
// Sets StreamFault on a corrupt record header.
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG ServiceReadFrame(_In_ DEVICE_CONTEXT* ctx, _Out_ const UCHAR** payload, _Out_ uint32_t* samples,
	_Out_ uint32_t* silenceSamples, _Out_ uint32_t* skippedSamples, _Out_ uint32_t* flags);

// Discards pending encoded frames so a new consumer starts at the current position.
_IRQL_requires_max_(DISPATCH_LEVEL)
void ServiceFlushRings(_In_ DEVICE_CONTEXT* ctx);
