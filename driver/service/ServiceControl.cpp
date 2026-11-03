#include "ServiceInternal.h"

namespace OpenA2dpDriver::Service
{
void OnControlDeviceCleanup(WDFOBJECT object)
{
	DEVICE_CONTEXT* ctx = GetControlDeviceContext(object)->Device;
	if (ctx == nullptr)
	{
		return;
	}
	// The framework is deleting the control device (driver unload or device removal):
	// forget the handle so ServiceStop does not delete it again.
	InterlockedCompareExchangePointer(
		reinterpret_cast<PVOID volatile*>(&ctx->ControlDevice), nullptr, object);
	InterlockedExchange(&ctx->ServiceActive, 0);
	// Release a parked WAIT_EVENT before the queue disappears.
	LatchEvents(ctx, Ioctl::kEventDeviceGone);
	UnmapRings(ctx);
	InterlockedExchange(&ctx->EncoderRegistered, 0);
}

void OnControlDeviceDestroy(WDFOBJECT object)
{
	WDFDEVICE owner = GetControlDeviceContext(object)->Owner;
	if (owner != nullptr)
	{
		WdfObjectDereferenceWithTag(owner, reinterpret_cast<PVOID>(OnControlDeviceDestroy));
	}
}

void Complete(WDFREQUEST request, NTSTATUS status)
{
	WdfRequestComplete(request, status);
}

void HandleDeviceControl(DEVICE_CONTEXT* ctx, SERVICE_FILE_CONTEXT* file, WDFFILEOBJECT fileObject,
	WDFREQUEST request, ULONG ioControlCode);

void OnDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength, size_t inputLength, ULONG ioControlCode)
{
	UNREFERENCED_PARAMETER(outputLength);
	UNREFERENCED_PARAMETER(inputLength);

	WDFDEVICE control = WdfIoQueueGetDevice(queue);
	WDFFILEOBJECT fileObject = WdfRequestGetFileObject(request);
	SERVICE_FILE_CONTEXT* file = fileObject != nullptr ? GetServiceFileContext(fileObject) : nullptr;
	DEVICE_CONTEXT* ctx = EnterControl(control);
	if (ctx == nullptr)
	{
		Complete(request, STATUS_INVALID_DEVICE_STATE);
		return;
	}
	if (file == nullptr)
	{
		Complete(request, STATUS_INVALID_DEVICE_STATE);
	}
	else
	{
		HandleDeviceControl(ctx, file, fileObject, request, ioControlCode);
	}
	LeaveControl(control);
}
} // namespace OpenA2dpDriver::Service
