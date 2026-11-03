#include "L2capInternal.h"

using namespace OpenA2dpDriver::L2cap;

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
		OA2DP_ERROR(ctx, "L2CAP", "signaling channel open submit failed 0x%08X", status);
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
		OA2DP_WARN(ctx, "L2CAP", "close signaling: open did not complete within %u ms", BrbControlTimeoutMs);
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
	// The last AVDTP packet (often the CLOSE) may still be in the transport. Cancel it
	// and give the completion, which takes no lock, a bounded moment before the channel
	// goes; a send that outlives the grace keeps its slot busy until it completes.
	L2capCancelSend(&ctx->SignalSend);
	if (!L2capSendIdle(&ctx->SignalSend))
	{
		LARGE_INTEGER grace;
		grace.QuadPart = -static_cast<LONGLONG>(SendCancelGraceMs) * 10000;
		if (KeWaitForSingleObject(&ctx->SignalSend.Idle, Executive, KernelMode, FALSE, &grace) != STATUS_SUCCESS)
		{
			OA2DP_WARN(ctx, "L2CAP", "signaling send still pending in the transport at close");
		}
	}

	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_CLOSE_CHANNEL);
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
	if (op != nullptr)
	{
		op->Brb.Close.BtAddress = ctx->RemoteAddress;
		op->Brb.Close.ChannelHandle = ctx->SignalingChannel;
		status = BrbOpRun(op, BrbControlTimeoutMs, false);
		BrbOpRelease(op);
	}
	OA2DP_INFO(ctx, "L2CAP", "signaling channel closed, status 0x%08X", status);

	ctx->SignalingChannel = nullptr;
	InterlockedExchange(&ctx->SignalingState, ChannelClosed);
	SignalingResetSession(ctx);
	AudioNotifyLinkState(ctx);
}
