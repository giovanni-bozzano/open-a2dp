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

// A configuration we select has to run at the endpoint format: PCM reaches the encoder at
// PinSampleRate/PinChannels, so a stream negotiated at another rate or channel count cannot
// play it. The selectors prefer those values, so a mismatch means the sink does not
// advertise them - refuse it now, where the log names the cause, instead of leaving the
// media-start check to fail after the sink has already started.
uint32_t AcceptSelected(void* context, Avdtp::CodecType codec, const uint8_t* configIe,
						size_t configIeLength, uint32_t selection)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	uint32_t sampleRate = 0;
	uint32_t channels = 0;
	if (codec == Avdtp::CodecType::Sbc)
	{
		SbcFixed::Params params;
		if (SbcFixed::ParseIe(configIe, configIeLength, params))
		{
			sampleRate = params.sampleRate;
			channels = params.Channels();
		}
	}
	else
	{
		AacConfig::Params params;
		if (AacConfig::ParseConfiguredIe(configIe, configIeLength, params))
		{
			sampleRate = params.sampleRate;
			channels = params.channels;
		}
	}
	if (sampleRate == ctx->PinSampleRate && channels == ctx->PinChannels)
	{
		return selection;
	}
	OA2DP_WARN(ctx, "AVDTP",
		"the sink cannot serve the endpoint format (%u Hz, %u ch): the %s configuration runs at "
		"%u Hz, %u ch, so no stream is offered",
		ctx->PinSampleRate, static_cast<unsigned>(ctx->PinChannels), CodecName(codec), sampleRate,
		channels);
	return 0;
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
		// Remember the rates and channel modes the sink advertises, so SET_PREFERENCE can
		// refuse a pinned endpoint format the sink cannot serve instead of storing it.
		InterlockedExchange(&ctx->SinkSbcCapabilities, remoteIeLength >= 1 ? remoteIe[0] : 0);
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
		// A stored bitpool caps the request; the selector still lifts it to the sink's
		// minimum and caps it at what the sink advertises for the chosen mode.
		const uint8_t bitpool = preference.sbcBitpool != 0
			? static_cast<uint8_t>(preference.sbcBitpool)
			: SbcMaxBitpool;
		// Stored frame parameters that the sink does not offer fall back to the
		// selector's own order instead of failing the negotiation.
		SbcConfig::FramePreference frame;
		frame.blocks = static_cast<uint8_t>(preference.sbcBlocks);
		frame.subbands = static_cast<uint8_t>(preference.sbcSubbands);
		frame.allocation = static_cast<uint8_t>(preference.sbcAllocation);
		frame.channels = static_cast<uint8_t>(ctx->PinChannels);
		// Ask for the endpoint's rate so the sink matches the PCM Windows produced; the
		// selector falls back to its own order when the sink lacks that rate.
		if (!SbcConfig::Select(playable, SbcConfig::kIeLength, ctx->PinSampleRate, bitpool, configIe, frame))
		{
			return 0;
		}
		configIeLength = SbcConfig::kIeLength;
		return AcceptSelected(ctx, codec, configIe, configIeLength, 1);
	}

	if (codec == Avdtp::CodecType::Aac)
	{
		// AAC exists only while an encoder service is registered for this device.
		if (!ServiceRegistered(ctx) || (forceCodec && preference.codec != Ioctl::kCodecAac))
		{
			return 0;
		}
		// Prefer the endpoint's rate so the encoder gets the PCM the stream produced, and
		// keep the bitrate to what one media packet can carry: A2DP sends one access unit
		// per packet, so a bitrate the MTU cannot serve would only lose frames.
		const size_t payload =
			(ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu) - Rtp::kHeaderSize;
		if (!AacConfig::Select(remoteIe, remoteIeLength, ctx->PinSampleRate, preference.aacBitrate,
							  configIe, static_cast<uint8_t>(ctx->PinChannels), payload))
		{
			return 0;
		}
		configIeLength = AacConfig::kIeLength;
		return AcceptSelected(ctx, codec, configIe, configIeLength, 2);
	}

	return 0;
}

// Sink-proposed configurations; returns 0 or an AVDTP error. The sink configures the
// stream itself here, so this is the only chance to refuse a format the audio endpoint
// cannot serve: the media path would otherwise start into a mismatch and stay silent.
uint8_t ValidateConfig(void* context, Avdtp::CodecType codec, const uint8_t* ie, size_t ieLength)
{
	DEVICE_CONTEXT* ctx = static_cast<DEVICE_CONTEXT*>(context);
	if (codec == Avdtp::CodecType::Sbc)
	{
		SbcFixed::Params params;
		if (!SbcFixed::ParseIe(ie, ieLength, params))
		{
			return static_cast<uint8_t>(Avdtp::ErrorCode::UnsupportedConfiguration);
		}
		if (params.sampleRate != ctx->PinSampleRate)
		{
			return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
		}
		if (params.Channels() != ctx->PinChannels)
		{
			return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannelMode);
		}
		return 0;
	}
	if (codec != Avdtp::CodecType::Aac)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	// The AAC SEP is always advertised, so a sink can propose an AAC configuration
	// even while no encoder exists. Refusing it here keeps the stream from opening
	// into a codec the driver has nothing to encode with.
	if (!ServiceRegistered(ctx))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	const uint8_t error = AacConfig::Validate(ie, ieLength);
	if (error != 0)
	{
		return error;
	}
	AacConfig::Params params;
	if (!AacConfig::ParseConfiguredIe(ie, ieLength, params))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::UnsupportedConfiguration);
	}
	if (params.sampleRate != ctx->PinSampleRate)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
	}
	if (params.channels != ctx->PinChannels)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannels);
	}
	// A bitrate whose frames do not fit one media packet would only lose frames.
	const size_t payload =
		(ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu) - Rtp::kHeaderSize;
	if (params.bitrate > AacConfig::MaxBitrateForMtu(payload, params.sampleRate))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedBitRate);
	}
	return 0;
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
	ctx->ReconfigurePhase = ReconfigureNone;
	ctx->AppliedPreferenceParameters = InterlockedCompareExchange(&ctx->PreferenceParameters, 0, 0);
	// Packets queued for the previous session must not reach the next one.
	ctx->SignalQueueHead = 0;
	ctx->SignalQueueCount = 0;
	PublishStatus(ctx);
	KeSetEvent(&ctx->SessionChanged, IO_NO_INCREMENT, FALSE);
}
} // namespace OpenA2dpDriver::Signaling
