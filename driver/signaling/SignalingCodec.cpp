#include "SignalingInternal.h"

namespace OpenA2dpDriver::Signaling
{
const char* DiscoveryFailureName(Avdtp::DiscoveryFailure reason)
{
	switch (reason)
	{
	case Avdtp::DiscoveryFailure::None: return "none";
	case Avdtp::DiscoveryFailure::Rejected: return "sink rejected the request";
	case Avdtp::DiscoveryFailure::NoUsableSink: return "no usable sink endpoint";
	case Avdtp::DiscoveryFailure::MalformedResponse: return "malformed response";
	case Avdtp::DiscoveryFailure::Timeout: return "no response (timeout)";
	}
	return "?";
}

// Scores each sink SEP: SBC 1, AAC 2, so AAC wins when both sides support it.
// Returns 0 for a codec we cannot configure (or that the user forced away from).
uint32_t SelectConfig(void* context, Avdtp::CodecType codec, const uint8_t* remoteIe, size_t remoteIeLength,
	uint8_t* configIe, size_t& configIeLength)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	Ioctl::Preference preference;
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		preference = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	}
	const bool forceCodec = (preference.flags & Ioctl::kPreferenceForceCodec) != 0;

	// Which codecs the sink actually advertises is otherwise invisible in the log,
	// and "AAC never wins" looks the same whether the sink lacks it or we skipped it.
	OA2DP_INFO(ctx, "AVDTP", "sink offers codec %s (%u IE bytes)", CodecName(codec),
		static_cast<unsigned>(remoteIeLength));

	if (codec == Avdtp::CodecType::Sbc)
	{
		if (forceCodec && preference.codec != Ioctl::kCodecSbc)
		{
			return 0;
		}
		// Offer the selector only what we can play, so it never picks a rate or
		// channel mode the media path would refuse.
		if (remoteIeLength < SbcConfig::kIeLength)
		{
			return 0;
		}
		uint8_t playable[SbcConfig::kIeLength];
		for (size_t i = 0; i < SbcConfig::kIeLength; ++i)
		{
			playable[i] = remoteIe[i];
		}
		playable[0] &= kSbcDriverCapabilities[0];
		if (!SbcConfig::Select(playable, SbcConfig::kIeLength, 0, SbcMaxBitpool, configIe))
		{
			return 0;
		}
		configIeLength = SbcConfig::kIeLength;
		return 1;
	}

	if (codec == Avdtp::CodecType::Aac)
	{
		// AAC exists only while an encoder service is registered for this device.
		if (!ServiceRegistered(ctx) || (forceCodec && preference.codec != Ioctl::kCodecAac))
		{
			return 0;
		}
		if (!AacConfig::Select(remoteIe, remoteIeLength, AudioSampleRate, 0, configIe))
		{
			return 0;
		}
		configIeLength = AacConfig::kIeLength;
		return 2;
	}

	return 0;
}

// Sink-proposed configurations for non-SBC codecs; returns 0 or an AVDTP error.
uint8_t ValidateConfig(void* context, Avdtp::CodecType codec, const uint8_t* ie, size_t ieLength)
{
	if (codec != Avdtp::CodecType::Aac)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	// The AAC SEP is always advertised, so a sink can propose an AAC configuration
	// even while no encoder exists. Refusing it here keeps the stream from opening
	// into a codec the driver has nothing to encode with.
	if (!ServiceRegistered(static_cast<DEVICE_CONTEXT*>(context)))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	return AacConfig::Validate(ie, ieLength);
}

// Refreshes the snapshot GET_STATUS reads without taking SessionLock. Caller holds SessionLock.
void PublishStatus(DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->AvdtpStateSnapshot, static_cast<LONG>(ctx->Session.State()));
	InterlockedExchange(&ctx->NegotiatedCodecSnapshot,
		ctx->Session.SelectedIeLength() != 0 ? static_cast<LONG>(ctx->Session.SelectedCodec()) : 0);
}

void ResetSession(DEVICE_CONTEXT* ctx)
{
	ctx->Session = Avdtp::SourceSession(Avdtp::CodecType::Sbc, kSbcDriverCapabilities,
		SbcConfig::kIeLength, &SelectConfig, ctx, &ValidateConfig);
	// Always advertise AAC. Registering the SEP only when the service happens to be
	// up already loses a race it usually loses: the sink opens the signaling
	// channel first, ResetSession runs with EncoderRegistered == 0, and the codec
	// would then be missing for the whole connection. Whether AAC may be selected is
	// decided per negotiation in SelectConfig, which is the state that actually
	// matters at that moment.
	ctx->Session.SetLocalSep(Avdtp::kLocalAacSeid, Avdtp::CodecType::Aac, AacConfig::kLocalCapabilities,
		AacConfig::kIeLength);
	ctx->LoggedState = Avdtp::SessionState::Idle;
	ctx->ResponseDeadline = 0;
	InterlockedExchange(&ctx->MediaOpenPending, 0);
	// A stale completion flag is harmless (FinishMediaOpen checks the operation itself);
	// the operation was reclaimed by the media channel close that preceded this reset.
	InterlockedExchange(&ctx->MediaOpenCompleted, 0);
	ctx->MediaOpenAttempts = 0;
	ctx->MediaOpenRetry = FALSE;
	ctx->CodecSwitchWanted = FALSE;
	ctx->CodecSwitchClosing = FALSE;
	// Packets queued for the previous session must not reach the next one.
	ctx->SignalQueueHead = 0;
	ctx->SignalQueueCount = 0;
	PublishStatus(ctx);
	KeSetEvent(&ctx->SessionChanged, IO_NO_INCREMENT, FALSE);
}
} // namespace OpenA2dpDriver::Signaling
