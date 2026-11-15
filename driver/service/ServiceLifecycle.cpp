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

// The control device is named, linked and initialized: point it at this device and wake the
// service, which then finds the link; nothing polls for it.
static void PublishControl(_In_ DEVICE_CONTEXT* ctx, _In_ WDFDEVICE control)
{
	GetControlDeviceContext(control)->Device = ctx;
	InterlockedExchange(&ctx->ServiceActive, 1);
	WdfDeviceSetDeviceInterfaceState(ctx->Device, &Ioctl::kControlArrivalInterface, nullptr, TRUE);
}

// The MS-DOS name the tools enumerate. The control device keeps its \Device name across a D0
// exit, but this link is removed while the device is quiesced so a sink that is asleep or
// disconnected is not listed; ServiceStart puts it back.
static NTSTATUS FormatControlLink(_In_ DEVICE_CONTEXT* ctx,
	_Out_writes_(capacity) WCHAR* linkName, size_t capacity)
{
	return RtlStringCchPrintfW(
		linkName, capacity, L"\\DosDevices\\OpenA2dpCtl-%012I64X", ctx->RemoteAddress);
}

static NTSTATUS CreateControlLink(_In_ DEVICE_CONTEXT* ctx, _In_ WDFDEVICE control)
{
	WCHAR linkName[48];
	UNICODE_STRING link;
	NTSTATUS status = FormatControlLink(ctx, linkName, ARRAYSIZE(linkName));
	if (NT_SUCCESS(status))
	{
		RtlInitUnicodeString(&link, linkName);
		status = WdfDeviceCreateSymbolicLink(control, &link);
	}
	return status;
}

static void DeleteControlLink(_In_ DEVICE_CONTEXT* ctx)
{
	WCHAR linkName[48];
	UNICODE_STRING link;
	NTSTATUS status = FormatControlLink(ctx, linkName, ARRAYSIZE(linkName));
	if (NT_SUCCESS(status))
	{
		RtlInitUnicodeString(&link, linkName);
		status = IoDeleteSymbolicLink(&link);
	}
	// Already gone is normal on a second quiesce, or when the framework deleted the device.
	if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND)
	{
		OA2DP_WARN(ctx, "Service", "control link removal failed 0x%08X", status);
	}
}

// A start that follows a power transition has the device object from before the D0 exit: the
// link was removed at the quiesce and the rundown it drained refuses every handler until it is
// reinitialized. The link goes back first, because the tools find a sink through it; a sink
// that is not there is withdrawn again when the signaling channel open fails.
static bool ResumeControl(_In_ DEVICE_CONTEXT* ctx, _In_ WDFDEVICE control)
{
	// A collision means the previous removal had not completed; the link already names this
	// control device, so it is usable.
	const NTSTATUS status = CreateControlLink(ctx, control);
	if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_COLLISION)
	{
		OA2DP_ERROR(ctx, "Service", "control link recreate failed 0x%08X", status);
		return false;
	}
	ExReInitializeRundownProtection(&GetControlDeviceContext(control)->Rundown);
	PublishControl(ctx, control);
	return true;
}

void ServiceStart(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->ServiceRawBase == nullptr)
	{
		return;
	}
	// The object and its \Device name outlive a D0 exit, so there is nothing to create here:
	// only the link is recreated, and keeping the object is what stops a name collision with a
	// copy that is still being destroyed.
	if (ctx->ControlDevice != nullptr)
	{
		if (ResumeControl(ctx, static_cast<WDFDEVICE>(ctx->ControlDevice)))
		{
			OA2DP_INFO(ctx, "Service", "control device resumed");
		}
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
	status = FormatControlLink(ctx, linkName, ARRAYSIZE(linkName));
	if (NT_SUCCESS(status))
	{
		UNICODE_STRING linkNameString;
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
	PublishControl(ctx, control);
	OA2DP_INFO(ctx, "Service", "control device ready at %ws", linkName);
}

// The link the tools enumerate is the sink's A2DP connection, not its PnP node: a paired
// sink keeps its node and this control device while the Bluetooth link is down, so the link
// is published when the signaling channel opens and withdrawn when it closes. Publishing is
// refused while the device is quiesced or stopping; withdrawing is always honoured.
void ServicePublishLink(_In_ DEVICE_CONTEXT* ctx, bool present)
{
	WDFDEVICE control = static_cast<WDFDEVICE>(ctx->ControlDevice);
	if (control == nullptr)
	{
		return;
	}
	if (!present)
	{
		// Before the interface removal: both tools reload on that notification and find sinks
		// through the link, so a link removed afterwards would leave the sink listed.
		DeleteControlLink(ctx);
		WdfDeviceSetDeviceInterfaceState(ctx->Device, &Ioctl::kControlArrivalInterface, nullptr, FALSE);
		OA2DP_INFO(ctx, "Service", "control link withdrawn; the sink is no longer listed");
		return;
	}
	if (InterlockedCompareExchange(&ctx->ServiceActive, 0, 0) == 0 ||
		KeReadStateEvent(&ctx->StopEvent) != 0)
	{
		return;
	}
	// A link that is already there names this control device: the sink can come back before
	// the previous removal reached the tools.
	const NTSTATUS status = CreateControlLink(ctx, control);
	if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_COLLISION)
	{
		OA2DP_ERROR(ctx, "Service", "control link create failed 0x%08X", status);
		return;
	}
	WdfDeviceSetDeviceInterfaceState(ctx->Device, &Ioctl::kControlArrivalInterface, nullptr, TRUE);
	OA2DP_INFO(ctx, "Service", "control link published; the sink is listed");
}

void ServiceQuiesce(_In_ DEVICE_CONTEXT* ctx)
{
	WDFDEVICE control = ctx->ControlDevice;
	if (control == nullptr)
	{
		return;
	}

	// Stop serving but keep the object: the service's next arrival is the interface state
	// below, and a name that is never released cannot collide with a recreate on the way back
	// to D0. The link goes before the interface removal is published, in ServicePublishLink.
	InterlockedExchange(&ctx->ServiceActive, 0);
	ServicePublishLink(ctx, false);
	LatchEvents(ctx, Ioctl::kEventDeviceGone);

	// Wait for every IOCTL and cleanup handler that is already running (none of them
	// blocks for long) and refuse new ones; after this nothing but a WAIT_EVENT cancel
	// routine can reach the device through the control device, and ServiceStart's resume
	// path is what reopens it.
	ExWaitForRundownProtectionRelease(&GetControlDeviceContext(control)->Rundown);
	// A handler that ran during the wait may have registered and parked a new WAIT_EVENT.
	LatchEvents(ctx, Ioctl::kEventDeviceGone);
	UnmapRings(ctx);
	// The registrations belonged to handles of the session that just ended; a handle that is
	// still open keeps its own state but no longer speaks for the device.
	InterlockedIncrement(&ctx->ServiceGeneration);
	InterlockedExchange(&ctx->EncoderRegistered, 0);
	OA2DP_INFO(ctx, "Service", "control device quiesced; the link is removed");
}

void ServiceStop(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->ControlDevice == nullptr)
	{
		return;
	}
	ServiceQuiesce(ctx);

	// Claim the handle so the framework's cleanup callback cannot delete it twice.
	WDFDEVICE control = static_cast<WDFDEVICE>(
		InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&ctx->ControlDevice), nullptr));
	if (control == nullptr)
	{
		return;
	}

	// The control device's parent is the driver object, so it can outlive this
	// FDO's start/stop cycle; quiesced, no handler of it touches ctx any more.
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
