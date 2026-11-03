#include "L2capInternal.h"

using namespace OpenA2dpDriver::L2cap;

NTSTATUS L2capInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeEvent(&ctx->OpenIdle, NotificationEvent, TRUE);
	KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
	KeInitializeEvent(&ctx->RejectIdle, NotificationEvent, TRUE);
	KeInitializeSpinLock(&ctx->PreferenceLock);
	RtlStringCchCopyA(ctx->LogTag, ARRAYSIZE(ctx->LogTag), "unidentified device");
	ctx->IoTarget = WdfDeviceGetIoTarget(ctx->Device);

	NTSTATUS status = WdfFdoQueryForInterface(ctx->Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
		reinterpret_cast<PINTERFACE>(&ctx->Profile), sizeof(ctx->Profile),
		BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, nullptr);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "L2CAP", "query profile driver interface failed 0x%08X", status);
		return status;
	}
	ctx->ProfileValid = TRUE;

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;

	status = WdfRequestCreate(&attrs, ctx->IoTarget, &ctx->OpenRequest);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = WdfMemoryCreatePreallocated(&attrs, &ctx->OpenBrb, sizeof(ctx->OpenBrb), &ctx->OpenBrbMemory);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = WdfRequestCreate(&attrs, ctx->IoTarget, &ctx->RejectRequest);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = WdfMemoryCreatePreallocated(&attrs, &ctx->RejectBrb, sizeof(ctx->RejectBrb), &ctx->RejectBrbMemory);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = InitSendSlot(ctx, &attrs, &ctx->SignalSend);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	return InitSendSlot(ctx, &attrs, &ctx->MediaSend);
}

NTSTATUS L2capRegisterServer(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->ServerHandle != nullptr)
	{
		return STATUS_SUCCESS;
	}

	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_REGISTER_SERVER);
	if (op == nullptr)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	auto& brb = op->Brb.Register;
	brb.BtAddress = ctx->RemoteAddress;
	brb.PSM = AvdtpPsm;
	brb.IndicationCallback = &OnServerIndication;
	brb.IndicationCallbackContext = ctx;
	brb.ReferenceObject = WdfDeviceWdmGetDeviceObject(ctx->Device);

	NTSTATUS status = BrbOpRun(op, BrbControlTimeoutMs, false);
	OA2DP_INFO(ctx, "L2CAP", "server register status 0x%08X", status);
	if (NT_SUCCESS(status))
	{
		ctx->ServerHandle = brb.ServerHandle;
	}
	BrbOpRelease(op);
	return status;
}

void L2capUnregisterServer(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->ServerHandle == nullptr)
	{
		return;
	}

	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_UNREGISTER_SERVER);
	if (op == nullptr)
	{
		return;
	}
	auto& brb = op->Brb.Unregister;
	brb.BtAddress = ctx->RemoteAddress;
	brb.Psm = AvdtpPsm;
	brb.ServerHandle = ctx->ServerHandle;
	NTSTATUS status = BrbOpRun(op, BrbControlTimeoutMs, false);
	OA2DP_INFO(ctx, "L2CAP", "server unregister status 0x%08X", status);
	ctx->ServerHandle = nullptr;
	BrbOpRelease(op);
}

NTSTATUS L2capRetrieveRemoteAddress(_In_ DEVICE_CONTEXT* ctx)
{
	BTH_DEVICE_INFO info = {};
	WDF_MEMORY_DESCRIPTOR out;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &info, sizeof(info));

	WDF_REQUEST_SEND_OPTIONS options;
	WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
	WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(BrbControlTimeoutMs));
	NTSTATUS status = WdfIoTargetSendInternalIoctlSynchronously(
		ctx->IoTarget, nullptr, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, nullptr, &out, &options, nullptr);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "L2CAP", "get device info failed 0x%08X", status);
		return status;
	}

	ctx->RemoteAddress = info.address;
	ctx->RemoteClass = (info.flags & BDIF_COD) != 0 ? info.classOfDevice : 0;

	ctx->RemoteName[0] = L'\0';
	if ((info.flags & BDIF_NAME) != 0)
	{
		info.name[BTH_MAX_NAME_SIZE - 1] = '\0';
		ULONG nameBytes = 0;
		if (NT_SUCCESS(RtlUTF8ToUnicodeN(ctx->RemoteName, sizeof(ctx->RemoteName) - sizeof(WCHAR), &nameBytes,
				info.name, static_cast<ULONG>(strlen(info.name)))))
		{
			ctx->RemoteName[nameBytes / sizeof(WCHAR)] = L'\0';
		}
	}

	// Log tag: "<name> (aa:bb:cc:dd:ee:ff)". The name is reduced to printable ASCII so the
	// tag can be printed at any IRQL.
	char name[BTH_MAX_NAME_SIZE + 1];
	size_t length = 0;
	for (; length < BTH_MAX_NAME_SIZE && ctx->RemoteName[length] != L'\0'; ++length)
	{
		const WCHAR c = ctx->RemoteName[length];
		name[length] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
	}
	name[length] = '\0';
	const BTH_ADDR a = info.address;
	RtlStringCchPrintfA(ctx->LogTag, ARRAYSIZE(ctx->LogTag), "%s (%02X:%02X:%02X:%02X:%02X:%02X)",
		length != 0 ? name : "unnamed", static_cast<unsigned>((a >> 40) & 0xFF), static_cast<unsigned>((a >> 32) & 0xFF),
		static_cast<unsigned>((a >> 24) & 0xFF), static_cast<unsigned>((a >> 16) & 0xFF),
		static_cast<unsigned>((a >> 8) & 0xFF), static_cast<unsigned>(a & 0xFF));

	OA2DP_INFO(ctx, "Device", "identified: class of device 0x%06X", ctx->RemoteClass);
	return STATUS_SUCCESS;
}
