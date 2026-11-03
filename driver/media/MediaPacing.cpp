#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
// SBC: pace from the endpoint clock and encode in the kernel. Caller holds MediaLock.
void TickSbc(DEVICE_CONTEXT* ctx)
{
	const uint32_t rate = ctx->Encoder.GetParams().sampleRate;
	const ULONGLONG elapsed = KeQueryInterruptTime() - ctx->MediaStartTime;
	const ULONGLONG target = elapsed * rate / 10000000ull + static_cast<ULONGLONG>(rate) * PrerollMs / 1000;

	for (int i = 0; i < MaxPacketsPerTick && ctx->MediaSamplesSent < target; ++i)
	{
		if (!ctx->MediaActive || ctx->MediaChannel == nullptr || !MediaSendReady(ctx))
		{
			return;
		}
		uint32_t samples = 0;
		const ULONG length = BuildSbcPacket(ctx, samples);
		if (length == 0)
		{
			OA2DP_ERROR(ctx, "Media", "packet build failed");
			ctx->MediaActive = FALSE;
			return;
		}
		const NTSTATUS status = L2capSend(ctx, &ctx->MediaSend, ctx->MediaChannel, ctx->MediaBuffer, length, MediaSendTimeoutMs);
		if (!NT_SUCCESS(status))
		{
			if (IsLinkLoss(ctx, status))
			{
				OA2DP_INFO(ctx, "Media", "stopped, headset link is down (0x%08X) after %u packets", status, ctx->RtpSequence);
			}
			else
			{
				OA2DP_ERROR(ctx, "Media", "send failed 0x%08X after %u packets", status, ctx->RtpSequence);
				InterlockedIncrement(&ctx->SendFailures);
				InterlockedExchange(&ctx->LastMediaError, status);
			}
			ctx->MediaActive = FALSE;
			SignalingRequestSuspend(ctx);
			return;
		}
		ctx->MediaSamplesSent += samples;
		if ((ctx->RtpSequence & 0xFF) == 0)
		{
			OA2DP_INFO(ctx, "Media", "%u packets sent", ctx->RtpSequence);
		}
	}
}

// AAC: drain the frame ring. The service owns the media clock, so there is no target.
void TickAac(DEVICE_CONTEXT* ctx)
{
	const ULONGLONG now = KeQueryInterruptTime();
	bool sent = false;
	for (int i = 0; i < MaxPacketsPerTick; ++i)
	{
		if (!ctx->MediaActive || ctx->MediaChannel == nullptr || !MediaSendReady(ctx))
		{
			break;
		}
		uint32_t samples = 0;
		const ULONG length = BuildAacPacket(ctx, samples);
		if (length == 0)
		{
			break;
		}
		const NTSTATUS status = L2capSend(ctx, &ctx->MediaSend, ctx->MediaChannel, ctx->MediaBuffer, length, MediaSendTimeoutMs);
		if (!NT_SUCCESS(status))
		{
			if (IsLinkLoss(ctx, status))
			{
				OA2DP_INFO(ctx, "Media", "AAC stopped, headset link is down (0x%08X) after %u packets", status, ctx->RtpSequence);
			}
			else
			{
				OA2DP_ERROR(ctx, "Media", "AAC send failed 0x%08X after %u packets", status, ctx->RtpSequence);
				InterlockedIncrement(&ctx->SendFailures);
				InterlockedExchange(&ctx->LastMediaError, status);
			}
			ctx->MediaActive = FALSE;
			SignalingRequestSuspend(ctx);
			return;
		}
		sent = true;
		ctx->MediaSamplesSent += samples;
		ctx->AacLastFrameTime = now;
	}

	if (!ctx->MediaActive)
	{
		return;
	}

	if (InterlockedCompareExchange(&ctx->StreamFault, 0, 0) != 0)
	{
		OA2DP_ERROR(ctx, "Media", "AAC frame ring fault, suspending");
		ctx->MediaActive = FALSE;
		SignalingRequestSuspend(ctx);
		return;
	}

	// Wake the service on every tick: for AAC it is the media clock.
	ServiceTick(ctx);
	if (sent)
	{
		return;
	}

	const ULONGLONG idle = now - ctx->AacLastFrameTime;
	if (idle >= StallTimeout)
	{
		OA2DP_WARN(ctx, "Media", "AAC encoder stalled for %I64u ms, suspending", idle / 10000ull);
		InterlockedExchange(&ctx->LastMediaError, STATUS_IO_TIMEOUT);
		ctx->MediaActive = FALSE;
		SignalingRequestSuspend(ctx);
		return;
	}
	if (idle >= UnderrunTimeout && now - ctx->AacLastUnderrunLog >= UnderrunLogInterval)
	{
		ctx->AacLastUnderrunLog = now;
		InterlockedIncrement(&ctx->Underruns);
		OA2DP_WARN(ctx, "Media", "AAC underrun (%I64u ms without a frame)", idle / 10000ull);
	}
}

void OnMediaWorkItem(_In_ WDFWORKITEM workItem)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfWorkItemGetParentObject(workItem));
	// A stop in progress clears MediaActive before it waits for MediaLock. Checking it
	// here, without the lock, means a stream that is being stopped never competes with
	// the stopper for the lock again, however the lock happens to be handed out.
	if (!ctx->MediaActive)
	{
		return;
	}
	WdfWaitLockAcquire(ctx->MediaLock, nullptr);

	if (ctx->MediaActive && ctx->MediaChannel != nullptr)
	{
		if (InterlockedCompareExchange(&ctx->MediaMode, 0, 0) == MediaModeAac)
		{
			TickAac(ctx);
		}
		else
		{
			TickSbc(ctx);
		}

		if (!ctx->MediaActive)
		{
			WdfTimerStop(ctx->MediaTimer, FALSE);
		}
	}

	WdfWaitLockRelease(ctx->MediaLock);
}
} // namespace OpenA2dpDriver::Media
