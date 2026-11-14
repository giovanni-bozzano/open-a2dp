#include "ServiceInternal.h"

namespace OpenA2dpDriver::Service
{
void OnFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT fileObject)
{
	// Remember the device so the IOCTL handlers can find the context.
	GetServiceFileContext(fileObject)->Device = GetControlDeviceContext(device)->Device;
	WdfRequestComplete(request, STATUS_SUCCESS);
}

void OnFileCleanup(WDFFILEOBJECT fileObject)
{
	SERVICE_FILE_CONTEXT* file = GetServiceFileContext(fileObject);
	WDFDEVICE control = WdfFileObjectGetDevice(fileObject);
	// Once ServiceStop has run there is nothing left to undo: it released the wait,
	// unmapped the rings and cleared the registration itself.
	DEVICE_CONTEXT* ctx = EnterControl(control);
	if (ctx == nullptr)
	{
		return;
	}

	// A handle that registered before a power transition no longer speaks for the device: the
	// quiesce cleared its codec bits, and a handle registered since may own them now.
	const ULONG generation = static_cast<ULONG>(InterlockedCompareExchange(&ctx->ServiceGeneration, 0, 0));
	// Release a parked WAIT_EVENT first: the closing thread must not block on it.
	// Only the registered handle owns the wait; closing another handle is unrelated.
	// The event is not latched: it belongs to the handle that is going away.
	if (file->Registered && file->Generation == generation)
	{
		ReleaseWait(ctx, Ioctl::kEventDeviceGone);
		file->Registered = FALSE;
		// Only the codecs this handle registered: a second handle may serve a different one, and
		// clearing the whole mask would drop a provider that is still alive.
		const LONG owned = file->CodecMask;
		file->CodecMask = 0;
		if (owned != 0)
		{
			InterlockedAnd(&ctx->EncoderRegistered, ~owned);
		}
		if (InterlockedCompareExchange(&ctx->ServiceActive, 0, 0) != 0 &&
			InterlockedCompareExchange(&ctx->MediaMode, 0, 0) == MediaModeService)
		{
			OA2DP_WARN(ctx, "Service", "encoder service lost while streaming; suspending");
			MediaStop(ctx);
			SignalingRequestSuspend(ctx);
		}
		// The provider that made the 24-in-32 formats available is gone; the deferred rebuild
		// happens in the signaling work item because this path holds the control lock.
		SignalingOnEncoderUnregistered(ctx);
	}
	file->Registered = FALSE;
	file->CodecMask = 0;
	file->Mapped = FALSE;
	UnmapRings(ctx, fileObject);
	LeaveControl(control);
}
} // namespace OpenA2dpDriver::Service
