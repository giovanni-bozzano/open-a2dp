#include "Driver.h"

using namespace OpenA2dp;

namespace
{

constexpr ULONG MediaPeriodMs = 10;
// Samples sent ahead of real time so the sink's jitter buffer fills.
constexpr uint32_t PrerollMs = 60;
constexpr int MaxPacketsPerTick = 8;

EVT_WDF_TIMER OnMediaTimer;
EVT_WDF_WORKITEM OnMediaWorkItem;

void OnMediaTimer(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	WdfWorkItemEnqueue(ctx->MediaWorkItem);
}

// Builds one RTP + SBC media packet into MediaBuffer. Caller holds MediaLock.
ULONG BuildPacket(DEVICE_CONTEXT* ctx, uint32_t& samples)
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

void OnMediaWorkItem(_In_ WDFWORKITEM workItem)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfWorkItemGetParentObject(workItem));
	WdfWaitLockAcquire(ctx->MediaLock, nullptr);

	if (ctx->MediaActive && ctx->MediaChannel != nullptr)
	{
		const uint32_t rate = ctx->Encoder.GetParams().sampleRate;
		const ULONGLONG elapsed = KeQueryInterruptTime() - ctx->MediaStartTime;
		const ULONGLONG target = elapsed * rate / 10000000ull + static_cast<ULONGLONG>(rate) * PrerollMs / 1000;

		for (int i = 0; i < MaxPacketsPerTick && ctx->MediaSamplesSent < target; ++i)
		{
			uint32_t samples = 0;
			const ULONG length = BuildPacket(ctx, samples);
			if (length == 0)
			{
				OA2DP_LOG("media packet build failed");
				ctx->MediaActive = FALSE;
				break;
			}
			const NTSTATUS status = L2capSend(ctx, ctx->MediaChannel, ctx->MediaBuffer, length);
			if (!NT_SUCCESS(status))
			{
				OA2DP_LOG("media send failed 0x%08X after %u packets", status, ctx->RtpSequence);
				ctx->MediaActive = FALSE;
				SignalingRequestSuspend(ctx);
				break;
			}
			ctx->MediaSamplesSent += samples;
			if ((ctx->RtpSequence & 0xFF) == 0)
			{
				OA2DP_LOG("media: %u packets sent", ctx->RtpSequence);
			}
		}

		if (!ctx->MediaActive)
		{
			WdfTimerStop(ctx->MediaTimer, FALSE);
		}
	}

	WdfWaitLockRelease(ctx->MediaLock);
}

} // namespace

NTSTATUS MediaInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;

	NTSTATUS status = WdfWaitLockCreate(&attrs, &ctx->MediaLock);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_WORKITEM_CONFIG workConfig;
	WDF_WORKITEM_CONFIG_INIT(&workConfig, OnMediaWorkItem);
	status = WdfWorkItemCreate(&workConfig, &attrs, &ctx->MediaWorkItem);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_TIMER_CONFIG timerConfig;
	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, OnMediaTimer, MediaPeriodMs);
	return WdfTimerCreate(&timerConfig, &attrs, &ctx->MediaTimer);
}

void MediaStart(_In_ DEVICE_CONTEXT* ctx, _In_reads_bytes_(ieLength) const UCHAR* ie, _In_ SIZE_T ieLength)
{
	SbcFixed::Params params;
	if (!SbcFixed::ParseIe(ie, ieLength, params) || params.sampleRate != AudioSampleRate || params.Channels() != AudioChannels)
	{
		OA2DP_LOG("media: unsupported SBC configuration");
		return;
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->Encoder.Reset(params);
	ctx->RtpSequence = 0;
	ctx->RtpTimestamp = 0;
	ctx->MediaSamplesSent = 0;
	ctx->MediaStartTime = KeQueryInterruptTime();
	ctx->MediaActive = TRUE;
	WdfWaitLockRelease(ctx->MediaLock);

	OA2DP_LOG("media: endpoint PCM, SBC %u Hz blocks %u subbands %u mode %u bitpool %u, frame %u bytes",
		params.sampleRate, params.blocks, params.subbands, static_cast<unsigned>(params.channelMode),
		params.bitpool, static_cast<unsigned>(SbcFixed::FrameLength(params)));
	WdfTimerStart(ctx->MediaTimer, 0);
}

void MediaStop(_In_ DEVICE_CONTEXT* ctx)
{
	WdfTimerStop(ctx->MediaTimer, TRUE);
	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	if (ctx->MediaActive)
	{
		OA2DP_LOG("media stopped after %u packets", ctx->RtpSequence);
	}
	ctx->MediaActive = FALSE;
	WdfWaitLockRelease(ctx->MediaLock);
}
