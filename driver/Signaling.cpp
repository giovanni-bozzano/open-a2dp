#include "Driver.h"
#include "Service.h"

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/SbcConfig.h>

using namespace OpenA2dp;

namespace
{

constexpr LONGLONG ResponseTimeoutMs = 3000;
constexpr uint8_t SbcMaxBitpool = 53;
// What the driver can actually play. The endpoint is fixed at 44.1 kHz stereo and
// MediaStart refuses anything else, so advertising 16/32/48 kHz or mono only invites
// a headset to configure a stream we then cannot start (PANTONE asked for 48 kHz).
// Byte 0: 44.1 kHz (0x20) | dual channel (0x04) | stereo (0x02) | joint stereo (0x01).
constexpr uint8_t kSbcDriverCapabilities[SbcConfig::kIeLength] = {
	0x27, 0xFF, SbcConfig::kMinBitpool, SbcConfig::kMaxBitpool};
// How long a headset-initiated connection gets to configure the stream itself
// before we start discovery.
constexpr LONGLONG kIncomingConnectDelayMs = 700;
// Bounded wait for a service that is installed but has not attached yet. The
// service polls for the control device every 250 ms.
constexpr unsigned kConnectWaitSteps = 8;
constexpr LONGLONG kConnectWaitMs = 100;

EVT_WDF_WORKITEM OnSignalingWorkItem;
EVT_WDF_TIMER OnResponseTimeout;
EVT_WDF_TIMER OnConnectDelay;
EVT_WDF_REQUEST_COMPLETION_ROUTINE OnReadComplete;

// Scores each sink SEP: SBC 1, AAC 2, so AAC wins when both sides support it.
// Returns 0 for a codec we cannot configure (or that the user forced away from).
uint32_t SelectConfig(void* context, Avdtp::CodecType codec, const uint8_t* remoteIe, size_t remoteIeLength,
	uint8_t* configIe, size_t& configIeLength)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	Ioctl::Preference preference;
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		preference = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	}
	const bool forceCodec = (preference.flags & Ioctl::kPreferenceForceCodec) != 0;

	// Which codecs the sink actually advertises is otherwise invisible in the log,
	// and "AAC never wins" looks the same whether the sink lacks it or we skipped it.
	OA2DP_LOG("AVDTP sink offers codec %u (%u IE bytes)", static_cast<unsigned>(codec),
		static_cast<unsigned>(remoteIeLength));

	if (codec == Avdtp::CodecType::Sbc)
	{
		if (forceCodec && preference.codec != Ioctl::kCodecSbc)
		{
			return 0;
		}
		// Offer the selector only what we can play, so it never picks a rate or
		// channel mode the media path would refuse.
		if (remoteIeLength < SbcConfig::kIeLength)
		{
			return 0;
		}
		uint8_t playable[SbcConfig::kIeLength];
		for (size_t i = 0; i < SbcConfig::kIeLength; ++i)
		{
			playable[i] = remoteIe[i];
		}
		playable[0] &= kSbcDriverCapabilities[0];
		if (!SbcConfig::Select(playable, SbcConfig::kIeLength, 0, SbcMaxBitpool, configIe))
		{
			return 0;
		}
		configIeLength = SbcConfig::kIeLength;
		return 1;
	}

	if (codec == Avdtp::CodecType::Aac)
	{
		// AAC exists only while an encoder service is registered for this device.
		if (!ServiceRegistered(ctx) || (forceCodec && preference.codec != Ioctl::kCodecAac))
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

// Refreshes the snapshot GET_STATUS reads without taking SessionLock. Caller holds SessionLock.
void PublishStatus(DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->AvdtpStateSnapshot, static_cast<LONG>(ctx->Session.State()));
	InterlockedExchange(&ctx->NegotiatedCodecSnapshot,
		ctx->Session.SelectedIeLength() != 0 ? static_cast<LONG>(ctx->Session.SelectedCodec()) : 0);
}

void ResetSession(DEVICE_CONTEXT* ctx)
{
	ctx->Session = Avdtp::SourceSession(Avdtp::CodecType::Sbc, kSbcDriverCapabilities,
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
	ctx->ResponseDeadline = 0;
	ctx->ConnectDelayDeadline = 0;
	InterlockedExchange(&ctx->MediaOpenPending, 0);
	PublishStatus(ctx);
}

// Deadlines protect a new timer arm from callbacks queued by an older arm.
void ArmConnectDelay(DEVICE_CONTEXT* ctx, LONGLONG milliseconds)
{
	ctx->ConnectDelayDeadline = KeQueryInterruptTime() + static_cast<ULONGLONG>(milliseconds) * 10000;
	WdfTimerStart(ctx->ConnectDelayTimer, WDF_REL_TIMEOUT_IN_MS(milliseconds));
}

// Sends what the session produced and arms or stops the response timer. Caller holds SessionLock.
void Transmit(DEVICE_CONTEXT* ctx, size_t length);

void Transmit(DEVICE_CONTEXT* ctx, size_t length)
{
	if (length != 0 && ctx->SignalingState != ChannelOpen)
	{
		// The channel is closing. L2capCloseSignalingChannel passes through SessionLock
		// after it leaves ChannelOpen, so no send that checked this under the lock can
		// still be running when the channel handle is closed.
		length = 0;
	}
	if (length != 0)
	{
		const NTSTATUS status = L2capSend(ctx, &ctx->SignalSend, ctx->SignalingChannel, ctx->WriteBuffer,
			static_cast<ULONG>(length), SignalSendTimeoutMs);
		if (!NT_SUCCESS(status))
		{
			if (IsLinkLoss(ctx, status))
			{
				OA2DP_LOG("signaling: send stopped, headset link is down (0x%08X)", status);
			}
			else
			{
				OA2DP_LOG("signaling send failed 0x%08X", status);
			}
		}
		if (length >= 2 && ctx->WriteBuffer[1] == static_cast<uint8_t>(Avdtp::Signal::Close))
		{
			OA2DP_LOG("AVDTP CLOSE sent (%u bytes), send status 0x%08X", static_cast<unsigned>(length), status);
		}
		if (length >= 2 && ctx->WriteBuffer[1] == static_cast<uint8_t>(Avdtp::Signal::Abort))
		{
			OA2DP_LOG("AVDTP ABORT sent (%u bytes), send status 0x%08X", static_cast<unsigned>(length), status);
		}
	}

	if (ctx->Session.HasPendingCommand())
	{
		ctx->ResponseDeadline = KeQueryInterruptTime() + ResponseTimeoutMs * 10000;
		WdfTimerStart(ctx->ResponseTimer, WDF_REL_TIMEOUT_IN_MS(ResponseTimeoutMs));
	}
	else
	{
		ctx->ResponseDeadline = 0;
		WdfTimerStop(ctx->ResponseTimer, FALSE);
	}

	const Avdtp::SessionState state = ctx->Session.State();
	PublishStatus(ctx);
	if (state != ctx->LoggedState)
	{
		ctx->LoggedState = state;
		OA2DP_LOG("AVDTP state %u (remote SEID %u)", static_cast<unsigned>(state), ctx->Session.RemoteSeid());
		if (state == Avdtp::SessionState::Failed && ctx->SignalingState == ChannelOpen)
		{
			const auto reason = ctx->Session.LastDiscoveryFailure();
			if (reason != Avdtp::DiscoveryFailure::None)
			{
				OA2DP_LOG("AVDTP discovery failed: reason %u (1=rejected 2=no usable sink 3=malformed 4=timeout), error present %u code 0x%02X",
					static_cast<unsigned>(reason), ctx->Session.HasDiscoveryError() ? 1u : 0u,
					static_cast<unsigned>(ctx->Session.DiscoveryError()));
			}
		}
		else if (state != Avdtp::SessionState::Idle && state != Avdtp::SessionState::Failed)
		{
			ctx->ConnectDelayDeadline = 0;
			WdfTimerStop(ctx->ConnectDelayTimer, FALSE);
		}
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
				// Opening the media channel waits on the transport, so it is not done here
				// but by the work item once it has re-armed the signaling read.
				InterlockedExchange(&ctx->MediaOpenPending, 1);
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
	// A read whose cancel the transport never completed still owns the request and
	// the buffer; reusing it would hand the transport a request it already holds.
	if (KeReadStateEvent(&ctx->ReadIdle) == 0)
	{
		OA2DP_LOG("signaling read still pending in the transport; not re-arming");
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

void OnConnectDelay(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	InterlockedExchange(&ctx->DeferredConnectPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

// Opens the media channel after the initiator's OPEN was accepted, then sends START.
// Runs in the signaling work item with SessionLock held; every wait on the transport
// is bounded, and StopEvent cuts the waits short when the device is going away.
void OpenMediaTransport(DEVICE_CONTEXT* ctx)
{
	// The sink sometimes refuses the first media channel (L2CAP response 4, "no
	// resources") because a previous session has not fully released it. Retry a few
	// times before giving up; the sink usually frees the channel within a couple of
	// hundred milliseconds.
	NTSTATUS mediaStatus = STATUS_UNSUCCESSFUL;
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		if (attempt != 0)
		{
			LARGE_INTEGER delay;
			delay.QuadPart = -10000LL * 200;
			WdfWaitLockRelease(ctx->SessionLock);
			const NTSTATUS slept = KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &delay);
			WdfWaitLockAcquire(ctx->SessionLock, nullptr);
			// The device may be going away or the channel may have closed while we waited.
			if (slept != STATUS_TIMEOUT || ctx->SignalingState != ChannelOpen ||
				ctx->Session.State() != Avdtp::SessionState::Open)
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
		if (mediaStatus == STATUS_CANCELLED)
		{
			return; // teardown cut the open short
		}
	}

	if (NT_SUCCESS(mediaStatus))
	{
		Transmit(ctx, ctx->Session.Start(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		return;
	}

	OA2DP_LOG("media channel open failed after retries; tearing down the signaling channel");
	// Tear the session down so the sink releases its resources and can be reconnected
	// cleanly instead of leaving a half-open AVDTP session that never reaches Streaming.
	L2capCloseSignalingChannel(ctx);
}

void OnSignalingWorkItem(_In_ WDFWORKITEM workItem)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfWorkItemGetParentObject(workItem));
	WdfWaitLockAcquire(ctx->SessionLock, nullptr);
	ctx->SignalingWorkItemThread = KeGetCurrentThread();

	if (InterlockedExchange(&ctx->ConnectPending, 0) != 0 && ctx->SignalingState == ChannelOpen)
	{
		AudioNotifyLinkState(ctx);
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
				// StopEvent ends the pause early when the device is going away.
				const NTSTATUS slept = KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &interval);
				WdfWaitLockAcquire(ctx->SessionLock, nullptr);
				if (slept != STATUS_TIMEOUT || ServiceRegistered(ctx) || ctx->SignalingState != ChannelOpen)
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
			if (InterlockedCompareExchange(&ctx->SignalingIncoming, 0, 0) != 0)
			{
				// The headset connected to us and normally starts its own AVDTP
				// discovery and configuration at once. Starting ours too makes both
				// sides configure the same stream: PANTONE then got its own
				// SET_CONFIGURATION rejected (SEP in use) and refused the media
				// channel with "no resources". Listen first; the library accepts the
				// headset's configuration, and we only discover if it stays silent.
				OA2DP_LOG("signaling: headset-initiated; waiting up to %u ms for it to configure the stream",
					static_cast<unsigned>(kIncomingConnectDelayMs));
				SubmitRead(ctx);
				ArmConnectDelay(ctx, kIncomingConnectDelayMs);
			}
			else
			{
				Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
				SubmitRead(ctx);
			}
		}
	}

	if (InterlockedExchange(&ctx->ReadCompleted, 0) != 0)
	{
		if (ctx->SignalingState == ChannelOpen)
		{
			const size_t length = ctx->ReadBrb.BufferSize;
			if (ctx->Session.State() == Avdtp::SessionState::Discovering && length >= 2 &&
				(ctx->ReadBuffer[0] & 0x0F) == 0x02 && ctx->ReadBuffer[1] == 0x01)
			{
				for (size_t pos = 2; pos + 2 <= length; pos += 2)
				{
					OA2DP_LOG("AVDTP discover SEP seid %u inUse %u media %u sepType %u",
						static_cast<unsigned>(ctx->ReadBuffer[pos] >> 2), static_cast<unsigned>((ctx->ReadBuffer[pos] >> 1) & 1),
						static_cast<unsigned>(ctx->ReadBuffer[pos + 1] >> 4), static_cast<unsigned>((ctx->ReadBuffer[pos + 1] >> 3) & 1));
				}
			}
			if ((ctx->Session.State() == Avdtp::SessionState::Aborting || ctx->Session.State() == Avdtp::SessionState::Closing) &&
				length >= 2 && ctx->ReadBuffer[1] == static_cast<uint8_t>(ctx->Session.State() == Avdtp::SessionState::Closing
					? Avdtp::Signal::Close : Avdtp::Signal::Abort))
			{
				OA2DP_LOG("AVDTP %s response type %u (2=accept 3=reject), length %u, code 0x%02X",
					ctx->Session.State() == Avdtp::SessionState::Closing ? "CLOSE" : "ABORT",
					static_cast<unsigned>(ctx->ReadBuffer[0] & 0x03), static_cast<unsigned>(length),
					static_cast<unsigned>(length > 2 ? ctx->ReadBuffer[2] : 0));
			}
			Transmit(ctx, ctx->Session.OnPacket(ctx->ReadBuffer, length, ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		}
		SubmitRead(ctx);
	}

	if (InterlockedExchange(&ctx->DeferredConnectPending, 0) != 0 && ctx->SignalingState == ChannelOpen &&
		ctx->ConnectDelayDeadline != 0)
	{
		const ULONGLONG now = KeQueryInterruptTime();
		if (now < ctx->ConnectDelayDeadline)
		{
			WdfTimerStart(ctx->ConnectDelayTimer, -static_cast<LONGLONG>(ctx->ConnectDelayDeadline - now));
		}
		else
		{
			ctx->ConnectDelayDeadline = 0;
			const auto state = ctx->Session.State();
			if (state == Avdtp::SessionState::Idle)
			{
				OA2DP_LOG("signaling: starting discovery");
				Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
			}
		}
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

	if (InterlockedExchange(&ctx->TimeoutPending, 0) != 0 && ctx->SignalingState == ChannelOpen &&
		ctx->ResponseDeadline != 0 && ctx->Session.HasPendingCommand())
	{
		const ULONGLONG now = KeQueryInterruptTime();
		if (now < ctx->ResponseDeadline)
		{
			WdfTimerStart(ctx->ResponseTimer, -static_cast<LONGLONG>(ctx->ResponseDeadline - now));
		}
		else
		{
			OA2DP_LOG("AVDTP response timeout in state %u", static_cast<unsigned>(ctx->Session.State()));
			Transmit(ctx, ctx->Session.OnTimeout(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		}
	}

	if (InterlockedExchange(&ctx->MediaOpenPending, 0) != 0 && ctx->SignalingState == ChannelOpen &&
		ctx->Session.State() == Avdtp::SessionState::Open && ctx->MediaChannel == nullptr)
	{
		OpenMediaTransport(ctx);
	}

	ctx->SignalingWorkItemThread = nullptr;
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

	WDF_TIMER_CONFIG delayConfig;
	WDF_TIMER_CONFIG_INIT(&delayConfig, OnConnectDelay);
	status = WdfTimerCreate(&delayConfig, &attrs, &ctx->ConnectDelayTimer);
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

void SignalingResetSession(_In_ DEVICE_CONTEXT* ctx)
{
	// Called after the signaling channel closed. The session is otherwise left in
	// whatever state the link died in (Open, Streaming), which both misreports the
	// connection and refuses a fresh SET_CONFIGURATION on reconnect. The signaling
	// work item reaches here through the media-open failure teardown while it holds
	// SessionLock, so only take the lock when this thread is not that work item.
	const bool lockHeld = ctx->SignalingWorkItemThread == KeGetCurrentThread();
	if (!lockHeld)
	{
		WdfWaitLockAcquire(ctx->SessionLock, nullptr);
	}
	ResetSession(ctx);
	if (!lockHeld)
	{
		WdfWaitLockRelease(ctx->SessionLock);
	}
}

bool SignalingAbortSession(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->SignalingState != ChannelOpen || ctx->SignalingWorkItemThread == KeGetCurrentThread())
	{
		return true;
	}

	LONGLONG lockTimeout = WDF_REL_TIMEOUT_IN_MS(2000);
	if (WdfWaitLockAcquire(ctx->SessionLock, &lockTimeout) != STATUS_SUCCESS)
	{
		OA2DP_LOG("AVDTP teardown: session is busy, skipping CLOSE");
		return false;
	}
	const auto state = ctx->Session.State();
	// A normal CLOSE lets the headset release its SEP; ABORT alone left PANTONE reporting it in use.
	size_t length = ctx->Session.Close(ctx->WriteBuffer, sizeof(ctx->WriteBuffer));
	const bool closing = length != 0;
	if (!closing)
	{
		length = ctx->Session.Abort(ctx->WriteBuffer, sizeof(ctx->WriteBuffer));
	}
	if (length != 0)
	{
		OA2DP_LOG("AVDTP %s before teardown (state %u)", closing ? "closing session" : "aborting session",
			static_cast<unsigned>(state));
		Transmit(ctx, length);
	}
	WdfWaitLockRelease(ctx->SessionLock);

	// Give the headset up to 1 s in total to answer, so the command takes effect before the
	// channel closes. The read work item applies the response; this thread only polls the
	// session state. Every lock wait is limited to what is left of the second.
	const ULONGLONG deadline = KeQueryInterruptTime() + 1000ull * 10000ull;
	for (;;)
	{
		const ULONGLONG now = KeQueryInterruptTime();
		if (now >= deadline)
		{
			break;
		}
		LONGLONG remaining = -static_cast<LONGLONG>(deadline - now);
		if (WdfWaitLockAcquire(ctx->SessionLock, &remaining) != STATUS_SUCCESS)
		{
			OA2DP_LOG("AVDTP teardown: session is busy, not waiting for the response");
			return true;
		}
		const auto sessionState = ctx->Session.State();
		const bool waiting = sessionState == Avdtp::SessionState::Aborting || sessionState == Avdtp::SessionState::Closing;
		WdfWaitLockRelease(ctx->SessionLock);
		if (!waiting)
		{
			return true;
		}
		LARGE_INTEGER delay;
		delay.QuadPart = -100000LL; // 10 ms
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
	}
	OA2DP_LOG("AVDTP %s response not received within 1 s", closing ? "CLOSE" : "ABORT");
	return true;
}

void SignalingStop(_In_ DEVICE_CONTEXT* ctx)
{
	LARGE_INTEGER timeout;
	timeout.QuadPart = -10000000LL; // 1 s

	WdfRequestCancelSentRequest(ctx->ReadRequest);
	if (KeWaitForSingleObject(&ctx->ReadIdle, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS)
	{
		OA2DP_LOG("signaling stop: read did not complete within 1 s");
	}
	WdfTimerStop(ctx->ResponseTimer, TRUE);
	WdfTimerStop(ctx->ConnectDelayTimer, TRUE);
	InterlockedExchange(&ctx->DeferredConnectPending, 0);
	if (ctx->SignalingWorkItemThread != KeGetCurrentThread())
	{
		WdfWorkItemFlush(ctx->SignalingWorkItem);
	}
	// A read may have been resubmitted by the work item before it saw the state change.
	WdfRequestCancelSentRequest(ctx->ReadRequest);
	timeout.QuadPart = -10000000LL;
	if (KeWaitForSingleObject(&ctx->ReadIdle, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS)
	{
		OA2DP_LOG("signaling stop: read did not complete within 1 s (2nd)");
	}
}
