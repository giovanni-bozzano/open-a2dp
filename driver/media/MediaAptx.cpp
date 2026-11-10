#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
// Validates the configured aptX IE and hands the stream to the encoder service. The service
// reads the endpoint's 16-bit PCM from the ring and writes records whose payload is the
// concatenated 4-byte frames; the driver only adds the RTP header.
void StartAptx(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength)
{
	uint8_t capabilities = 0;
	if (!Aptx::ParseIe(ie, ieLength, capabilities))
	{
		OA2DP_WARN(ctx, "Media", "invalid aptX configuration");
		return;
	}
	const uint32_t sampleRate = (capabilities & Aptx::kSampleRate44100) != 0 ? 44100u : 48000u;
	// The encoder is 16-bit stereo by definition; the endpoint's ring is what it reads, so a
	// different depth cannot be fed to it without a conversion this path deliberately does not
	// do.
	if (sampleRate != ctx->PinSampleRate || ctx->PinChannels != 2 ||
		ctx->PinValidBits != Aptx::kBitsPerSample)
	{
		OA2DP_WARN(ctx, "Media",
			"aptX %u Hz stereo 16-bit does not match the endpoint format %u Hz %u ch %u-bit",
			sampleRate, ctx->PinSampleRate, static_cast<unsigned>(ctx->PinChannels),
			static_cast<unsigned>(ctx->PinValidBits));
		return;
	}
	if (!ServiceCodecRegistered(ctx, Ioctl::kCodecAptx))
	{
		OA2DP_WARN(ctx, "Media", "aptX configured but no aptX encoder is registered");
		return;
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->ActiveCodec = Ioctl::kCodecAptx;
	ctx->MediaBusyTicks = 0;
	ctx->MediaSendUnchecked = FALSE;
	ctx->ActiveSampleRate = sampleRate;
	ctx->ActiveChannels = 2;
	// Four bytes per frame of four samples per channel: eight bits per sample pair.
	ctx->ActiveBitrate = sampleRate * 8;
	ctx->ActiveObjectType = 0;
	ctx->RtpSequence = 0;
	ctx->RtpTimestamp = 0;
	ctx->MediaSamplesSent = 0;
	ctx->MediaStartTime = KeQueryInterruptTime();
	ctx->ServiceLastFrameTime = ctx->MediaStartTime;
	ctx->ServiceLastUnderrunLog = 0;
	InterlockedExchange(&ctx->StreamFault, 0);
	ServiceFlushRings(ctx);
	ServiceStreamStart(ctx);
	InterlockedExchange(&ctx->MediaMode, MediaModeService);
	ctx->MediaActive = TRUE;
	WdfWaitLockRelease(ctx->MediaLock);

	OA2DP_INFO(ctx, "Media", "aptX %u Hz stereo 16-bit, %u bps, frames from service", sampleRate,
		ctx->ActiveBitrate);
	WdfTimerStart(ctx->MediaTimer, 0);
}
} // namespace OpenA2dpDriver::Media
