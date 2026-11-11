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
	if (params.sampleRate != ctx->PinSampleRate || params.channels != ctx->PinChannels)
	{
		OA2DP_WARN(ctx, "Media", "AAC %u Hz %u ch does not match the endpoint format %u Hz %u ch",
			params.sampleRate, params.channels, ctx->PinSampleRate, ctx->PinChannels);
		return;
	}
	if (params.bitrate == 0 || params.bitrate > AacConfig::kMaxBitrate)
	{
		OA2DP_WARN(ctx, "Media", "unsupported AAC bitrate %u", params.bitrate);
		return;
	}
	// One access unit per packet: the bitrate must leave room for the LATM header and the
	// CBR overshoot inside a single media packet, so the ceiling follows the media MTU.
	const size_t payload =
		(ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu) - Rtp::kHeaderSize;
	const uint32_t mtuLimit = AacConfig::MaxBitrateForMtu(payload, params.sampleRate);
	if (params.bitrate > mtuLimit)
	{
		OA2DP_WARN(ctx, "Media", "AAC bitrate %u does not fit the media MTU (max %u)", params.bitrate,
			mtuLimit);
		// The bitrate was picked before this channel opened, so the MTU it assumed can be
		// larger than the one the sink reported. Renegotiate once at a bitrate the link
		// carries instead of leaving the stream silent.
		SignalingRequestMtuRenegotiation(ctx);
		return;
	}
	if (!ServiceRegistered(ctx))
	{
		OA2DP_WARN(ctx, "Media", "AAC configured but no encoder service is registered");
		return;
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->ActiveCodec = Ioctl::kCodecAac;
	ctx->MediaBusySince = 0;
	// Unknown until the first record arrives: every tick until then, as before.
	ctx->ServiceWakeTicks = 0;
	ctx->ServiceWakeSkip = 0;
	ctx->MediaSendUnchecked = FALSE;
	ctx->ActiveSampleRate = params.sampleRate;
	ctx->ActiveChannels = params.channels;
	ctx->ActiveBitrate = params.bitrate;
	ctx->ActiveObjectType = params.objectType;
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

	OA2DP_INFO(ctx, "Media", "AAC %u Hz %u ch %u bps, object 0x%02X, frames from service", params.sampleRate,
		params.channels, params.bitrate, params.objectType);
	WdfTimerStart(ctx->MediaTimer, 0);
}
} // namespace OpenA2dpDriver::Media
