#include "Driver.h"

namespace
{

NTSTATUS SubmitBrbSynchronously(_In_ DEVICE_CONTEXT* ctx, _Inout_ PBRB brb, _In_ ULONG size)
{
	WDF_MEMORY_DESCRIPTOR desc;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, brb, size);

	WDF_REQUEST_SEND_OPTIONS options;
	WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
	WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(5000));

	return WdfIoTargetSendInternalIoctlOthersSynchronously(
		ctx->IoTarget, nullptr, IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, nullptr, nullptr, &options, nullptr);
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
		SignalingOnChannelOpen(ctx);
	}
	else
	{
		InterlockedExchange(&ctx->SignalingState, ChannelClosed);
		OA2DP_LOG("signaling channel open failed 0x%08X (L2CAP response %u/%u)",
			status, ctx->OpenBrb.Response, ctx->OpenBrb.ResponseStatus);
	}

	KeSetEvent(&ctx->OpenIdle, IO_NO_INCREMENT, FALSE);
}

void InitSignalingConfig(_In_ DEVICE_CONTEXT* ctx, _Inout_ struct _BRB_L2CA_OPEN_CHANNEL& brb)
{
	brb.BtAddress = ctx->RemoteAddress;
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
}

// Sends the preallocated open (or open-response) BRB asynchronously.
NTSTATUS SendOpenRequest(_In_ DEVICE_CONTEXT* ctx)
{
	NTSTATUS status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, ctx->OpenRequest,
		IOCTL_INTERNAL_BTH_SUBMIT_BRB, ctx->OpenBrbMemory, nullptr, nullptr, nullptr, nullptr, nullptr);
	if (NT_SUCCESS(status))
	{
		WdfRequestSetCompletionRoutine(ctx->OpenRequest, OnOpenComplete, ctx);
		if (WdfRequestSend(ctx->OpenRequest, ctx->IoTarget, WDF_NO_SEND_OPTIONS))
		{
			return STATUS_SUCCESS;
		}
		status = WdfRequestGetStatus(ctx->OpenRequest);
	}

	InterlockedExchange(&ctx->SignalingState, ChannelClosed);
	KeSetEvent(&ctx->OpenIdle, IO_NO_INCREMENT, FALSE);
	return status;
}

void OnRejectComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	if (InterlockedExchange(&ctx->MediaAcceptPending, 0) != 0)
	{
		if (NT_SUCCESS(params->IoStatus.Status))
		{
			ctx->MediaOutMtu = ctx->RejectBrb.OutResults.Params.Mtu;
			ctx->MediaChannel = ctx->RejectBrb.ChannelHandle;
			OA2DP_LOG("incoming media channel open, out MTU %u", ctx->MediaOutMtu);
			InterlockedExchange(&ctx->RejectBusy, 0);
			SignalingOnMediaChannelOpen(ctx);
			return;
		}
		OA2DP_LOG("incoming media accept failed 0x%08X", params->IoStatus.Status);
		InterlockedExchange(&ctx->MediaAcceptAllowed, 1);
	}
	else
	{
		OA2DP_LOG("incoming connection refused 0x%08X", params->IoStatus.Status);
	}
	InterlockedExchange(&ctx->RejectBusy, 0);
}

// Answers an incoming connection through the preallocated RejectRequest:
// refuses it, or (accept == true) accepts it as the media transport channel.
void RespondIncoming(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE handle, _In_ BTH_ADDR address, _In_ bool accept)
{
	if (InterlockedCompareExchange(&ctx->RejectBusy, 1, 0) != 0)
	{
		OA2DP_LOG("incoming connection dropped (response in flight)");
		return;
	}

	WDF_REQUEST_REUSE_PARAMS reuse;
	WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
	NTSTATUS status = WdfRequestReuse(ctx->RejectRequest, &reuse);
	if (NT_SUCCESS(status))
	{
		auto& brb = ctx->RejectBrb;
		ctx->Profile.BthReuseBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_OPEN_CHANNEL_RESPONSE);
		if (accept)
		{
			InitSignalingConfig(ctx, brb);
			brb.ConfigOut.Mtu.Max = MediaMaxMtu;
			brb.ConfigOut.Mtu.Preferred = MediaMaxMtu;
			brb.IncomingQueueDepth = 10;
			InterlockedExchange(&ctx->MediaAcceptPending, 1);
		}
		brb.BtAddress = address;
		brb.ChannelHandle = handle;
		brb.Response = accept ? CONNECT_RSP_RESULT_SUCCESS : CONNECT_RSP_RESULT_NO_RESOURCES;
		status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, ctx->RejectRequest,
			IOCTL_INTERNAL_BTH_SUBMIT_BRB, ctx->RejectBrbMemory, nullptr, nullptr, nullptr, nullptr, nullptr);
	}
	if (NT_SUCCESS(status))
	{
		WdfRequestSetCompletionRoutine(ctx->RejectRequest, OnRejectComplete, ctx);
		if (WdfRequestSend(ctx->RejectRequest, ctx->IoTarget, WDF_NO_SEND_OPTIONS))
		{
			return;
		}
		status = WdfRequestGetStatus(ctx->RejectRequest);
	}
	OA2DP_LOG("incoming connection response failed 0x%08X", status);
	if (InterlockedExchange(&ctx->MediaAcceptPending, 0) != 0)
	{
		InterlockedExchange(&ctx->MediaAcceptAllowed, 1);
	}
	InterlockedExchange(&ctx->RejectBusy, 0);
}

// Server indication: the headset opened an L2CAP channel on the AVDTP PSM.
// The first channel becomes the signaling channel. When the headset configured
// and opened the stream itself, the next channel is the media transport;
// anything else is refused.
void OnServerIndication(_In_opt_ PVOID context, _In_ INDICATION_CODE indication, _In_ PINDICATION_PARAMETERS parameters)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	if (ctx == nullptr || indication != IndicationRemoteConnect)
	{
		return;
	}

	if (parameters->BtAddress != ctx->RemoteAddress || parameters->Parameters.Connect.Request.PSM != AvdtpPsm)
	{
		RespondIncoming(ctx, parameters->ConnectionHandle, parameters->BtAddress, false);
		return;
	}

	if (ctx->SignalingState == ChannelOpen && InterlockedCompareExchange(&ctx->MediaAcceptAllowed, 0, 1) == 1)
	{
		OA2DP_LOG("incoming media connection");
		RespondIncoming(ctx, parameters->ConnectionHandle, parameters->BtAddress, true);
		return;
	}

	if (InterlockedCompareExchange(&ctx->SignalingState, ChannelOpening, ChannelClosed) != ChannelClosed)
	{
		RespondIncoming(ctx, parameters->ConnectionHandle, parameters->BtAddress, false);
		return;
	}

	OA2DP_LOG("incoming signaling connection");
	InterlockedExchange(&ctx->SignalingIncoming, 1);
	KeClearEvent(&ctx->OpenIdle);

	WDF_REQUEST_REUSE_PARAMS reuse;
	WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
	NTSTATUS status = WdfRequestReuse(ctx->OpenRequest, &reuse);
	if (NT_SUCCESS(status))
	{
		auto& brb = ctx->OpenBrb;
		ctx->Profile.BthReuseBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_OPEN_CHANNEL_RESPONSE);
		InitSignalingConfig(ctx, brb);
		brb.ChannelHandle = parameters->ConnectionHandle;
		brb.Response = CONNECT_RSP_RESULT_SUCCESS;
		status = SendOpenRequest(ctx);
	}
	else
	{
		InterlockedExchange(&ctx->SignalingState, ChannelClosed);
		KeSetEvent(&ctx->OpenIdle, IO_NO_INCREMENT, FALSE);
	}

	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("incoming signaling accept failed 0x%08X", status);
	}
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

	return WdfMemoryCreatePreallocated(&attrs, &ctx->RejectBrb, sizeof(ctx->RejectBrb), &ctx->RejectBrbMemory);
}

NTSTATUS L2capRegisterServer(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->ServerHandle != nullptr)
	{
		return STATUS_SUCCESS;
	}

	struct _BRB_L2CA_REGISTER_SERVER brb;
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_REGISTER_SERVER);
	brb.BtAddress = ctx->RemoteAddress;
	brb.PSM = AvdtpPsm;
	brb.IndicationCallback = &OnServerIndication;
	brb.IndicationCallbackContext = ctx;
	brb.ReferenceObject = WdfDeviceWdmGetDeviceObject(ctx->Device);

	NTSTATUS status = SubmitBrbSynchronously(ctx, reinterpret_cast<PBRB>(&brb), sizeof(brb));
	OA2DP_LOG("L2CAP server register 0x%08X", status);
	if (NT_SUCCESS(status))
	{
		ctx->ServerHandle = brb.ServerHandle;
	}
	return status;
}

void L2capUnregisterServer(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->ServerHandle == nullptr)
	{
		return;
	}

	struct _BRB_L2CA_UNREGISTER_SERVER brb;
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_UNREGISTER_SERVER);
	brb.BtAddress = ctx->RemoteAddress;
	brb.Psm = AvdtpPsm;
	brb.ServerHandle = ctx->ServerHandle;
	NTSTATUS status = SubmitBrbSynchronously(ctx, reinterpret_cast<PBRB>(&brb), sizeof(brb));
	OA2DP_LOG("L2CAP server unregister 0x%08X", status);
	ctx->ServerHandle = nullptr;
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
	ctx->RemoteClass = (info.flags & BDIF_COD) != 0 ? info.classOfDevice : 0;
	OA2DP_LOG("remote device %012llX, CoD 0x%06X", info.address, ctx->RemoteClass);

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
	return STATUS_SUCCESS;
}

NTSTATUS L2capOpenSignalingChannel(_In_ DEVICE_CONTEXT* ctx)
{
	if (InterlockedCompareExchange(&ctx->SignalingState, ChannelOpening, ChannelClosed) != ChannelClosed)
	{
		return STATUS_SUCCESS;
	}

	KeClearEvent(&ctx->OpenIdle);
	InterlockedExchange(&ctx->SignalingIncoming, 0);

	WDF_REQUEST_REUSE_PARAMS reuse;
	WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
	NTSTATUS status = WdfRequestReuse(ctx->OpenRequest, &reuse);

	if (NT_SUCCESS(status))
	{
		auto& brb = ctx->OpenBrb;
		ctx->Profile.BthReuseBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_OPEN_CHANNEL);
		InitSignalingConfig(ctx, brb);
		brb.Psm = AvdtpPsm;
		status = SendOpenRequest(ctx);
	}
	else
	{
		InterlockedExchange(&ctx->SignalingState, ChannelClosed);
		KeSetEvent(&ctx->OpenIdle, IO_NO_INCREMENT, FALSE);
	}

	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("signaling channel open submit failed 0x%08X", status);
	}
	return status;
}

void L2capCloseSignalingChannel(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->SignalingState == ChannelOpening)
	{
		WdfRequestCancelSentRequest(ctx->OpenRequest);
	}
	LARGE_INTEGER openTimeout;
	openTimeout.QuadPart = -10000000LL;
	if (KeWaitForSingleObject(&ctx->OpenIdle, Executive, KernelMode, FALSE, &openTimeout) != STATUS_SUCCESS)
	{
		OA2DP_LOG("close signaling: open did not complete within 1 s");
	}
	if (InterlockedCompareExchange(&ctx->SignalingState, ChannelClosing, ChannelOpen) != ChannelOpen)
	{
		return;
	}

	SignalingStop(ctx);
	L2capCloseMediaChannel(ctx);

	struct _BRB_L2CA_CLOSE_CHANNEL brb;
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_CLOSE_CHANNEL);
	brb.BtAddress = ctx->RemoteAddress;
	brb.ChannelHandle = ctx->SignalingChannel;

	NTSTATUS status = SubmitBrbSynchronously(ctx, reinterpret_cast<PBRB>(&brb), sizeof(brb));
	OA2DP_LOG("signaling channel closed 0x%08X", status);

	ctx->SignalingChannel = nullptr;
	InterlockedExchange(&ctx->SignalingState, ChannelClosed);
	SignalingResetSession(ctx);
	AudioNotifyLinkState(ctx);
}

NTSTATUS L2capSend(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE channel, _In_reads_bytes_(length) PVOID data, _In_ ULONG length)
{
	struct _BRB_L2CA_ACL_TRANSFER brb;
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_ACL_TRANSFER);
	brb.BtAddress = ctx->RemoteAddress;
	brb.ChannelHandle = channel;
	brb.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
	brb.BufferSize = length;
	brb.Buffer = data;

	// A stalled transfer must not block teardown forever (it holds MediaLock/SessionLock).
	WDF_REQUEST_SEND_OPTIONS options;
	WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
	WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(1000));
	WDF_MEMORY_DESCRIPTOR desc;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, &brb, sizeof(brb));
	return WdfIoTargetSendInternalIoctlOthersSynchronously(
		ctx->IoTarget, nullptr, IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, nullptr, nullptr, &options, nullptr);
}

bool IsLinkLoss(_In_ DEVICE_CONTEXT* ctx, _In_ NTSTATUS status)
{
	switch (status)
	{
	case STATUS_DEVICE_OFF_LINE:
	case STATUS_DEVICE_NOT_CONNECTED:
	case STATUS_CONNECTION_DISCONNECTED:
	case STATUS_CONNECTION_RESET:
	case STATUS_CANCELLED:
		return true;
	default:
		break;
	}
	// Any failure while the signaling link is closing or closed is part of the teardown.
	return InterlockedCompareExchange(&ctx->SignalingState, ChannelOpen, ChannelOpen) != ChannelOpen;
}

NTSTATUS L2capOpenMediaChannel(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->MediaChannel != nullptr)
	{
		return STATUS_SUCCESS;
	}

	struct _BRB_L2CA_OPEN_CHANNEL brb;
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_OPEN_CHANNEL);
	brb.BtAddress = ctx->RemoteAddress;
	brb.Psm = AvdtpPsm;
	brb.ChannelFlags = CF_ROLE_EITHER;
	brb.ConfigOut.Flags = CFG_MTU;
	brb.ConfigOut.Mtu.Max = MediaMaxMtu;
	brb.ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
	brb.ConfigOut.Mtu.Preferred = MediaMaxMtu;
	brb.ConfigIn.Flags = CFG_MTU;
	brb.ConfigIn.Mtu.Max = L2CAP_DEFAULT_MTU;
	brb.ConfigIn.Mtu.Min = L2CAP_MIN_MTU;
	brb.ConfigIn.Mtu.Preferred = L2CAP_DEFAULT_MTU;
	brb.CallbackFlags = CALLBACK_DISCONNECT;
	brb.Callback = &OnIndication;
	brb.CallbackContext = ctx;
	brb.ReferenceObject = WdfDeviceWdmGetDeviceObject(ctx->Device);
	brb.IncomingQueueDepth = 10;

	NTSTATUS status = SubmitBrbSynchronously(ctx, reinterpret_cast<PBRB>(&brb), sizeof(brb));
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("media channel open failed 0x%08X (L2CAP response %u/%u)", status, brb.Response, brb.ResponseStatus);
		return status;
	}

	ctx->MediaChannel = brb.ChannelHandle;
	ctx->MediaOutMtu = brb.OutResults.Params.Mtu;
	OA2DP_LOG("media channel open, out MTU %u", ctx->MediaOutMtu);
	return STATUS_SUCCESS;
}

void L2capCloseMediaChannel(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->MediaAcceptAllowed, 0);
	MediaStop(ctx);
	if (ctx->MediaChannel == nullptr)
	{
		return;
	}

	struct _BRB_L2CA_CLOSE_CHANNEL brb;
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_CLOSE_CHANNEL);
	brb.BtAddress = ctx->RemoteAddress;
	brb.ChannelHandle = ctx->MediaChannel;
	NTSTATUS status = SubmitBrbSynchronously(ctx, reinterpret_cast<PBRB>(&brb), sizeof(brb));
	OA2DP_LOG("media channel closed 0x%08X", status);
	ctx->MediaChannel = nullptr;
}
