#pragma once

// Encoder-service interface (AAC): per-device control device, IOCTLs and the shared
// PCM/frame rings. The ABI and the lifecycle rules are in
// common/include/OpenA2dp/Ioctl.h and docs/architecture/AAC-Service-Boundary.md.

#include "Driver.h"

// Allocates the shared block, initializes both rings and the WAIT_EVENT state.
// Must run before AudioInitialize (which starts producing into the PCM ring).
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ServiceInitialize(_In_ DEVICE_CONTEXT* ctx);

// Creates/deletes the per-device control device that exposes the interface. Called
// from self-managed IO start/suspend, so the interface only exists while the device
// is started.
_IRQL_requires_(PASSIVE_LEVEL)
void ServiceStart(_In_ DEVICE_CONTEXT* ctx);

_IRQL_requires_(PASSIVE_LEVEL)
void ServiceStop(_In_ DEVICE_CONTEXT* ctx);

// Device cleanup: completes a parked WAIT_EVENT, unmaps the rings and clears the
// registration.
_IRQL_requires_(PASSIVE_LEVEL)
void ServiceCleanup(_In_ DEVICE_CONTEXT* ctx);

// True while an encoder service is registered for this device.
_IRQL_requires_max_(DISPATCH_LEVEL)
bool ServiceRegistered(_In_ DEVICE_CONTEXT* ctx);

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
// Sets StreamFault on a corrupt record header.
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG ServiceReadFrame(_In_ DEVICE_CONTEXT* ctx, _Out_ const UCHAR** payload, _Out_ uint32_t* samples,
	_Out_ uint32_t* flags);

// Discards pending encoded frames so a new consumer starts at the current position.
_IRQL_requires_max_(DISPATCH_LEVEL)
void ServiceFlushRings(_In_ DEVICE_CONTEXT* ctx);
