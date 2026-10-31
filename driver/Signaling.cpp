#include "Driver.h"
#include "Service.h"

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/SbcConfig.h>

using namespace OpenA2dp;

namespace
{

constexpr LONGLONG ResponseTimeoutMs = 3000;
constexpr uint8_t SbcMaxBitpool = 53;
// Bounded wait for a service that is installed but has not attached yet. The
// service polls for the control device every 250 ms.
constexpr unsigned kConnectWaitSteps = 8;
constexpr LONGLONG kConnectWaitMs = 100;

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

	// Which codecs the sink actually advertises is otherwise invisible in the log,
	// and "AAC never wins" looks the same whether the sink lacks it or we skipped it.
	OA2DP_LOG("AVDTP sink offers codec %u (%u IE bytes)", static_cast<unsigned>(codec),
		static_cast<unsigned>(remoteIeLength));

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
uint8_t ValidateConfig(void* context, Avdtp::CodecType codec, const uint8_t* ie, size_t ieLength)
{
	if (codec != Avdtp::CodecType::Aac)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	// The AAC SEP is always advertised, so a sink can propose an AAC configuration
	// even while no encoder exists. Refusing it here keeps the stream from opening
	// into a codec the driver has nothing to encode with.
	if (!ServiceRegistered(static_cast<DEVICE_CONTEXT*>(context)))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	return AacConfig::Validate(ie, ieLength);
}

void ResetSession(DEVICE_CONTEXT* ctx)
{
	ctx->Session = Avdtp::SourceSession(Avdtp::CodecType::Sbc, SbcConfig::kLocalCapabilities,
		SbcConfig::kIeLength, &SelectConfig, ctx, &ValidateConfig);
	// Always advertise AAC. Registering the SEP only when the service happens to be
	// up already loses a race it usually loses: the headset opens the signaling
	// channel first, ResetSession runs with EncoderRegistered == 0, and the codec
	// would then be missing for the whole connection. Whether AAC may be selected is
	// decided per negotiation in SelectConfig, which is the state that actually
	// matters at that moment.
	ctx->Session.SetLocalSep(Avdtp::kLocalAacSeid, Avdtp::CodecType::Aac, AacConfig::kLocalCapabilities,
		AacConfig::kIeLength);
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
			else
			{
				// The sink sometimes refuses the first media channel (L2CAP response 4,
				// "no resources") because a previous session has not fully released it.
				// Retry a few times before giving up; the sink usually frees the channel
				// within a couple of hundred milliseconds.
				NTSTATUS mediaStatus = STATUS_UNSUCCESSFUL;
				for (int attempt = 0; attempt < 3; ++attempt)
				{
					if (attempt != 0)
					{
						LARGE_INTEGER delay;
						delay.QuadPart = -10000LL * 200;
						WdfWaitLockRelease(ctx->SessionLock);
						KeDelayExecutionThread(KernelMode, FALSE, &delay);
						WdfWaitLockAcquire(ctx->SessionLock, nullptr);
						// The channel may have closed while we waited.
						if (ctx->SignalingState != ChannelOpen)
						{
							return;
						}
					}
					mediaStatus = L2capOpenMediaChannel(ctx);
					if (NT_SUCCESS(mediaStatus))
					{
						break;
					}
					OA2DP_LOG("media channel open attempt %d failed 0x%08X", attempt + 1, mediaStatus);
				}
				if (NT_SUCCESS(mediaStatus))
				{
					Transmit(ctx, ctx->Session.Start(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
				}
				else
				{
					OA2DP_LOG("media channel open failed after retries; tearing down the signaling channel");
					// Tear the session down so the sink releases its resources and can
					// be reconnected cleanly instead of leaving a half-open AVDTP
					// session that never reaches Streaming.
					L2capCloseSignalingChannel(ctx);
				}
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
		// The control device appears when the headset connects and the encoder service
		// polls for it every 250 ms, so the service normally registers after discovery
		// has already run and AAC is lost for the whole connection. Wait for it briefly,
		// but only when the service is installed: a machine without it must connect
		// exactly as fast as before. The wait is bounded and the channel can close
		// underneath us, so re-check both after every step.
		if (!ServiceRegistered(ctx) && ServiceInstalled())
		{
			OA2DP_LOG("signaling: waiting up to %u ms for the encoder service", kConnectWaitSteps * kConnectWaitMs);
			for (unsigned step = 0; step < kConnectWaitSteps; ++step)
			{
				WdfWaitLockRelease(ctx->SessionLock);
				LARGE_INTEGER interval;
				interval.QuadPart = -10000LL * kConnectWaitMs;
				KeDelayExecutionThread(KernelMode, FALSE, &interval);
				WdfWaitLockAcquire(ctx->SessionLock, nullptr);
				if (ServiceRegistered(ctx) || ctx->SignalingState != ChannelOpen)
				{
					break;
				}
			}
			OA2DP_LOG("signaling: service %s after the wait",
				ServiceRegistered(ctx) ? "registered" : "absent");
		}

		if (ctx->SignalingState == ChannelOpen)
		{
			ResetSession(ctx);
			Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
			SubmitRead(ctx);
		}
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
