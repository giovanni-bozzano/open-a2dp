#include "SignalingInternal.h"

namespace OpenA2dpDriver::Signaling
{
// Kicks off the initiator's media channel open; its completion re-enters the work item.
static void StartMediaOpen(DEVICE_CONTEXT* ctx)
{
	++ctx->MediaOpenAttempts;
	const NTSTATUS status = L2capStartMediaOpen(ctx);
	if (status != STATUS_PENDING)
	{
		OA2DP_ERROR(ctx, "L2CAP", "media channel open could not start 0x%08X; tearing down the signaling channel", status);
		L2capCloseSignalingChannel(ctx);
	}
}

// Collects a finished media channel open. The sink sometimes refuses the first media
// channel (L2CAP response 4, "no resources") because a previous session has not fully
// released it. Rather than sleeping and retrying blindly, ABORT the stream and
// rediscover once the ABORT response arrives: the round trips give the sink the event
// it needs to free the channel. Give up after a few attempts.
static void FinishMediaOpen(DEVICE_CONTEXT* ctx)
{
	NTSTATUS status;
	if (!L2capFinishMediaOpen(ctx, &status))
	{
		return;
	}
	if (NT_SUCCESS(status))
	{
		if (ctx->Session.State() == Avdtp::SessionState::Open)
		{
			Transmit(ctx, ctx->Session.Start(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		}
		else
		{
			// The session left Open while the channel was opening; the next Idle closes it.
			OA2DP_INFO(ctx, "L2CAP", "media channel opened in state %s", AvdtpStateName(ctx->Session.State()));
		}
		return;
	}
	if (ctx->Session.State() != Avdtp::SessionState::Open)
	{
		return; // the session moved on while the channel was opening; nothing to recover
	}
	if (ctx->MediaOpenAttempts < kMediaOpenAttempts)
	{
		OA2DP_WARN(ctx, "L2CAP", "media channel open attempt %u failed; aborting the stream to retry",
			static_cast<unsigned>(ctx->MediaOpenAttempts));
		ctx->MediaOpenRetry = TRUE;
		Transmit(ctx, ctx->Session.Abort(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		if (ctx->Session.State() == Avdtp::SessionState::Aborting)
		{
			return;
		}
		ctx->MediaOpenRetry = FALSE;
	}
	OA2DP_ERROR(ctx, "L2CAP", "media channel open failed after %u attempts; tearing down the signaling channel",
		static_cast<unsigned>(ctx->MediaOpenAttempts));
	// Tear the session down so the sink releases its resources and can be reconnected
	// cleanly instead of leaving a half-open AVDTP session that never reaches Streaming.
	L2capCloseSignalingChannel(ctx);
}

// Restarts discovery once the ABORT sent for a failed media open has been answered.
static void ContinueMediaOpenRetry(DEVICE_CONTEXT* ctx)
{
	const auto state = ctx->Session.State();
	if (state == Avdtp::SessionState::Idle)
	{
		ctx->MediaOpenRetry = FALSE;
		OA2DP_INFO(ctx, "Signaling", "stream aborted; rediscovering for media open attempt %u",
			static_cast<unsigned>(ctx->MediaOpenAttempts + 1));
		Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}
	else if (state == Avdtp::SessionState::Failed)
	{
		ctx->MediaOpenRetry = FALSE;
		OA2DP_ERROR(ctx, "Signaling", "ABORT for the media open retry failed; tearing down the signaling channel");
		L2capCloseSignalingChannel(ctx);
	}
	else if (state != Avdtp::SessionState::Aborting)
	{
		ctx->MediaOpenRetry = FALSE; // the sink moved the session on by itself
	}
}

// True while Windows is running the render stream.
static bool AudioRunActive(DEVICE_CONTEXT* ctx)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const bool running = ctx->AudioRunning != FALSE;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return running;
}

// The codec the current preference selects, or false when it cannot be honoured
// (for example AAC forced while no encoder service is registered). RemoteOffers is
// the previous discovery's view; after a reconnect the new discovery decides.
static bool DesiredCodec(DEVICE_CONTEXT* ctx, Avdtp::CodecType& codec)
{
	Ioctl::Preference preference;
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		preference = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	}
	const bool forceCodec = (preference.flags & Ioctl::kPreferenceForceCodec) != 0;
	if (forceCodec)
	{
		if (preference.codec == Ioctl::kCodecSbc)
		{
			codec = Avdtp::CodecType::Sbc;
			return true;
		}
		if (preference.codec != Ioctl::kCodecAac || !ServiceRegistered(ctx) || !ctx->Session.RemoteOffers(Avdtp::CodecType::Aac))
		{
			return false;
		}
		codec = Avdtp::CodecType::Aac;
		return true;
	}
	// Automatic order: AAC when the sink offers it and an encoder service is up.
	codec = ServiceRegistered(ctx) && ctx->Session.RemoteOffers(Avdtp::CodecType::Aac)
		? Avdtp::CodecType::Aac
		: Avdtp::CodecType::Sbc;
	return true;
}

// A codec selection input changed (SET_PREFERENCE or REGISTER_ENCODER). Every session
// transition re-runs the work item, so the decision waits for a settled state:
// Streaming with the wrong codec is CLOSEd and, once the CLOSE is answered,
// rediscovered. Any other outcome drops the request. The close handshake is resumed
// through CodecSwitchClosing, because the latched event is consumed when the CLOSE
// goes out and no new event marks the response.
static void ContinueCodecSwitch(DEVICE_CONTEXT* ctx)
{
	const auto state = ctx->Session.State();
	if (ctx->CodecSwitchClosing)
	{
		if (state == Avdtp::SessionState::Closing)
		{
			return;
		}
		ctx->CodecSwitchClosing = FALSE;
		ctx->CodecSwitchWanted = FALSE;
		if (state == Avdtp::SessionState::Idle && !ctx->MediaOpenRetry)
		{
			Avdtp::CodecType desired;
			if (DesiredCodec(ctx, desired))
			{
				OA2DP_INFO(ctx, "Signaling", "stream closed; rediscovering to negotiate %s", CodecName(desired));
				Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
			}
		}
		return;
	}
	if (state != Avdtp::SessionState::Streaming && state != Avdtp::SessionState::Idle &&
		state != Avdtp::SessionState::Failed)
	{
		return; // not settled yet; the next transition re-runs this
	}
	ctx->CodecSwitchWanted = FALSE;
	if (state != Avdtp::SessionState::Streaming)
	{
		return; // Idle/Failed: the next discovery already sees the current preference
	}
	Avdtp::CodecType desired;
	if (!DesiredCodec(ctx, desired))
	{
		return;
	}
	const bool codecDiffers = ctx->Session.SelectedCodec() != desired;
	const LONG parameters = InterlockedCompareExchange(&ctx->PreferenceParameters, 0, 0);
	const bool parametersDiffer = ctx->AppliedPreferenceParameters != parameters;
	if (!codecDiffers && !parametersDiffer)
	{
		return;
	}
	if (codecDiffers)
	{
		OA2DP_INFO(ctx, "Signaling", "%s stream selected but the preference wants %s; closing to renegotiate",
			CodecName(ctx->Session.SelectedCodec()), CodecName(desired));
	}
	else
	{
		OA2DP_INFO(ctx, "Signaling", "%s parameters changed; closing to renegotiate", CodecName(desired));
	}
	ctx->AppliedPreferenceParameters = parameters;
	Transmit(ctx, ctx->Session.Close(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	ctx->CodecSwitchClosing = ctx->Session.State() == Avdtp::SessionState::Closing;
	if (ctx->CodecSwitchClosing)
	{
		// Until a stream runs the new codec, a teardown by the sink still belongs to
		// this switch and is recovered by reconnecting.
		ctx->CodecSwitchRecover = TRUE;
		ctx->CodecSwitchReconnects = 0;
	}
}

void OnSignalingWorkItem(_In_ WDFWORKITEM workItem)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfWorkItemGetParentObject(workItem));
	WdfWaitLockAcquire(ctx->SessionLock, nullptr);
	ctx->SignalingWorkItemThread = KeGetCurrentThread();
	PumpSignalSends(ctx);

	if (InterlockedExchange(&ctx->ConnectPending, 0) != 0 && ctx->SignalingState == ChannelOpen)
	{
		AudioNotifyLinkState(ctx);
		// A service that registers after this discovery renegotiates the stream as AAC
		// (SignalingOnEncoderRegistered), so nothing waits for it here.
		if (ctx->SignalingState == ChannelOpen)
		{
			ResetSession(ctx);
			if (InterlockedCompareExchange(&ctx->SignalingIncoming, 0, 0) != 0)
			{
				// The sink connected to us and normally starts its own AVDTP
				// discovery and configuration at once. Starting ours too makes both
				// sides configure the same stream: the sink can then get its own
				// SET_CONFIGURATION rejected (SEP in use) and refuse the media
				// channel with "no resources". Listen first; the library accepts the
				// sink's configuration, and we discover only when Windows plays
				// audio while the session is still Idle (here or SignalingOnAudioRun).
				if (AudioRunActive(ctx))
				{
					OA2DP_INFO(ctx, "Signaling", "sink-initiated with audio running; starting discovery");
					Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
				}
				else
				{
					OA2DP_INFO(ctx, "Signaling", "sink-initiated; waiting for it to configure the stream or for audio");
				}
				SubmitRead(ctx);
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
					OA2DP_INFO(ctx, "AVDTP", "discover SEP seid %u inUse %u media %u sepType %u",
						static_cast<unsigned>(ctx->ReadBuffer[pos] >> 2), static_cast<unsigned>((ctx->ReadBuffer[pos] >> 1) & 1),
						static_cast<unsigned>(ctx->ReadBuffer[pos + 1] >> 4), static_cast<unsigned>((ctx->ReadBuffer[pos + 1] >> 3) & 1));
				}
			}
			if ((ctx->Session.State() == Avdtp::SessionState::Aborting || ctx->Session.State() == Avdtp::SessionState::Closing) &&
				length >= 2 && ctx->ReadBuffer[1] == static_cast<uint8_t>(ctx->Session.State() == Avdtp::SessionState::Closing
					? Avdtp::Signal::Close : Avdtp::Signal::Abort))
			{
				const unsigned responseType = ctx->ReadBuffer[0] & 0x03;
				OA2DP_INFO(ctx, "AVDTP", "%s response: %s (type %u), length %u, code 0x%02X",
					ctx->Session.State() == Avdtp::SessionState::Closing ? "CLOSE" : "ABORT",
					responseType == 2 ? "accepted" : (responseType == 3 ? "rejected" : "unexpected"), responseType,
					static_cast<unsigned>(length), static_cast<unsigned>(length > 2 ? ctx->ReadBuffer[2] : 0));
			}
			Transmit(ctx, ctx->Session.OnPacket(ctx->ReadBuffer, length, ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		}
		SubmitRead(ctx);
	}

	if (InterlockedExchange(&ctx->AudioRunPending, 0) != 0 && ctx->SignalingState == ChannelOpen &&
		InterlockedCompareExchange(&ctx->SignalingIncoming, 0, 0) != 0 &&
		ctx->Session.State() == Avdtp::SessionState::Idle)
	{
		OA2DP_INFO(ctx, "Signaling", "audio started on a silent sink-initiated link; starting discovery");
		Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}

	if (InterlockedExchange(&ctx->StartPending, 0) != 0 && ctx->Session.State() == Avdtp::SessionState::Open &&
		ctx->MediaChannel != nullptr)
	{
		Transmit(ctx, ctx->Session.Start(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}

	if (InterlockedExchange(&ctx->SuspendPending, 0) != 0 && ctx->Session.State() == Avdtp::SessionState::Streaming)
	{
		OA2DP_INFO(ctx, "AVDTP", "suspending stream");
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
			OA2DP_WARN(ctx, "AVDTP", "no response within %u ms in state %s (%u)", static_cast<unsigned>(ResponseTimeoutMs),
				AvdtpStateName(ctx->Session.State()), static_cast<unsigned>(ctx->Session.State()));
			Transmit(ctx, ctx->Session.OnTimeout(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		}
	}

	if (InterlockedExchange(&ctx->MediaOpenPending, 0) != 0 && ctx->SignalingState == ChannelOpen &&
		ctx->Session.State() == Avdtp::SessionState::Open && ctx->MediaChannel == nullptr && ctx->MediaOpenOp == nullptr)
	{
		StartMediaOpen(ctx);
	}

	if (InterlockedExchange(&ctx->MediaOpenCompleted, 0) != 0 && ctx->SignalingState == ChannelOpen)
	{
		FinishMediaOpen(ctx);
	}

	if (ctx->MediaOpenRetry && ctx->SignalingState == ChannelOpen)
	{
		ContinueMediaOpenRetry(ctx);
	}

	if (InterlockedExchange(&ctx->CodecSwitchPending, 0) != 0)
	{
		ctx->CodecSwitchWanted = TRUE;
	}
	// A close in flight keeps the decision alive with no event of its own.
	if ((ctx->CodecSwitchWanted || ctx->CodecSwitchClosing) && ctx->SignalingState == ChannelOpen)
	{
		ContinueCodecSwitch(ctx);
	}

	ctx->SignalingWorkItemThread = nullptr;
	WdfWaitLockRelease(ctx->SessionLock);
}
} // namespace OpenA2dpDriver::Signaling
