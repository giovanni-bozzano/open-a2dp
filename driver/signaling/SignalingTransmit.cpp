#include "SignalingInternal.h"

namespace OpenA2dpDriver::Signaling
{
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
				OA2DP_INFO(ctx, "Signaling", "send stopped, headset link is down (0x%08X)", status);
			}
			else
			{
				OA2DP_ERROR(ctx, "Signaling", "send failed 0x%08X", status);
			}
		}
		if (length >= 2 && ctx->WriteBuffer[1] == static_cast<uint8_t>(Avdtp::Signal::Close))
		{
			OA2DP_INFO(ctx, "AVDTP", "CLOSE sent (%u bytes), send status 0x%08X", static_cast<unsigned>(length), status);
		}
		if (length >= 2 && ctx->WriteBuffer[1] == static_cast<uint8_t>(Avdtp::Signal::Abort))
		{
			OA2DP_INFO(ctx, "AVDTP", "ABORT sent (%u bytes), send status 0x%08X", static_cast<unsigned>(length), status);
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
	KeSetEvent(&ctx->SessionChanged, IO_NO_INCREMENT, FALSE);
	if (state != ctx->LoggedState)
	{
		ctx->LoggedState = state;
		OA2DP_INFO(ctx, "AVDTP", "state %s (%u), remote SEID %u", AvdtpStateName(state), static_cast<unsigned>(state), ctx->Session.RemoteSeid());
		if (state == Avdtp::SessionState::Failed && ctx->SignalingState == ChannelOpen)
		{
			const auto reason = ctx->Session.LastDiscoveryFailure();
			if (reason != Avdtp::DiscoveryFailure::None)
			{
				OA2DP_ERROR(ctx, "AVDTP", "discovery failed: %s (reason %u), sink error present %u, code 0x%02X",
					DiscoveryFailureName(reason), static_cast<unsigned>(reason), ctx->Session.HasDiscoveryError() ? 1u : 0u,
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
			OA2DP_INFO(ctx, "AVDTP", "configured codec %s IE %02X %02X %02X %02X",
				CodecName(ctx->Session.SelectedCodec()), ie[0], ie[1], ie[2], ie[3]);

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
			OA2DP_INFO(ctx, "AVDTP", "streaming, media MTU %u", ctx->MediaOutMtu);
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
} // namespace OpenA2dpDriver::Signaling
