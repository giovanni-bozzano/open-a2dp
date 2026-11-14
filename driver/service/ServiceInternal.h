#pragma once

#include "../Service.h"

#include <ntstrsafe.h>
#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/FrameRecord.h>

using namespace OpenA2dp;

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::Service
{
// Context of the per-device control device that exposes the service interface.
// The control device's parent is the driver, so its handles can outlive the FDO's
// start/stop cycle. Every handler enters through Rundown, which ServiceQuiesce drains
// before the link goes away; the control device also holds a reference on the FDO
// (Owner) until it is destroyed, so DEVICE_CONTEXT memory stays valid for anything
// that still runs afterwards (a WAIT_EVENT cancel routine).
struct CONTROL_DEVICE_CONTEXT
{
	DEVICE_CONTEXT* Device;
	WDFDEVICE Owner;
	EX_RUNDOWN_REF Rundown;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROL_DEVICE_CONTEXT, GetControlDeviceContext)

// Context of one service handle.
struct SERVICE_FILE_CONTEXT
{
	DEVICE_CONTEXT* Device;
	BOOLEAN Registered;
	BOOLEAN Mapped;
	// The Ioctl::EncoderBit mask this handle registered, so its cleanup clears only those and
	// leaves a codec another handle owns registered.
	LONG CodecMask;
	// ctx->ServiceGeneration when it registered: a handle from before a power transition owns
	// nothing after it, so its cleanup must clear nothing.
	ULONG Generation;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SERVICE_FILE_CONTEXT, GetServiceFileContext)

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL OnDeviceControl;
EVT_WDF_DEVICE_FILE_CREATE OnFileCreate;
EVT_WDF_FILE_CLEANUP OnFileCleanup;
EVT_WDF_REQUEST_CANCEL OnWaitCancel;
EVT_WDF_OBJECT_CONTEXT_CLEANUP OnControlDeviceCleanup;
EVT_WDF_OBJECT_CONTEXT_DESTROY OnControlDeviceDestroy;

constexpr ULONG kRingHeaderBytes = static_cast<ULONG>(sizeof(OpenA2dp::RingHeader));
constexpr ULONG kLayoutBytes =
	kRingHeaderBytes + Ioctl::kPcmRingBytes + kRingHeaderBytes + Ioctl::kFrameRingBytes;
// Extra bytes so the layout base can be aligned to a cache line.
constexpr ULONG kAllocationBytes = kLayoutBytes + 63;

void CompleteWait(WDFREQUEST request, const Ioctl::WaitEventOut& state, uint32_t events);
void LatchEvents(DEVICE_CONTEXT* ctx, uint32_t events);
void ReleaseWait(DEVICE_CONTEXT* ctx, uint32_t events);
void OnWaitCancel(WDFREQUEST request);
void UnmapRings(DEVICE_CONTEXT* ctx, PVOID owner = nullptr);
NTSTATUS MapRings(DEVICE_CONTEXT* ctx, PVOID owner);
void OnControlDeviceCleanup(WDFOBJECT object);
void OnControlDeviceDestroy(WDFOBJECT object);
void Complete(WDFREQUEST request, NTSTATUS status);
void OnDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength, size_t inputLength, ULONG ioControlCode);
void HandleDeviceControl(DEVICE_CONTEXT* ctx, SERVICE_FILE_CONTEXT* file, WDFFILEOBJECT fileObject,
	WDFREQUEST request, ULONG ioControlCode);
void OnFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT fileObject);
void OnFileCleanup(WDFFILEOBJECT fileObject);
DEVICE_CONTEXT* EnterControl(WDFDEVICE control);
void LeaveControl(WDFDEVICE control);
} // namespace OpenA2dpDriver::Service
