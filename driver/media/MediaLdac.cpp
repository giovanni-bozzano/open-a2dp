#include "MediaInternal.h"

namespace OpenA2dpDriver::Media
{
// Validates the configured LDAC IE and hands the stream to the encoder service. The service
// reads the endpoint's PCM from the ring and writes records whose payload is the encoder's own
// packet of transport frames; the driver only adds the RTP header.
void StartLdac(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength)
{
	uint8_t sampleRates = 0;
	uint8_t channelModes = 0;
	if (!Ldac::ParseIe(ie, ieLength, sampleRates, channelModes))
	{
		OA2DP_WARN(ctx, "Media", "invalid LDAC configuration");
		return;
	}
	const uint32_t sampleRate = (sampleRates & Ldac::kSampleRate44100) != 0 ? 44100u : 48000u;
	// The encoder takes 16-bit stereo by definition; the service narrows a 24-in-32 ring, so
	// the endpoint may run either depth.
	if (sampleRate != ctx->PinSampleRate || ctx->PinChannels != 2 ||
		!AptxPinDepthUsable(ctx->PinValidBits, Ldac::kBitsPerSample))
	{
		OA2DP_WARN(ctx, "Media",
			"LDAC %u Hz stereo does not match the endpoint format %u Hz %u ch %u-bit",
			sampleRate, ctx->PinSampleRate, static_cast<unsigned>(ctx->PinChannels),
			static_cast<unsigned>(ctx->PinValidBits));
		return;
	}
	if (!ServiceCodecRegistered(ctx, Ioctl::kCodecLdac))
	{
		OA2DP_WARN(ctx, "Media", "LDAC configured but no LDAC encoder is registered");
		return;
	}

	uint32_t quality = Ldac::kQualityHigh;
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		quality = ctx->Preference.ldacQuality;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->ActiveCodec = Ioctl::kCodecLdac;
	ctx->MediaBusySince = 0;
	ctx->ServiceUnitSamples = 0;
	ctx->MediaSendUnchecked = FALSE;
	ctx->ActiveSampleRate = sampleRate;
	ctx->ActiveChannels = 2;
	// The nominal bitrate of the chosen quality mode; the encoder reports the real one.
	ctx->ActiveBitrate = Ldac::BitrateBps(static_cast<uint8_t>(quality), sampleRate);
	ctx->ActiveObjectType = 0;
	ctx->ActiveLdacQuality = quality;
	ctx->RtpSequence = 0;
	ctx->PacketsSent = 0;
	ctx->RtpTimestamp = 0;
	ctx->MediaSamplesSent = 0;
	ctx->MediaStartTime = KeQueryInterruptTime();
	ctx->ServiceLastFrameTime = ctx->MediaStartTime;
	ctx->ServiceLastUnderrunLog = 0;
	MediaStatsReset(ctx);
	// A record is one encoder packet of transport frames, so the lead has to hold one plus the
	// copy tick that fills it.
	ApplyLeadFloor(ctx, sampleRate * Ioctl::kServiceRecordMs / 1000);
	InterlockedExchange(&ctx->StreamFault, 0);
	ServiceFlushRings(ctx);
	ServiceStreamStart(ctx);
	InterlockedExchange(&ctx->MediaMode, MediaModeService);
	ctx->MediaActive = TRUE;
	WdfWaitLockRelease(ctx->MediaLock);

	OA2DP_INFO(ctx, "Media", "LDAC %u Hz stereo 16-bit, %u bps, frames from service", sampleRate,
		ctx->ActiveBitrate);
	WdfTimerStart(ctx->MediaTimer, 0);
}
} // namespace OpenA2dpDriver::Media
