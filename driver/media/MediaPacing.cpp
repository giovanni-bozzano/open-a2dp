#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
// Samples the stream must have sent by now: the media clock plus the pacing lead. The pacer
// and the watchdog both read it, so they cannot disagree about when the next packet is due.
// Caller holds MediaLock.
static ULONGLONG SbcTargetSamples(DEVICE_CONTEXT* ctx)
{
	const uint32_t rate = ctx->Encoder.GetParams().sampleRate;
	const ULONGLONG elapsed = KeQueryInterruptTime() - ctx->MediaStartTime;
	return elapsed * rate / 10000000ull + static_cast<ULONGLONG>(rate) * ctx->PacingLeadMs / 1000;
}

// SBC: pace from the endpoint clock and encode in the kernel. One packet per run; its
// completion enqueues the next run, so a backlog drains at transport speed. Caller holds MediaLock.
void TickSbc(DEVICE_CONTEXT* ctx, bool tick)
{
	const ULONGLONG target = SbcTargetSamples(ctx);

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
	if ((ctx->PacketsSent & 0xFF) == 0)
	{
		OA2DP_INFO(ctx, "Media", "%I64u packets sent", ctx->PacketsSent);
	}
}

// Service-encoded codecs (AAC, aptX HD): drain the frame ring, one packet per run. The
// service owns the media clock, so there is no target; clock ticks account underruns and,
// at the codec's access-unit cadence, wake it. Caller holds MediaLock.
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
	// Wake the service when an access unit is due, not on every clock tick: it paces itself
	// against the clock it is handed, so a wake that finds nothing due is a context switch
	// and no audio. Its own records say how long one access unit lasts.
	if (ctx->ServiceWakeSkip != 0)
	{
		--ctx->ServiceWakeSkip;
	}
	else
	{
		ctx->ServiceWakeSkip = ctx->ServiceWakeTicks > 0 ? ctx->ServiceWakeTicks - 1 : 0;
		ServiceTick(ctx);
	}

	if (!AudioStreamRunning(ctx))
	{
		// A paused endpoint has nothing to encode, so the frames the service did not produce are
		// a pause and not a gap; moving the reference keeps the idle span from counting against
		// the first frame after Windows resumes either.
		ctx->ServiceLastFrameTime = now;
		return;
	}
	const ULONGLONG idle = now - ctx->ServiceLastFrameTime;
	if (idle >= UnderrunTimeout && now - ctx->ServiceLastUnderrunLog >= UnderrunLogInterval)
	{
		ctx->ServiceLastUnderrunLog = now;
		InterlockedIncrement64(&ctx->Underruns);
		OA2DP_WARN(ctx, "Media", "%s underrun (%I64u ms without a frame)", CodecValueName(ctx->ActiveCodec),
			idle / 10000ull);
	}
}

// Arms the one-shot media timer for the next wake this stream needs. A pacing stream is driven
// by send completions - each one enqueues the next run - so the timer only has to cover what a
// completion cannot: the moment the next SBC packet is due, and a send that never completes.
// Caller holds MediaLock.
static void ArmMediaTimer(DEVICE_CONTEXT* ctx)
{
	ULONG waitMs = MediaPeriodMs;
	if (L2capSendIdle(&ctx->MediaSend) &&
		InterlockedCompareExchange(&ctx->MediaMode, 0, 0) == MediaModeSbc)
	{
		const uint32_t rate = ctx->Encoder.GetParams().sampleRate;
		const ULONGLONG target = SbcTargetSamples(ctx);
		// A packet carries audio from past the clock; the next one is due when that audio has
		// been played out. Waking then, instead of one fixed period later, means a wake that
		// finds nothing due cannot happen.
		if (rate != 0 && ctx->MediaSamplesSent >= target)
		{
			const ULONGLONG dueMs =
				((ctx->MediaSamplesSent - target) * 1000ull + rate - 1) / rate;
			waitMs = dueMs < 1 ? 1 : static_cast<ULONG>(dueMs);
		}
	}
	WdfTimerStart(ctx->MediaTimer, WDF_REL_TIMEOUT_IN_MS(waitMs));
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
		else
		{
			ArmMediaTimer(ctx);
		}
	}

	WdfWaitLockRelease(ctx->MediaLock);
}
} // namespace OpenA2dpDriver::Media
