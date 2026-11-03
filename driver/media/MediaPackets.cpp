#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
void OnMediaTimer(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	WdfWorkItemEnqueue(ctx->MediaWorkItem);
}

// Caller holds MediaLock. False when the previous send has not completed yet, in which
// case this tick sends nothing and nothing is consumed. A send that stays pending for
// MediaBusyLimitTicks ticks ends the stream instead of leaving it silently stuck.
bool MediaSendReady(DEVICE_CONTEXT* ctx)
{
	if (L2capSendIdle(&ctx->MediaSend))
	{
		ctx->MediaBusyTicks = 0;
		return true;
	}
	if (++ctx->MediaBusyTicks == MediaBusyLimitTicks)
	{
		OA2DP_WARN(ctx, "Media", "the previous send is still pending in the transport after %u ms; stopping",
			MediaBusyLimitTicks * MediaPeriodMs);
		InterlockedIncrement(&ctx->SendFailures);
		InterlockedExchange(&ctx->LastMediaError, STATUS_IO_TIMEOUT);
		ctx->MediaActive = FALSE;
		SignalingRequestSuspend(ctx);
	}
	return false;
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

// Builds one RTP + AAC/LATM media packet from the frame ring. Caller holds MediaLock.
// One record is one packet: the service already wrote a complete AudioMuxElement.
ULONG BuildAacPacket(DEVICE_CONTEXT* ctx, uint32_t& samples)
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
