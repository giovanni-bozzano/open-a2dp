#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
// Validates the configured aptX IE and hands the stream to the encoder service. The service
// reads the endpoint's 16-bit PCM from the ring and writes records whose payload is the
// concatenated 4-byte frames; the driver only adds the RTP header.
void StartAptx(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength, uint32_t codecValue)
{
	const bool ll = codecValue == Ioctl::kCodecAptxLl;
	uint8_t capabilities = 0;
	const bool parsed = ll ? AptxLl::ParseIe(ie, ieLength, capabilities)
						   : Aptx::ParseIe(ie, ieLength, capabilities);
	if (!parsed)
	{
		OA2DP_WARN(ctx, "Media", "invalid %s configuration", CodecValueName(codecValue));
		AbandonStream(ctx);
		return;
	}
	const uint32_t sampleRate = (capabilities & Aptx::kSampleRate44100) != 0 ? 44100u : 48000u;
	// The encoder is 16-bit stereo by definition; the service narrows a 24-in-32 ring, so the
	// endpoint may run either depth.
	const PinFormat pin = SnapshotPinFormat(ctx);
	const uint16_t bitsPerSample = ll ? AptxLl::kBitsPerSample : Aptx::kBitsPerSample;
	if (sampleRate != pin.sampleRate || pin.channels != 2 ||
		!AptxPinDepthUsable(pin.validBits, bitsPerSample))
	{
		OA2DP_WARN(ctx, "Media",
			"%s %u Hz stereo does not match the endpoint format %u Hz %u ch %u-bit",
			CodecValueName(codecValue), sampleRate, pin.sampleRate,
			static_cast<unsigned>(pin.channels), static_cast<unsigned>(pin.validBits));
		AbandonStream(ctx);
		return;
	}
	if (!ServiceCodecRegistered(ctx, codecValue))
	{
		OA2DP_WARN(ctx, "Media", "%s configured but no encoder is registered",
			CodecValueName(codecValue));
		AbandonStream(ctx);
		return;
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->ActiveCodec = codecValue;
	ctx->MediaBusySince = 0;
	ctx->ServiceUnitSamples = 0;
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
	MediaStatsReset(ctx);
	// A record covers one clock span of the codec, so the lead has to hold one plus the copy
	// tick that fills it.
	ApplyLeadFloor(ctx, sampleRate * Ioctl::kServiceRecordMs / 1000);
	InterlockedExchange(&ctx->StreamFault, 0);
	ServiceFlushRings(ctx);
	ServiceStreamStart(ctx);
	InterlockedExchange(&ctx->MediaMode, MediaModeService);
	SetMediaActive(ctx, true);
	WdfWaitLockRelease(ctx->MediaLock);

	OA2DP_INFO(ctx, "Media", "%s %u Hz stereo 16-bit, %u bps, frames from service",
		CodecValueName(codecValue), sampleRate, ctx->ActiveBitrate);
	WdfTimerStart(ctx->MediaTimer, 0);
}
} // namespace OpenA2dpDriver::Media
