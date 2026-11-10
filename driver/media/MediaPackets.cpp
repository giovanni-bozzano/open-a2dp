#include "MediaInternal.h"

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
		OA2DP_INFO(ctx, "Media", "stopped, sink link is down (0x%08X) after %u packets", status, ctx->RtpSequence);
	}
	else
	{
		OA2DP_ERROR(ctx, "Media", "send failed 0x%08X after %u packets", status, ctx->RtpSequence);
		InterlockedIncrement(&ctx->SendFailures);
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

// Builds one RTP + SBC media packet into MediaBuffer. Caller holds MediaLock.
ULONG BuildSbcPacket(DEVICE_CONTEXT* ctx, uint32_t& samples)
{
	const SbcFixed::Params& p = ctx->Encoder.GetParams();
	const size_t frameCount = SbcFixed::FramesPerPacket(p, ctx->MediaOutMtu - Rtp::kHeaderSize);
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
	for (size_t i = 0; i < frameCount; ++i)
	{
		(void)AudioReadPcm(ctx, ctx->PcmBuffer, spf);
		const size_t written = ctx->Encoder.Encode(ctx->PcmBuffer, ctx->MediaBuffer + offset, sizeof(ctx->MediaBuffer) - offset);
		if (written == 0)
		{
			return 0;
		}
		offset += written;
	}

	samples = static_cast<uint32_t>(frameCount * spf);
	++ctx->RtpSequence;
	ctx->RtpTimestamp += samples;
	return static_cast<ULONG>(offset);
}

// Builds one RTP packet from the frame ring. Caller holds MediaLock. One record is one
// packet: the service already wrote the codec's media payload (a LATM AudioMuxElement, or
// the concatenated aptX HD frames) with the sample count it covers.
ULONG BuildServicePacket(DEVICE_CONTEXT* ctx, uint32_t& samples)
{
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
	ctx->RtpTimestamp += frameSamples;
	return static_cast<ULONG>(offset + length);
}
} // namespace OpenA2dpDriver::Media
