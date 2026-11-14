#include "MediaInternal.h"

#include <OpenA2dp/FrameRecord.h>

namespace OpenA2dpDriver::Media
{
void OnMediaTimer(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	InterlockedExchange(&ctx->MediaTickPending, 1);
	WdfWorkItemEnqueue(ctx->MediaWorkItem);
}

// Ends the stream after a failed media send. Caller holds MediaLock.
static void MediaSendFailed(DEVICE_CONTEXT* ctx, NTSTATUS status)
{
	if (IsLinkLoss(ctx, status))
	{
		OA2DP_INFO(ctx, "Media", "stopped, sink link is down (0x%08X) after %I64u packets", status, ctx->PacketsSent);
	}
	else
	{
		OA2DP_ERROR(ctx, "Media", "send failed 0x%08X after %I64u packets", status, ctx->PacketsSent);
		InterlockedIncrement64(&ctx->SendFailures);
		InterlockedExchange(&ctx->LastMediaError, status);
	}
	SetMediaActive(ctx, false);
	SignalingRequestSuspend(ctx);
}

// Caller holds MediaLock. True when the slot is free and the previous send succeeded.
// The send completion enqueues the media work item, so a busy slot needs no retry here.
bool MediaSendReady(DEVICE_CONTEXT* ctx, bool tick)
{
	if (!L2capSendIdle(&ctx->MediaSend))
	{
		// Real time, not ticks: the watchdog wakes on demand, so a tick count would stretch
		// the diagnostic with whatever the wake interval happened to be.
		if (tick)
		{
			const ULONGLONG now = KeQueryInterruptTime();
			if (ctx->MediaBusySince == 0)
			{
				ctx->MediaBusySince = now;
			}
			else if (now - ctx->MediaBusySince >= MediaBusyLimit)
			{
				ctx->MediaBusySince = now;
				OA2DP_WARN(ctx, "Media",
					"the previous send is still pending in the transport after %u ms", MediaBusyLimitMs);
			}
		}
		return false;
	}
	ctx->MediaBusySince = 0;
	if (ctx->MediaSendUnchecked)
	{
		ctx->MediaSendUnchecked = FALSE;
		const NTSTATUS status = static_cast<NTSTATUS>(InterlockedCompareExchange(&ctx->MediaSend.Status, 0, 0));
		if (!NT_SUCCESS(status))
		{
			MediaSendFailed(ctx, status);
			return false;
		}
	}
	return true;
}

// Submits the packet in MediaBuffer. Caller holds MediaLock and checked MediaSendReady.
bool SubmitPacket(DEVICE_CONTEXT* ctx, ULONG length)
{
	// A stop clears MediaActive before it waits for the lock, so this run can have built a packet
	// for a stream that was already ended. That packet belongs to no session the sink is in, so it
	// is dropped here rather than sent into a stream the stop has moved past.
	if (!ctx->MediaActive)
	{
		return false;
	}
	// Stamped before the submit: a completion that arrives inline must not predate the start, or
	// the split between transport time and the next run would be unmeasurable for that send.
	ctx->MediaSendStarted = KeQueryInterruptTime();
	const NTSTATUS status = L2capSendAsync(ctx, &ctx->MediaSend, ctx->MediaChannel, ctx->MediaBuffer, length);
	if (!NT_SUCCESS(status))
	{
		MediaSendFailed(ctx, status);
		return false;
	}
	ctx->MediaSendUnchecked = TRUE;
	// The completion work item notices the send left and closes the interval, so a link that
	// buffers or retries becomes a number instead of only a trim the user cannot explain.
	ctx->MediaSendPending = TRUE;
	return true;
}

// A frame a running stream's PCM ring cannot fill is padded with silence: the stream keeps its
// place on the timeline, but the listener hears a gap. Count it in status.underruns and log it at
// most once a second, so a pacing lead shorter than the sink needs is a number rather than only a
// sound.
// Caller holds MediaLock.
static void CountSbcUnderrun(DEVICE_CONTEXT* ctx, uint32_t silentSamples)
{
	InterlockedIncrement64(&ctx->Underruns);
	const ULONGLONG now = KeQueryInterruptTime();
	const ULONG packets = ++ctx->SbcUnderrunPackets;
	if (now - ctx->SbcUnderrunLogTime < UnderrunLogInterval)
	{
		return;
	}
	const uint32_t rate = ctx->Encoder.GetParams().sampleRate;
	const ULONG silentMs =
		rate == 0 ? 0 : static_cast<ULONG>(static_cast<uint64_t>(silentSamples) * 1000 / rate);
	OA2DP_WARN(ctx, "Media", "SBC underrun, %u ms of silence in this packet, %u since the last report",
		silentMs, packets);
	ctx->SbcUnderrunPackets = 0;
	ctx->SbcUnderrunLogTime = now;
}

// Builds one RTP + SBC media packet into MediaBuffer. `dueSamples` is how much of it the media
// clock has already asked for; anything beyond that is normally unavailable because the sink is
// meant to buffer it, so only a gap inside the due window counts as an underrun. Caller holds
// MediaLock.
ULONG BuildSbcPacket(DEVICE_CONTEXT* ctx, uint32_t dueSamples, uint32_t& samples)
{
	const SbcFixed::Params& p = ctx->Encoder.GetParams();
	// A stored cap shortens the packet: less audio buffered per packet, at the cost of more
	// packets per second. 0 keeps the MTU's fill.
	const size_t frameCount =
		SbcFixed::FramesPerPacket(p, MediaPayloadCapacity(ctx), ctx->SbcFramesPerPacket);
	if (frameCount == 0)
	{
		return 0;
	}

	Rtp::Header rtp = {};
	rtp.sequence = ctx->RtpSequence;
	rtp.timestamp = ctx->RtpTimestamp;
	rtp.ssrc = 1;
	rtp.payloadType = Rtp::kDynamicPayloadType;
	size_t offset = Rtp::Write(rtp, ctx->MediaBuffer, sizeof(ctx->MediaBuffer));
	ctx->MediaBuffer[offset++] = SbcFixed::MediaPayloadHeader(frameCount);

	const uint32_t spf = p.SamplesPerFrame();
	uint32_t silentSamples = 0;
	for (size_t i = 0; i < frameCount; ++i)
	{
		if (ctx->SbcLeadSilenceFrames != 0)
		{
			// The lead goes in front of the stream's first audio and is not a short read.
			--ctx->SbcLeadSilenceFrames;
			RtlZeroMemory(ctx->PcmBuffer, sizeof(ctx->PcmBuffer));
		}
		else
		{
			// AudioReadPcm returns frames, which is the same unit as SamplesPerFrame: both count
			// one sample per channel. The shortfall is already samples, so it is added as read.
			const ULONG read = AudioReadPcm(ctx, ctx->PcmBuffer, spf);
			if (read < spf)
			{
				silentSamples += spf - read;
			}
		}
		const size_t written = ctx->Encoder.Encode(ctx->PcmBuffer, ctx->MediaBuffer + offset, sizeof(ctx->MediaBuffer) - offset);
		if (written == 0)
		{
			return 0;
		}
		offset += written;
	}
	// Only silence inside the pacing window is a gap the listener hears; LatencyBudget decides
	// what counts, because a packet's tail lies past the clock and is meant to be buffered.
	// A paused endpoint asks for nothing at all: the pacer still pads these packets so the
	// stream keeps its place, but Windows rendering nothing is a pause, not a starved ring.
	uint32_t missing = 0;
	if (AudioStreamRunning(ctx))
	{
		missing = LatencyBudget::UnderrunSamples(silentSamples, dueSamples);
	}
	if (missing != 0)
	{
		CountSbcUnderrun(ctx, missing);
	}

	samples = static_cast<uint32_t>(frameCount * spf);
	// The sequence, timestamp and count advance where the packet is accepted for submission, not
	// here: a packet the transport refuses was never sent and must not be reported as one.
	return static_cast<ULONG>(offset);
}

// Keeps the frame ring near live audio. A transport stall otherwise leaves a backlog that is
// then handed to the sink at link speed, which it cannot buffer; dropping the oldest records
// costs one gap instead. The record headers carry the sample count, so the backlog is measured
// exactly, and the RTP timestamp moves past the gap so the sink hears one jump, not old audio.
// Caller holds MediaLock.
static void TrimFrameBacklog(DEVICE_CONTEXT* ctx)
{
	if (ctx->FrameRingHeader == nullptr || ctx->ActiveSampleRate == 0)
	{
		return;
	}
	RingBufferView frames(ctx->FrameRingHeader, ctx->FrameRingData);
	// The lead is what the sink is meant to have queued, so only what the ring holds past it is
	// excess worth dropping. As on the PCM side that excess has to persist to be dropped, and it
	// is then dropped back to the lead itself so one trim buys an interval of headroom.
	const uint32_t limitMs =
		LatencyBudget::RingLimitMs(ctx->PacingLeadMs, LatencyBudget::kFrameSlackMs);
	const uint32_t limit = ctx->ActiveSampleRate * limitMs / 1000;
	const uint32_t targetMs =
		LatencyBudget::TrimTargetMs(ctx->PacingLeadMs, LatencyBudget::kFrameSlackMs);
	const uint32_t target = ctx->ActiveSampleRate * targetMs / 1000;
	if (!LatencyBudget::TrimNow(FrameRecord::Buffered(frames) > limit, ctx->FrameTrimStrikes))
	{
		return;
	}
	const uint32_t trimmedSamples = FrameRecord::Trim(frames, target);
	if (trimmedSamples == 0)
	{
		return;
	}
	ctx->RtpTimestamp += trimmedSamples;
	InterlockedIncrement64(&ctx->FramesTrimmed);
	const ULONGLONG now = KeQueryInterruptTime();
	if (now - ctx->FrameTrimLogTime >= UnderrunLogInterval)
	{
		ctx->FrameTrimLogTime = now;
		OA2DP_WARN(ctx, "Media", "dropped %I64u ms of buffered %s to stay live (%I64u records trimmed)",
			trimmedSamples * 1000 / ctx->ActiveSampleRate, CodecValueName(ctx->ActiveCodec),
			static_cast<ULONGLONG>(InterlockedCompareExchange64(&ctx->FramesTrimmed, 0, 0)));
	}
}

// The service says how big one access unit of its codec is by writing records: the sample
// count in each one. That is how much audio one wake has to be worth, so the driver stops
// waking it on every clock tick. Caller holds MediaLock.
static void NoteServiceRecord(DEVICE_CONTEXT* ctx, uint32_t samples)
{
	if (samples != 0)
	{
		// The interval between two records is how long the service had to produce one: the
		// driver-side lead has to cover that, or the ring empties between them.
		OA2dpNoteGapMs(ctx->LastRecordTime, ctx->WorstServiceGapMs, KeQueryInterruptTime());
		ctx->ServiceUnitSamples = samples;
	}
}

// Builds one RTP packet from the frame ring. Caller holds MediaLock. One record is one
// packet: the service already wrote the codec's media payload (a LATM AudioMuxElement, or
// the concatenated aptX HD frames) with the sample count it covers.
ULONG BuildServicePacket(DEVICE_CONTEXT* ctx, uint32_t& samples)
{
	TrimFrameBacklog(ctx);
	const UCHAR* payload = nullptr;
	uint32_t frameSamples = 0;
	uint32_t silenceSamples = 0;
	uint32_t skippedSamples = 0;
	uint32_t flags = 0;
	const ULONG length = ServiceReadFrame(ctx, &payload, &frameSamples, &silenceSamples, &skippedSamples, &flags);
	if (silenceSamples != 0)
	{
		// The service could not fill this record from the ring: the gap is the listener's, so it
		// is counted with the other losses instead of living in the service's own statistics.
		InterlockedAdd64(&ctx->ServiceSilenceSamples, silenceSamples);
	}
	// The service skipped its clock over PCM the copy discarded after a stall: the timeline moves
	// past the gap here, at the record it precedes, whether or not this packet goes out.
	ctx->RtpTimestamp += skippedSamples;
	ctx->MediaSamplesSent += skippedSamples;
	if (length == 0)
	{
		return 0;
	}

	Rtp::Header rtp = {};
	rtp.sequence = ctx->RtpSequence;
	rtp.timestamp = ctx->RtpTimestamp;
	rtp.ssrc = 1;
	rtp.payloadType = Rtp::kDynamicPayloadType;
	const size_t offset = Rtp::Write(rtp, ctx->MediaBuffer, sizeof(ctx->MediaBuffer));
	RtlCopyMemory(ctx->MediaBuffer + offset, payload, length);

	samples = frameSamples;
	NoteServiceRecord(ctx, frameSamples);
	// Advanced by the caller once the packet is submitted; a refused packet was not sent.
	return static_cast<ULONG>(offset + length);
}
} // namespace OpenA2dpDriver::Media
