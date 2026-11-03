#include "SignalingInternal.h"

namespace OpenA2dpDriver::Signaling
{
// Caller holds SessionLock.
void SubmitRead(DEVICE_CONTEXT* ctx)
{
	if (ctx->SignalingState != ChannelOpen)
	{
		KeSetEvent(&ctx->ReadIdle, IO_NO_INCREMENT, FALSE);
		return;
	}
	// A read whose cancel the transport never completed still owns the request and
	// the buffer; reusing it would hand the transport a request it already holds.
	if (KeReadStateEvent(&ctx->ReadIdle) == 0)
	{
		OA2DP_WARN(ctx, "Signaling", "read still pending in the transport; not re-arming");
		return;
	}

	KeClearEvent(&ctx->ReadIdle);

	WDF_REQUEST_REUSE_PARAMS reuse;
	WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
	NTSTATUS status = WdfRequestReuse(ctx->ReadRequest, &reuse);
	if (NT_SUCCESS(status))
	{
		auto& brb = ctx->ReadBrb;
		ctx->Profile.BthReuseBrb(reinterpret_cast<PBRB>(&brb), BRB_L2CA_ACL_TRANSFER);
		brb.BtAddress = ctx->RemoteAddress;
		brb.ChannelHandle = ctx->SignalingChannel;
		brb.TransferFlags = ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK;
		brb.BufferSize = sizeof(ctx->ReadBuffer);
		brb.Buffer = ctx->ReadBuffer;

		status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, ctx->ReadRequest,
			IOCTL_INTERNAL_BTH_SUBMIT_BRB, ctx->ReadBrbMemory, nullptr, nullptr, nullptr, nullptr, nullptr);
	}

	if (NT_SUCCESS(status))
	{
		WdfRequestSetCompletionRoutine(ctx->ReadRequest, OnReadComplete, ctx);
		if (WdfRequestSend(ctx->ReadRequest, ctx->IoTarget, WDF_NO_SEND_OPTIONS))
		{
			return;
		}
		status = WdfRequestGetStatus(ctx->ReadRequest);
	}

	OA2DP_ERROR(ctx, "Signaling", "read submit failed 0x%08X", status);
	KeSetEvent(&ctx->ReadIdle, IO_NO_INCREMENT, FALSE);
}

void OnReadComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	const NTSTATUS status = params->IoStatus.Status;
	if (!NT_SUCCESS(status))
	{
		if (status != STATUS_CANCELLED)
		{
			OA2DP_ERROR(ctx, "Signaling", "read failed 0x%08X", status);
		}
		KeSetEvent(&ctx->ReadIdle, IO_NO_INCREMENT, FALSE);
		return;
	}
	// The transport is done with the request; the work item re-arms it. Idle must be
	// signaled here too, or SubmitRead's pending-read guard refuses every re-arm.
	InterlockedExchange(&ctx->ReadCompleted, 1);
	KeSetEvent(&ctx->ReadIdle, IO_NO_INCREMENT, FALSE);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void OnResponseTimeout(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	InterlockedExchange(&ctx->TimeoutPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}
} // namespace OpenA2dpDriver::Signaling
