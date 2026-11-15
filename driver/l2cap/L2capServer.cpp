#include "L2capInternal.h"

namespace OpenA2dpDriver::L2cap
{
namespace
{
// Clears RejectBusy first, so a waiter woken by RejectIdle sees the slot free.
void ReleaseReject(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->RejectBusy, 0);
	KeSetEvent(&ctx->RejectIdle, IO_NO_INCREMENT, FALSE);
}
} // namespace

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
			OA2DP_INFO(ctx, "L2CAP", "incoming media channel open, out MTU %u", ctx->MediaOutMtu);
			ReleaseReject(ctx);
			SignalingOnMediaChannelOpen(ctx);
			return;
		}
		OA2DP_ERROR(ctx, "L2CAP", "incoming media accept failed 0x%08X", params->IoStatus.Status);
		if (ctx->SignalingState == ChannelOpen)
		{
			InterlockedExchange(&ctx->MediaAcceptAllowed, 1);
		}
	}
	else if (accepted && NT_SUCCESS(params->IoStatus.Status))
	{
		// The media channel was closed (teardown) while this accept was in flight.
		RequestOrphanCleanup(ctx, ctx->RejectBrb.ChannelHandle, false);
	}
	else
	{
		OA2DP_INFO(ctx, "L2CAP", "incoming connection refused, completion status 0x%08X", params->IoStatus.Status);
	}
	ReleaseReject(ctx);
}

// Answers an incoming connection through the preallocated RejectRequest:
// refuses it, or (accept == true) accepts it as the media transport channel.
void RespondIncoming(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE handle, _In_ BTH_ADDR address, _In_ bool accept)
{
	if (InterlockedCompareExchange(&ctx->RejectBusy, 1, 0) != 0)
	{
		// The response slot is still in flight, and an answer cannot be built without it. An
		// accept that was already granted stays granted: its caller consumed the record of it,
		// and losing it here would leave this session unable to accept the media transport,
		// which the sink opens but once.
		OA2DP_WARN(ctx, "L2CAP", "incoming connection dropped (response in flight)");
		if (accept)
		{
			InterlockedExchange(&ctx->MediaAcceptAllowed, 1);
		}
		return;
	}
	// Only the owner of RejectBusy clears or sets RejectIdle, and nothing can complete
	// before the request is sent below.
	KeClearEvent(&ctx->RejectIdle);

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
	OA2DP_ERROR(ctx, "L2CAP", "incoming connection response failed 0x%08X", status);
	if (InterlockedExchange(&ctx->MediaAcceptPending, 0) != 0)
	{
		InterlockedExchange(&ctx->MediaAcceptAllowed, 1);
	}
	ReleaseReject(ctx);
}

// Server indication: the sink opened an L2CAP channel on the AVDTP PSM.
// The first channel becomes the signaling channel. When the sink configured
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
		OA2DP_INFO(ctx, "L2CAP", "incoming media connection");
		RespondIncoming(ctx, parameters->ConnectionHandle, parameters->BtAddress, true);
		return;
	}

	if (InterlockedCompareExchange(&ctx->SignalingState, ChannelOpening, ChannelClosed) != ChannelClosed)
	{
		OA2DP_WARN(ctx, "L2CAP", "incoming signaling connection refused: signaling channel already in state %ld",
			static_cast<long>(ctx->SignalingState));
		RespondIncoming(ctx, parameters->ConnectionHandle, parameters->BtAddress, false);
		return;
	}
	InterlockedIncrement(&ctx->SignalingGeneration);

	OA2DP_INFO(ctx, "L2CAP", "incoming signaling connection");
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
		OA2DP_ERROR(ctx, "L2CAP", "incoming signaling accept failed 0x%08X", status);
	}
}
} // namespace OpenA2dpDriver::L2cap
