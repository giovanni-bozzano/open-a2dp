#include "L2capInternal.h"

namespace OpenA2dpDriver::L2cap
{
void OnSendComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context)
{
	auto* slot = static_cast<SEND_SLOT*>(context);
	// Read before Idle is set: from then on the slot may be reused by its owner.
	const WDFWORKITEM workItem = slot->CompletionWorkItem;
	InterlockedExchange(&slot->Status, params->IoStatus.Status);
	KeSetEvent(&slot->Idle, IO_NO_INCREMENT, FALSE);
	if (workItem != nullptr)
	{
		WdfWorkItemEnqueue(workItem);
	}
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
		OA2DP_INFO(ctx, "L2CAP", "signaling channel remote disconnect, reason %d", static_cast<int>(parameters->Parameters.Disconnect.Reason));
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
		RequestOrphanCleanup(ctx, ctx->OpenBrb.ChannelHandle, false);
	}
	else if (NT_SUCCESS(status))
	{
		ctx->SignalingChannel = ctx->OpenBrb.ChannelHandle;
		ctx->SignalingOutMtu = ctx->OpenBrb.OutResults.Params.Mtu;
		InterlockedExchange(&ctx->SignalingState, ChannelOpen);
		OA2DP_INFO(ctx, "L2CAP", "signaling channel open, out MTU %u", ctx->SignalingOutMtu);
		SignalingOnChannelOpen(ctx);
	}
	else
	{
		InterlockedExchange(&ctx->SignalingState, ChannelClosed);
		OA2DP_ERROR(ctx, "L2CAP", "signaling channel open failed 0x%08X (L2CAP response %u/%u)",
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
} // namespace OpenA2dpDriver::L2cap
