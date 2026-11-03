#include "L2capInternal.h"

using namespace OpenA2dpDriver::L2cap;

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
		OA2DP_ERROR(ctx, "L2CAP", "media channel open failed 0x%08X (L2CAP response %u/%u)", status, brb.Response, brb.ResponseStatus);
		BrbOpRelease(op);
		return status;
	}

	ctx->MediaChannel = brb.ChannelHandle;
	ctx->MediaOutMtu = brb.OutResults.Params.Mtu;
	OA2DP_INFO(ctx, "L2CAP", "media channel open, out MTU %u", ctx->MediaOutMtu);
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
	OA2DP_INFO(ctx, "L2CAP", "media channel closed, status 0x%08X", status);
	ctx->MediaChannel = nullptr;
}
