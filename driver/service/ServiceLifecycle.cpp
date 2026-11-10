#include "ServiceInternal.h"

using namespace OpenA2dpDriver::Service;

NTSTATUS ServiceInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeSpinLock(&ctx->WaitLock);
	KeInitializeMutex(&ctx->RingLock, 0);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;

	PVOID raw = nullptr;
	NTSTATUS status = WdfMemoryCreate(
		&attrs, NonPagedPoolNx, OpenA2dpPoolTag, kAllocationBytes, &ctx->ServiceMemory, &raw);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Service", "shared memory allocation failed 0x%08X", status);
		return status;
	}

	ctx->ServiceRawBase = raw;
	ctx->ServiceBlockBytes = kAllocationBytes;
	UCHAR* base = reinterpret_cast<UCHAR*>(
		(reinterpret_cast<ULONG_PTR>(raw) + 63) & ~static_cast<ULONG_PTR>(63));
	RtlZeroMemory(base, kLayoutBytes);
	ctx->ServiceBase = base;

	ctx->PcmRingHeader = reinterpret_cast<OpenA2dp::RingHeader*>(base);
	ctx->PcmRingData = base + kRingHeaderBytes;
	ctx->FrameRingHeader = reinterpret_cast<OpenA2dp::RingHeader*>(base + kRingHeaderBytes + Ioctl::kPcmRingBytes);
	ctx->FrameRingData = base + kRingHeaderBytes + Ioctl::kPcmRingBytes + kRingHeaderBytes;

	RingBufferView::Initialize(ctx->PcmRingHeader, Ioctl::kPcmRingBytes);
	RingBufferView::Initialize(ctx->FrameRingHeader, Ioctl::kFrameRingBytes);

	ctx->Rings = {};
	ctx->Rings.magic = Ioctl::kRingsMagic;
	ctx->Rings.abiVersion = Ioctl::kAbiVersion;
	ctx->Rings.totalBytes = kLayoutBytes;
	ctx->Rings.pcmHeaderOffset = 0;
	ctx->Rings.pcmDataOffset = kRingHeaderBytes;
	ctx->Rings.pcmCapacity = Ioctl::kPcmRingBytes;
	ctx->Rings.frameHeaderOffset = kRingHeaderBytes + Ioctl::kPcmRingBytes;
	ctx->Rings.frameDataOffset = ctx->Rings.frameHeaderOffset + kRingHeaderBytes;
	ctx->Rings.frameCapacity = Ioctl::kFrameRingBytes;
	return STATUS_SUCCESS;
}

void ServiceStart(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->ControlDevice != nullptr || ctx->ServiceRawBase == nullptr)
	{
		return;
	}

	UNICODE_STRING sddl;
	RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
	PWDFDEVICE_INIT init = WdfControlDeviceInitAllocate(WdfDeviceGetDriver(ctx->Device), &sddl);
	if (init == nullptr)
	{
		OA2DP_ERROR(ctx, "Service", "control device init failed");
		return;
	}
	WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, FALSE);

	WDF_FILEOBJECT_CONFIG fileConfig;
	WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, OnFileCreate, WDF_NO_EVENT_CALLBACK, OnFileCleanup);
	WDF_OBJECT_ATTRIBUTES fileAttrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttrs, SERVICE_FILE_CONTEXT);
	WdfDeviceInitSetFileObjectConfig(init, &fileConfig, &fileAttrs);

	// The address names the control device and its symbolic link. Device
	// interfaces are not supported on control devices, so the CLI and the
	// service enumerate the symbolic links (QueryDosDevice) instead; the PnP
	// device's kControlArrivalInterface tells the service when to look.
	WCHAR address[16];
	NTSTATUS status = RtlStringCchPrintfW(address, ARRAYSIZE(address), L"%012I64X", ctx->RemoteAddress);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Service", "address format failed 0x%08X", status);
		WdfDeviceInitFree(init);
		return;
	}

	WCHAR deviceName[48];
	UNICODE_STRING deviceNameString;
	status = RtlStringCchPrintfW(deviceName, ARRAYSIZE(deviceName), L"\\Device\\OpenA2dpCtl%ws", address);
	if (NT_SUCCESS(status))
	{
		RtlInitUnicodeString(&deviceNameString, deviceName);
		status = WdfDeviceInitAssignName(init, &deviceNameString);
	}
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Service", "control device name failed 0x%08X", status);
		WdfDeviceInitFree(init);
		return;
	}

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, CONTROL_DEVICE_CONTEXT);
	// A device object's parent must stay the driver object (WdfDeviceCreate
	// rejects ParentObject); ServiceStop deletes the control device explicitly.
	attrs.EvtCleanupCallback = OnControlDeviceCleanup;
	attrs.EvtDestroyCallback = OnControlDeviceDestroy;
	// A parked WAIT_EVENT must not block the
	// control queue, so the IOCTL callbacks run at PASSIVE_LEVEL in parallel.
	attrs.ExecutionLevel = WdfExecutionLevelPassive;

	WDFDEVICE control = nullptr;
	status = WdfDeviceCreate(&init, &attrs, &control);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Service", "control device create failed 0x%08X", status);
		if (init != nullptr)
		{
			WdfDeviceInitFree(init);
		}
		return;
	}
	GetControlDeviceContext(control)->Device = ctx;
	// Keeps DEVICE_CONTEXT memory alive until the control device is destroyed;
	// dropped in OnControlDeviceDestroy, which also runs on the failure paths below.
	{
		CONTROL_DEVICE_CONTEXT* cc = GetControlDeviceContext(control);
		ExInitializeRundownProtection(&cc->Rundown);
		WdfObjectReferenceWithTag(ctx->Device, reinterpret_cast<PVOID>(OnControlDeviceDestroy));
		cc->Owner = ctx->Device;
	}

	WDF_IO_QUEUE_CONFIG queueConfig;
	// Parallel: a parked WAIT_EVENT (the service's tick source) must not stop the
	// CLI or the service from issuing GET_STATUS/SET_PREFERENCE on the same device.
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
	queueConfig.EvtIoDeviceControl = OnDeviceControl;
	status = WdfIoQueueCreate(control, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Service", "queue create failed 0x%08X", status);
		WdfObjectDelete(control);
		return;
	}

	WCHAR linkName[48];
	UNICODE_STRING linkNameString;
	status = RtlStringCchPrintfW(linkName, ARRAYSIZE(linkName), L"\\DosDevices\\OpenA2dpCtl-%ws", address);
	if (NT_SUCCESS(status))
	{
		RtlInitUnicodeString(&linkNameString, linkName);
		status = WdfDeviceCreateSymbolicLink(control, &linkNameString);
	}
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Service", "symbolic link failed 0x%08X", status);
		WdfObjectDelete(control);
		return;
	}

	// The system does not deliver I/O to a control device before this call.
	WdfControlFinishInitializing(control);

	ctx->ControlDevice = control;
	InterlockedExchange(&ctx->ServiceActive, 1);
	OA2DP_INFO(ctx, "Service", "control device ready at %ws", linkName);
	// Wakes the service, which then finds the link; nothing polls for it.
	WdfDeviceSetDeviceInterfaceState(ctx->Device, &Ioctl::kControlArrivalInterface, nullptr, TRUE);
}

void ServiceStop(_In_ DEVICE_CONTEXT* ctx)
{
	// Claim the handle so the framework's cleanup callback cannot delete it twice.
	WDFDEVICE control = static_cast<WDFDEVICE>(
		InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&ctx->ControlDevice), nullptr));
	if (control == nullptr)
	{
		return;
	}

	// The next ServiceStart re-enables it, which is the service's next arrival.
	WdfDeviceSetDeviceInterfaceState(ctx->Device, &Ioctl::kControlArrivalInterface, nullptr, FALSE);
	InterlockedExchange(&ctx->ServiceActive, 0);
	LatchEvents(ctx, Ioctl::kEventDeviceGone);

	// Wait for every IOCTL and cleanup handler that is already running (none of them
	// blocks for long) and refuse new ones; after this nothing but a WAIT_EVENT cancel
	// routine can reach the device through the control device.
	CONTROL_DEVICE_CONTEXT* cc = GetControlDeviceContext(control);
	ExWaitForRundownProtectionRelease(&cc->Rundown);
	cc->Device = nullptr;
	// A handler that ran during the wait may have registered and parked a new WAIT_EVENT.
	LatchEvents(ctx, Ioctl::kEventDeviceGone);
	UnmapRings(ctx);
	InterlockedExchange(&ctx->EncoderRegistered, 0);

	// The control device's parent is the driver object, so it can outlive this
	// FDO's start/stop cycle; with Device cleared its handlers no longer touch ctx.
	WdfObjectDelete(control);
	OA2DP_INFO(ctx, "Service", "control device removed");
}

void ServiceCleanup(_In_ DEVICE_CONTEXT* ctx)
{
	ServiceStop(ctx);
	UnmapRings(ctx);
	InterlockedExchange(&ctx->EncoderRegistered, 0);
}

bool ServiceRegistered(_In_ DEVICE_CONTEXT* ctx)
{
	return InterlockedCompareExchange(&ctx->EncoderRegistered, 0, 0) != 0;
}

bool ServiceCodecRegistered(_In_ DEVICE_CONTEXT* ctx, uint32_t codec)
{
	const LONG bit = static_cast<LONG>(Ioctl::EncoderBit(codec));
	return bit != 0 && (InterlockedCompareExchange(&ctx->EncoderRegistered, 0, 0) & bit) != 0;
}
