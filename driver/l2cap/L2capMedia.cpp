#include "L2capInternal.h"

using namespace OpenA2dpDriver::L2cap;

NTSTATUS L2capStartMediaOpen(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->MediaChannel != nullptr || ctx->MediaOpenOp != nullptr)
	{
		return STATUS_INVALID_DEVICE_STATE;
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

	// Nobody waits: the completion notifies the signaling work item, which collects the
	// result. Published before the send, so the completion always finds the owner set.
	op->NotifyFlag = &ctx->MediaOpenCompleted;
	op->NotifyWorkItem = ctx->SignalingWorkItem;
	ctx->MediaOpenOp = op;
	const NTSTATUS status = BrbOpStart(op);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "L2CAP", "media channel open submit failed 0x%08X", status);
		ctx->MediaOpenOp = nullptr;
		BrbOpRelease(op);
		return status;
	}
	return STATUS_PENDING;
}

bool L2capFinishMediaOpen(_In_ DEVICE_CONTEXT* ctx, _Out_ NTSTATUS* status)
{
	*status = STATUS_PENDING;
	auto* op = static_cast<BRB_OP*>(ctx->MediaOpenOp);
	if (op == nullptr || KeReadStateEvent(&op->Done) == 0)
	{
		return false; // a stale notification from an operation already collected or closed
	}
	ctx->MediaOpenOp = nullptr;
	*status = op->Status;
	auto& brb = op->Brb.Open;
	if (NT_SUCCESS(op->Status))
	{
		ctx->MediaChannel = brb.ChannelHandle;
		ctx->MediaOutMtu = brb.OutResults.Params.Mtu;
		OA2DP_INFO(ctx, "L2CAP", "media channel open, out MTU %u", ctx->MediaOutMtu);
	}
	else
	{
		OA2DP_ERROR(ctx, "L2CAP", "media channel open failed 0x%08X (L2CAP response %u/%u)", op->Status, brb.Response,
			brb.ResponseStatus);
	}
	BrbOpRelease(op);
	return true;
}

// Takes over an open still in flight at close. A result that arrives within the grace
// period is adopted so the close below releases it; otherwise the operation is
// abandoned and its completion closes the orphaned channel itself.
static void ReclaimMediaOpen(_In_ DEVICE_CONTEXT* ctx)
{
	auto* op = static_cast<BRB_OP*>(ctx->MediaOpenOp);
	if (op == nullptr)
	{
		return;
	}
	ctx->MediaOpenOp = nullptr;
	if (KeReadStateEvent(&op->Done) == 0)
	{
		WdfRequestCancelSentRequest(op->Request);
		LARGE_INTEGER grace;
		grace.QuadPart = -static_cast<LONGLONG>(SendCancelGraceMs) * 10000;
		(void)KeWaitForSingleObject(&op->Done, Executive, KernelMode, FALSE, &grace);
	}
	// Status is written before State, so a completion that wins the race below is visible.
	if (KeReadStateEvent(&op->Done) != 0 || InterlockedCompareExchange(&op->State, 2, 0) == 1)
	{
		if (NT_SUCCESS(op->Status) && ctx->MediaChannel == nullptr)
		{
			ctx->MediaChannel = op->Brb.Open.ChannelHandle;
		}
	}
	else
	{
		OA2DP_WARN(ctx, "L2CAP", "media channel open still pending at close; abandoning it");
	}
	BrbOpRelease(op);
}

void L2capCloseMediaChannel(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->MediaAcceptAllowed, 0);
	MediaStop(ctx);
	// An accept still in flight would otherwise set MediaChannel after the close below.
	// Revoke it (a late success then closes the channel itself) and wait, bounded, for
	// the completion to signal RejectIdle so it is not halfway through adopting the channel.
	InterlockedExchange(&ctx->MediaAcceptPending, 0);
	const ULONGLONG deadline = KeQueryInterruptTime() + 1000ull * 10000ull;
	while (InterlockedCompareExchange(&ctx->RejectBusy, 0, 0) != 0)
	{
		const ULONGLONG now = KeQueryInterruptTime();
		if (now >= deadline)
		{
			break;
		}
		LARGE_INTEGER wait;
		wait.QuadPart = -static_cast<LONGLONG>(deadline - now);
		KeWaitForSingleObject(&ctx->RejectIdle, Executive, KernelMode, FALSE, &wait);
	}
	ReclaimMediaOpen(ctx);
	if (ctx->MediaChannel == nullptr)
	{
		return;
	}

	// A send the transport still holds would keep the channel referenced; cancel it and
	// give the completion a bounded moment. The completion takes no lock, so waiting
	// here under SessionLock cannot deadlock. A send that still does not complete keeps
	// its slot busy (the slot owns the request and data) until it does.
	L2capCancelSend(&ctx->MediaSend);
	if (!L2capSendIdle(&ctx->MediaSend))
	{
		LARGE_INTEGER grace;
		grace.QuadPart = -static_cast<LONGLONG>(SendCancelGraceMs) * 10000;
		if (KeWaitForSingleObject(&ctx->MediaSend.Idle, Executive, KernelMode, FALSE, &grace) != STATUS_SUCCESS)
		{
			OA2DP_WARN(ctx, "L2CAP", "media send still pending in the transport at close");
		}
	}

	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_CLOSE_CHANNEL);
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
	if (op != nullptr)
	{
		op->Brb.Close.BtAddress = ctx->RemoteAddress;
		op->Brb.Close.ChannelHandle = ctx->MediaChannel;
		status = BrbOpRun(op, BrbControlTimeoutMs, false);
		BrbOpRelease(op);
	}
	OA2DP_INFO(ctx, "L2CAP", "media channel closed, status 0x%08X", status);
	ctx->MediaChannel = nullptr;
}
