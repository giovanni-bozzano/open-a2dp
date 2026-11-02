#include "Driver.h"

namespace
{

// A control BRB (register/unregister server, open/close channel) in flight. The BRB
// lives here rather than on the caller's stack so a caller that stops waiting can
// return while the transport still owns it. Two references: the caller's and the
// completion routine's; the last one to let go frees the operation.
struct BRB_OP
{
	DEVICE_CONTEXT* Ctx;
	WDFREQUEST Request;
	WDFMEMORY Memory;
	KEVENT Done;
	volatile LONG State; // 0 running, 1 completed, 2 abandoned by its waiter
	volatile LONG Refs;
	BOOLEAN IsOpen;
	BOOLEAN IsRegister;
	NTSTATUS Status;
	union
	{
		struct _BRB_L2CA_OPEN_CHANNEL Open;
		struct _BRB_L2CA_CLOSE_CHANNEL Close;
		struct _BRB_L2CA_REGISTER_SERVER Register;
		struct _BRB_L2CA_UNREGISTER_SERVER Unregister;
	} Brb;
};

void BrbOpRelease(_In_ BRB_OP* op)
{
	if (InterlockedDecrement(&op->Refs) == 0)
	{
		WdfObjectDelete(op->Request); // also deletes the memory object parented to it
		ExFreePoolWithTag(op, OpenA2dpPoolTag);
	}
}

BRB_OP* BrbOpCreate(_In_ DEVICE_CONTEXT* ctx, _In_ BRB_TYPE type)
{
	auto* op = static_cast<BRB_OP*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(BRB_OP), OpenA2dpPoolTag));
	if (op == nullptr)
	{
		return nullptr;
	}
	op->Ctx = ctx;
	op->Refs = 2;
	op->IsOpen = type == BRB_L2CA_OPEN_CHANNEL;
	op->IsRegister = type == BRB_L2CA_REGISTER_SERVER;
	KeInitializeEvent(&op->Done, NotificationEvent, FALSE);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;
	if (!NT_SUCCESS(WdfRequestCreate(&attrs, ctx->IoTarget, &op->Request)))
	{
		ExFreePoolWithTag(op, OpenA2dpPoolTag);
		return nullptr;
	}
	attrs.ParentObject = op->Request;
	if (!NT_SUCCESS(WdfMemoryCreatePreallocated(&attrs, &op->Brb, sizeof(op->Brb), &op->Memory)))
	{
		WdfObjectDelete(op->Request);
		ExFreePoolWithTag(op, OpenA2dpPoolTag);
		return nullptr;
	}
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&op->Brb), type);
	return op;
}

void CloseOrphanedChannel(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE channel);
void UnregisterOrphanedServer(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_SERVER_HANDLE server);

void OnBrbOpComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context)
{
	auto* op = static_cast<BRB_OP*>(context);
	op->Status = params->IoStatus.Status;
	if (InterlockedCompareExchange(&op->State, 1, 0) == 2 && NT_SUCCESS(op->Status))
	{
		// The waiter gave up and nobody will ever use this result; do not leak it.
		if (op->IsOpen)
		{
			CloseOrphanedChannel(op->Ctx, op->Brb.Open.ChannelHandle);
		}
		else if (op->IsRegister)
		{
			UnregisterOrphanedServer(op->Ctx, op->Brb.Register.ServerHandle);
		}
	}
	KeSetEvent(&op->Done, IO_NO_INCREMENT, FALSE);
	BrbOpRelease(op);
}

// Sends the operation. On failure the completion routine will not run, so the
// completion reference is dropped here.
NTSTATUS BrbOpStart(_In_ BRB_OP* op)
{
	DEVICE_CONTEXT* ctx = op->Ctx;
	NTSTATUS status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, op->Request,
		IOCTL_INTERNAL_BTH_SUBMIT_BRB, op->Memory, nullptr, nullptr, nullptr, nullptr, nullptr);
	if (NT_SUCCESS(status))
	{
		WdfRequestSetCompletionRoutine(op->Request, OnBrbOpComplete, op);
		if (WdfRequestSend(op->Request, ctx->IoTarget, WDF_NO_SEND_OPTIONS))
		{
			return STATUS_SUCCESS;
		}
		status = WdfRequestGetStatus(op->Request);
	}
	op->Status = status;
	InterlockedExchange(&op->State, 1);
	KeSetEvent(&op->Done, IO_NO_INCREMENT, FALSE);
	BrbOpRelease(op);
	return status;
}

void CloseOrphanedChannel(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE channel)
{
	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_CLOSE_CHANNEL);
	if (op == nullptr)
	{
		return;
	}
	op->Brb.Close.BtAddress = ctx->RemoteAddress;
	op->Brb.Close.ChannelHandle = channel;
	op->State = 2; // nobody waits for this one
	(void)BrbOpStart(op);
	BrbOpRelease(op);
	OA2DP_LOG("closed a channel whose open finished after its waiter gave up");
}

void UnregisterOrphanedServer(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_SERVER_HANDLE server)
{
	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_UNREGISTER_SERVER);
	if (op == nullptr)
	{
		return;
	}
	op->Brb.Unregister.BtAddress = ctx->RemoteAddress;
	op->Brb.Unregister.Psm = AvdtpPsm;
	op->Brb.Unregister.ServerHandle = server;
	op->State = 2;
	(void)BrbOpStart(op);
	BrbOpRelease(op);
	OA2DP_LOG("unregistered a server whose registration finished after its waiter gave up");
}

// Runs a prepared BRB operation and waits at most timeoutMs. With abortable the wait
// also ends when teardown begins (StopEvent). When the wait ends early the request is
// cancelled and, if the transport still does not complete it, abandoned: the operation
// outlives this call and the completion routine cleans up. The caller still owns its
// reference and must call BrbOpRelease. Results in op->Brb are valid only when the
// returned status is a success.
NTSTATUS BrbOpRun(_In_ BRB_OP* op, _In_ ULONG timeoutMs, _In_ bool abortable)
{
	DEVICE_CONTEXT* ctx = op->Ctx;
	NTSTATUS status = BrbOpStart(op);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	LARGE_INTEGER timeout;
	timeout.QuadPart = -static_cast<LONGLONG>(timeoutMs) * 10000;
	NTSTATUS wait;
	if (abortable)
	{
		PVOID objects[2] = {&op->Done, &ctx->StopEvent};
		wait = KeWaitForMultipleObjects(2, objects, WaitAny, Executive, KernelMode, FALSE, &timeout, nullptr);
	}
	else
	{
		wait = KeWaitForSingleObject(&op->Done, Executive, KernelMode, FALSE, &timeout);
	}
	if (wait == STATUS_WAIT_0)
	{
		return op->Status;
	}

	WdfRequestCancelSentRequest(op->Request);
	timeout.QuadPart = -1000000LL; // 100 ms for a well-behaved transport to complete the cancel
	if (KeWaitForSingleObject(&op->Done, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS)
	{
		return op->Status;
	}
	if (InterlockedCompareExchange(&op->State, 2, 0) == 1)
	{
		return op->Status; // completed in the instant between the wait and the abandon
	}
	OA2DP_LOG("BRB did not complete (%s); abandoning it", wait == STATUS_TIMEOUT ? "timeout" : "teardown");
	return wait == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : STATUS_CANCELLED;
}

void OnSendComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context)
{
	auto* slot = static_cast<SEND_SLOT*>(context);
	InterlockedExchange(&slot->Status, params->IoStatus.Status);
	KeSetEvent(&slot->Idle, IO_NO_INCREMENT, FALSE);
}

NTSTATUS InitSendSlot(_In_ DEVICE_CONTEXT* ctx, _In_ WDF_OBJECT_ATTRIBUTES* attrs, _Inout_ SEND_SLOT* slot)
{
	KeInitializeEvent(&slot->Idle, NotificationEvent, TRUE);
	NTSTATUS status = WdfRequestCreate(attrs, ctx->IoTarget, &slot->Request);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	return WdfMemoryCreatePreallocated(attrs, &slot->Brb, sizeof(slot->Brb), &slot->BrbMemory);
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

	if (NT_SUCCESS(status) && KeReadStateEvent(&ctx->StopEvent) != 0)
	{
		// Teardown gave up waiting for this open; nobody will use the channel.
		InterlockedExchange(&ctx->SignalingState, ChannelClosed);
		CloseOrphanedChannel(ctx, ctx->OpenBrb.ChannelHandle);
	}
	else if (NT_SUCCESS(status))
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
	const bool accepted = ctx->RejectBrb.Response == CONNECT_RSP_RESULT_SUCCESS;
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
		if (ctx->SignalingState == ChannelOpen)
		{
			InterlockedExchange(&ctx->MediaAcceptAllowed, 1);
		}
	}
	else if (accepted && NT_SUCCESS(params->IoStatus.Status))
	{
		// The media channel was closed (teardown) while this accept was in flight.
		CloseOrphanedChannel(ctx, ctx->RejectBrb.ChannelHandle);
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

	if (KeReadStateEvent(&ctx->StopEvent) != 0)
	{
		RespondIncoming(ctx, parameters->ConnectionHandle, parameters->BtAddress, false);
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
		OA2DP_LOG("incoming signaling connection refused: signaling channel already in state %ld",
			static_cast<long>(ctx->SignalingState));
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
	KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
	KeInitializeSpinLock(&ctx->PreferenceLock);
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
	OA2DP_LOG("L2CAP server register 0x%08X", status);
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
	OA2DP_LOG("L2CAP server unregister 0x%08X", status);
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
	openTimeout.QuadPart = -static_cast<LONGLONG>(BrbControlTimeoutMs) * 10000;
	if (KeWaitForSingleObject(&ctx->OpenIdle, Executive, KernelMode, FALSE, &openTimeout) != STATUS_SUCCESS)
	{
		OA2DP_LOG("close signaling: open did not complete within %u ms", BrbControlTimeoutMs);
	}
	if (InterlockedCompareExchange(&ctx->SignalingState, ChannelClosing, ChannelOpen) != ChannelOpen)
	{
		return;
	}

	SignalingStop(ctx);
	// Barrier: the signaling work item may have been queued again after SignalingStop
	// flushed it (a late accept, a media fault). It checks SignalingState under
	// SessionLock before every send and before starting media, so once this thread has
	// passed through the lock no send can start on the channels closed below.
	if (ctx->SignalingWorkItemThread != KeGetCurrentThread())
	{
		WdfWaitLockAcquire(ctx->SessionLock, nullptr);
		WdfWaitLockRelease(ctx->SessionLock);
	}
	L2capCloseMediaChannel(ctx);
	L2capCancelSend(&ctx->SignalSend);

	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_CLOSE_CHANNEL);
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
	if (op != nullptr)
	{
		op->Brb.Close.BtAddress = ctx->RemoteAddress;
		op->Brb.Close.ChannelHandle = ctx->SignalingChannel;
		status = BrbOpRun(op, BrbControlTimeoutMs, false);
		BrbOpRelease(op);
	}
	OA2DP_LOG("signaling channel closed 0x%08X", status);

	ctx->SignalingChannel = nullptr;
	InterlockedExchange(&ctx->SignalingState, ChannelClosed);
	SignalingResetSession(ctx);
	AudioNotifyLinkState(ctx);
}

bool L2capSendIdle(_In_ SEND_SLOT* slot)
{
	return KeReadStateEvent(&slot->Idle) != 0;
}

void L2capCancelSend(_In_ SEND_SLOT* slot)
{
	if (!L2capSendIdle(slot))
	{
		WdfRequestCancelSentRequest(slot->Request);
	}
}

NTSTATUS L2capSend(_In_ DEVICE_CONTEXT* ctx, _In_ SEND_SLOT* slot, _In_ L2CAP_CHANNEL_HANDLE channel,
	_In_reads_bytes_(length) const VOID* data, _In_ ULONG length, _In_ ULONG timeoutMs)
{
	if (length > sizeof(slot->Buffer))
	{
		return STATUS_BUFFER_OVERFLOW;
	}
	if (!L2capSendIdle(slot))
	{
		return STATUS_DEVICE_BUSY;
	}
	KeClearEvent(&slot->Idle);
	RtlCopyMemory(slot->Buffer, data, length);

	WDF_REQUEST_REUSE_PARAMS reuse;
	WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
	NTSTATUS status = WdfRequestReuse(slot->Request, &reuse);
	if (NT_SUCCESS(status))
	{
		auto& brb = slot->Brb;
		ctx->Profile.BthReuseBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_ACL_TRANSFER);
		brb.BtAddress = ctx->RemoteAddress;
		brb.ChannelHandle = channel;
		brb.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
		brb.BufferSize = length;
		brb.Buffer = slot->Buffer;
		status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, slot->Request,
			IOCTL_INTERNAL_BTH_SUBMIT_BRB, slot->BrbMemory, nullptr, nullptr, nullptr, nullptr, nullptr);
	}
	if (NT_SUCCESS(status))
	{
		WdfRequestSetCompletionRoutine(slot->Request, OnSendComplete, slot);
		if (!WdfRequestSend(slot->Request, ctx->IoTarget, WDF_NO_SEND_OPTIONS))
		{
			status = WdfRequestGetStatus(slot->Request);
		}
	}
	if (!NT_SUCCESS(status))
	{
		KeSetEvent(&slot->Idle, IO_NO_INCREMENT, FALSE);
		return status;
	}

	LARGE_INTEGER timeout;
	timeout.QuadPart = -static_cast<LONGLONG>(timeoutMs) * 10000;
	if (KeWaitForSingleObject(&slot->Idle, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS)
	{
		return static_cast<NTSTATUS>(slot->Status);
	}

	// The transport did not finish the send in time. Cancel it and leave it in flight if
	// it still does not complete: the slot keeps the request, BRB and data alive, and
	// stays busy until the completion arrives.
	WdfRequestCancelSentRequest(slot->Request);
	timeout.QuadPart = -1000000LL;
	if (KeWaitForSingleObject(&slot->Idle, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS)
	{
		return static_cast<NTSTATUS>(slot->Status);
	}
	OA2DP_LOG("send did not complete within %u ms; it stays pending in the transport", timeoutMs);
	return STATUS_IO_TIMEOUT;
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

	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_OPEN_CHANNEL);
	if (op == nullptr)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	auto& brb = op->Brb.Open;
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

	// Abortable: this runs in the signaling work item, which teardown has to wait for.
	NTSTATUS status = BrbOpRun(op, BrbControlTimeoutMs, true);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("media channel open failed 0x%08X (L2CAP response %u/%u)", status, brb.Response, brb.ResponseStatus);
		BrbOpRelease(op);
		return status;
	}

	ctx->MediaChannel = brb.ChannelHandle;
	ctx->MediaOutMtu = brb.OutResults.Params.Mtu;
	OA2DP_LOG("media channel open, out MTU %u", ctx->MediaOutMtu);
	BrbOpRelease(op);
	return STATUS_SUCCESS;
}

void L2capCloseMediaChannel(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->MediaAcceptAllowed, 0);
	MediaStop(ctx);
	// An accept still in flight would otherwise set MediaChannel after the close below.
	// Revoke it (a late success then closes the channel itself) and give the completion
	// a bounded moment so it is not halfway through adopting the channel.
	InterlockedExchange(&ctx->MediaAcceptPending, 0);
	for (int i = 0; i < 100 && InterlockedCompareExchange(&ctx->RejectBusy, 0, 0) != 0; ++i)
	{
		LARGE_INTEGER delay;
		delay.QuadPart = -100000LL; // 10 ms
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
	}
	if (ctx->MediaChannel == nullptr)
	{
		return;
	}

	// A send the transport still holds would keep the channel referenced; cancel it first.
	L2capCancelSend(&ctx->MediaSend);

	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_CLOSE_CHANNEL);
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
	if (op != nullptr)
	{
		op->Brb.Close.BtAddress = ctx->RemoteAddress;
		op->Brb.Close.ChannelHandle = ctx->MediaChannel;
		status = BrbOpRun(op, BrbControlTimeoutMs, false);
		BrbOpRelease(op);
	}
	OA2DP_LOG("media channel closed 0x%08X", status);
	ctx->MediaChannel = nullptr;
}
