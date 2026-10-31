#include "Service.h"

#include <ntstrsafe.h>
#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/FrameRecord.h>

using namespace OpenA2dp;

namespace
{

// Context of the per-device control device that exposes the service interface.
struct CONTROL_DEVICE_CONTEXT
{
	DEVICE_CONTEXT* Device;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROL_DEVICE_CONTEXT, GetControlDeviceContext)

// Context of one service handle.
struct SERVICE_FILE_CONTEXT
{
	DEVICE_CONTEXT* Device;
	BOOLEAN Registered;
	BOOLEAN Mapped;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SERVICE_FILE_CONTEXT, GetServiceFileContext)

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL OnDeviceControl;
EVT_WDF_DEVICE_FILE_CREATE OnFileCreate;
EVT_WDF_FILE_CLEANUP OnFileCleanup;
EVT_WDF_REQUEST_CANCEL OnWaitCancel;
EVT_WDF_OBJECT_CONTEXT_CLEANUP OnControlDeviceCleanup;

constexpr ULONG kRingHeaderBytes = static_cast<ULONG>(sizeof(OpenA2dp::RingHeader));
constexpr ULONG kLayoutBytes =
	kRingHeaderBytes + Ioctl::kPcmRingBytes + kRingHeaderBytes + Ioctl::kFrameRingBytes;
// Extra bytes so the layout base can be aligned to a cache line.
constexpr ULONG kAllocationBytes = kLayoutBytes + 63;

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
// A handle closing only concerns the waiter of that handle: leaving the event
// pending would hand it to the next registration, which is how a stale
// kEventDeviceGone used to keep every new service session from ever waiting.
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

void UnmapRings(DEVICE_CONTEXT* ctx)
{
	// A user-mode mapping must be released in the context of the process that mapped
	// it; device teardown runs in a system thread, so attach when needed.
	PEPROCESS process = static_cast<PEPROCESS>(InterlockedExchangePointer(&ctx->RingProcess, nullptr));
	if (ctx->RingMdl != nullptr)
	{
		if (ctx->RingUserVa != nullptr)
		{
			KAPC_STATE apc;
			bool attached = false;
			if (process != nullptr && process != IoGetCurrentProcess() &&
				PsGetProcessExitStatus(process) == STATUS_PENDING)
			{
				KeStackAttachProcess(process, &apc);
				attached = true;
			}
			// If the process already exited its address space is gone, so there is
			// nothing to unmap and touching it would hit another process.
			if (process == nullptr || process == IoGetCurrentProcess() || attached)
			{
				MmUnmapLockedPages(ctx->RingUserVa, ctx->RingMdl);
			}
			if (attached)
			{
				KeUnstackDetachProcess(&apc);
			}
			ctx->RingUserVa = nullptr;
		}
		IoFreeMdl(ctx->RingMdl);
		ctx->RingMdl = nullptr;
	}
	if (process != nullptr)
	{
		ObDereferenceObject(process);
	}
	InterlockedExchange(&ctx->RingsMapped, 0);
}

NTSTATUS MapRings(DEVICE_CONTEXT* ctx)
{
	if (ctx->ServiceRawBase == nullptr)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (InterlockedCompareExchange(&ctx->RingsMapped, 1, 0) != 0)
	{
		return STATUS_DEVICE_BUSY;
	}

	PMDL mdl = IoAllocateMdl(ctx->ServiceRawBase, kAllocationBytes, FALSE, FALSE, nullptr);
	if (mdl == nullptr)
	{
		InterlockedExchange(&ctx->RingsMapped, 0);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	MmBuildMdlForNonPagedPool(mdl);

	// IOCTLs run in the caller's process context, so the pages land in the service.
	PVOID userVa = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, nullptr, FALSE, NormalPagePriority);
	if (userVa == nullptr)
	{
		IoFreeMdl(mdl);
		InterlockedExchange(&ctx->RingsMapped, 0);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	ctx->RingMdl = mdl;
	ctx->RingUserVa = userVa;
	// Keep the mapping process alive so teardown can attach and unmap.
	PEPROCESS process = IoGetCurrentProcess();
	ObReferenceObject(process);
	ctx->RingProcess = process;
	return STATUS_SUCCESS;
}

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

void Complete(WDFREQUEST request, NTSTATUS status)
{
	WdfRequestComplete(request, status);
}

void OnDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength, size_t inputLength, ULONG ioControlCode)
{
	UNREFERENCED_PARAMETER(outputLength);
	UNREFERENCED_PARAMETER(inputLength);

	DEVICE_CONTEXT* ctx = GetControlDeviceContext(WdfIoQueueGetDevice(queue))->Device;
	WDFFILEOBJECT fileObject = WdfRequestGetFileObject(request);
	SERVICE_FILE_CONTEXT* file = fileObject != nullptr ? GetServiceFileContext(fileObject) : nullptr;
	if (ctx == nullptr || file == nullptr)
	{
		Complete(request, STATUS_INVALID_DEVICE_STATE);
		return;
	}

	switch (ioControlCode)
	{
	case Ioctl::kIoctlGetVersion:
	{
		Ioctl::VersionInfo* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::VersionInfo), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		*out = {};
		out->abiVersion = Ioctl::kAbiVersion;
		out->driverVersion = 0x00010000;
		out->maxFramePayload = Ioctl::kMaxFramePayload;
		out->maxPcmRingBytes = Ioctl::kPcmRingBytes;
		out->maxFrameRingBytes = Ioctl::kFrameRingBytes;
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::VersionInfo));
		return;
	}

	case Ioctl::kIoctlRegisterEncoder:
	{
		Ioctl::RegisterEncoderIn* in = nullptr;
		Ioctl::RegisterEncoderOut* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveInputBuffer(
			request, sizeof(Ioctl::RegisterEncoderIn), reinterpret_cast<PVOID*>(&in), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::RegisterEncoderOut), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		if (in->abiVersion != Ioctl::kAbiVersion)
		{
			Complete(request, STATUS_REVISION_MISMATCH);
			return;
		}
		if (in->codec != Ioctl::kCodecAac)
		{
			Complete(request, STATUS_NOT_SUPPORTED);
			return;
		}
		if (ctx->PcmRingHeader == nullptr)
		{
			Complete(request, STATUS_DEVICE_NOT_READY);
			return;
		}
		if (InterlockedCompareExchange(&ctx->EncoderRegistered, 1, 0) != 0)
		{
			Complete(request, STATUS_DEVICE_BUSY);
			return;
		}
		file->Registered = TRUE;
		// A new service session starts with no latched events. Anything left over
		// belongs to the previous handle (for example kEventDeviceGone latched when
		// it closed) and must not be delivered to this one.
		{
			KIRQL irql;
			KeAcquireSpinLock(&ctx->WaitLock, &irql);
			ctx->PendingEvents = 0;
			ctx->WaitState = {};
			KeReleaseSpinLock(&ctx->WaitLock, irql);
		}
		*out = {};
		out->abiVersion = Ioctl::kAbiVersion;
		out->codec = Ioctl::kCodecAac;
		out->maxFramePayload = Ioctl::kMaxFramePayload;
		out->pcmSampleRate = AudioSampleRate;
		out->pcmChannels = AudioChannels;
		OA2DP_LOG("service: encoder registered (max frame %u bytes)", out->maxFramePayload);
		if (InterlockedCompareExchange(&ctx->MediaMode, 0, 0) == MediaModeSbc)
		{
			// The stream was negotiated before the service came up, so AAC was not
			// offered for it. Say so: otherwise this looks like AAC silently failing.
			OA2DP_LOG("service: stream is already SBC; reconnect the headset to negotiate AAC");
		}
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::RegisterEncoderOut));
		return;
	}

	case Ioctl::kIoctlMapRings:
	{
		if (!file->Registered)
		{
			Complete(request, STATUS_INVALID_DEVICE_STATE);
			return;
		}
		if (file->Mapped)
		{
			Complete(request, STATUS_DEVICE_BUSY);
			return;
		}
		Ioctl::SharedRings* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::SharedRings), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		status = MapRings(ctx);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		file->Mapped = TRUE;
		*out = ctx->Rings;
		const auto offset = static_cast<uint64_t>(ctx->ServiceBase - static_cast<PUCHAR>(ctx->ServiceRawBase));
		out->userBase = reinterpret_cast<uintptr_t>(ctx->RingUserVa) + offset;
		OA2DP_LOG("service: rings mapped at 0x%I64X", out->userBase);
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::SharedRings));
		return;
	}

	case Ioctl::kIoctlWaitEvent:
	{
		if (!file->Registered)
		{
			Complete(request, STATUS_INVALID_DEVICE_STATE);
			return;
		}
		Ioctl::WaitEventOut* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::WaitEventOut), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}

		Ioctl::WaitEventOut state = {};
		uint32_t deliver = 0;
		KIRQL irql;
		KeAcquireSpinLock(&ctx->WaitLock, &irql);
		if (ctx->WaitRequest != nullptr)
		{
			KeReleaseSpinLock(&ctx->WaitLock, irql);
			Complete(request, STATUS_DEVICE_BUSY);
			return;
		}
		if (ctx->PendingEvents != 0)
		{
			deliver = ctx->PendingEvents;
			ctx->PendingEvents = 0;
			state = ctx->WaitState;
			KeReleaseSpinLock(&ctx->WaitLock, irql);
			*out = state;
			out->events = deliver;
			WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::WaitEventOut));
			return;
		}
		ctx->WaitRequest = request;
		KeReleaseSpinLock(&ctx->WaitLock, irql);

		status = WdfRequestMarkCancelableEx(request, OnWaitCancel);
		if (!NT_SUCCESS(status))
		{
			KeAcquireSpinLock(&ctx->WaitLock, &irql);
			if (ctx->WaitRequest == request)
			{
				ctx->WaitRequest = nullptr;
			}
			KeReleaseSpinLock(&ctx->WaitLock, irql);
			Complete(request, status);
		}
		return;
	}

	case Ioctl::kIoctlGetStatus:
	{
		Ioctl::Status* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::Status), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		*out = {};
		out->abiVersion = Ioctl::kAbiVersion;
		WdfWaitLockAcquire(ctx->SessionLock, nullptr);
		out->avdtpState = static_cast<uint32_t>(ctx->Session.State());
		out->negotiatedCodec = ctx->Session.SelectedIeLength() != 0
			? static_cast<uint32_t>(ctx->Session.SelectedCodec())
			: 0;
		out->preferenceFlags = ctx->Preference.flags;
		out->preferenceCodec = ctx->Preference.codec;
		WdfWaitLockRelease(ctx->SessionLock);
		const LONG mode = InterlockedCompareExchange(&ctx->MediaMode, 0, 0);
		if (InterlockedCompareExchange(&ctx->StreamFault, 0, 0) != 0)
		{
			out->mediaState = Ioctl::MediaState::Failed;
		}
		else if (!ctx->MediaActive)
		{
			out->mediaState = Ioctl::MediaState::Idle;
		}
		else if (mode == MediaModeAac && ctx->RtpSequence == 0)
		{
			out->mediaState = Ioctl::MediaState::WaitingForEncoder;
		}
		else
		{
			out->mediaState = Ioctl::MediaState::Streaming;
		}
		out->codec = ctx->ActiveCodec;
		out->sampleRate = ctx->ActiveSampleRate;
		out->channels = ctx->ActiveChannels;
		out->bitrate = ctx->ActiveBitrate;
		out->objectType = ctx->ActiveObjectType;
		out->encoderRegistered = InterlockedCompareExchange(&ctx->EncoderRegistered, 0, 0) != 0 ? 1u : 0u;
		if (ctx->PcmRingHeader != nullptr)
		{
			out->pcmBufferedBytes = RingBufferView(ctx->PcmRingHeader, ctx->PcmRingData).ReadableBytes();
		}
		out->pcmCapacity = Ioctl::kPcmRingBytes;
		if (ctx->FrameRingHeader != nullptr)
		{
			out->frameBufferedBytes = RingBufferView(ctx->FrameRingHeader, ctx->FrameRingData).ReadableBytes();
		}
		out->frameCapacity = Ioctl::kFrameRingBytes;
		out->packetsSent = ctx->RtpSequence;
		out->framesDropped = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->FramesDropped, 0, 0));
		out->underruns = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->Underruns, 0, 0));
		out->sendFailures = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->SendFailures, 0, 0));
		out->lastError = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->LastMediaError, 0, 0));
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::Status));
		return;
	}

	case Ioctl::kIoctlSetPreference:
	{
		Ioctl::Preference* in = nullptr;
		NTSTATUS status = WdfRequestRetrieveInputBuffer(
			request, sizeof(Ioctl::Preference), reinterpret_cast<PVOID*>(&in), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		if (in->abiVersion != Ioctl::kAbiVersion)
		{
			Complete(request, STATUS_REVISION_MISMATCH);
			return;
		}
		if ((in->flags & Ioctl::kPreferenceForceCodec) != 0 && in->codec != Ioctl::kCodecSbc &&
			in->codec != Ioctl::kCodecAac)
		{
			Complete(request, STATUS_INVALID_PARAMETER);
			return;
		}
		WdfWaitLockAcquire(ctx->SessionLock, nullptr);
		ctx->Preference = *in;
		WdfWaitLockRelease(ctx->SessionLock);
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 0);
		return;
	}

	default:
		Complete(request, STATUS_INVALID_DEVICE_REQUEST);
		return;
	}
}

void OnFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT fileObject)
{
	// Remember the device so the IOCTL handlers can find the context.
	GetServiceFileContext(fileObject)->Device = GetControlDeviceContext(device)->Device;
	WdfRequestComplete(request, STATUS_SUCCESS);
}

void OnFileCleanup(WDFFILEOBJECT fileObject)
{
	SERVICE_FILE_CONTEXT* file = GetServiceFileContext(fileObject);
	DEVICE_CONTEXT* ctx = file->Device;
	if (ctx == nullptr)
	{
		return;
	}

	// Release a parked WAIT_EVENT first: the closing thread must not block on it.
	// Only the registered handle owns the wait; closing another handle is unrelated.
	// The event is not latched: it belongs to the handle that is going away.
	if (file->Registered)
	{
		ReleaseWait(ctx, Ioctl::kEventDeviceGone);
		file->Registered = FALSE;
		InterlockedExchange(&ctx->EncoderRegistered, 0);
		// During device teardown ServiceStop has already stopped media and latched
		// kEventDeviceGone; do not touch the media/signaling work items then.
		if (InterlockedCompareExchange(&ctx->ServiceActive, 0, 0) != 0 &&
			InterlockedCompareExchange(&ctx->MediaMode, 0, 0) == MediaModeAac)
		{
			OA2DP_LOG("service: encoder lost while streaming AAC; suspending");
			MediaStop(ctx);
			SignalingRequestSuspend(ctx);
		}
	}
	if (file->Mapped)
	{
		file->Mapped = FALSE;
		UnmapRings(ctx);
	}
}

} // namespace

NTSTATUS ServiceInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeSpinLock(&ctx->WaitLock);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;

	PVOID raw = nullptr;
	NTSTATUS status = WdfMemoryCreate(
		&attrs, NonPagedPoolNx, OpenA2dpPoolTag, kAllocationBytes, &ctx->ServiceMemory, &raw);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("service: shared memory allocation failed 0x%08X", status);
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
		OA2DP_LOG("service: control device init failed");
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
	// service enumerate the symbolic links (QueryDosDevice) instead.
	WCHAR address[16];
	NTSTATUS status = RtlStringCchPrintfW(address, ARRAYSIZE(address), L"%012I64X", ctx->RemoteAddress);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("service: address format failed 0x%08X", status);
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
		OA2DP_LOG("service: control device name failed 0x%08X", status);
		WdfDeviceInitFree(init);
		return;
	}

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, CONTROL_DEVICE_CONTEXT);
	// A device object's parent must stay the driver object (WdfDeviceCreate
	// rejects ParentObject); ServiceStop deletes the control device explicitly.
	attrs.EvtCleanupCallback = OnControlDeviceCleanup;
	// GET_STATUS takes SessionLock, and a parked WAIT_EVENT must not block the
	// control queue, so the IOCTL callbacks run at PASSIVE_LEVEL in parallel.
	attrs.ExecutionLevel = WdfExecutionLevelPassive;

	WDFDEVICE control = nullptr;
	status = WdfDeviceCreate(&init, &attrs, &control);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("service: control device create failed 0x%08X", status);
		if (init != nullptr)
		{
			WdfDeviceInitFree(init);
		}
		return;
	}
	GetControlDeviceContext(control)->Device = ctx;

	WDF_IO_QUEUE_CONFIG queueConfig;
	// Parallel: a parked WAIT_EVENT (the service's tick source) must not stop the
	// CLI or the service from issuing GET_STATUS/SET_PREFERENCE on the same device.
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
	queueConfig.EvtIoDeviceControl = OnDeviceControl;
	status = WdfIoQueueCreate(control, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("service: queue create failed 0x%08X", status);
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
		OA2DP_LOG("service: symbolic link failed 0x%08X", status);
		WdfObjectDelete(control);
		return;
	}

	// The system does not deliver I/O to a control device before this call.
	WdfControlFinishInitializing(control);

	ctx->ControlDevice = control;
	InterlockedExchange(&ctx->ServiceActive, 1);
	OA2DP_LOG("service: control device ready at %ws", linkName);
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

	InterlockedExchange(&ctx->ServiceActive, 0);
	LatchEvents(ctx, Ioctl::kEventDeviceGone);
	UnmapRings(ctx);
	InterlockedExchange(&ctx->EncoderRegistered, 0);

	// The control device's parent is the driver object, so it can outlive this
	// FDO's context. Detach it before deleting so the cleanup callback cannot
	// dereference a freed DEVICE_CONTEXT.
	GetControlDeviceContext(control)->Device = nullptr;
	WdfObjectDelete(control);
	OA2DP_LOG("service: control device removed");
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

bool ServiceInstalled()
{
	UNICODE_STRING name;
	RtlInitUnicodeString(&name, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\OpenA2dpSvc");

	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);

	HANDLE key = nullptr;
	const NTSTATUS status = ZwOpenKey(&key, KEY_READ, &attributes);
	if (!NT_SUCCESS(status))
	{
		return false;
	}
	ZwClose(key);
	return true;
}

void ServiceStreamStart(_In_ DEVICE_CONTEXT* ctx)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->WaitLock, &irql);
	ctx->WaitState = {};
	ctx->WaitState.state = Ioctl::MediaState::Streaming;
	ctx->WaitState.codec = ctx->ActiveCodec;
	ctx->WaitState.sampleRate = ctx->ActiveSampleRate;
	ctx->WaitState.channels = ctx->ActiveChannels;
	ctx->WaitState.bitrate = ctx->ActiveBitrate;
	ctx->WaitState.objectType = ctx->ActiveObjectType;
	KeReleaseSpinLock(&ctx->WaitLock, irql);

	LatchEvents(ctx, Ioctl::kEventStreamStart);
}

void ServiceStreamStop(_In_ DEVICE_CONTEXT* ctx)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->WaitLock, &irql);
	ctx->WaitState = {};
	ctx->WaitState.state = Ioctl::MediaState::Idle;
	KeReleaseSpinLock(&ctx->WaitLock, irql);

	LatchEvents(ctx, Ioctl::kEventStreamStop);
}

void ServiceTick(_In_ DEVICE_CONTEXT* ctx)
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
		deliver = ctx->PendingEvents | Ioctl::kEventTick;
		ctx->PendingEvents = 0;
		state = ctx->WaitState;
	}
	KeReleaseSpinLock(&ctx->WaitLock, irql);

	if (request != nullptr)
	{
		CompleteWait(request, state, deliver);
	}
}

ULONG ServiceReadFrame(_In_ DEVICE_CONTEXT* ctx, _Out_ const UCHAR** payload, _Out_ uint32_t* samples,
	_Out_ uint32_t* flags)
{
	*payload = nullptr;
	*samples = 0;
	*flags = 0;
	if (ctx->FrameRingHeader == nullptr)
	{
		return 0;
	}

	RingBufferView frames(ctx->FrameRingHeader, ctx->FrameRingData);
	FrameRecord::Record record;
	const FrameRecord::ReadResult result = FrameRecord::Read(frames, ctx->FrameRecord, sizeof(ctx->FrameRecord), record);
	if (result == FrameRecord::ReadResult::Corrupt)
	{
		// Not repairable from the consumer side: stop consuming and let media stop.
		OA2DP_LOG("service: corrupt frame record");
		InterlockedExchange(&ctx->LastMediaError, STATUS_DATA_ERROR);
		InterlockedExchange(&ctx->StreamFault, 1);
		return 0;
	}
	if (result == FrameRecord::ReadResult::Dropped)
	{
		InterlockedIncrement(&ctx->FramesDropped);
		return 0;
	}
	if (result != FrameRecord::ReadResult::Record)
	{
		return 0;
	}

	*payload = record.payload;
	*samples = record.samples;
	*flags = record.flags;
	return record.payloadLength;
}

void ServiceFlushRings(_In_ DEVICE_CONTEXT* ctx)
{
	// Only the frame ring: the driver is its consumer. The PCM ring's consumer is the
	// service, which discards buffered audio itself when it sees kEventStreamStart.
	if (ctx->FrameRingHeader != nullptr)
	{
		RingBufferView frames(ctx->FrameRingHeader, ctx->FrameRingData);
		frames.Skip(frames.ReadableBytes());
	}
}
