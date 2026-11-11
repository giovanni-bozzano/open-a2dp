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
	ctx->MediaActive = FALSE;
	SignalingRequestSuspend(ctx);
}

// Caller holds MediaLock. True when the slot is free and the previous send succeeded.
// The send completion enqueues the media work item, so a busy slot needs no retry here.
bool MediaSendReady(DEVICE_CONTEXT* ctx, bool tick)
{
	if (!L2capSendIdle(&ctx->MediaSend))
	{
		if (tick && ++ctx->MediaBusyTicks == MediaBusyLimitTicks)
		{
			OA2DP_WARN(ctx, "Media", "the previous send is still pending in the transport after %u ms",
				MediaBusyLimitMs);
		}
		return false;
	}
	ctx->MediaBusyTicks = 0;
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
	const NTSTATUS status = L2capSendAsync(ctx, &ctx->MediaSend, ctx->MediaChannel, ctx->MediaBuffer, length);
	if (!NT_SUCCESS(status))
	{
		MediaSendFailed(ctx, status);
		return false;
	}
	ctx->MediaSendUnchecked = TRUE;
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
		SbcFixed::FramesPerPacket(p, ctx->MediaOutMtu - Rtp::kHeaderSize, ctx->SbcFramesPerPacket);
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
		const ULONG read = AudioReadPcm(ctx, ctx->PcmBuffer, spf);
		if (read < spf)
		{
			silentSamples += (spf - read) * spf;
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
		missing = LatencyBudget::UnderrunSamples(silentSamples, dueSamples, ctx->MediaCaughtUp != FALSE);
	}
	if (missing != 0)
	{
		CountSbcUnderrun(ctx, missing);
	}

	samples = static_cast<uint32_t>(frameCount * spf);
	++ctx->RtpSequence;
	++ctx->PacketsSent;
	ctx->RtpTimestamp += samples;
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
	// excess worth dropping.
	const uint32_t limitMs =
		LatencyBudget::RingLimitMs(ctx->PacingLeadMs, LatencyBudget::kFrameSlackMs);
	const uint32_t limit = ctx->ActiveSampleRate * limitMs / 1000;
	const uint32_t trimmedSamples = FrameRecord::Trim(frames, limit);
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

// Builds one RTP packet from the frame ring. Caller holds MediaLock. One record is one
// packet: the service already wrote the codec's media payload (a LATM AudioMuxElement, or
// the concatenated aptX HD frames) with the sample count it covers.
ULONG BuildServicePacket(DEVICE_CONTEXT* ctx, uint32_t& samples)
{
	TrimFrameBacklog(ctx);
	const UCHAR* payload = nullptr;
	uint32_t frameSamples = 0;
	uint32_t flags = 0;
	const ULONG length = ServiceReadFrame(ctx, &payload, &frameSamples, &flags);
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
	++ctx->RtpSequence;
	++ctx->PacketsSent;
	ctx->RtpTimestamp += frameSamples;
	return static_cast<ULONG>(offset + length);
}
} // namespace OpenA2dpDriver::Media
