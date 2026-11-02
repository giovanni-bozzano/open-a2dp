#include "Driver.h"
#include "Service.h"

#include <OpenA2dp/AacConfig.h>

using namespace OpenA2dp;

namespace
{

constexpr ULONG MediaPeriodMs = 10;
// Samples sent ahead of real time so the sink's jitter buffer fills.
constexpr uint32_t PrerollMs = 60;
constexpr int MaxPacketsPerTick = 8;
// AAC: no encoded frame for this long counts as an underrun (logged once per second);
// no frame for StallTimeoutMs stops the stream so it reconnects.
constexpr ULONGLONG UnderrunTimeout = 200ull * 10000ull;
constexpr ULONGLONG UnderrunLogInterval = 10000000ull;
constexpr ULONGLONG StallTimeout = 2000ull * 10000ull;
// Ticks (10 ms each) a media send may stay pending in the transport before the stream is stopped.
constexpr ULONG MediaBusyLimitTicks = 300;

EVT_WDF_TIMER OnMediaTimer;
EVT_WDF_WORKITEM OnMediaWorkItem;

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
		OA2DP_LOG("media: the previous send is still pending in the transport after %u ms; stopping",
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
			OA2DP_LOG("media packet build failed");
			ctx->MediaActive = FALSE;
			return;
		}
		const NTSTATUS status = L2capSend(ctx, &ctx->MediaSend, ctx->MediaChannel, ctx->MediaBuffer, length, MediaSendTimeoutMs);
		if (!NT_SUCCESS(status))
		{
			if (IsLinkLoss(ctx, status))
			{
				OA2DP_LOG("media: stopped, headset link is down (0x%08X) after %u packets", status, ctx->RtpSequence);
			}
			else
			{
				OA2DP_LOG("media send failed 0x%08X after %u packets", status, ctx->RtpSequence);
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
			OA2DP_LOG("media: %u packets sent", ctx->RtpSequence);
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
				OA2DP_LOG("media: AAC stopped, headset link is down (0x%08X) after %u packets", status, ctx->RtpSequence);
			}
			else
			{
				OA2DP_LOG("media: AAC send failed 0x%08X after %u packets", status, ctx->RtpSequence);
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
		OA2DP_LOG("media: AAC frame ring fault, suspending");
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
		OA2DP_LOG("media: AAC encoder stalled for %I64u ms, suspending", idle / 10000ull);
		InterlockedExchange(&ctx->LastMediaError, STATUS_IO_TIMEOUT);
		ctx->MediaActive = FALSE;
		SignalingRequestSuspend(ctx);
		return;
	}
	if (idle >= UnderrunTimeout && now - ctx->AacLastUnderrunLog >= UnderrunLogInterval)
	{
		ctx->AacLastUnderrunLog = now;
		InterlockedIncrement(&ctx->Underruns);
		OA2DP_LOG("media: AAC underrun (%I64u ms without a frame)", idle / 10000ull);
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

// Validates the configured AAC IE and hands the stream to the encoder service.
void StartAac(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength)
{
	AacConfig::Params params;
	if (!AacConfig::ParseConfiguredIe(ie, ieLength, params))
	{
		OA2DP_LOG("media: invalid AAC configuration");
		return;
	}
	if (params.sampleRate != AudioSampleRate || params.channels != AudioChannels)
	{
		OA2DP_LOG("media: unsupported AAC format %u Hz %u ch", params.sampleRate, params.channels);
		return;
	}
	if (params.bitrate == 0 || params.bitrate > AacConfig::kMaxBitrate)
	{
		OA2DP_LOG("media: unsupported AAC bitrate %u", params.bitrate);
		return;
	}
	// The average frame plus the LATM and RTP headers must fit one packet; the
	// service drops and counts the occasional frame that does not.
	const uint32_t average =
		params.bitrate / 8 * Ioctl::kAacSamplesPerFrame / params.sampleRate;
	if (average + 64 > Ioctl::kMaxFramePayload)
	{
		OA2DP_LOG("media: AAC bitrate %u does not fit the media MTU", params.bitrate);
		return;
	}
	if (!ServiceRegistered(ctx))
	{
		OA2DP_LOG("media: AAC configured but no encoder service is registered");
		return;
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->ActiveCodec = Ioctl::kCodecAac;
	ctx->MediaBusyTicks = 0;
	ctx->ActiveSampleRate = params.sampleRate;
	ctx->ActiveChannels = params.channels;
	ctx->ActiveBitrate = params.bitrate;
	ctx->ActiveObjectType = params.objectType;
	ctx->RtpSequence = 0;
	ctx->RtpTimestamp = 0;
	ctx->MediaSamplesSent = 0;
	ctx->MediaStartTime = KeQueryInterruptTime();
	ctx->AacLastFrameTime = ctx->MediaStartTime;
	ctx->AacLastUnderrunLog = 0;
	InterlockedExchange(&ctx->StreamFault, 0);
	ServiceFlushRings(ctx);
	ServiceStreamStart(ctx);
	InterlockedExchange(&ctx->MediaMode, MediaModeAac);
	ctx->MediaActive = TRUE;
	WdfWaitLockRelease(ctx->MediaLock);

	OA2DP_LOG("media: AAC %u Hz %u ch %u bps, object 0x%02X, frames from service", params.sampleRate,
		params.channels, params.bitrate, params.objectType);
	WdfTimerStart(ctx->MediaTimer, 0);
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

void MediaStart(_In_ DEVICE_CONTEXT* ctx, Avdtp::CodecType codec, _In_reads_bytes_(ieLength) const UCHAR* ie,
	_In_ SIZE_T ieLength)
{
	if (codec == Avdtp::CodecType::Aac)
	{
		StartAac(ctx, ie, ieLength);
		return;
	}

	SbcFixed::Params params;
	if (!SbcFixed::ParseIe(ie, ieLength, params) || params.sampleRate != AudioSampleRate || params.Channels() != AudioChannels)
	{
		OA2DP_LOG("media: unsupported SBC configuration");
		return;
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->Encoder.Reset(params);
	ctx->MediaBusyTicks = 0;
	ctx->ActiveCodec = Ioctl::kCodecSbc;
	ctx->ActiveSampleRate = params.sampleRate;
	ctx->ActiveChannels = params.Channels();
	ctx->ActiveBitrate = 0;
	ctx->ActiveObjectType = 0;
	ctx->RtpSequence = 0;
	ctx->RtpTimestamp = 0;
	ctx->MediaSamplesSent = 0;
	ctx->MediaStartTime = KeQueryInterruptTime();
	InterlockedExchange(&ctx->StreamFault, 0);
	InterlockedExchange(&ctx->MediaMode, MediaModeSbc);
	ctx->MediaActive = TRUE;
	WdfWaitLockRelease(ctx->MediaLock);

	OA2DP_LOG("media: endpoint PCM, SBC %u Hz blocks %u subbands %u mode %u bitpool %u, frame %u bytes",
		params.sampleRate, params.blocks, params.subbands, static_cast<unsigned>(params.channelMode),
		params.bitpool, static_cast<unsigned>(SbcFixed::FrameLength(params)));
	WdfTimerStart(ctx->MediaTimer, 0);
}

void MediaStop(_In_ DEVICE_CONTEXT* ctx)
{
	// Clear the flag first, atomically and without the lock: the media work item and
	// its per-packet checks read it, so the stream stops within one packet and the
	// work item stops taking MediaLock. In the failing hang the lock was held by a work
	// item that kept running every 10 ms for two minutes while this thread waited.
	const bool wasActive = InterlockedExchange8(reinterpret_cast<volatile CHAR*>(&ctx->MediaActive), 0) != 0;
	WdfTimerStop(ctx->MediaTimer, TRUE);
	// MediaLock is only ever held for one bounded work item run, so this wait ends. It
	// is retried rather than abandoned: stopping without the lock would race a send.
	for (;;)
	{
		LONGLONG lockTimeout = WDF_REL_TIMEOUT_IN_MS(1000);
		if (WdfWaitLockAcquire(ctx->MediaLock, &lockTimeout) == STATUS_SUCCESS)
		{
			break;
		}
		OA2DP_LOG("media stop: still waiting for the media work item to release the lock");
	}
	const LONG mode = InterlockedExchange(&ctx->MediaMode, MediaModeNone);
	if (wasActive)
	{
		OA2DP_LOG("media stopped after %u packets", ctx->RtpSequence);
	}
	WdfWaitLockRelease(ctx->MediaLock);

	InterlockedExchange(&ctx->StreamFault, 0);
	if (mode == MediaModeAac)
	{
		// The driver consumes the frame ring, so it can discard what is left.
		ServiceFlushRings(ctx);
		ServiceStreamStop(ctx);
	}
}
