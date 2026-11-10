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

// The rate and channel count a configuration IE describes; 0 when it does not parse.
void ConfigFormat(Avdtp::CodecType codec, const uint8_t* ie, size_t ieLength, uint32_t& sampleRate,
				  uint32_t& channels)
{
	sampleRate = 0;
	channels = 0;
	if (codec == Avdtp::CodecType::Sbc)
	{
		SbcFixed::Params params;
		if (SbcFixed::ParseIe(ie, ieLength, params))
		{
			sampleRate = params.sampleRate;
			channels = params.Channels();
		}
	}
	else if (codec == Avdtp::CodecType::Aac)
	{
		AacConfig::Params params;
		if (AacConfig::ParseConfiguredIe(ie, ieLength, params))
		{
			sampleRate = params.sampleRate;
			channels = params.channels;
		}
	}
	else if (codec == Avdtp::CodecType::Vendor)
	{
		uint8_t capabilities = 0;
		if (AptxHd::ParseIe(ie, ieLength, capabilities))
		{
			sampleRate = (capabilities & AptxHd::kSampleRate44100) != 0 ? 44100u : 48000u;
			channels = (capabilities & AptxHd::kChannelStereo) != 0 ? 2u : 1u;
		}
	}
}

// True while the configuration the stream was negotiated with still runs at the endpoint
// format. A false answer means Windows opened the endpoint at another rate or channel
// count after the configuration was picked, so the stream has to be renegotiated.
bool SelectedFormatMatchesPin(DEVICE_CONTEXT* ctx)
{
	uint32_t sampleRate = 0;
	uint32_t channels = 0;
	ConfigFormat(ctx->Session.SelectedCodec(), ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength(),
				 sampleRate, channels);
	if (sampleRate == 0)
	{
		return true; // an IE we cannot read is not a format change to chase
	}
	return sampleRate == ctx->PinSampleRate && channels == ctx->PinChannels;
}

// The AAC bitrate is chosen during discovery, before the media channel exists, so the MTU
// the negotiation assumed can be larger than the one the link reports. Everything else in
// the configuration (object type, rate, channels) stays as negotiated and only the bitrate
// is lowered, which is exactly what RECONFIGURE carries.
bool BuildMtuCappedConfiguration(DEVICE_CONTEXT* ctx, uint8_t* ie, size_t& ieLength)
{
	if (ctx->Session.SelectedCodec() != Avdtp::CodecType::Aac)
	{
		return false;
	}
	AacConfig::Params params;
	if (!AacConfig::ParseConfiguredIe(ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength(), params))
	{
		return false;
	}
	const size_t payload = (ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu) - Rtp::kHeaderSize;
	const uint32_t mtuLimit = AacConfig::MaxBitrateForMtu(payload, params.sampleRate);
	if (mtuLimit == 0 || params.bitrate <= mtuLimit)
	{
		return false;
	}
	OA2DP_INFO(ctx, "AVDTP", "media MTU %u carries up to %u bps; lowering the AAC bitrate from %u",
		static_cast<unsigned>(ctx->MediaOutMtu), mtuLimit, params.bitrate);
	params.bitrate = mtuLimit;
	AacConfig::WriteIe(params, ie);
	ieLength = AacConfig::kIeLength;
	return true;
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
	ConfigFormat(codec, configIe, configIeLength, sampleRate, channels);
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

// Scores each sink SEP: SBC 1, AAC 2, aptX HD 3. Quality order, so the best codec both sides
// can run wins; registration and a format the endpoint cannot serve still return 0.
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
	OA2DP_INFO(ctx, "AVDTP", "sink offers codec %s (%u IE bytes)", CodecLabel(codec, remoteIe, remoteIeLength),
		static_cast<unsigned>(remoteIeLength));

	if (codec == Avdtp::CodecType::Sbc)
	{
		if (remoteIeLength < SbcConfig::kIeLength)
		{
			return 0;
		}
		// PCM availability is independent of codec and parameter preferences.
		const uint8_t sinkFormats = remoteIe[0];
		InterlockedOr(&ctx->SinkCodecs, static_cast<LONG>(Ioctl::kSinkCodecSbc));
		if (InterlockedExchange(&ctx->SinkSbcCapabilities, sinkFormats) != static_cast<LONG>(sinkFormats))
		{
			OA2DP_INFO(ctx, "AVDTP", "sink SBC PCM capabilities: rates 0x%02X, channels 0x%02X",
				SbcConfig::AdvertisedRates(sinkFormats), SbcConfig::AdvertisedChannels(sinkFormats));
			const NTSTATUS formatStatus = AudioApplyEndpointFormats(ctx);
			if (!NT_SUCCESS(formatStatus))
			{
				OA2DP_WARN(ctx, "AVDTP", "applying the sink's formats to the endpoint failed 0x%08X", formatStatus);
			}
		}
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
		// Reaching this branch means the sink advertises an AAC SEP.
		InterlockedOr(&ctx->SinkCodecs, static_cast<LONG>(Ioctl::kSinkCodecAac));
		// AAC exists only while its encoder provider is registered for this device.
		if (!ServiceCodecRegistered(ctx, Ioctl::kCodecAac) ||
			(forceCodec && preference.codec != Ioctl::kCodecAac))
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

	if (codec == Avdtp::CodecType::Vendor)
	{
		uint8_t capabilities = 0;
		if (!AptxHd::ParseIe(remoteIe, remoteIeLength, capabilities))
		{
			// A vendor codec this driver does not implement.
			return 0;
		}
		// Record the advertisement whether or not HD can be selected now: the endpoint's
		// 24-bit formats depend on it, and an encoder registered later must be able to use
		// the discovery that already ran.
		const bool offered = (InterlockedOr(&ctx->SinkCodecs, static_cast<LONG>(Ioctl::kSinkCodecAptxHd)) &
							  static_cast<LONG>(Ioctl::kSinkCodecAptxHd)) != 0;
		InterlockedExchange(&ctx->SinkHdCapabilities, capabilities);
		if (!offered)
		{
			OA2DP_INFO(ctx, "AVDTP", "sink offers aptX HD (rates 0x%02X, modes 0x%02X)",
				static_cast<unsigned>(capabilities & AptxHd::kSampleRateMask),
				static_cast<unsigned>(capabilities & AptxHd::kChannelMask));
			const NTSTATUS formatStatus = AudioApplyEndpointFormats(ctx);
			if (!NT_SUCCESS(formatStatus))
			{
				OA2DP_WARN(ctx, "AVDTP", "applying the sink's formats to the endpoint failed 0x%08X",
					formatStatus);
			}
		}
		if (!ServiceCodecRegistered(ctx, Ioctl::kCodecAptxHd))
		{
			return 0;
		}
		if (forceCodec && preference.codec != Ioctl::kCodecAptxHd)
		{
			return 0;
		}
		// The encoder is stereo and 24-bit, and the endpoint format is what the PCM ring
		// carries; a mono or 16-bit pin would need a downmix or an expansion this codec does
		// not do, so the stream falls back to a 16-bit codec instead.
		if (ctx->PinChannels != 2 || ctx->PinValidBits != AptxHd::kBitsPerSample)
		{
			OA2DP_WARN(ctx, "AVDTP", "aptX HD needs 24-bit stereo, the endpoint format is %u ch %u-bit",
				static_cast<unsigned>(ctx->PinChannels), static_cast<unsigned>(ctx->PinValidBits));
			return 0;
		}
		const uint8_t rate = ctx->PinSampleRate == 44100 ? AptxHd::kSampleRate44100
			: ctx->PinSampleRate == 48000
			? AptxHd::kSampleRate48000
			: 0;
		if (rate == 0 || (capabilities & rate) == 0)
		{
			OA2DP_WARN(ctx, "AVDTP", "the sink cannot serve the endpoint's %u Hz with aptX HD",
				ctx->PinSampleRate);
			return 0;
		}
		AptxHd::WriteIe(rate, configIe);
		configIeLength = AptxHd::kIeLength;
		return AcceptSelected(ctx, codec, configIe, configIeLength, 3);
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
	if (codec == Avdtp::CodecType::Vendor)
	{
		// The HD SEP is always advertised, so a sink can propose an HD configuration even
		// while no encoder exists. Refusing it here keeps the stream from opening into a
		// codec the driver has nothing to encode with.
		if (!ServiceCodecRegistered(ctx, Ioctl::kCodecAptxHd))
		{
			return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
		}
		const uint8_t error = AptxHd::ValidateIe(ie, ieLength);
		if (error != 0)
		{
			return error;
		}
		uint8_t capabilities = 0;
		(void)AptxHd::ParseIe(ie, ieLength, capabilities);
		// aptX HD is stereo-only, and the driver reads 24-in-32 PCM from the endpoint.
		if (ctx->PinChannels != 2 || ctx->PinValidBits != AptxHd::kBitsPerSample)
		{
			return static_cast<uint8_t>(Avdtp::ErrorCode::UnsupportedConfiguration);
		}
		const uint8_t rate = ctx->PinSampleRate == 44100 ? AptxHd::kSampleRate44100
			: ctx->PinSampleRate == 48000
			? AptxHd::kSampleRate48000
			: 0;
		if (rate == 0 || (capabilities & rate) == 0)
		{
			return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
		}
		return 0;
	}
	if (codec != Avdtp::CodecType::Aac)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	// Like AAC, the SEP is always advertised; the provider decides whether it can run.
	if (!ServiceCodecRegistered(ctx, Ioctl::kCodecAac))
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
	LONG codec = 0;
	if (ctx->Session.SelectedIeLength() != 0)
	{
		const Avdtp::CodecType selected = ctx->Session.SelectedCodec();
		if (selected != Avdtp::CodecType::Vendor)
		{
			codec = static_cast<LONG>(selected);
		}
		else
		{
			uint8_t capabilities = 0;
			codec = AptxHd::ParseIe(ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength(), capabilities)
				? static_cast<LONG>(Ioctl::kCodecAptxHd)
				: static_cast<LONG>(selected);
		}
	}
	InterlockedExchange(&ctx->NegotiatedCodecSnapshot, codec);
}

void ResetSession(DEVICE_CONTEXT* ctx)
{
	ctx->Session = Avdtp::SourceSession(Avdtp::CodecType::Sbc, kSbcDriverCapabilities,
		SbcConfig::kIeLength, &SelectConfig, ctx, &ValidateConfig);
	// Always advertise AAC and aptX HD. Registering a SEP only when a provider happens to be
	// up already loses a race it usually loses: the sink opens the signaling channel first,
	// ResetSession runs with no provider registered, and the codec would then be missing for
	// the whole connection. Whether either may be selected is decided per negotiation in
	// SelectConfig, which is the state that actually matters at that moment.
	ctx->Session.SetLocalSep(Avdtp::kLocalAacSeid, Avdtp::CodecType::Aac, AacConfig::kLocalCapabilities,
		AacConfig::kIeLength);
	uint8_t hdIe[AptxHd::kIeLength] = {};
	AptxHd::WriteIe(static_cast<uint8_t>(AptxHd::kSampleRate44100 | AptxHd::kSampleRate48000), hdIe);
	ctx->Session.SetLocalSep(Avdtp::kLocalAptxHdSeid, Avdtp::CodecType::Vendor, hdIe, sizeof(hdIe));
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
	ctx->FormatSwitchPending = FALSE;
	ctx->ReconfigurePhase = ReconfigureNone;
	// The link that reported the MTU is gone, so the next negotiation caps against the
	// default again until the new channel reports its own.
	ctx->MediaOutMtu = 0;
	// A new connection gets its own single MTU renegotiation attempt.
	InterlockedExchange(&ctx->MtuRenegotiate, 0);
	InterlockedExchange(&ctx->MtuRenegotiationAttempts, 0);
	// The codecs are re-learned by this connection's discovery.
	InterlockedExchange(&ctx->SinkCodecs, 0);
	InterlockedExchange(&ctx->SinkHdCapabilities, 0);
	ctx->AppliedPreferenceParameters = InterlockedCompareExchange(&ctx->PreferenceParameters, 0, 0);
	// Packets queued for the previous session must not reach the next one.
	ctx->SignalQueueHead = 0;
	ctx->SignalQueueCount = 0;
	PublishStatus(ctx);
	KeSetEvent(&ctx->SessionChanged, IO_NO_INCREMENT, FALSE);
}
} // namespace OpenA2dpDriver::Signaling
