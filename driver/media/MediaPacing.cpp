#include "MediaInternal.h"

#include <OpenA2dp/FrameRecord.h>

namespace OpenA2dpDriver::Media
{
// Samples the stream must have sent by now: the media clock plus the pacing lead. The pacer,
// the watchdog and the service wake all read it, so none of them can disagree about what is
// due. Caller holds MediaLock.
static ULONGLONG TargetSamples(DEVICE_CONTEXT* ctx, uint32_t rate)
{
	const ULONGLONG elapsed = KeQueryInterruptTime() - ctx->MediaStartTime;
	return elapsed * rate / 10000000ull + static_cast<ULONGLONG>(rate) * ctx->PacingLeadMs / 1000;
}

// PCM samples (per channel) one packet of this stream carries. Caller holds MediaLock.
static uint32_t PacketSamples(DEVICE_CONTEXT* ctx)
{
	const SbcFixed::Params& params = ctx->Encoder.GetParams();
	const size_t frames =
		SbcFixed::FramesPerPacket(params, ctx->MediaOutMtu - Rtp::kHeaderSize, ctx->SbcFramesPerPacket);
	return static_cast<uint32_t>(frames * params.SamplesPerFrame());
}

// True when the PCM ring holds at least the audio of one whole packet. Caller holds MediaLock.
static bool PcmHasPacket(DEVICE_CONTEXT* ctx, uint32_t packetSamples)
{
	const ULONG frameBytes = ctx->PinBytesPerFrame;
	if (packetSamples == 0 || frameBytes == 0)
	{
		return true;
	}
	const RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
	return ring.ReadableBytes() >= static_cast<ULONGLONG>(packetSamples) * frameBytes;
}

// SBC: pace from the endpoint clock and encode in the kernel. One packet per run; its
// completion enqueues the next run, so a backlog drains at transport speed. Caller holds MediaLock.
void TickSbc(DEVICE_CONTEXT* ctx, bool tick)
{
	const ULONGLONG target = TargetSamples(ctx, ctx->Encoder.GetParams().sampleRate);

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
	// A packet built from less than a packet of PCM pads the rest with silence, which is a gap the
	// listener hears. The ring runs level with the clock, so a late copy tick leaves a packet
	// short; waiting moves that audio from the sink's lead into the ring, and the copy wakes this
	// work item as soon as a whole packet is there. The wait is bounded by the copy's own slack:
	// past it the PCM is not late but missing, and holding the packet back would shrink the
	// sink's lead for the rest of the stream without a single underrun being counted. A paused
	// endpoint asks for nothing, so it keeps its place instead.
	const uint32_t packetSamples = PacketSamples(ctx);
	const ULONGLONG waitLimit = packetSamples +
		static_cast<ULONGLONG>(ctx->Encoder.GetParams().sampleRate) * LatencyBudget::kPcmSlackMs / 1000;
	if (ctx->MediaCaughtUp && target - ctx->MediaSamplesSent < waitLimit && AudioStreamRunning(ctx) &&
		!PcmHasPacket(ctx, packetSamples))
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

// Service-encoded codecs (AAC, aptX): drain the frame ring, one packet per run. The service
// owns the encoder clock; the pacer target here only decides when a wake has audio to produce
// and accounts underruns. Caller holds MediaLock.
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
	// One wake produces one access unit, so it is only worth one when a further unit is due
	// beyond what the service has already produced: the records still queued in the ring cover
	// the rest, and a wake that arrives before that only encodes padding. The records say how
	// big a unit is and how much is queued, both without consuming anything.
	uint32_t queued = 0;
	if (ctx->FrameRingHeader != nullptr)
	{
		RingBufferView frames(ctx->FrameRingHeader, ctx->FrameRingData);
		queued = FrameRecord::Buffered(frames);
	}
	const uint32_t unit = ctx->ServiceUnitSamples;
	if (unit == 0 || ctx->ActiveSampleRate == 0 ||
		TargetSamples(ctx, ctx->ActiveSampleRate) >= ctx->MediaSamplesSent + queued + unit)
	{
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
		const ULONGLONG target = TargetSamples(ctx, rate);
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
