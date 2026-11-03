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
		ctx->MediaOpenRetry = FALSE; // the headset moved the session on by itself
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
		// The control device appears when the device starts and the encoder service
		// attaches on its arrival notification, but a service started later (or slower
		// than the headset) registers after discovery has already run and AAC is lost
		// for the whole connection. Wait for it briefly,
		// but only when the service is installed: a machine without it must connect
		// exactly as fast as before. The wait is bounded and the channel can close
		// underneath us, so re-check both after every step.
		if (!ServiceRegistered(ctx) && ServiceInstalled())
		{
			OA2DP_INFO(ctx, "Signaling", "waiting up to %u ms for the encoder service", kConnectWaitSteps * kConnectWaitMs);
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
			OA2DP_INFO(ctx, "Signaling", "service %s after the wait",
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
				OA2DP_INFO(ctx, "Signaling", "headset-initiated; waiting up to %u ms for it to configure the stream",
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
				OA2DP_INFO(ctx, "Signaling", "starting discovery");
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

	ctx->SignalingWorkItemThread = nullptr;
	WdfWaitLockRelease(ctx->SessionLock);
}
} // namespace OpenA2dpDriver::Signaling
