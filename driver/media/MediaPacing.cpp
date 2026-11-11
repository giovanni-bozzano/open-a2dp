#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
// SBC: pace from the endpoint clock and encode in the kernel. One packet per run; its
// completion enqueues the next run, so a backlog drains at transport speed. Caller holds MediaLock.
void TickSbc(DEVICE_CONTEXT* ctx, bool tick)
{
	const uint32_t rate = ctx->Encoder.GetParams().sampleRate;
	const ULONGLONG elapsed = KeQueryInterruptTime() - ctx->MediaStartTime;
	const ULONGLONG target =
			elapsed * rate / 10000000ull + static_cast<ULONGLONG>(rate) * ctx->PacingLeadMs / 1000;

	if (ctx->MediaSamplesSent >= target)
	{
		// Caught up with the lead once: from here a short read is starvation, not startup.
		ctx->MediaCaughtUp = TRUE;
		return;
	}
	if (!MediaSendReady(ctx, tick))
	{
		return;
	}
	// Samples the clock says are due now. A packet may carry more than that (it is built whole),
	// and the frames past this point are legitimately audio from the future: the sink buffers
	// them. Only what is missing inside this window is an underrun.
	const ULONGLONG shortfall = target - ctx->MediaSamplesSent;
	const uint32_t due = shortfall > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(shortfall);
	uint32_t samples = 0;
	const ULONG length = BuildSbcPacket(ctx, due, samples);
	if (length == 0)
	{
		OA2DP_ERROR(ctx, "Media", "packet build failed");
		ctx->MediaActive = FALSE;
		return;
	}
	if (!SubmitPacket(ctx, length))
	{
		return;
	}
	ctx->MediaSamplesSent += samples;
	if ((ctx->RtpSequence & 0xFF) == 0)
	{
		OA2DP_INFO(ctx, "Media", "%u packets sent", ctx->RtpSequence);
	}
}

// Service-encoded codecs (AAC, aptX HD): drain the frame ring, one packet per run. The
// service owns the media clock, so there is no target; clock ticks wake it and account
// underruns. Caller holds MediaLock.
void TickService(DEVICE_CONTEXT* ctx, bool tick)
{
	const ULONGLONG now = KeQueryInterruptTime();
	if (MediaSendReady(ctx, tick))
	{
		uint32_t samples = 0;
		const ULONG length = BuildServicePacket(ctx, samples);
		if (length != 0)
		{
			if (!SubmitPacket(ctx, length))
			{
				return;
			}
			ctx->MediaSamplesSent += samples;
			ctx->ServiceLastFrameTime = now;
		}
	}

	if (!ctx->MediaActive)
	{
		return;
	}

	if (InterlockedCompareExchange(&ctx->StreamFault, 0, 0) != 0)
	{
		OA2DP_ERROR(ctx, "Media", "frame ring fault, suspending");
		ctx->MediaActive = FALSE;
		SignalingRequestSuspend(ctx);
		return;
	}

	if (!tick)
	{
		return;
	}
	// Wake the service on every clock tick: it is the media clock for these codecs.
	ServiceTick(ctx);

	const ULONGLONG idle = now - ctx->ServiceLastFrameTime;
	if (idle >= UnderrunTimeout && now - ctx->ServiceLastUnderrunLog >= UnderrunLogInterval)
	{
		ctx->ServiceLastUnderrunLog = now;
		InterlockedIncrement(&ctx->Underruns);		OA2DP_WARN(ctx, "Media", "%s underrun (%I64u ms without a frame)", CodecValueName(ctx->ActiveCodec),
			idle / 10000ull);
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

	// Runs come from the media clock (timer) and from send completions; only clock
	// ticks wake the AAC service and count a busy transport.
	const bool tick = InterlockedExchange(&ctx->MediaTickPending, 0) != 0;
	if (ctx->MediaActive && ctx->MediaChannel != nullptr)
	{
		if (InterlockedCompareExchange(&ctx->MediaMode, 0, 0) == MediaModeService)
		{
			TickService(ctx, tick);
		}
		else
		{
			TickSbc(ctx, tick);
		}

		if (!ctx->MediaActive)
		{
			WdfTimerStop(ctx->MediaTimer, FALSE);
		}
	}

	WdfWaitLockRelease(ctx->MediaLock);
}
} // namespace OpenA2dpDriver::Media
