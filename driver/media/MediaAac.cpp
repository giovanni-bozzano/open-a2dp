#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
// Validates the configured AAC IE and hands the stream to the encoder service.
void StartAac(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength)
{
	AacConfig::Params params;
	if (!AacConfig::ParseConfiguredIe(ie, ieLength, params))
	{
		OA2DP_WARN(ctx, "Media", "invalid AAC configuration");
		return;
	}
	if (params.sampleRate != AudioSampleRate || params.channels != AudioChannels)
	{
		OA2DP_WARN(ctx, "Media", "unsupported AAC format %u Hz %u ch", params.sampleRate, params.channels);
		return;
	}
	if (params.bitrate == 0 || params.bitrate > AacConfig::kMaxBitrate)
	{
		OA2DP_WARN(ctx, "Media", "unsupported AAC bitrate %u", params.bitrate);
		return;
	}
	// The average frame plus the LATM and RTP headers must fit one packet; the
	// service drops and counts the occasional frame that does not.
	const uint32_t average =
		params.bitrate / 8 * Ioctl::kAacSamplesPerFrame / params.sampleRate;
	if (average + 64 > Ioctl::kMaxFramePayload)
	{
		OA2DP_WARN(ctx, "Media", "AAC bitrate %u does not fit the media MTU", params.bitrate);
		return;
	}
	if (!ServiceRegistered(ctx))
	{
		OA2DP_WARN(ctx, "Media", "AAC configured but no encoder service is registered");
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

	OA2DP_INFO(ctx, "Media", "AAC %u Hz %u ch %u bps, object 0x%02X, frames from service", params.sampleRate,
		params.channels, params.bitrate, params.objectType);
	WdfTimerStart(ctx->MediaTimer, 0);
}
} // namespace OpenA2dpDriver::Media
