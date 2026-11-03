#include "SignalingInternal.h"

namespace OpenA2dpDriver::Signaling
{
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
		OA2DP_WARN(ctx, "L2CAP", "media channel open attempt %d failed 0x%08X", attempt + 1, mediaStatus);
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

	OA2DP_ERROR(ctx, "L2CAP", "media channel open failed after retries; tearing down the signaling channel");
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
		ctx->Session.State() == Avdtp::SessionState::Open && ctx->MediaChannel == nullptr)
	{
		OpenMediaTransport(ctx);
	}

	ctx->SignalingWorkItemThread = nullptr;
	WdfWaitLockRelease(ctx->SessionLock);
}
} // namespace OpenA2dpDriver::Signaling
