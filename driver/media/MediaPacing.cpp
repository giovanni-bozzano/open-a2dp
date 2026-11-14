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
uint32_t SbcPacketSamples(DEVICE_CONTEXT* ctx)
{
	const SbcFixed::Params& params = ctx->Encoder.GetParams();
	const size_t frames =
		SbcFixed::FramesPerPacket(params, MediaPayloadCapacity(ctx), ctx->SbcFramesPerPacket);
	return static_cast<uint32_t>(frames * params.SamplesPerFrame());
}

// A lead shorter than one access unit cannot be served: the pacer asks for a packet's audio
// before the producer has had an interval to fill it, so the deficit is padded with silence on
// every packet - an underrun no sink and no amount of CPU could avoid. The lead is held at one
// unit plus the copy tick that has to produce it. This is a floor, never a target: a stored lead
// above it is used exactly as stored, so the setting still names the latency it produces.
void ApplyLeadFloor(DEVICE_CONTEXT* ctx, uint32_t unitSamples)
{
	const uint32_t rate = ctx->ActiveSampleRate;
	const uint32_t floorMs = LatencyBudget::LeadFloorMs(unitSamples, rate);
	if (floorMs == 0 || ctx->PacingLeadMs >= floorMs)
	{
		return;
	}
	OA2DP_INFO(ctx, "Media", "stored pacing lead %u ms is shorter than one %s unit (%u samples, %u ms); using %u ms",
		ctx->PacingLeadMs, CodecValueName(ctx->ActiveCodec), unitSamples,
		(unitSamples * 1000u + rate - 1) / rate, floorMs);
	ctx->PacingLeadMs = floorMs;
	if (ctx->LatencyPeakMs < static_cast<LONG>(floorMs))
	{
		ctx->LatencyPeakMs = static_cast<LONG>(floorMs);
	}
}

// PCM samples (per channel) the ring holds at the endpoint's format. Caller holds MediaLock.
static uint64_t PcmSamples(DEVICE_CONTEXT* ctx)
{
	const ULONG frameBytes = ctx->PinBytesPerFrame;
	if (frameBytes == 0)
	{
		return 0;
	}
	const RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
	return ring.ReadableBytes() / frameBytes;
}

// SBC: pace from the endpoint clock and encode in the kernel. One packet per run; its
// completion enqueues the next run, so a backlog drains at transport speed. Caller holds MediaLock.
void TickSbc(DEVICE_CONTEXT* ctx, bool tick)
{
	const uint32_t rate = ctx->Encoder.GetParams().sampleRate;
	const ULONGLONG target = TargetSamples(ctx, rate);
	if (ctx->MediaSamplesSent >= target)
	{
		return;
	}
	if (!MediaSendReady(ctx, tick))
	{
		return;
	}
	// A packet is built from its own PCM: the copy wakes this work item as soon as the ring holds
	// it, and one built earlier would pad audio that is about to arrive with silence. A paused
	// endpoint asks for nothing, so its packets are padded to keep the stream's place.
	const uint32_t packetSamples = SbcPacketSamples(ctx);
	const uint32_t frameSamples = ctx->Encoder.GetParams().SamplesPerFrame();
	const uint64_t buffered = PcmSamples(ctx);
	const uint64_t lead = static_cast<uint64_t>(rate) * ctx->PacingLeadMs / 1000;
	if (LatencyBudget::Stalled(target - ctx->MediaSamplesSent,
			buffered + static_cast<uint64_t>(ctx->SbcLeadSilenceFrames) * frameSamples, lead, packetSamples, rate))
	{
		// The transport held the stream off for longer than the sink can buffer and the copy trimmed
		// the ring back to the lead meanwhile. Padding the gap would send it as a burst of silence the
		// sink then plays late for the rest of the stream; the stream picks up at the lead instead,
		// with the RTP timestamp past the gap.
		const LatencyBudget::Restart restart =
			LatencyBudget::RestartAt(target - ctx->MediaSamplesSent, buffered, lead);
		ctx->MediaSamplesSent += restart.skipSamples;
		ctx->RtpTimestamp += static_cast<ULONG>(restart.skipSamples);
		ctx->SbcLeadSilenceFrames = static_cast<ULONG>(restart.leadSilenceSamples / frameSamples);
		OA2DP_WARN(ctx, "Media", "stream was held off %I64u ms; resuming %u ms ahead of the clock",
			restart.skipSamples * 1000 / rate, ctx->PacingLeadMs);
	}
	const uint64_t available = buffered + static_cast<uint64_t>(ctx->SbcLeadSilenceFrames) * frameSamples;
	if (AudioStreamRunning(ctx) &&
		!LatencyBudget::UnitReady(available, packetSamples, target - ctx->MediaSamplesSent, rate))
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
		// Nothing can carry this stream - no MTU to fill, or an encoder that refused the frame -
		// so it ends here and signaling takes the sink out of Streaming, instead of leaving a
		// session that claims to play and sends nothing.
		OA2DP_ERROR(ctx, "Media", "packet build failed; suspending the stream");
		SetMediaActive(ctx, false);
		AbandonStream(ctx);
		return;
	}
	if (!SubmitPacket(ctx, length))
	{
		return;
	}
	ctx->MediaSamplesSent += samples;
	// Counted only now, so the counters describe the packets the transport accepted.
	++ctx->RtpSequence;
	++ctx->PacketsSent;
	ctx->RtpTimestamp += samples;
}

// Service-encoded codecs (AAC, aptX): drain the frame ring, one packet per run. The service
// owns the encoder clock; the pacer target here only decides when a wake has audio to produce
// and accounts underruns. Caller holds MediaLock.
void TickService(DEVICE_CONTEXT* ctx, bool tick)
{
	const ULONGLONG now = KeQueryInterruptTime();
	// While the transport still holds the previous packet no record can be consumed, so a growing
	// gap measures the link, not the encoder: move the reference instead of reporting a stall the
	// service did not cause as a codec underrun.
	if (!L2capSendIdle(&ctx->MediaSend))
	{
		ctx->ServiceLastFrameTime = now;
	}
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
			// Counted only now: the record was accepted for submission.
			++ctx->RtpSequence;
			++ctx->PacketsSent;
			ctx->RtpTimestamp += samples;
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
		SetMediaActive(ctx, false);
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
	// And never while the frame ring is already over its limit: the record would only be trimmed
	// away, which is the same gap plus an encode nobody hears, and the backlog would keep growing
	// at the expensive stage. The pacer drains what is queued and TrimFrameBacklog drops it if the
	// transport never catches up, so the stall costs one gap either way and no CPU.
	const uint32_t limitMs =
		LatencyBudget::RingLimitMs(ctx->PacingLeadMs, LatencyBudget::kFrameSlackMs);
	const uint32_t limit = ctx->ActiveSampleRate * limitMs / 1000;
	const bool due = unit == 0 || ctx->ActiveSampleRate == 0 ||
		TargetSamples(ctx, ctx->ActiveSampleRate) >= ctx->MediaSamplesSent + queued + unit;
	if (due && queued < limit)
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
	if (idle >= ServiceSilenceLimit)
	{
		// The service is alive and the endpoint plays, but nothing has come out of it for over a
		// second: the stream cannot be kept, because the sink would buffer silence behind a
		// session that claims to play. Suspending returns the session to Open, where the endpoint
		// edge starts it again - a bounded number of times - or leaves it suspended.
		OA2DP_ERROR(ctx, "Media", "%s produced no frame for %I64u ms; suspending the stream",
			CodecValueName(ctx->ActiveCodec), idle / 10000ull);
		SetMediaActive(ctx, false);
		AbandonStream(ctx);
		return;
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

	// A run that finds the slot idle again after a submit is the first run after that send
	// completed. The completion routine stamped when the transport finished, so this closes the
	// interval as two waits: the link, and the delay until this work item ran. Splitting them is
	// what tells a stalled Bluetooth link from a PASSIVE work item the machine did not schedule;
	// the total alone cannot. Closed here rather than in the completion routine: that one runs at
	// DISPATCH_LEVEL with the slot but no device context, so it cannot tell the media send from
	// the signaling one.
	if (ctx->MediaSendPending && L2capSendIdle(&ctx->MediaSend))
	{
		const ULONGLONG now = KeQueryInterruptTime();
		const LatencyBudget::SendGap gap =
			LatencyBudget::SplitSendGap(ctx->MediaSendStarted, ctx->MediaSend.CompletedAt, now);
		OA2dpNoteWorstMs(gap.transportMs, ctx->WorstSendTransportMs);
		OA2dpNoteWorstMs(gap.resumeMs, ctx->WorstSendResumeMs);
		// One line an outlier, not one a packet: a starved work item would otherwise flood the
		// debug output and slow down the very path being measured.
		if ((gap.transportMs >= MediaSplitLogMs || gap.resumeMs >= MediaSplitLogMs) &&
			now - ctx->MediaSendLogTime >= UnderrunLogInterval)
		{
			ctx->MediaSendLogTime = now;
			OA2DP_INFO(ctx, "Media", "media send %u ms in flight, %u ms until the next run",
				gap.transportMs, gap.resumeMs);
		}
		OA2dpNoteGapMs(ctx->MediaSendStarted, ctx->WorstSendMs, now);
		ctx->MediaSendPending = FALSE;
	}

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
