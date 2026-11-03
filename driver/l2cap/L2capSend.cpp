#include "L2capInternal.h"

using namespace OpenA2dpDriver::L2cap;

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

NTSTATUS L2capSendAsync(_In_ DEVICE_CONTEXT* ctx, _In_ SEND_SLOT* slot, _In_ L2CAP_CHANNEL_HANDLE channel,
	_In_reads_bytes_(length) const VOID* data, _In_ ULONG length)
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
	}
	return status;
}

NTSTATUS L2capSend(_In_ DEVICE_CONTEXT* ctx, _In_ SEND_SLOT* slot, _In_ L2CAP_CHANNEL_HANDLE channel,
	_In_reads_bytes_(length) const VOID* data, _In_ ULONG length, _In_ ULONG timeoutMs)
{
	const NTSTATUS status = L2capSendAsync(ctx, slot, channel, data, length);
	if (!NT_SUCCESS(status))
	{
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
	OA2DP_WARN(ctx, "L2CAP", "send did not complete within %u ms; it stays pending in the transport", timeoutMs);
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
