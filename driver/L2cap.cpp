#include "Driver.h"

namespace
{

NTSTATUS SubmitBrbSynchronously(_In_ DEVICE_CONTEXT* ctx, _Inout_ PBRB brb, _In_ ULONG size)
{
	WDF_MEMORY_DESCRIPTOR desc;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, brb, size);
	return WdfIoTargetSendInternalIoctlOthersSynchronously(
		ctx->IoTarget, nullptr, IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, nullptr, nullptr, nullptr, nullptr);
}

void OnIndication(_In_opt_ PVOID context, _In_ INDICATION_CODE indication, _In_ PINDICATION_PARAMETERS parameters)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	if (ctx == nullptr)
	{
		return;
	}

	if (indication == IndicationRemoteDisconnect)
	{
		OA2DP_LOG("signaling channel remote disconnect, reason %d", static_cast<int>(parameters->Parameters.Disconnect.Reason));
		WdfWorkItemEnqueue(ctx->DisconnectWorkItem);
	}
}

void OnOpenComplete(
	_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	NTSTATUS status = params->IoStatus.Status;

	if (NT_SUCCESS(status))
	{
		ctx->SignalingChannel = ctx->OpenBrb.ChannelHandle;
		ctx->SignalingOutMtu = ctx->OpenBrb.OutResults.Params.Mtu;
		InterlockedExchange(&ctx->SignalingState, ChannelOpen);
		OA2DP_LOG("signaling channel open, out MTU %u", ctx->SignalingOutMtu);
	}
	else
	{
		InterlockedExchange(&ctx->SignalingState, ChannelClosed);
		OA2DP_LOG("signaling channel open failed 0x%08X (L2CAP response %u/%u)",
			status, ctx->OpenBrb.Response, ctx->OpenBrb.ResponseStatus);
	}

	KeSetEvent(&ctx->OpenIdle, IO_NO_INCREMENT, FALSE);
}

} // namespace

NTSTATUS L2capInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeEvent(&ctx->OpenIdle, NotificationEvent, TRUE);
	ctx->IoTarget = WdfDeviceGetIoTarget(ctx->Device);

	NTSTATUS status = WdfFdoQueryForInterface(ctx->Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
		reinterpret_cast<PINTERFACE>(&ctx->Profile), sizeof(ctx->Profile),
		BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, nullptr);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("query profile driver interface failed 0x%08X", status);
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

	return WdfMemoryCreatePreallocated(&attrs, &ctx->OpenBrb, sizeof(ctx->OpenBrb), &ctx->OpenBrbMemory);
}

NTSTATUS L2capRetrieveRemoteAddress(_In_ DEVICE_CONTEXT* ctx)
{
	BTH_DEVICE_INFO info = {};
	WDF_MEMORY_DESCRIPTOR out;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &info, sizeof(info));

	NTSTATUS status = WdfIoTargetSendInternalIoctlSynchronously(
		ctx->IoTarget, nullptr, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, nullptr, &out, nullptr, nullptr);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("get device info failed 0x%08X", status);
		return status;
	}

	ctx->RemoteAddress = info.address;
	OA2DP_LOG("remote device %012llX", info.address);
	return STATUS_SUCCESS;
}

NTSTATUS L2capOpenSignalingChannel(_In_ DEVICE_CONTEXT* ctx)
{
	if (InterlockedCompareExchange(&ctx->SignalingState, ChannelOpening, ChannelClosed) != ChannelClosed)
	{
		return STATUS_SUCCESS;
	}

	KeClearEvent(&ctx->OpenIdle);

	WDF_REQUEST_REUSE_PARAMS reuse;
	WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
	NTSTATUS status = WdfRequestReuse(ctx->OpenRequest, &reuse);

	if (NT_SUCCESS(status))
	{
		auto& brb = ctx->OpenBrb;
		ctx->Profile.BthReuseBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_OPEN_CHANNEL);
		brb.BtAddress = ctx->RemoteAddress;
		brb.Psm = AvdtpPsm;
		brb.ChannelFlags = CF_ROLE_EITHER;
		brb.ConfigOut.Flags = CFG_MTU;
		brb.ConfigOut.Mtu.Max = L2CAP_DEFAULT_MTU;
		brb.ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
		brb.ConfigOut.Mtu.Preferred = L2CAP_DEFAULT_MTU;
		brb.ConfigIn.Flags = CFG_MTU;
		brb.ConfigIn.Mtu.Max = L2CAP_DEFAULT_MTU;
		brb.ConfigIn.Mtu.Min = L2CAP_MIN_MTU;
		brb.ConfigIn.Mtu.Preferred = L2CAP_DEFAULT_MTU;
		brb.CallbackFlags = CALLBACK_DISCONNECT;
		brb.Callback = &OnIndication;
		brb.CallbackContext = ctx;
		brb.ReferenceObject = WdfDeviceWdmGetDeviceObject(ctx->Device);
		brb.IncomingQueueDepth = 50;

		status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, ctx->OpenRequest,
			IOCTL_INTERNAL_BTH_SUBMIT_BRB, ctx->OpenBrbMemory, nullptr, nullptr, nullptr, nullptr, nullptr);
	}

	if (NT_SUCCESS(status))
	{
		WdfRequestSetCompletionRoutine(ctx->OpenRequest, OnOpenComplete, ctx);
		if (WdfRequestSend(ctx->OpenRequest, ctx->IoTarget, WDF_NO_SEND_OPTIONS))
		{
			return STATUS_SUCCESS;
		}
		status = WdfRequestGetStatus(ctx->OpenRequest);
	}

	OA2DP_LOG("signaling channel open submit failed 0x%08X", status);
	InterlockedExchange(&ctx->SignalingState, ChannelClosed);
	KeSetEvent(&ctx->OpenIdle, IO_NO_INCREMENT, FALSE);
	return status;
}

void L2capCloseSignalingChannel(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->SignalingState == ChannelOpening)
	{
		WdfRequestCancelSentRequest(ctx->OpenRequest);
	}
	KeWaitForSingleObject(&ctx->OpenIdle, Executive, KernelMode, FALSE, nullptr);

	if (InterlockedCompareExchange(&ctx->SignalingState, ChannelClosing, ChannelOpen) != ChannelOpen)
	{
		return;
	}

	struct _BRB_L2CA_CLOSE_CHANNEL brb;
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_CLOSE_CHANNEL);
	brb.BtAddress = ctx->RemoteAddress;
	brb.ChannelHandle = ctx->SignalingChannel;

	NTSTATUS status = SubmitBrbSynchronously(ctx, reinterpret_cast<PBRB>(&brb), sizeof(brb));
	OA2DP_LOG("signaling channel closed 0x%08X", status);

	ctx->SignalingChannel = nullptr;
	InterlockedExchange(&ctx->SignalingState, ChannelClosed);
}
