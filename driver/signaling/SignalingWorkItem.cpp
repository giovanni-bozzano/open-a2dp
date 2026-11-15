#include "SignalingInternal.h"

namespace OpenA2dpDriver::Signaling
{
// Kicks off the initiator's media channel open; its completion re-enters the work item.
static void StartMediaOpen(DEVICE_CONTEXT* ctx)
{
	++ctx->MediaOpenAttempts;
	const NTSTATUS status = L2capStartMediaOpen(ctx);
	if (status != STATUS_PENDING)
	{
		OA2DP_ERROR(ctx, "L2CAP", "media channel open could not start 0x%08X; tearing down the signaling channel", status);
		L2capCloseSignalingChannel(ctx);
	}
}

// Collects a finished media channel open. The sink sometimes refuses the first media
// channel (L2CAP response 4, "no resources") because a previous session has not fully
// released it. Rather than sleeping and retrying blindly, ABORT the stream and
// rediscover once the ABORT response arrives: the round trips give the sink the event
// it needs to free the channel. Give up after a few attempts.
static void FinishMediaOpen(DEVICE_CONTEXT* ctx)
{
	NTSTATUS status;
	if (!L2capFinishMediaOpen(ctx, &status))
	{
		return;
	}
	if (NT_SUCCESS(status))
	{
		if (ctx->Session.State() == Avdtp::SessionState::Open)
		{
			// START is the endpoint's request, not the channel's: a renegotiation with nothing
			// playing opens a channel for a stream that must stay suspended, and starting it
			// would put the sink in Streaming with silence behind it and no pause to end it.
			// The endpoint edge is what starts the stream, and it is decided below.
			InterlockedExchange(&ctx->EndpointStreamPending, 1);
		}
		else
		{
			// The session left Open while the channel was opening; the next Idle closes it.
			OA2DP_INFO(ctx, "L2CAP", "media channel opened in state %s", AvdtpStateName(ctx->Session.State()));
		}
		return;
	}
	if (ctx->Session.State() != Avdtp::SessionState::Open)
	{
		return; // the session moved on while the channel was opening; nothing to recover
	}
	if (ctx->MediaOpenAttempts < kMediaOpenAttempts)
	{
		OA2DP_WARN(ctx, "L2CAP", "media channel open attempt %u failed; aborting the stream to retry",
			static_cast<unsigned>(ctx->MediaOpenAttempts));
		ctx->MediaOpenRetry = TRUE;
		Transmit(ctx, ctx->Session.Abort(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		if (ctx->Session.State() == Avdtp::SessionState::Aborting)
		{
			return;
		}
		ctx->MediaOpenRetry = FALSE;
	}
	OA2DP_ERROR(ctx, "L2CAP", "media channel open failed after %u attempts; tearing down the signaling channel",
		static_cast<unsigned>(ctx->MediaOpenAttempts));
	// Tear the session down so the sink releases its resources and can be reconnected
	// cleanly instead of leaving a half-open AVDTP session that never reaches Streaming.
	L2capCloseSignalingChannel(ctx);
}

// Restarts discovery once the ABORT sent for a failed media open has been answered.
static void ContinueMediaOpenRetry(DEVICE_CONTEXT* ctx)
{
	const auto state = ctx->Session.State();
	if (state == Avdtp::SessionState::Idle)
	{
		ctx->MediaOpenRetry = FALSE;
		OA2DP_INFO(ctx, "Signaling", "stream aborted; rediscovering for media open attempt %u",
			static_cast<unsigned>(ctx->MediaOpenAttempts + 1));
		Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}
	else if (state == Avdtp::SessionState::Failed)
	{
		ctx->MediaOpenRetry = FALSE;
		OA2DP_ERROR(ctx, "Signaling", "ABORT for the media open retry failed; tearing down the signaling channel");
		L2capCloseSignalingChannel(ctx);
	}
	else if (state != Avdtp::SessionState::Aborting)
	{
		ctx->MediaOpenRetry = FALSE; // the sink moved the session on by itself
	}
}

// The codec the current preference selects, or false when it cannot be honoured
// (for example AAC forced while no encoder service is registered). RemoteOffers is
// the previous discovery's view; after a reconnect the new discovery decides.
// `codecValue` is the Ioctl::kCodec* value, which is the only way to tell the two aptX
// codecs apart once they share Avdtp::CodecType::Vendor.
// True for the codecs an encoder provider supplies; the kernel encodes SBC itself, so a
// missing provider can never make it unusable.
static bool ProviderCodec(uint32_t codecValue) noexcept
{
	return codecValue == Ioctl::kCodecAac || codecValue == Ioctl::kCodecAptx ||
		codecValue == Ioctl::kCodecAptxHd || codecValue == Ioctl::kCodecLdac;
}

static bool DesiredCodec(DEVICE_CONTEXT* ctx, Avdtp::CodecType& codec, uint32_t& codecValue)
{
	Ioctl::Preference preference;
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		preference = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	}
	const LONG sinkCodecs = InterlockedCompareExchange(&ctx->SinkCodecs, 0, 0);
	const auto offers = [sinkCodecs](uint32_t bit) { return sinkCodecs != 0 && (sinkCodecs & static_cast<LONG>(bit)) != 0; };
	// aptX HD runs only on the 24-bit endpoint format and in stereo; anything else has to use
	// a codec that consumes 16-bit PCM.
	const bool hdUsable = ServiceCodecRegistered(ctx, Ioctl::kCodecAptxHd) && offers(Ioctl::kSinkCodecAptxHd) &&
		ctx->PinValidBits == AptxHd::kBitsPerSample && ctx->PinChannels == 2;
	// Classic aptX takes 16-bit PCM and narrows a 24-in-32 pin in the service, in stereo.
	const bool aptxUsable = ServiceCodecRegistered(ctx, Ioctl::kCodecAptx) && offers(Ioctl::kSinkCodecAptx) &&
		AptxPinDepthUsable(ctx->PinValidBits, Aptx::kBitsPerSample) && ctx->PinChannels == 2;
	// LDAC likewise narrows a 24-in-32 pin, and needs a link that carries its 2-DH5 payload.
	const size_t ldacPayload =
		(ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu) - Rtp::kHeaderSize;
	const bool ldacUsable = ServiceCodecRegistered(ctx, Ioctl::kCodecLdac) && offers(Ioctl::kSinkCodecLdac) &&
		AptxPinDepthUsable(ctx->PinValidBits, Ldac::kBitsPerSample) && ctx->PinChannels == 2 &&
		ldacPayload >= Ldac::kMinMtuBytes;
	// A forced codec that cannot run keeps the stream silent, which is what the preference
	// asks for - except while a provider that served the configured stream is missing: the
	// fallback then takes a codec the driver encodes itself, and the provider's return
	// switches back to the stored preference.
	const bool fallback = InterlockedCompareExchange(&ctx->ProviderFallback, 0, 0) != 0;
	const bool forceCodec = (preference.flags & Ioctl::kPreferenceForceCodec) != 0;
	if (forceCodec)
	{
		switch (preference.codec)
		{
		case Ioctl::kCodecSbc:
			codec = Avdtp::CodecType::Sbc;
			codecValue = Ioctl::kCodecSbc;
			return true;
		case Ioctl::kCodecLdac:
			if (ldacUsable)
			{
				codec = Avdtp::CodecType::Vendor;
				codecValue = Ioctl::kCodecLdac;
				return true;
			}
			break;
		case Ioctl::kCodecAptxHd:
			if (hdUsable)
			{
				codec = Avdtp::CodecType::Vendor;
				codecValue = Ioctl::kCodecAptxHd;
				return true;
			}
			break;
		case Ioctl::kCodecAptx:
			if (aptxUsable)
			{
				codec = Avdtp::CodecType::Vendor;
				codecValue = Ioctl::kCodecAptx;
				return true;
			}
			break;
		case Ioctl::kCodecAac:
			if (ServiceCodecRegistered(ctx, Ioctl::kCodecAac) && offers(Ioctl::kSinkCodecAac))
			{
				codec = Avdtp::CodecType::Aac;
				codecValue = Ioctl::kCodecAac;
				return true;
			}
			break;
		default:
			return false;
		}
		if (!fallback)
		{
			return false;
		}
	}
	// Automatic order is quality order: LDAC, aptX HD, classic aptX, AAC, then the mandatory
	// SBC fallback.
	if (ldacUsable)
	{
		codec = Avdtp::CodecType::Vendor;
		codecValue = Ioctl::kCodecLdac;
		return true;
	}
	if (hdUsable)
	{
		codec = Avdtp::CodecType::Vendor;
		codecValue = Ioctl::kCodecAptxHd;
		return true;
	}
	if (aptxUsable)
	{
		codec = Avdtp::CodecType::Vendor;
		codecValue = Ioctl::kCodecAptx;
		return true;
	}
	if (ServiceCodecRegistered(ctx, Ioctl::kCodecAac) && offers(Ioctl::kSinkCodecAac))
	{
		codec = Avdtp::CodecType::Aac;
		codecValue = Ioctl::kCodecAac;
		return true;
	}
	codec = Avdtp::CodecType::Sbc;
	codecValue = Ioctl::kCodecSbc;
	return true;
}

// Sends the CLOSE half of a switch and arranges the rediscovery that follows its
// response, for changes AVDTP cannot apply in place (a codec change, or a rejected
// RECONFIGURE).
static void BeginCodecSwitchClose(DEVICE_CONTEXT* ctx)
{
	ctx->ReconfigurePhase = ReconfigureNone;
	// A renegotiation is a new stream, so the endpoint gets fresh START attempts for it.
	InterlockedExchange(&ctx->EndpointStartAttempts, 0);
	// A rediscovery after a close is the retry for an MTU mismatch, so the pending
	// in-place reconfiguration must not also run.
	InterlockedExchange(&ctx->MtuRenegotiate, 0);
	if (ctx->Session.State() == Avdtp::SessionState::Streaming)
	{
		// The CLOSE ends this stream, and a codec change cannot be applied in place: nothing
		// more may be sent under the configuration the sink is being told to drop.
		MediaStop(ctx);
	}
	Transmit(ctx, ctx->Session.Close(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	ctx->CodecSwitchClosing = ctx->Session.State() == Avdtp::SessionState::Closing;
	if (ctx->CodecSwitchClosing)
	{
		// Until a stream runs the new codec, a teardown by the sink still belongs to
		// this switch and is recovered by reconnecting.
		ctx->CodecSwitchRecover = TRUE;
		ctx->CodecSwitchReconnects = 0;
	}
	else
	{
		ctx->CodecSwitchWanted = TRUE; // no CLOSE was possible; retry on the next event
	}
}

// Sends the CLOSE half of a format change. AVDTP cannot change the sampling frequency or
// the channel count with RECONFIGURE, so the stream has to be closed; unlike a codec switch
// the sink SEP and its capabilities stay known, so FormatSwitchPending re-configures that
// SEP instead of rediscovering the sink.
static void BeginFormatSwitch(DEVICE_CONTEXT* ctx)
{
	ctx->ReconfigurePhase = ReconfigureNone;
	InterlockedExchange(&ctx->EndpointStartAttempts, 0);
	InterlockedExchange(&ctx->MtuRenegotiate, 0);
	if (ctx->Session.State() == Avdtp::SessionState::Streaming)
	{
		// The endpoint already runs at the new format, so nothing more may be sent at the
		// old one while the CLOSE is in flight.
		MediaStop(ctx);
	}
	ctx->FormatSwitchPending = TRUE;
	Transmit(ctx, ctx->Session.Close(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	ctx->CodecSwitchClosing = ctx->Session.State() == Avdtp::SessionState::Closing;
	if (ctx->CodecSwitchClosing)
	{
		// A sink that answers this CLOSE by dropping the signaling channel is reconnected,
		// so the new configuration still reaches it.
		ctx->CodecSwitchRecover = TRUE;
		ctx->CodecSwitchReconnects = 0;
	}
	else
	{
		ctx->FormatSwitchPending = FALSE;
		ctx->CodecSwitchWanted = TRUE; // no CLOSE was possible; retry on the next event
	}
}

// Configures the SEP of the stream that was just closed for the format the endpoint runs at.
// False when the SEP is no longer known or the request could not be sent, which leaves the
// caller to rediscover; a refused configuration fails the session, and the retry flag makes
// the next event rediscover as well.
static bool ReconfigureClosedStream(DEVICE_CONTEXT* ctx)
{
	ctx->FormatSwitchPending = FALSE;
	uint8_t ie[Avdtp::kMaxCodecIe];
	size_t ieLength = 0;
	if (!ctx->Session.RefreshConfiguration(ie, ieLength))
	{
		return false;
	}
	const size_t length = ctx->Session.Configure(ie, ieLength, ctx->WriteBuffer, sizeof(ctx->WriteBuffer));
	if (length == 0)
	{
		return false;
	}
	OA2DP_INFO(ctx, "Signaling",
		"stream closed; configuring the same SEP for %u Hz %u ch without rediscovering",
		ctx->PinSampleRate, static_cast<unsigned>(ctx->PinChannels));
	ctx->CodecSwitchWanted = TRUE;
	Transmit(ctx, length);
	// The media channel stays open across this CLOSE, so nothing else says the stream came
	// back: the endpoint edge is what starts it again when audio plays, and leaves it
	// suspended when it does not.
	InterlockedExchange(&ctx->EndpointStreamPending, 1);
	return true;
}

// Sends RECONFIGURE from the Open state with the configuration the current parameters
// select. The sink's advertised capabilities for the SEP in use are re-run through the
// selector, so the request is clamped to the same range discovery would use.
static void BeginReconfigure(DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->EndpointStartAttempts, 0);
	uint8_t ie[Avdtp::kMaxCodecIe];
	size_t ieLength = 0;
	// A stream left without media because its AAC bitrate does not fit the real media MTU
	// is reconfigured at a bitrate the link carries; anything else is rebuilt from the sink
	// capabilities the original selection used.
	const bool mtuCapped = InterlockedExchange(&ctx->MtuRenegotiate, 0) != 0;
	const bool built = mtuCapped ? BuildMtuCappedConfiguration(ctx, ie, ieLength)
								 : ctx->Session.RefreshConfiguration(ie, ieLength);
	if (!built)
	{
		OA2DP_WARN(ctx, "Signaling", "no configuration can be built from the new parameters; closing to renegotiate");
		BeginCodecSwitchClose(ctx);
		return;
	}
	if (mtuCapped)
	{
		OA2DP_INFO(ctx, "AVDTP", "sending RECONFIGURE at a bitrate the media MTU carries (%u bytes)",
			static_cast<unsigned>(ieLength));
	}
	else
	{
		OA2DP_INFO(ctx, "AVDTP", "%s parameters changed; sending RECONFIGURE (%u bytes)",
			CodecName(ctx->Session.SelectedCodec()), static_cast<unsigned>(ieLength));
	}
	Transmit(ctx, ctx->Session.Reconfigure(ie, ieLength, ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	if (ctx->Session.HasPendingCommand())
	{
		ctx->ReconfigurePhase = ReconfigureConfiguring;
	}
	else
	{
		ctx->CodecSwitchWanted = TRUE; // could not send; retry on the next event
	}
}

// Continues a parameter-only change through SUSPEND -> RECONFIGURE -> START. The
// stream keeps its codec, SEP and media channel, so the sink does not rediscover and
// the connection is not dropped.
static void ContinueReconfigure(DEVICE_CONTEXT* ctx)
{
	const auto state = ctx->Session.State();
	if (ctx->ReconfigurePhase == ReconfigureSuspending)
	{
		if (state == Avdtp::SessionState::Suspending)
		{
			return; // response still out
		}
		ctx->ReconfigurePhase = ReconfigureNone;
		if (state == Avdtp::SessionState::Streaming)
		{
			OA2DP_WARN(ctx, "Signaling", "sink refused to suspend; closing to apply the parameters");
			BeginCodecSwitchClose(ctx);
		}
		else if (state == Avdtp::SessionState::Open)
		{
			BeginReconfigure(ctx);
		}
		return;
	}
	if (ctx->ReconfigurePhase == ReconfigureConfiguring)
	{
		if (state != Avdtp::SessionState::Open)
		{
			ctx->ReconfigurePhase = ReconfigureNone; // the session moved on; nothing to resume
			return;
		}
		if (ctx->Session.HasPendingCommand())
		{
			return; // response still out
		}
		if (!ctx->Session.LastReconfigureAccepted())
		{
			OA2DP_WARN(ctx, "Signaling", "sink rejected the new parameters; closing to renegotiate");
			BeginCodecSwitchClose(ctx);
			return;
		}
		if (!AudioStreamPlaying(ctx))
		{
			// The parameters were renegotiated for a stream that is not playing - a setting
			// changed while nothing plays, or the endpoint paused during the renegotiation. There
			// is nothing to resume, and START here would put the sink in Streaming with silence
			// behind it and no pause to end it. The next audio starts the stream.
			ctx->ReconfigurePhase = ReconfigureNone;
			OA2DP_INFO(ctx, "Signaling", "parameters applied while nothing plays; the stream stays suspended");
			return;
		}
		Transmit(ctx, ctx->Session.Start(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		if (ctx->Session.HasPendingCommand())
		{
			ctx->ReconfigurePhase = ReconfigureStarting;
		}
		else
		{
			ctx->ReconfigurePhase = ReconfigureNone;
			BeginCodecSwitchClose(ctx);
		}
		return;
	}
	// ReconfigureStarting
	if (state == Avdtp::SessionState::Starting)
	{
		return; // response still out
	}
	ctx->ReconfigurePhase = ReconfigureNone;
	if (state == Avdtp::SessionState::Open)
	{
		OA2DP_WARN(ctx, "Signaling", "sink refused to restart the reconfigured stream; closing to renegotiate");
		BeginCodecSwitchClose(ctx);
	}
	// Streaming: the media start on the transition already picked up the new IE.
}

// A codec or parameter selection input changed (SET_PREFERENCE or REGISTER_ENCODER).
// Every session transition re-runs the work item, so the decision waits for a settled
// state: a different codec closes and rediscovers, while changed parameters are applied
// in place. Any other outcome drops the request. Both handshakes are resumed through
// their phase flag, because the latched event is consumed when the command goes out
// and no new event marks the response.
static void ContinueCodecSwitch(DEVICE_CONTEXT* ctx)
{
	const auto state = ctx->Session.State();
	if (ctx->CodecSwitchClosing)
	{
		if (state == Avdtp::SessionState::Closing)
		{
			return;
		}
		ctx->CodecSwitchClosing = FALSE;
		ctx->CodecSwitchWanted = FALSE;
		if (ctx->FormatSwitchPending && state == Avdtp::SessionState::Idle && !ctx->MediaOpenRetry)
		{
			if (ReconfigureClosedStream(ctx))
			{
				return;
			}
			OA2DP_WARN(ctx, "Signaling", "the closed stream cannot be configured again; rediscovering");
		}
		// Only this CLOSE was meant to re-configure the SEP; anything else that settles the
		// session (a media retry, a failed close) must not carry the flag into a later switch.
		ctx->FormatSwitchPending = FALSE;
		if (state == Avdtp::SessionState::Idle && !ctx->MediaOpenRetry)
		{
			Avdtp::CodecType desired;
			uint32_t desiredValue = 0;
			if (DesiredCodec(ctx, desired, desiredValue))
			{
				OA2DP_INFO(ctx, "Signaling", "stream closed; rediscovering to negotiate %s",
					CodecValueName(desiredValue));
				Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
			}
		}
		return;
	}
	if (ctx->ReconfigurePhase != ReconfigureNone)
	{
		ContinueReconfigure(ctx);
		return;
	}
	if (state != Avdtp::SessionState::Streaming && state != Avdtp::SessionState::Open &&
		state != Avdtp::SessionState::Idle && state != Avdtp::SessionState::Failed)
	{
		return; // not settled yet; the next transition re-runs this
	}
	const bool retry = ctx->CodecSwitchWanted != FALSE;
	ctx->CodecSwitchWanted = FALSE;
	if (state == Avdtp::SessionState::Failed && retry)
	{
		// A configuration the sink refused (or a CLOSE that never landed) leaves the stream
		// unusable; a discovery rebuilds it from the sink's own capabilities.
		OA2DP_INFO(ctx, "Signaling", "the sink refused the new configuration; rediscovering");
		Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		return;
	}
	if (state == Avdtp::SessionState::Idle || state == Avdtp::SessionState::Failed)
	{
		return; // the next discovery already sees the current preference
	}
	Avdtp::CodecType desired;
	uint32_t desiredValue = 0;
	if (!DesiredCodec(ctx, desired, desiredValue))
	{
		return;
	}
	// The two aptX codecs share Avdtp::CodecType::Vendor, so the negotiated value is the
	// only thing that can tell them apart; an HD-to-aptX change is a codec change too.
	uint32_t selectedValue = 0;
	const bool codecDiffers = SelectedCodecValue(ctx, selectedValue)
		? selectedValue != desiredValue
		: ctx->Session.SelectedCodec() != desired;
	const bool formatDiffers = !SelectedFormatMatchesPin(ctx);
	// Set by the media path when the stream started without audio because the AAC bitrate
	// the negotiation picked does not fit the MTU the media channel reports.
	bool mtuDiffers = InterlockedCompareExchange(&ctx->MtuRenegotiate, 0, 0) != 0;
	const LONG parameters = InterlockedCompareExchange(&ctx->PreferenceParameters, 0, 0);
	const bool parametersDiffer = ctx->AppliedPreferenceParameters != parameters;
	if (parametersDiffer && mtuDiffers)
	{
		// The renegotiation for the parameter change rebuilds the configuration from the sink
		// capabilities, which clamps to the real MTU too, so the MTU-only retry is covered.
		InterlockedExchange(&ctx->MtuRenegotiate, 0);
		mtuDiffers = false;
	}
	if (!codecDiffers && !formatDiffers && !parametersDiffer && !mtuDiffers)
	{
		return;
	}
	// AppliedPreferenceParameters records the parameters the stream was actually rebuilt
	// with, so it is advanced only where the change is committed. Marking it here would
	// lose the request on the two deferred paths below: they retry through
	// CodecSwitchWanted and would then find no difference left to apply.
	if (codecDiffers)
	{
		ctx->AppliedPreferenceParameters = parameters; // the rediscovery renegotiates from it
		OA2DP_INFO(ctx, "Signaling", "%s stream selected but the preference wants %s; closing to renegotiate",
			CodecLabel(ctx->Session.SelectedCodec(), ctx->Session.SelectedIe(), ctx->Session.SelectedIeLength()),
			CodecValueName(desiredValue));
		BeginCodecSwitchClose(ctx);
		return;
	}
	if (formatDiffers)
	{
		// RECONFIGURE cannot change the sampling frequency or the channel count of a
		// configured stream, so it is closed and configured again at the new format - on the
		// same sink SEP, with no discovery and no new connection.
		ctx->AppliedPreferenceParameters = parameters; // the reconfiguration negotiates from it
		OA2DP_INFO(ctx, "Signaling",
			"the endpoint now runs at %u Hz %u ch while the stream is configured for another format; closing to "
			"reconfigure it",
			ctx->PinSampleRate, static_cast<unsigned>(ctx->PinChannels));
		BeginFormatSwitch(ctx);
		return;
	}
	if (mtuDiffers)
	{
		OA2DP_INFO(ctx, "Signaling",
			"the AAC bitrate does not fit the media MTU; reconfiguring at a bitrate it carries");
	}
	if (state == Avdtp::SessionState::Streaming)
	{
		if (parametersDiffer)
		{
			OA2DP_INFO(ctx, "Signaling", "%s parameters changed; suspending to reconfigure",
				CodecValueName(desiredValue));
		}
		// Stop the encoder before the sink suspends so no packet describes the old
		// configuration once the new one is negotiated.
		MediaStop(ctx);
		Transmit(ctx, ctx->Session.Suspend(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		if (ctx->Session.State() == Avdtp::SessionState::Suspending)
		{
			ctx->ReconfigurePhase = ReconfigureSuspending;
			ctx->AppliedPreferenceParameters = parameters;
		}
		else
		{
			ctx->CodecSwitchWanted = TRUE; // could not send; retry on the next event
		}
		return;
	}
	// Open: the stream is already suspended, so RECONFIGURE applies directly. Wait
	// for the media transport first, since START would otherwise have no channel.
	if (ctx->MediaChannel == nullptr)
	{
		ctx->CodecSwitchWanted = TRUE; // retried when the channel open completes
		return;
	}
	ctx->AppliedPreferenceParameters = parameters;
	BeginReconfigure(ctx);
}

// Makes the AVDTP stream follow the endpoint: streaming while it plays audio, suspended
// otherwise. Opening the transport, finishing a renegotiation and every transition to Open or
// Streaming ask for this instead of deciding themselves: none of them says whether anything is
// playing, and a stream started for an endpoint with nothing to play leaves the sink in
// Streaming with silence behind it. SUSPEND is legal only from Streaming and START only from
// Open, so an edge that arrives mid-handshake leaves the request pending and the response that
// settles the session runs this again.
static void ContinueEndpointStream(DEVICE_CONTEXT* ctx)
{
	// Claimed before the endpoint is read: an edge that lands after this sets the request again
	// and re-queues the work item, whereas clearing it after the read would erase an edge - the
	// first audio arriving just after `playing` was read - and leave the session silent. Every
	// path that is not done with the request puts it back.
	InterlockedExchange(&ctx->EndpointStreamPending, 0);
	const Avdtp::SessionState state = ctx->Session.State();
	const bool playing = AudioStreamPlaying(ctx);
	// A running stream keeps streaming even before its first audio: that is a resume, and
	// suspending it would only make the audio that follows pay for START. A paused one keeps
	// it for the grace too, so a short pause costs nothing either.
	const bool keepStreaming =
		AudioStreamRunning(ctx) || InterlockedCompareExchange(&ctx->PauseGrace, 0, 0) != 0;
	if (state == Avdtp::SessionState::Streaming && !keepStreaming)
	{
		OA2DP_INFO(ctx, "AVDTP", "suspending stream, the endpoint stayed paused");
		Transmit(ctx, ctx->Session.Suspend(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		if (ctx->Session.State() != Avdtp::SessionState::Suspending)
		{
			InterlockedExchange(&ctx->EndpointStreamPending, 1); // the SUSPEND never left
		}
		else
		{
			// The request is queued, so this stream ends here: nothing more is sent into it, and a
			// sink that refuses the suspend comes back through the transition to Streaming, where
			// MediaStart builds the stream again instead of re-initialising a live one under it.
			MediaStop(ctx);
		}
		return;
	}
	if (state == Avdtp::SessionState::Open && playing)
	{
		// The channel and a settled command, or START would race the open or a reconfigure.
		// Both re-enter this through their own completion.
		if (ctx->MediaChannel == nullptr || ctx->ReconfigurePhase != ReconfigureNone ||
			ctx->Session.HasPendingCommand())
		{
			InterlockedExchange(&ctx->EndpointStreamPending, 1);
			return;
		}
		// Every return to Open asks for this, so a sink that refuses START or a media path that
		// fails again at once would be restarted forever; a run or a renegotiation re-arms it.
		const LONG attempt = InterlockedIncrement(&ctx->EndpointStartAttempts);
		if (attempt > kEndpointStartAttempts)
		{
			if (attempt == kEndpointStartAttempts + 1)
			{
				OA2DP_WARN(ctx, "AVDTP", "the stream did not stay up after %u starts; waiting for the next run",
					static_cast<unsigned>(kEndpointStartAttempts));
			}
			return;
		}
		OA2DP_INFO(ctx, "AVDTP", "starting stream, the endpoint plays");
		// The link may be in sniff from the paused stretch: ask for active mode first so START and
		// the first packets do not wait for the next sniff slot.
		L2capRequestActiveMode(ctx);
		Transmit(ctx, ctx->Session.Start(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		if (ctx->Session.State() != Avdtp::SessionState::Starting)
		{
			InterlockedDecrement(&ctx->EndpointStartAttempts);
			InterlockedExchange(&ctx->EndpointStreamPending, 1); // the START never left
		}
		return;
	}
	if (state == Avdtp::SessionState::Idle && playing)
	{
		// A sink tears down a stream that stays Open without START, and an endpoint that is
		// already running brings no run edge to rediscover it. Audio on an Idle session is
		// what brings the stream back. A close or a media retry in flight rediscovers by itself.
		if (ctx->CodecSwitchClosing || ctx->MediaOpenRetry || ctx->Session.HasPendingCommand())
		{
			InterlockedExchange(&ctx->EndpointStreamPending, 1);
			return;
		}
		const LONG attempt = InterlockedIncrement(&ctx->EndpointStartAttempts);
		if (attempt > kEndpointStartAttempts)
		{
			if (attempt == kEndpointStartAttempts + 1)
			{
				OA2DP_WARN(ctx, "AVDTP", "the stream did not stay up after %u starts; waiting for the next run",
					static_cast<unsigned>(kEndpointStartAttempts));
			}
			return;
		}
		OA2DP_INFO(ctx, "Signaling", "the endpoint plays on an idle session; discovering");
		Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		return;
	}
	// Settled and already matching, or no session to move: the request is spent. Every other
	// state is a handshake in flight, which keeps it so the response decides.
	const bool settled = (state == Avdtp::SessionState::Streaming && keepStreaming) ||
		(state == Avdtp::SessionState::Open && !playing);
	const bool unusable = state == Avdtp::SessionState::Idle || state == Avdtp::SessionState::Failed ||
		state == Avdtp::SessionState::Closing || state == Avdtp::SessionState::Aborting;
	if (!settled && !unusable)
	{
		InterlockedExchange(&ctx->EndpointStreamPending, 1);
	}
}

void OnSignalingWorkItem(_In_ WDFWORKITEM workItem)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfWorkItemGetParentObject(workItem));
	// Before the session lock: the rebuild notifies ACX, and the callers that only set the flag
	// (the encoder service's cleanup) hold the control lock while doing so.
	if (InterlockedExchange(&ctx->FormatsPending, 0) != 0)
	{
		const NTSTATUS status = AudioApplyEndpointFormats(ctx);
		if (!NT_SUCCESS(status))
		{
			OA2DP_WARN(ctx, "Signaling",
				"applying the endpoint formats after a provider change failed 0x%08X", status);
		}
	}
	// A pause that keeps the stream Streaming (the grace) leaves START unsent, so the wake on
	// that path never runs for the resume that follows. Done before the session lock: the
	// request touches no session state and must not lengthen the wait behind it.
	if (InterlockedExchange(&ctx->PauseWakePending, 0) != 0)
	{
		L2capRequestActiveMode(ctx);
	}
	WdfWaitLockAcquire(ctx->SessionLock, nullptr);
	ctx->SignalingWorkItemThread = KeGetCurrentThread();
	PumpSignalSends(ctx);

	if (InterlockedExchange(&ctx->ReadFault, 0) != 0)
	{
		// The read that lets the session hear the sink failed and was not re-armed. Closing the
		// channel is the only recovery that leaves a usable state; reached from inside the work
		// item, so the close skips the lock this thread holds and does not flush itself.
		OA2DP_ERROR(ctx, "Signaling", "signaling read failed; closing the channel");
		L2capCloseSignalingChannel(ctx);
		ctx->SignalingWorkItemThread = nullptr;
		WdfWaitLockRelease(ctx->SessionLock);
		return;
	}

	if (InterlockedExchange(&ctx->ConnectPending, 0) != 0 && ctx->SignalingState == ChannelOpen)
	{
		// The sink can come back after a disconnect left its node loaded: the tools find it
		// through the link, so it goes back before the endpoint is plugged in.
		ServicePublishLink(ctx, true);
		AudioNotifyLinkState(ctx);
		// A service that registers after this discovery renegotiates the stream as AAC
		// (SignalingOnEncoderRegistered), so nothing waits for it here.
		if (ctx->SignalingState == ChannelOpen)
		{
			ResetSession(ctx);
			InterlockedExchange(&ctx->EndpointStartAttempts, 0);
			if (InterlockedCompareExchange(&ctx->SignalingIncoming, 0, 0) != 0)
			{
				// The sink connected to us and normally starts its own AVDTP
				// discovery and configuration at once. Starting ours too makes both
				// sides configure the same stream: the sink can then get its own
				// SET_CONFIGURATION rejected (SEP in use) and refuse the media
				// channel with "no resources". Listen first; the library accepts the
				// sink's configuration, and we discover only when Windows plays
				// audio while the session is still Idle (here or SignalingOnAudioRun).
				if (AudioStreamRunning(ctx))
				{
					OA2DP_INFO(ctx, "Signaling", "sink-initiated with audio running; starting discovery");
					Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
				}
				else
				{
					OA2DP_INFO(ctx, "Signaling", "sink-initiated; waiting for it to configure the stream or for audio");
				}
				SubmitRead(ctx);
			}
			else
			{
				Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
				SubmitRead(ctx);
			}
		}
	}

	if (InterlockedExchange(&ctx->ReadCompleted, 0) != 0)
	{
		if (ctx->SignalingState == ChannelOpen)
		{
			const size_t length = ctx->ReadBrb.BufferSize;
			if (ctx->Session.State() == Avdtp::SessionState::Discovering && length >= 2 &&
				(ctx->ReadBuffer[0] & 0x0F) == 0x02 && ctx->ReadBuffer[1] == 0x01)
			{
				for (size_t pos = 2; pos + 2 <= length; pos += 2)
				{
					OA2DP_INFO(ctx, "AVDTP", "discover SEP seid %u inUse %u media %u sepType %u",
						static_cast<unsigned>(ctx->ReadBuffer[pos] >> 2), static_cast<unsigned>((ctx->ReadBuffer[pos] >> 1) & 1),
						static_cast<unsigned>(ctx->ReadBuffer[pos + 1] >> 4), static_cast<unsigned>((ctx->ReadBuffer[pos + 1] >> 3) & 1));
				}
			}
			if ((ctx->Session.State() == Avdtp::SessionState::Aborting || ctx->Session.State() == Avdtp::SessionState::Closing) &&
				length >= 2 && ctx->ReadBuffer[1] == static_cast<uint8_t>(ctx->Session.State() == Avdtp::SessionState::Closing
					? Avdtp::Signal::Close : Avdtp::Signal::Abort))
			{
				const unsigned responseType = ctx->ReadBuffer[0] & 0x03;
				OA2DP_INFO(ctx, "AVDTP", "%s response: %s (type %u), length %u, code 0x%02X",
					ctx->Session.State() == Avdtp::SessionState::Closing ? "CLOSE" : "ABORT",
					responseType == 2 ? "accepted" : (responseType == 3 ? "rejected" : "unexpected"), responseType,
					static_cast<unsigned>(length), static_cast<unsigned>(length > 2 ? ctx->ReadBuffer[2] : 0));
			}
			Transmit(ctx, ctx->Session.OnPacket(ctx->ReadBuffer, length, ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		}
		SubmitRead(ctx);
	}

	if (InterlockedExchange(&ctx->AudioRunPending, 0) != 0 && ctx->SignalingState == ChannelOpen &&
		InterlockedCompareExchange(&ctx->SignalingIncoming, 0, 0) != 0 &&
		ctx->Session.State() == Avdtp::SessionState::Idle)
	{
		OA2DP_INFO(ctx, "Signaling", "audio started on a silent sink-initiated link; starting discovery");
		Transmit(ctx, ctx->Session.Connect(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}

	if (InterlockedExchange(&ctx->SuspendPending, 0) != 0 && ctx->Session.State() == Avdtp::SessionState::Streaming)
	{
		OA2DP_INFO(ctx, "AVDTP", "suspending stream");
		Transmit(ctx, ctx->Session.Suspend(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
	}

	if (InterlockedExchange(&ctx->TimeoutPending, 0) != 0 && ctx->SignalingState == ChannelOpen &&
		ctx->ResponseDeadline != 0 && ctx->Session.HasPendingCommand())
	{
		const ULONGLONG now = KeQueryInterruptTime();
		if (now < ctx->ResponseDeadline)
		{
			WdfTimerStart(ctx->ResponseTimer, -static_cast<LONGLONG>(ctx->ResponseDeadline - now));
		}
		else
		{
			OA2DP_WARN(ctx, "AVDTP", "no response within %u ms in state %s (%u)", static_cast<unsigned>(ResponseTimeoutMs),
				AvdtpStateName(ctx->Session.State()), static_cast<unsigned>(ctx->Session.State()));
			Transmit(ctx, ctx->Session.OnTimeout(ctx->WriteBuffer, sizeof(ctx->WriteBuffer)));
		}
	}

	if (InterlockedExchange(&ctx->MediaOpenPending, 0) != 0 && ctx->SignalingState == ChannelOpen &&
		ctx->Session.State() == Avdtp::SessionState::Open && ctx->MediaChannel == nullptr && ctx->MediaOpenOp == nullptr)
	{
		StartMediaOpen(ctx);
	}

	if (InterlockedExchange(&ctx->MediaOpenCompleted, 0) != 0 && ctx->SignalingState == ChannelOpen)
	{
		FinishMediaOpen(ctx);
	}

	if (ctx->MediaOpenRetry && ctx->SignalingState == ChannelOpen)
	{
		ContinueMediaOpenRetry(ctx);
	}

	if (InterlockedExchange(&ctx->ProviderLossCheck, 0) != 0)
	{
		// The encoder service closed while this stream was configured. Wait out the grace
		// period before rebuilding it: its registration disarms this, so a restart costs no
		// renegotiation and only a service that stays away changes the stream.
		uint32_t selectedValue = 0;
		if (SelectedCodecValue(ctx, selectedValue) && ProviderCodec(selectedValue) &&
			!ServiceCodecRegistered(ctx, selectedValue))
		{
			OA2DP_WARN(ctx, "Signaling",
				"the encoder service left while %s was configured; falling back in %u ms unless it returns",
				CodecValueName(selectedValue), static_cast<unsigned>(ProviderLossGraceMs));
			WdfTimerStart(ctx->ProviderLossTimer,
				-static_cast<LONGLONG>(WDF_REL_TIMEOUT_IN_MS(ProviderLossGraceMs)));
		}
	}

	if (InterlockedExchange(&ctx->CodecSwitchPending, 0) != 0)
	{
		ctx->CodecSwitchWanted = TRUE;
	}
	// A close or a reconfigure step in flight keeps the decision alive with no event
	// of its own; the response re-enters the work item through the read completion.
	if ((ctx->CodecSwitchWanted || ctx->CodecSwitchClosing || ctx->ReconfigurePhase != ReconfigureNone ||
			InterlockedCompareExchange(&ctx->MtuRenegotiate, 0, 0) != 0) &&
		ctx->SignalingState == ChannelOpen)
	{
		ContinueCodecSwitch(ctx);
	}
	// Runs after the codec work: a reconfigure keeps the stream it needs and this then only
	// corrects what is still out of step with the endpoint.
	if (InterlockedCompareExchange(&ctx->EndpointStreamPending, 0, 0) != 0 &&
		ctx->SignalingState == ChannelOpen)
	{
		ContinueEndpointStream(ctx);
	}

	ctx->SignalingWorkItemThread = nullptr;
	WdfWaitLockRelease(ctx->SessionLock);
}
} // namespace OpenA2dpDriver::Signaling
