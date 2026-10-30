#include "Driver.h"
#include "Service.h"

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/SbcConfig.h>

using namespace OpenA2dp;

namespace
{

constexpr LONGLONG ResponseTimeoutMs = 3000;
constexpr uint8_t SbcMaxBitpool = 53;

EVT_WDF_WORKITEM OnSignalingWorkItem;
EVT_WDF_TIMER OnResponseTimeout;
EVT_WDF_REQUEST_COMPLETION_ROUTINE OnReadComplete;

// Scores each sink SEP: SBC 1, AAC 2, so AAC wins when both sides support it.
// Returns 0 for a codec we cannot configure (or that the user forced away from).
uint32_t SelectConfig(void* context, Avdtp::CodecType codec, const uint8_t* remoteIe, size_t remoteIeLength,
	uint8_t* configIe, size_t& configIeLength)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	const bool forceCodec = (ctx->Preference.flags & Ioctl::kPreferenceForceCodec) != 0;

	if (codec == Avdtp::CodecType::Sbc)
	{
		if (forceCodec && ctx->Preference.codec != Ioctl::kCodecSbc)
		{
			return 0;
		}
		if (!SbcConfig::Select(remoteIe, remoteIeLength, 0, SbcMaxBitpool, configIe))
		{
			return 0;
		}
		configIeLength = SbcConfig::kIeLength;
		return 1;
	}

	if (codec == Avdtp::CodecType::Aac)
	{
		// AAC exists only while an encoder service is registered for this device.
		if (!ServiceRegistered(ctx) || (forceCodec && ctx->Preference.codec != Ioctl::kCodecAac))
		{
			return 0;
		}
		if (!AacConfig::Select(remoteIe, remoteIeLength, AudioSampleRate, 0, configIe))
		{
			return 0;
		}
		configIeLength = AacConfig::kIeLength;
		return 2;
	}

	return 0;
}

// Sink-proposed configurations for non-SBC codecs; returns 0 or an AVDTP error.
uint8_t ValidateConfig(void*, Avdtp::CodecType codec, const uint8_t* ie, size_t ieLength)
{
	if (codec != Avdtp::CodecType::Aac)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	return AacConfig::Validate(ie, ieLength);
}

void ResetSession(DEVICE_CONTEXT* ctx)
{
	ctx->Session = Avdtp::SourceSession(Avdtp::CodecType::Sbc, SbcConfig::kLocalCapabilities,
		SbcConfig::kIeLength, &SelectConfig, ctx, &ValidateConfig);
	if (ServiceRegistered(ctx))
	{
		ctx->Session.SetLocalSep(Avdtp::kLocalAacSeid, Avdtp::CodecType::Aac, AacConfig::kLocalCapabilities,
			AacConfig::kIeLength);
	}
	ctx->LoggedState = Avdtp::SessionState::Idle;
}

// Sends what the session produced and arms or stops the response timer. Caller holds SessionLock.
void Transmit(DEVICE_CONTEXT* ctx, size_t length);

void Transmit(DEVICE_CONTEXT* ctx, size_t length)
{
	if (length != 0)
	{
		NTSTATUS status = L2capSend(ctx, ctx->SignalingChannel, ctx->WriteBuffer, static_cast<ULONG>(length));
		if (!NT_SUCCESS(status))
		{
			OA2DP_LOG("signaling send failed 0x%08X", status);
		}
	}

	if (ctx->Session.HasPendingCommand())
	{
		WdfTimerStart(ctx->ResponseTimer, WDF_REL_TIMEOUT_IN_MS(ResponseTimeoutMs));
	}
	else
	{
		WdfTimerStop(ctx->ResponseTimer, FALSE);
	}

	const Avdtp::SessionState state = ctx->Session.State();
	if (state != ctx->LoggedState)
	{
		ctx->LoggedState = state;
		OA2DP_LOG("AVDTP state %u (remote SEID %u)", static_cast<unsigned>(state), ctx->Session.RemoteSeid());
		if (state == Avdtp::SessionState::Open)
		{
			const uint8_t* ie = ctx->Session.SelectedIe();
			OA2DP_LOG("AVDTP configured codec %u IE %02X %02X %02X %02X",
				static_cast<unsigned>(ctx->Session.SelectedCodec()), ie[0], ie[1], ie[2], ie[3]);

			// AVDTP 6.11: the first L2CAP channel after OPEN is the media transport,
			// opened by whichever side sent OPEN.
			if (ctx->Session.IsAcceptor())
			{
				if (ctx->MediaChannel == nullptr)
				{
					InterlockedExchange(&ctx->MediaAcceptAllowed, 1);
				}
			}
			else if (NT_SUCCESS(L2capOpenMediaChannel(ctx)))
			{
				Transmit(ctx, ctx->Session.Start(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
			}
		}
		else if (state == Avdtp::SessionState::Streaming)
		{
			OA2DP_LOG("AVDTP streaming, media MTU %u", ctx->MediaOutMtu);
			MediaStart(ctx, ctx->Session.SelectedCodec(), ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength());
		}
		else if (state == Avdtp::SessionState::Open)
		{
			MediaStop(ctx);
		}
		else if (state == Avdtp::SessionState::Idle || state == Avdtp::SessionState::Failed)
		{
			L2capCloseMediaChannel(ctx);
		}
	}
}

// Caller holds SessionLock.
void SubmitRead(DEVICE_CONTEXT* ctx)
{
	if (ctx->SignalingState != ChannelOpen)
	{
		KeSetEvent(&ctx->ReadIdle, IO_NO_INCREMENT, FALSE);
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

	OA2DP_LOG("signaling read submit failed 0x%08X", status);
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
			OA2DP_LOG("signaling read failed 0x%08X", status);
		}
		KeSetEvent(&ctx->ReadIdle, IO_NO_INCREMENT, FALSE);
		return;
	}
	InterlockedExchange(&ctx->ReadCompleted, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void OnResponseTimeout(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	InterlockedExchange(&ctx->TimeoutPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void OnSignalingWorkItem(_In_ WDFWORKITEM workItem)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfWorkItemGetParentObject(workItem));
	WdfWaitLockAcquire(ctx->SessionLock, nullptr);

	if (InterlockedExchange(&ctx->ConnectPending, 0) != 0 && ctx->SignalingState == ChannelOpen)
	{
		ResetSession(ctx);
		Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		SubmitRead(ctx);
	}

	if (InterlockedExchange(&ctx->ReadCompleted, 0) != 0)
	{
		const size_t length = ctx->ReadBrb.BufferSize;
		Transmit(ctx, ctx->Session.OnPacket(ctx->ReadBuffer, length, ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		SubmitRead(ctx);
	}

	if (InterlockedExchange(&ctx->StartPending, 0) != 0 && ctx->Session.State() == Avdtp::SessionState::Open &&
		ctx->MediaChannel != nullptr)
	{
		Transmit(ctx, ctx->Session.Start(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}

	if (InterlockedExchange(&ctx->SuspendPending, 0) != 0 && ctx->Session.State() == Avdtp::SessionState::Streaming)
	{
		OA2DP_LOG("AVDTP suspending stream");
		Transmit(ctx, ctx->Session.Suspend(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}

	if (InterlockedExchange(&ctx->TimeoutPending, 0) != 0 && ctx->Session.HasPendingCommand())
	{
		OA2DP_LOG("AVDTP response timeout in state %u", static_cast<unsigned>(ctx->Session.State()));
		Transmit(ctx, ctx->Session.OnTimeout(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}

	WdfWaitLockRelease(ctx->SessionLock);
}

} // namespace

NTSTATUS SignalingInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeEvent(&ctx->ReadIdle, NotificationEvent, TRUE);
	ResetSession(ctx);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;

	NTSTATUS status = WdfWaitLockCreate(&attrs, &ctx->SessionLock);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_WORKITEM_CONFIG workConfig;
	WDF_WORKITEM_CONFIG_INIT(&workConfig, OnSignalingWorkItem);
	status = WdfWorkItemCreate(&workConfig, &attrs, &ctx->SignalingWorkItem);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_TIMER_CONFIG timerConfig;
	WDF_TIMER_CONFIG_INIT(&timerConfig, OnResponseTimeout);
	status = WdfTimerCreate(&timerConfig, &attrs, &ctx->ResponseTimer);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = WdfRequestCreate(&attrs, ctx->IoTarget, &ctx->ReadRequest);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return WdfMemoryCreatePreallocated(&attrs, &ctx->ReadBrb, sizeof(ctx->ReadBrb), &ctx->ReadBrbMemory);
}

void SignalingRequestSuspend(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->SuspendPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingOnMediaChannelOpen(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->StartPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingOnChannelOpen(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->ConnectPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingStop(_In_ DEVICE_CONTEXT* ctx)
{
	WdfRequestCancelSentRequest(ctx->ReadRequest);
	KeWaitForSingleObject(&ctx->ReadIdle, Executive, KernelMode, FALSE, nullptr);
	WdfTimerStop(ctx->ResponseTimer, TRUE);
	WdfWorkItemFlush(ctx->SignalingWorkItem);
	// A read may have been resubmitted by the work item before it saw the state change.
	WdfRequestCancelSentRequest(ctx->ReadRequest);
	KeWaitForSingleObject(&ctx->ReadIdle, Executive, KernelMode, FALSE, nullptr);
}
