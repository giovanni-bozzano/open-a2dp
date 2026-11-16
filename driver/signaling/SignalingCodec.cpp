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
		uint8_t channelModes = 0;
		if (Ldac::ParseIe(ie, ieLength, capabilities, channelModes))
		{
			sampleRate = Ldac::RateValue(capabilities);
			channels = (channelModes & Ldac::kChannelStereo) != 0 ? 2u : 1u;
		}
		else if (AptxLl::ParseIe(ie, ieLength, capabilities))
		{
			sampleRate = (capabilities & VendorAptx::kSampleRate44100) != 0 ? 44100u : 48000u;
			channels = (capabilities & VendorAptx::kChannelStereo) != 0 ? 2u : 1u;
		}
		else if (AptxHd::ParseIe(ie, ieLength, capabilities) || Aptx::ParseIe(ie, ieLength, capabilities))
		{
			sampleRate = (capabilities & VendorAptx::kSampleRate44100) != 0 ? 44100u : 48000u;
			channels = (capabilities & VendorAptx::kChannelStereo) != 0 ? 2u : 1u;
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

// Scores each sink SEP: SBC 1, AAC 2, aptX 3, aptX LL 3 (forced only), aptX HD 4, LDAC 5.
// Quality order, so the best codec both sides can run wins; registration and a format the
// endpoint cannot serve still return 0.
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
		uint8_t ldacRates = 0;
		uint8_t ldacModes = 0;
		if (Ldac::ParseIe(remoteIe, remoteIeLength, ldacRates, ldacModes))
		{
			// Record the advertisement whether or not the codec can run now, for the same reason
			// as the aptX ones: the settings window refuses to force a codec the sink lacks.
			const bool ldacOffered = (InterlockedOr(&ctx->SinkCodecs,
											  static_cast<LONG>(Ioctl::kSinkCodecLdac)) &
									 static_cast<LONG>(Ioctl::kSinkCodecLdac)) != 0;
			const LONG ldacPacked = static_cast<LONG>((static_cast<LONG>(ldacRates) << 8) | ldacModes);
			const bool ldacChanged =
				InterlockedExchange(&ctx->SinkLdacCapabilities, ldacPacked) != ldacPacked;
			if (!ldacOffered)
			{
				OA2DP_INFO(ctx, "AVDTP", "sink offers LDAC (rates 0x%02X, modes 0x%02X)",
					static_cast<unsigned>(ldacRates), static_cast<unsigned>(ldacModes));
			}
			// The endpoint's offered formats depend on these rates, so a new advertisement rebuilds
			// them here as the SBC and aptX branches do: discovery can reach this SEP before or after
			// the SBC one, and no later event is guaranteed to rebuild the list.
			if (ldacChanged)
			{
				const NTSTATUS formatStatus = AudioApplyEndpointFormats(ctx);
				if (!NT_SUCCESS(formatStatus))
				{
					OA2DP_WARN(ctx, "AVDTP", "applying the sink's formats to the endpoint failed 0x%08X",
						formatStatus);
				}
			}
			if (!ServiceCodecRegistered(ctx, Ioctl::kCodecLdac) ||
				(forceCodec && preference.codec != Ioctl::kCodecLdac))
			{
				return 0;
			}
			// LDAC is specified around 2-DH5 packets; a link whose payload is smaller cannot
			// carry it, and the encoder would spend the stream cutting frames to fit.
			const size_t ldacPayload =
				(ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu) - Rtp::kHeaderSize;
			if (ldacPayload < Ldac::kMinMtuBytes)
			{
				OA2DP_WARN(ctx, "AVDTP", "the link carries %u bytes per packet, LDAC needs %u",
					static_cast<unsigned>(ldacPayload), static_cast<unsigned>(Ldac::kMinMtuBytes));
				return 0;
			}
			if (ctx->PinChannels != 2 || !AptxPinDepthUsable(ctx->PinValidBits, Ldac::kBitsPerSample))
			{
				OA2DP_WARN(ctx, "AVDTP", "LDAC needs 16- or 24-bit stereo, the endpoint format is %u ch %u-bit",
					static_cast<unsigned>(ctx->PinChannels), static_cast<unsigned>(ctx->PinValidBits));
				return 0;
			}
			const uint8_t ldacRate = Ldac::RateBit(ctx->PinSampleRate);
			if (ldacRate == 0 || (ldacRates & ldacRate) == 0)
			{
				OA2DP_WARN(ctx, "AVDTP", "the sink cannot serve the endpoint's %u Hz with LDAC",
					ctx->PinSampleRate);
				return 0;
			}
			Ldac::WriteIe(ldacRate, Ldac::kChannelStereo, configIe);
			configIeLength = Ldac::kIeLength;
			return AcceptSelected(ctx, codec, configIe, configIeLength, 5);
		}

		uint8_t llCapabilities = 0;
		if (AptxLl::ParseIe(remoteIe, remoteIeLength, llCapabilities))
		{
			// LL is a latency choice, not a quality one, so the automatic order never picks
			// it: the user has to force it. The advertisement is still recorded, because the
			// settings window refuses to force a codec the sink does not offer.
			const bool llOffered = (InterlockedOr(&ctx->SinkCodecs,
				static_cast<LONG>(Ioctl::kSinkCodecAptxLl)) &
				static_cast<LONG>(Ioctl::kSinkCodecAptxLl)) != 0;
			InterlockedExchange(&ctx->SinkAptxLlCapabilities, llCapabilities);
			if (!llOffered)
			{
				OA2DP_INFO(ctx, "AVDTP", "sink offers aptX LL (rates 0x%02X, modes 0x%02X)",
					static_cast<unsigned>(llCapabilities & AptxLl::kSampleRateMask),
					static_cast<unsigned>(llCapabilities & AptxLl::kChannelMask));
				const NTSTATUS formatStatus = AudioApplyEndpointFormats(ctx);
				if (!NT_SUCCESS(formatStatus))
				{
					OA2DP_WARN(ctx, "AVDTP",
						"applying the sink's formats to the endpoint failed 0x%08X", formatStatus);
				}
			}
			if (!forceCodec || preference.codec != Ioctl::kCodecAptxLl ||
				!ServiceCodecRegistered(ctx, Ioctl::kCodecAptxLl))
			{
				return 0;
			}
			if (ctx->PinChannels != 2 ||
				!AptxPinDepthUsable(ctx->PinValidBits, AptxLl::kBitsPerSample))
			{
				OA2DP_WARN(ctx, "AVDTP",
					"aptX LL needs 16-bit stereo, the endpoint format is %u ch %u-bit",
					static_cast<unsigned>(ctx->PinChannels),
					static_cast<unsigned>(ctx->PinValidBits));
				return 0;
			}
			const uint8_t llRate = ctx->PinSampleRate == 44100 ? AptxLl::kSampleRate44100
				: ctx->PinSampleRate == 48000 ? AptxLl::kSampleRate48000 : 0;
			if (llRate == 0 || (llCapabilities & llRate) == 0)
			{
				OA2DP_WARN(ctx, "AVDTP",
					"the sink cannot serve the endpoint's %u Hz with aptX LL", ctx->PinSampleRate);
				return 0;
			}
			AptxLl::WriteIe(llRate, configIe);
			configIeLength = AptxLl::kIeLength;
			return AcceptSelected(ctx, codec, configIe, configIeLength, 3);
		}

		uint8_t capabilities = 0;
		const bool hd = AptxHd::ParseIe(remoteIe, remoteIeLength, capabilities);
		const bool aptx = !hd && Aptx::ParseIe(remoteIe, remoteIeLength, capabilities);
		if (!hd && !aptx)
		{
			// A vendor codec this driver does not implement.
			return 0;
		}
		const uint32_t codecValue = hd ? Ioctl::kCodecAptxHd : Ioctl::kCodecAptx;
		const uint32_t sinkBit = hd ? Ioctl::kSinkCodecAptxHd : Ioctl::kSinkCodecAptx;
		const uint16_t bitsPerSample = hd ? AptxHd::kBitsPerSample : Aptx::kBitsPerSample;
		// Record the advertisement whether or not the codec can be selected now: the endpoint's
		// formats depend on it, and an encoder registered later must be able to use the
		// discovery that already ran.
		const bool offered =
			(InterlockedOr(&ctx->SinkCodecs, static_cast<LONG>(sinkBit)) & static_cast<LONG>(sinkBit)) != 0;
		InterlockedExchange(hd ? &ctx->SinkHdCapabilities : &ctx->SinkAptxCapabilities, capabilities);
		if (!offered)
		{
			OA2DP_INFO(ctx, "AVDTP", "sink offers %s (rates 0x%02X, modes 0x%02X)",
				hd ? "aptX HD" : "aptX",
				static_cast<unsigned>(capabilities & VendorAptx::kSampleRateMask),
				static_cast<unsigned>(capabilities & VendorAptx::kChannelMask));
			const NTSTATUS formatStatus = AudioApplyEndpointFormats(ctx);
			if (!NT_SUCCESS(formatStatus))
			{
				OA2DP_WARN(ctx, "AVDTP", "applying the sink's formats to the endpoint failed 0x%08X",
					formatStatus);
			}
		}
		if (!ServiceCodecRegistered(ctx, codecValue) || (forceCodec && preference.codec != codecValue))
		{
			return 0;
		}
		// Both encoders are stereo. HD takes 24-in-32 only; classic takes 16-bit and narrows a
		// 24-in-32 pin in the service. Anything else would need a conversion this path does not
		// do, so the stream falls back to a codec that fits instead.
		if (ctx->PinChannels != 2 || !AptxPinDepthUsable(ctx->PinValidBits, bitsPerSample))
		{
			OA2DP_WARN(ctx, "AVDTP", "%s needs %u-bit stereo, the endpoint format is %u ch %u-bit",
				hd ? "aptX HD" : "aptX", static_cast<unsigned>(bitsPerSample),
				static_cast<unsigned>(ctx->PinChannels), static_cast<unsigned>(ctx->PinValidBits));
			return 0;
		}
		const uint8_t rate = ctx->PinSampleRate == 44100 ? VendorAptx::kSampleRate44100
			: ctx->PinSampleRate == 48000
			? VendorAptx::kSampleRate48000
			: 0;
		if (rate == 0 || (capabilities & rate) == 0)
		{
			OA2DP_WARN(ctx, "AVDTP", "the sink cannot serve the endpoint's %u Hz with %s",
				ctx->PinSampleRate, hd ? "aptX HD" : "aptX");
			return 0;
		}
		if (hd)
		{
			AptxHd::WriteIe(rate, configIe);
			configIeLength = AptxHd::kIeLength;
		}
		else
		{
			Aptx::WriteIe(rate, configIe);
			configIeLength = Aptx::kIeLength;
		}
		return AcceptSelected(ctx, codec, configIe, configIeLength, hd ? 4u : 3u);
	}

	return 0;
}

// The checks an aptX configuration shares: a registered provider, the codec's depth in
// stereo, and the endpoint's rate among those the sink offered.
static uint8_t ValidateAptxConfiguration(DEVICE_CONTEXT* ctx, uint32_t codecValue, uint16_t bitsPerSample,
										 uint8_t capabilities)
{
	if (!ServiceCodecRegistered(ctx, codecValue))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
	}
	if (ctx->PinChannels != 2 || !AptxPinDepthUsable(ctx->PinValidBits, bitsPerSample))
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::UnsupportedConfiguration);
	}
	const uint8_t rate = ctx->PinSampleRate == 44100 ? VendorAptx::kSampleRate44100
		: ctx->PinSampleRate == 48000
		? VendorAptx::kSampleRate48000
		: 0;
	if (rate == 0 || (capabilities & rate) == 0)
	{
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
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
		// The vendor SEPs are always advertised, so a sink can propose their configurations
		// even while no encoder exists. Refusing them here keeps the stream from opening into
		// a codec the driver has nothing to encode with.
		uint8_t ldacRates = 0;
		uint8_t ldacModes = 0;
		if (Ldac::ParseIe(ie, ieLength, ldacRates, ldacModes))
		{
			const uint8_t ldacError = Ldac::ValidateIe(ie, ieLength);
			if (ldacError != 0)
			{
				return ldacError;
			}
			if (!ServiceCodecRegistered(ctx, Ioctl::kCodecLdac))
			{
				return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
			}
			const size_t ldacPayload =
				(ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu) - Rtp::kHeaderSize;
			if (ldacPayload < Ldac::kMinMtuBytes)
			{
				return static_cast<uint8_t>(Avdtp::ErrorCode::UnsupportedConfiguration);
			}
			if (ctx->PinChannels != 2 || !AptxPinDepthUsable(ctx->PinValidBits, Ldac::kBitsPerSample))
			{
				return static_cast<uint8_t>(Avdtp::ErrorCode::UnsupportedConfiguration);
			}
			const uint8_t rate = Ldac::RateBit(ctx->PinSampleRate);
			if (rate == 0 || (ldacRates & rate) == 0)
			{
				return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency);
			}
			return 0;
		}
		uint8_t capabilities = 0;
		if (AptxLl::ParseIe(ie, ieLength, capabilities))
		{
			// A sink may configure LL itself, but only a forced preference makes it this
			// driver's choice; otherwise the user asked for another codec.
			Ioctl::Preference preference;
			{
				KIRQL irql;
				KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
				preference = ctx->Preference;
				KeReleaseSpinLock(&ctx->PreferenceLock, irql);
			}
			if ((preference.flags & Ioctl::kPreferenceForceCodec) == 0 ||
				preference.codec != Ioctl::kCodecAptxLl)
			{
				return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
			}
			const uint8_t llError = AptxLl::ValidateIe(ie, ieLength);
			if (llError != 0)
			{
				return llError;
			}
			return ValidateAptxConfiguration(ctx, Ioctl::kCodecAptxLl, AptxLl::kBitsPerSample, capabilities);
		}
		if (AptxHd::ParseIe(ie, ieLength, capabilities))
		{
			const uint8_t error = AptxHd::ValidateIe(ie, ieLength);
			if (error != 0)
			{
				return error;
			}
			return ValidateAptxConfiguration(ctx, Ioctl::kCodecAptxHd, AptxHd::kBitsPerSample, capabilities);
		}
		if (Aptx::ParseIe(ie, ieLength, capabilities))
		{
			const uint8_t error = Aptx::ValidateIe(ie, ieLength);
			if (error != 0)
			{
				return error;
			}
			return ValidateAptxConfiguration(ctx, Ioctl::kCodecAptx, Aptx::kBitsPerSample, capabilities);
		}
		return static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedCodecType);
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

bool SelectedCodecValue(DEVICE_CONTEXT* ctx, uint32_t& codecValue)
{
	if (ctx->Session.SelectedIeLength() == 0)
	{
		return false;
	}
	switch (ctx->Session.SelectedCodec())
	{
	case Avdtp::CodecType::Sbc:
		codecValue = Ioctl::kCodecSbc;
		return true;
	case Avdtp::CodecType::Aac:
		codecValue = Ioctl::kCodecAac;
		return true;
	case Avdtp::CodecType::Vendor:
		break;
	default:
		return false;
	}
	uint8_t capabilities = 0;
	uint8_t channelModes = 0;
	if (Ldac::ParseIe(ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength(), capabilities,
					  channelModes))
	{
		codecValue = Ioctl::kCodecLdac;
		return true;
	}
	if (AptxLl::ParseIe(ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength(), capabilities))
	{
		codecValue = Ioctl::kCodecAptxLl;
		return true;
	}
	if (AptxHd::ParseIe(ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength(), capabilities))
	{
		codecValue = Ioctl::kCodecAptxHd;
		return true;
	}
	if (Aptx::ParseIe(ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength(), capabilities))
	{
		codecValue = Ioctl::kCodecAptx;
		return true;
	}
	return false;
}

// Refreshes the snapshot GET_STATUS reads without taking SessionLock. Caller holds SessionLock.
void PublishStatus(DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->AvdtpStateSnapshot, static_cast<LONG>(ctx->Session.State()));
	LONG codec = 0;
	uint32_t value = 0;
	if (SelectedCodecValue(ctx, value))
	{
		codec = static_cast<LONG>(value);
	}
	else if (ctx->Session.SelectedIeLength() != 0)
	{
		// A vendor IE the driver does not implement: report its AVDTP type as before.
		codec = static_cast<LONG>(ctx->Session.SelectedCodec());
	}
	InterlockedExchange(&ctx->NegotiatedCodecSnapshot, codec);
}

// Registers a local SEP and reports when the table cannot hold it. A SEP that silently does
// not fit is never advertised, so the codec it carries can never be negotiated.
static void AddLocalSep(DEVICE_CONTEXT* ctx, uint8_t seid, uint32_t codecValue, Avdtp::CodecType codec,
						const uint8_t* ie, size_t ieLength)
{
	if (!ctx->Session.SetLocalSep(seid, codec, ie, ieLength))
	{
		OA2DP_WARN(ctx, "AVDTP", "cannot advertise the %s SEP (SEID %u); the sink will not be offered it",
			CodecValueName(codecValue), static_cast<unsigned>(seid));
	}
}

void ResetSession(DEVICE_CONTEXT* ctx)
{
	ctx->Session = Avdtp::SourceSession(Avdtp::CodecType::Sbc, kSbcDriverCapabilities,
		SbcConfig::kIeLength, &SelectConfig, ctx, &ValidateConfig);
	// Always advertise AAC and both aptX codecs. Registering a SEP only when a provider
	// happens to be up already loses a race it usually loses: the sink opens the signaling
	// channel first, ResetSession runs with no provider registered, and the codec would then
	// be missing for the whole connection. Whether any may be selected is decided per
	// negotiation in SelectConfig, which is the state that actually matters at that moment.
	AddLocalSep(ctx, Avdtp::kLocalAacSeid, Ioctl::kCodecAac, Avdtp::CodecType::Aac,
		AacConfig::kLocalCapabilities, AacConfig::kIeLength);
	uint8_t aptxHdIe[AptxHd::kIeLength] = {};
	AptxHd::WriteIe(static_cast<uint8_t>(AptxHd::kSampleRate44100 | AptxHd::kSampleRate48000), aptxHdIe);
	AddLocalSep(ctx, Avdtp::kLocalAptxHdSeid, Ioctl::kCodecAptxHd, Avdtp::CodecType::Vendor, aptxHdIe,
		sizeof(aptxHdIe));
	uint8_t aptxIe[Aptx::kIeLength] = {};
	Aptx::WriteIe(static_cast<uint8_t>(Aptx::kSampleRate44100 | Aptx::kSampleRate48000), aptxIe);
	AddLocalSep(ctx, Avdtp::kLocalAptxSeid, Ioctl::kCodecAptx, Avdtp::CodecType::Vendor, aptxIe,
		sizeof(aptxIe));
	uint8_t aptxLlIe[AptxLl::kIeLength] = {};
	AptxLl::WriteIe(static_cast<uint8_t>(AptxLl::kSampleRate44100 | AptxLl::kSampleRate48000), aptxLlIe);
	AddLocalSep(ctx, Avdtp::kLocalAptxLlSeid, Ioctl::kCodecAptxLl, Avdtp::CodecType::Vendor, aptxLlIe,
		sizeof(aptxLlIe));
	uint8_t ldacIe[Ldac::kIeLength] = {};
	// Every rate the endpoint can be opened at: the sink picks among them, and SelectConfig
	// narrows the choice to the endpoint's own rate.
	Ldac::WriteIe(static_cast<uint8_t>(Ldac::kSampleRate44100 | Ldac::kSampleRate48000 |
									   Ldac::kSampleRate88200 | Ldac::kSampleRate96000),
		Ldac::kChannelStereo, ldacIe);
	AddLocalSep(ctx, Avdtp::kLocalLdacSeid, Ioctl::kCodecLdac, Avdtp::CodecType::Vendor, ldacIe,
		sizeof(ldacIe));
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
	// A read fault belongs to the connection that hit it.
	InterlockedExchange(&ctx->ReadFault, 0);
	// The codecs are re-learned by this connection's discovery.
	InterlockedExchange(&ctx->SinkCodecs, 0);
	InterlockedExchange(&ctx->SinkHdCapabilities, 0);
	InterlockedExchange(&ctx->SinkAptxCapabilities, 0);
	InterlockedExchange(&ctx->SinkAptxLlCapabilities, 0);
	InterlockedExchange(&ctx->SinkLdacCapabilities, 0);
	ctx->AppliedPreferenceParameters = InterlockedCompareExchange(&ctx->PreferenceParameters, 0, 0);
	// Packets queued for the previous session must not reach the next one.
	ctx->SignalQueueHead = 0;
	ctx->SignalQueueCount = 0;
	PublishStatus(ctx);
	KeSetEvent(&ctx->SessionChanged, IO_NO_INCREMENT, FALSE);
}
} // namespace OpenA2dpDriver::Signaling
