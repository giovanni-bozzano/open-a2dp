#include "ServiceInternal.h"

namespace OpenA2dpDriver::Service
{
// Completes a parked WAIT_EVENT. The request may already have been cancelled, in
// which case the cancel routine completed it and this must not touch it again.
void CompleteWait(WDFREQUEST request, const Ioctl::WaitEventOut& state, uint32_t events)
{
	if (WdfRequestUnmarkCancelable(request) == STATUS_CANCELLED)
	{
		return;
	}

	Ioctl::WaitEventOut* out = nullptr;
	const NTSTATUS status =
		WdfRequestRetrieveOutputBuffer(request, sizeof(Ioctl::WaitEventOut), reinterpret_cast<PVOID*>(&out), nullptr);
	if (!NT_SUCCESS(status))
	{
		WdfRequestComplete(request, status);
		return;
	}

	*out = state;
	out->events = events;
	WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::WaitEventOut));
}

// Latches lifecycle events; completes a parked request with everything latched.
void LatchEvents(DEVICE_CONTEXT* ctx, uint32_t events)
{
	WDFREQUEST request = nullptr;
	Ioctl::WaitEventOut state = {};
	uint32_t deliver = 0;

	KIRQL irql;
	KeAcquireSpinLock(&ctx->WaitLock, &irql);
	ctx->PendingEvents |= events;
	request = ctx->WaitRequest;
	if (request != nullptr)
	{
		ctx->WaitRequest = nullptr;
		deliver = ctx->PendingEvents;
		ctx->PendingEvents = 0;
		state = ctx->WaitState;
	}
	KeReleaseSpinLock(&ctx->WaitLock, irql);

	if (request != nullptr)
	{
		CompleteWait(request, state, deliver);
	}
}

// Completes a parked WAIT_EVENT without latching the events for later.
// A handle closing only concerns the waiter of that handle: an event left
// pending would be handed to the next registration, so a later session would
// consume a stale kEventDeviceGone and never wait on its own.
void ReleaseWait(DEVICE_CONTEXT* ctx, uint32_t events)
{
	WDFREQUEST request = nullptr;
	Ioctl::WaitEventOut state = {};
	uint32_t deliver = 0;

	KIRQL irql;
	KeAcquireSpinLock(&ctx->WaitLock, &irql);
	request = ctx->WaitRequest;
	if (request != nullptr)
	{
		ctx->WaitRequest = nullptr;
		deliver = ctx->PendingEvents | events;
		ctx->PendingEvents = 0;
		state = ctx->WaitState;
	}
	KeReleaseSpinLock(&ctx->WaitLock, irql);

	if (request != nullptr)
	{
		CompleteWait(request, state, deliver);
	}
}

void OnWaitCancel(WDFREQUEST request)
{
	WDFFILEOBJECT fileObject = WdfRequestGetFileObject(request);
	SERVICE_FILE_CONTEXT* file = GetServiceFileContext(fileObject);
	DEVICE_CONTEXT* ctx = file->Device;

	KIRQL irql;
	KeAcquireSpinLock(&ctx->WaitLock, &irql);
	if (ctx->WaitRequest == request)
	{
		ctx->WaitRequest = nullptr;
	}
	KeReleaseSpinLock(&ctx->WaitLock, irql);

	WdfRequestComplete(request, STATUS_CANCELLED);
}
} // namespace OpenA2dpDriver::Service
