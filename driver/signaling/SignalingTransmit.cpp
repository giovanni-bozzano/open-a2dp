#include "SignalingInternal.h"

namespace OpenA2dpDriver::Signaling
{
static void LogSendFailure(DEVICE_CONTEXT* ctx, NTSTATUS status)
{
	if (IsLinkLoss(ctx, status))
	{
		OA2DP_INFO(ctx, "Signaling", "send stopped, device link is down (0x%08X)", status);
	}
	else
	{
		OA2DP_ERROR(ctx, "Signaling", "send failed 0x%08X", status);
	}
}

// Submits one packet to the idle send slot. A lost signaling packet surfaces through
// the response timer (commands) or the peer's own timeout (responses).
static void SubmitSignal(DEVICE_CONTEXT* ctx, const UCHAR* data, ULONG length)
{
	const NTSTATUS status = L2capSendAsync(ctx, &ctx->SignalSend, ctx->SignalingChannel, data, length);
	if (NT_SUCCESS(status))
	{
		ctx->SignalSendUnchecked = TRUE;
	}
	else
	{
		LogSendFailure(ctx, status);
	}
	if (length >= 2 && (data[1] == static_cast<uint8_t>(Avdtp::Signal::Close) ||
		data[1] == static_cast<uint8_t>(Avdtp::Signal::Abort)))
	{
		OA2DP_INFO(ctx, "AVDTP", "%s sent (%u bytes), submit status 0x%08X",
			data[1] == static_cast<uint8_t>(Avdtp::Signal::Close) ? "CLOSE" : "ABORT", static_cast<unsigned>(length), status);
	}
}

void PumpSignalSends(DEVICE_CONTEXT* ctx)
{
	if (!L2capSendIdle(&ctx->SignalSend))
	{
		return; // its completion enqueues the work item again
	}
	if (ctx->SignalSendUnchecked)
	{
		ctx->SignalSendUnchecked = FALSE;
		const NTSTATUS status = static_cast<NTSTATUS>(InterlockedCompareExchange(&ctx->SignalSend.Status, 0, 0));
		if (!NT_SUCCESS(status))
		{
			LogSendFailure(ctx, status);
		}
	}
	if (ctx->SignalingState != ChannelOpen)
	{
		ctx->SignalQueueCount = 0;
		return;
	}
	while (ctx->SignalQueueCount != 0 && L2capSendIdle(&ctx->SignalSend))
	{
		const SIGNAL_PACKET& packet = ctx->SignalQueue[ctx->SignalQueueHead];
		ctx->SignalQueueHead = (ctx->SignalQueueHead + 1) % SignalQueueDepth;
		--ctx->SignalQueueCount;
		SubmitSignal(ctx, packet.Data, packet.Length);
	}
}

// Sends what the session produced and arms or stops the response timer. Caller holds SessionLock.
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
		PumpSignalSends(ctx);
		if (ctx->SignalQueueCount == 0 && L2capSendIdle(&ctx->SignalSend))
		{
			SubmitSignal(ctx, ctx->WriteBuffer, static_cast<ULONG>(length));
		}
		else if (ctx->SignalQueueCount < SignalQueueDepth)
		{
			// Wait for the send in flight; never block SessionLock on the transport.
			SIGNAL_PACKET& packet = ctx->SignalQueue[(ctx->SignalQueueHead + ctx->SignalQueueCount) % SignalQueueDepth];
			packet.Length = static_cast<ULONG>(length);
			RtlCopyMemory(packet.Data, ctx->WriteBuffer, length);
			++ctx->SignalQueueCount;
		}
		else
		{
			OA2DP_ERROR(ctx, "Signaling", "send queue full; dropping a %u-byte packet", static_cast<unsigned>(length));
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
				// The work item starts the media channel open once it has re-armed the read;
				// the open's completion re-enters it to send START.
				InterlockedExchange(&ctx->MediaOpenPending, 1);
			}
		}
		else if (state == Avdtp::SessionState::Streaming)
		{
			OA2DP_INFO(ctx, "AVDTP", "streaming, media MTU %u", ctx->MediaOutMtu);
			ctx->MediaOpenAttempts = 0;
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
