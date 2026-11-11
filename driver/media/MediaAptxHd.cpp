#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
// Validates the configured aptX HD IE and hands the stream to the encoder service. The
// service reads the endpoint's 24-in-32 PCM from the ring and writes records whose payload
// is the concatenated 6-byte frames; the driver only adds the RTP header.
void StartAptxHd(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength)
{
	uint8_t capabilities = 0;
	if (!AptxHd::ParseIe(ie, ieLength, capabilities))
	{
		OA2DP_WARN(ctx, "Media", "invalid aptX HD configuration");
		return;
	}
	const uint32_t sampleRate = (capabilities & AptxHd::kSampleRate44100) != 0 ? 44100u : 48000u;
	// The encoder is 24-in-32 stereo by definition; a different endpoint format cannot be
	// fed to it without a conversion this path deliberately does not do.
	if (sampleRate != ctx->PinSampleRate || ctx->PinChannels != 2 ||
		ctx->PinValidBits != AptxHd::kBitsPerSample)
	{
		OA2DP_WARN(ctx, "Media",
			"aptX HD %u Hz stereo 24-bit does not match the endpoint format %u Hz %u ch %u-bit",
			sampleRate, ctx->PinSampleRate, static_cast<unsigned>(ctx->PinChannels),
			static_cast<unsigned>(ctx->PinValidBits));
		return;
	}
	if (!ServiceCodecRegistered(ctx, Ioctl::kCodecAptxHd))
	{
		OA2DP_WARN(ctx, "Media", "aptX HD configured but no aptX HD encoder is registered");
		return;
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->ActiveCodec = Ioctl::kCodecAptxHd;
	ctx->MediaBusySince = 0;
	ctx->ServiceWakeTicks = 0;
	ctx->ServiceWakeSkip = 0;
	ctx->MediaSendUnchecked = FALSE;
	ctx->ActiveSampleRate = sampleRate;
	ctx->ActiveChannels = 2;
	// Six bytes per frame of four samples per channel.
	ctx->ActiveBitrate = sampleRate * 12;
	ctx->ActiveObjectType = 0;
	ctx->RtpSequence = 0;
	ctx->PacketsSent = 0;
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

	OA2DP_INFO(ctx, "Media", "aptX HD %u Hz stereo 24-bit, %u bps, frames from service", sampleRate,
		ctx->ActiveBitrate);
	WdfTimerStart(ctx->MediaTimer, 0);
}
} // namespace OpenA2dpDriver::Media
