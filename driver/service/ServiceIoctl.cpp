#include "ServiceInternal.h"

#include <OpenA2dp/SbcConfig.h>

namespace OpenA2dpDriver::Service
{
void HandleDeviceControl(DEVICE_CONTEXT* ctx, SERVICE_FILE_CONTEXT* file, WDFFILEOBJECT fileObject,
	WDFREQUEST request, ULONG ioControlCode)
{
	switch (ioControlCode)
	{
	case Ioctl::kIoctlGetVersion:
	{
		Ioctl::VersionInfo* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::VersionInfo), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		*out = {};
		out->abiVersion = Ioctl::kAbiVersion;
		out->driverVersion = 0x00010000;
		out->maxFramePayload = Ioctl::kMaxFramePayload;
		out->maxPcmRingBytes = Ioctl::kPcmRingBytes;
		out->maxFrameRingBytes = Ioctl::kFrameRingBytes;
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::VersionInfo));
		return;
	}

	case Ioctl::kIoctlRegisterEncoder:
	{
		Ioctl::RegisterEncoderIn* in = nullptr;
		Ioctl::RegisterEncoderOut* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveInputBuffer(
			request, sizeof(Ioctl::RegisterEncoderIn), reinterpret_cast<PVOID*>(&in), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::RegisterEncoderOut), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		if (in->abiVersion != Ioctl::kAbiVersion)
		{
			Complete(request, STATUS_REVISION_MISMATCH);
			return;
		}
		if (in->codec != Ioctl::kCodecAac && in->codec != Ioctl::kCodecAptx &&
			in->codec != Ioctl::kCodecAptxHd && in->codec != Ioctl::kCodecLdac)
		{
			Complete(request, STATUS_NOT_SUPPORTED);
			return;
		}
		if (ctx->PcmRingHeader == nullptr)
		{
			Complete(request, STATUS_DEVICE_NOT_READY);
			return;
		}
		// One provider per codec; a second registration of the same codec is a second
		// service and would race the first for the frame ring.
		const LONG bit = static_cast<LONG>(Ioctl::EncoderBit(in->codec));
		LONG registered = InterlockedCompareExchange(&ctx->EncoderRegistered, 0, 0);
		for (;;)
		{
			if ((registered & bit) != 0)
			{
				Complete(request, STATUS_DEVICE_BUSY);
				return;
			}
			const LONG updated = InterlockedCompareExchange(&ctx->EncoderRegistered, registered | bit, registered);
			if (updated == registered)
			{
				break;
			}
			registered = updated;
		}

		file->Registered = TRUE;
		file->CodecMask |= bit;
		file->Generation = static_cast<ULONG>(InterlockedCompareExchange(&ctx->ServiceGeneration, 0, 0));
		// A new service session starts with no latched events. Anything left over
		// belongs to the previous handle (for example kEventDeviceGone latched when
		// it closed) and must not be delivered to this one.
		{
			KIRQL irql;
			KeAcquireSpinLock(&ctx->WaitLock, &irql);
			ctx->PendingEvents = 0;
			ctx->WaitState = {};
			KeReleaseSpinLock(&ctx->WaitLock, irql);
		}
		*out = {};
		out->abiVersion = Ioctl::kAbiVersion;
		out->codec = in->codec;
		out->maxFramePayload = Ioctl::kMaxFramePayload;
		out->pcmSampleRate = ctx->PinSampleRate;
		out->pcmChannels = ctx->PinChannels;
		out->pcmValidBits = ctx->PinValidBits;
		OA2DP_INFO(ctx, "Service", "%s encoder registered (max frame %u bytes)",
			in->codec == Ioctl::kCodecLdac   ? "LDAC"
			: in->codec == Ioctl::kCodecAptxHd ? "aptX HD"
			: in->codec == Ioctl::kCodecAptx   ? "aptX"
													 : "AAC",
			out->maxFramePayload);
		if (InterlockedCompareExchange(&ctx->MediaMode, 0, 0) == MediaModeSbc)
		{
			// The stream was negotiated before the service came up, so that codec was not
			// offered for it. Say so: otherwise this looks like the codec silently failing.
			OA2DP_INFO(ctx, "Service", "stream is SBC; renegotiating to offer the newly registered codec");
		}
		SignalingOnEncoderRegistered(ctx);
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::RegisterEncoderOut));
		return;
	}

	case Ioctl::kIoctlMapRings:
	{
		if (!file->Registered)
		{
			Complete(request, STATUS_INVALID_DEVICE_STATE);
			return;
		}
		if (file->Mapped)
		{
			Complete(request, STATUS_DEVICE_BUSY);
			return;
		}
		Ioctl::SharedRings* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::SharedRings), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		status = MapRings(ctx, fileObject);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		file->Mapped = TRUE;
		*out = ctx->Rings;
		const auto offset = static_cast<uint64_t>(ctx->ServiceBase - static_cast<PUCHAR>(ctx->ServiceRawBase));
		out->userBase = reinterpret_cast<uintptr_t>(ctx->RingUserVa) + offset;
		OA2DP_INFO(ctx, "Service", "rings mapped at 0x%I64X", out->userBase);
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::SharedRings));
		return;
	}

	case Ioctl::kIoctlWaitEvent:
	{
		if (!file->Registered)
		{
			Complete(request, STATUS_INVALID_DEVICE_STATE);
			return;
		}
		Ioctl::WaitEventOut* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::WaitEventOut), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}

		Ioctl::WaitEventOut state = {};
		uint32_t deliver = 0;
		KIRQL irql;
		KeAcquireSpinLock(&ctx->WaitLock, &irql);
		if (ctx->WaitRequest != nullptr)
		{
			KeReleaseSpinLock(&ctx->WaitLock, irql);
			Complete(request, STATUS_DEVICE_BUSY);
			return;
		}
		if (ctx->PendingEvents != 0)
		{
			deliver = ctx->PendingEvents;
			ctx->PendingEvents = 0;
			state = ctx->WaitState;
			KeReleaseSpinLock(&ctx->WaitLock, irql);
			*out = state;
			out->events = deliver;
			WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::WaitEventOut));
			return;
		}
		ctx->WaitRequest = request;
		KeReleaseSpinLock(&ctx->WaitLock, irql);

		status = WdfRequestMarkCancelableEx(request, OnWaitCancel);
		if (!NT_SUCCESS(status))
		{
			KeAcquireSpinLock(&ctx->WaitLock, &irql);
			if (ctx->WaitRequest == request)
			{
				ctx->WaitRequest = nullptr;
			}
			KeReleaseSpinLock(&ctx->WaitLock, irql);
			Complete(request, status);
		}
		return;
	}

	case Ioctl::kIoctlGetStatus:
	{
		Ioctl::Status* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::Status), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		*out = {};
		out->abiVersion = Ioctl::kAbiVersion;
		// Everything here comes from lock-free snapshots or short spin locks, except the link
		// state below: that one asks the stack, with a bound, because only the stack knows it.
		out->avdtpState = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->AvdtpStateSnapshot, 0, 0));
		out->negotiatedCodec = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->NegotiatedCodecSnapshot, 0, 0));
		{
			KIRQL irql;
			KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
			out->preferenceFlags = ctx->Preference.flags;
			out->preferenceCodec = ctx->Preference.codec;
			KeReleaseSpinLock(&ctx->PreferenceLock, irql);
		}
		const LONG mode = InterlockedCompareExchange(&ctx->MediaMode, 0, 0);
		if (InterlockedCompareExchange(&ctx->StreamFault, 0, 0) != 0)
		{
			out->mediaState = Ioctl::MediaState::Failed;
		}
		else if (!ctx->MediaActive)
		{
			// A configured stream that is not sending exists when Windows has the endpoint paused:
			// the work item suspends the stream and the next run starts it again. Anything else
			// without media is idle, and the difference is what tells a reader whether play will
			// resume this stream or configure a new one.
			const bool configured = out->avdtpState == static_cast<uint32_t>(Avdtp::SessionState::Open);
			const bool suspended = configured && !AudioStreamRunning(ctx);
			out->mediaState = suspended ? Ioctl::MediaState::Suspended : Ioctl::MediaState::Idle;
		}
		else if (mode == MediaModeService && ctx->MediaSamplesSent == 0)
		{
			// Samples, not packets: a reset of the counters must not make a stream that is sending
			// look like it is still waiting for its first record.
			out->mediaState = Ioctl::MediaState::WaitingForEncoder;
		}
		else
		{
			out->mediaState = Ioctl::MediaState::Streaming;
		}
		out->endpointRunning = AudioStreamRunning(ctx) ? 1 : 0;
		out->listedFormats = 0;
		const ULONG listed = ctx->FormatsListed;
		for (ULONG bit = 0; bit < AudioRenderFormatCount; ++bit)
		{
			out->listedFormats += (listed & (1u << bit)) != 0 ? 1 : 0;
		}
		out->refusedStreams = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->RefusedStreams, 0, 0));
		const ULONGLONG refused =
			static_cast<ULONGLONG>(InterlockedCompareExchange64(&ctx->RefusedFormat, 0, 0));
		out->refusedRate = static_cast<uint32_t>(refused >> 32);
		out->refusedChannels = static_cast<uint32_t>((refused >> 16) & 0xFFFF);
		out->refusedBits = static_cast<uint32_t>(refused & 0xFFFF);
		out->jackFlaps = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->JackFlaps, 0, 0));
		out->jackUnplugged = InterlockedCompareExchange(&ctx->JackFlap, 0, 0) != 0 ? 1 : 0;
		out->codec = ctx->ActiveCodec;
		out->sampleRate = ctx->ActiveSampleRate;
		out->channels = ctx->ActiveChannels;
		out->bitrate = ctx->ActiveBitrate;
		out->objectType = ctx->ActiveObjectType;
		out->encoderRegistered = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->EncoderRegistered, 0, 0));
		if (ctx->PcmRingHeader != nullptr)
		{
			out->pcmBufferedBytes = RingBufferView(ctx->PcmRingHeader, ctx->PcmRingData).ReadableBytes();
		}
		out->pcmCapacity = Ioctl::kPcmRingBytes;
		if (ctx->FrameRingHeader != nullptr)
		{
			out->frameBufferedBytes = RingBufferView(ctx->FrameRingHeader, ctx->FrameRingData).ReadableBytes();
		}
		out->frameCapacity = Ioctl::kFrameRingBytes;
		out->packetsSent = ctx->PacketsSent;
		out->framesDropped = static_cast<uint64_t>(InterlockedCompareExchange64(&ctx->FramesDropped, 0, 0));
		out->underruns = static_cast<uint64_t>(InterlockedCompareExchange64(&ctx->Underruns, 0, 0));
		out->sendFailures = static_cast<uint64_t>(InterlockedCompareExchange64(&ctx->SendFailures, 0, 0));
		// The two losses no other counter describes: audio the endpoint produced that never reached
		// the ring, and PCM the encoder service zero-filled because the ring was short.
		out->pcmDroppedBytes = ctx->AudioDroppedBytes;
		out->serviceSilenceSamples =
			static_cast<uint64_t>(InterlockedCompareExchange64(&ctx->ServiceSilenceSamples, 0, 0));
		out->lastError = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->LastMediaError, 0, 0));
		// What the bitrate is allowed to be: one access unit per media packet, so the
		// ceiling follows the negotiated MTU and the endpoint's sample rate.
		KIRQL irql;
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		const ULONG sampleRate = ctx->PinSampleRate;
		const USHORT pinChannels = ctx->PinChannels;
		const USHORT pinValidBits = ctx->PinValidBits;
		KeReleaseSpinLock(&ctx->AudioLock, irql);
		// Buffered media in milliseconds: what the stream is behind live audio. The PCM ring
		// follows the depth it carries, and the frame ring the codec bitrate, which AAC and
		// aptX hold constant.
		const ULONG pcmFrameBytes = pinChannels * AudioRenderSampleBytes(pinValidBits);
		if (pcmFrameBytes != 0 && sampleRate != 0)
		{
			out->pcmBufferedMs = static_cast<uint32_t>(static_cast<uint64_t>(out->pcmBufferedBytes) * 1000 /
				(static_cast<uint64_t>(sampleRate) * pcmFrameBytes));
		}
		if (out->bitrate != 0)
		{
			out->frameBufferedMs =
				static_cast<uint32_t>(static_cast<uint64_t>(out->frameBufferedBytes) * 8000 / out->bitrate);
		}
		out->pcmTrimmedBytes = ctx->AudioTrimmedBytes;
		out->framesTrimmed = static_cast<uint64_t>(InterlockedCompareExchange64(&ctx->FramesTrimmed, 0, 0));
		out->pacingLeadMs = ctx->PacingLeadMs;
		out->latencyPeakMs = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->LatencyPeakMs, 0, 0));
		out->worstRenderGapMs = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->WorstRenderGapMs, 0, 0));
		out->worstServiceGapMs = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->WorstServiceGapMs, 0, 0));
		out->worstSendMs = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->WorstSendMs, 0, 0));
		out->worstSendTransportMs =
			static_cast<uint32_t>(InterlockedCompareExchange(&ctx->WorstSendTransportMs, 0, 0));
		out->worstSendResumeMs =
			static_cast<uint32_t>(InterlockedCompareExchange(&ctx->WorstSendResumeMs, 0, 0));
		// A link in sniff is why a start can be slow with nothing else to show for it, and the
		// wake is what the driver's own request to leave it cost. Both are read here, at the
		// moment the report is built, because the stack changes them on its own.
		out->linkMode = L2capQueryLinkMode(ctx);
		out->lastWakeMs = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->LinkWakeMs, 0, 0));
		out->mediaMtu = ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu;
		const size_t payloadCapacity = out->mediaMtu > Rtp::kHeaderSize
			? static_cast<size_t>(out->mediaMtu) - Rtp::kHeaderSize
			: 0;
		out->bitrateLimit =
			payloadCapacity != 0 ? AacConfig::MaxBitrateForMtu(payloadCapacity, sampleRate) : 0;
		// SBC PCM capabilities are the baseline, not the AAC capability set or Windows mix format.
		const uint8_t sinkFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0));
		out->sinkRates = SbcConfig::AdvertisedRates(sinkFormats);
		out->sinkChannels = SbcConfig::AdvertisedChannels(sinkFormats);
		out->sinkCodecs = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->SinkCodecs, 0, 0));
		// The format the codec in use pins: the forced one, or with an automatic choice the codec
		// the stream runs. 0/0 when that codec has no stored format.
		const ULONG pinnedCodec = AudioPinnedCodec(ctx);
		KIRQL pinIrql;
		KeAcquireSpinLock(&ctx->AudioLock, &pinIrql);
		out->endpointRate = pinnedCodec < Ioctl::kCodecCount ? ctx->CodecSampleRate[pinnedCodec] : 0;
		out->endpointChannels = pinnedCodec < Ioctl::kCodecCount ? ctx->CodecChannels[pinnedCodec] : 0;
		KeReleaseSpinLock(&ctx->AudioLock, pinIrql);
		// The name the sink reported: captured once while the device was added, before this
		// control device existed, so it needs no lock and cannot change under the copy.
		for (uint32_t i = 0; i + 1 < Ioctl::kSinkNameChars && ctx->RemoteName[i] != L'\0'; ++i)
		{
			out->sinkName[i] = ctx->RemoteName[i];
		}
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::Status));
		return;
	}

	case Ioctl::kIoctlGetPreference:
	{
		Ioctl::Preference* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::Preference), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		*out = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
		out->abiVersion = Ioctl::kAbiVersion;
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::Preference));
		return;
	}

	case Ioctl::kIoctlSetPreference:
	{
		Ioctl::Preference* in = nullptr;
		NTSTATUS status = WdfRequestRetrieveInputBuffer(
			request, sizeof(Ioctl::Preference), reinterpret_cast<PVOID*>(&in), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		if (in->abiVersion != Ioctl::kAbiVersion)
		{
			Complete(request, STATUS_REVISION_MISMATCH);
			return;
		}
		if (!Ioctl::ValidPreference(*in))
		{
			Complete(request, STATUS_INVALID_PARAMETER);
			return;
		}
		// A codec the sink does not advertise can never be selected, so forcing it is refused
		// instead of stored. SBC is mandatory, so only AAC and the aptX codecs can be missing.
		const LONG sinkCodecs = InterlockedCompareExchange(&ctx->SinkCodecs, 0, 0);
		const LONG requiredBit = in->codec == Ioctl::kCodecAac    ? static_cast<LONG>(Ioctl::kSinkCodecAac)
			: in->codec == Ioctl::kCodecAptx   ? static_cast<LONG>(Ioctl::kSinkCodecAptx)
			: in->codec == Ioctl::kCodecAptxHd ? static_cast<LONG>(Ioctl::kSinkCodecAptxHd)
			: in->codec == Ioctl::kCodecLdac   ? static_cast<LONG>(Ioctl::kSinkCodecLdac)
													   : 0;
		if ((in->flags & Ioctl::kPreferenceForceCodec) != 0 && requiredBit != 0 && sinkCodecs != 0 &&
			(sinkCodecs & requiredBit) == 0)
		{
			OA2DP_WARN(ctx, "Service", "the sink does not advertise %s, so forcing it is refused",
				in->codec == Ioctl::kCodecLdac   ? "LDAC"
				: in->codec == Ioctl::kCodecAptxHd ? "aptX HD"
				: in->codec == Ioctl::kCodecAptx   ? "aptX"
														 : "AAC");
			Complete(request, STATUS_NOT_SUPPORTED);
			return;
		}
		bool changed = false;
		bool selectionChanged = false;
		bool parametersChanged = false;
		bool parametersAffectStream = false;
		{
			KIRQL irql;
			KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
			selectionChanged =
				ctx->Preference.flags != in->flags || ctx->Preference.codec != in->codec;
			const bool sbcChanged = ctx->Preference.sbcBitpool != in->sbcBitpool ||
									ctx->Preference.sbcBlocks != in->sbcBlocks ||
									ctx->Preference.sbcSubbands != in->sbcSubbands ||
									ctx->Preference.sbcAllocation != in->sbcAllocation ||
									ctx->Preference.sbcFramesPerPacket != in->sbcFramesPerPacket;
			const bool aacChanged = ctx->Preference.aacBitrate != in->aacBitrate;
			const bool ldacChanged = ctx->Preference.ldacQuality != in->ldacQuality;
			// The pacing lead shapes every codec, so it always belongs to the stream.
			const bool leadChanged = ctx->Preference.prerollMs != in->prerollMs;
			// The grace shapes the next pause, not the stream that is running, so it is stored
			// without a renegotiation.
			const bool graceChanged = ctx->Preference.pauseGraceMs != in->pauseGraceMs;
			parametersChanged = sbcChanged || aacChanged || ldacChanged || leadChanged || graceChanged;
			// A setting that belongs to a codec the stream is not running changes nothing it
			// sends: storing it is enough, and suspending a playing stream to apply it would
			// be a restart the listener pays for and cannot hear. The codec in use is the one
			// the running stream encodes, which is also the one the settings window shows
			// while the choice is automatic.
			parametersAffectStream = leadChanged ||
								 (sbcChanged && ctx->ActiveCodec == Ioctl::kCodecSbc) ||
								 (aacChanged && ctx->ActiveCodec == Ioctl::kCodecAac) ||
								 (ldacChanged && ctx->ActiveCodec == Ioctl::kCodecLdac);
			changed = selectionChanged || parametersChanged;
			ctx->Preference = *in;
			KeReleaseSpinLock(&ctx->PreferenceLock, irql);
		}
		if (changed)
		{
			if (parametersAffectStream)
			{
				// A parameter-only change keeps the codec, so the work item needs this
				// marker to tell that the running stream is stale.
				InterlockedIncrement(&ctx->PreferenceParameters);
			}
			OA2DP_INFO(ctx, "Service",
				"codec preference changed (flags 0x%08X, codec %u, SBC bitpool %u, blocks %u, subbands %u, "
				"allocation 0x%02X, AAC bitrate %u, preroll %u ms, SBC frames per packet %u, "
				"LDAC quality %u, pause grace %u ms)",
				in->flags, static_cast<unsigned>(in->codec), in->sbcBitpool, in->sbcBlocks, in->sbcSubbands,
				in->sbcAllocation, in->aacBitrate, in->prerollMs, in->sbcFramesPerPacket, in->ldacQuality,
				in->pauseGraceMs);
			// Stored before the renegotiation so the switch survives a reload; the IOCTL
			// still succeeds in memory if the file cannot be written.
			ConfigSaveSettings(ctx);
			if (selectionChanged)
			{
				// Another codec pins another stored format, so the listed formats follow it in
				// place; a list that did not move logs and notifies nothing.
				const NTSTATUS formatStatus = AudioApplyEndpointFormats(ctx);
				if (!NT_SUCCESS(formatStatus))
				{
					OA2DP_WARN(ctx, "Service",
							   "applying the endpoint formats after the codec change failed 0x%08X",
							   formatStatus);
				}
			}
			if (selectionChanged || parametersAffectStream)
			{
				SignalingOnPreferenceChanged(ctx);
			}
		}
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 0);
		return;
	}

	case Ioctl::kIoctlGetEndpointFormat:
	{
		Ioctl::EndpointFormat* out = nullptr;
		NTSTATUS status = WdfRequestRetrieveOutputBuffer(
			request, sizeof(Ioctl::EndpointFormat), reinterpret_cast<PVOID*>(&out), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		KIRQL irql;
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
		{
			out->sampleRate[codec] = ctx->CodecSampleRate[codec];
			out->channels[codec] = ctx->CodecChannels[codec];
		}
		KeReleaseSpinLock(&ctx->AudioLock, irql);
		out->abiVersion = Ioctl::kAbiVersion;
		out->reserved = 0;
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(Ioctl::EndpointFormat));
		return;
	}

	case Ioctl::kIoctlSetEndpointFormat:
	{
		Ioctl::EndpointFormat* in = nullptr;
		NTSTATUS status = WdfRequestRetrieveInputBuffer(
			request, sizeof(Ioctl::EndpointFormat), reinterpret_cast<PVOID*>(&in), nullptr);
		if (!NT_SUCCESS(status))
		{
			Complete(request, status);
			return;
		}
		if (in->abiVersion != Ioctl::kAbiVersion)
		{
			Complete(request, STATUS_REVISION_MISMATCH);
			return;
		}
		if (!Ioctl::ValidEndpointFormat(*in))
		{
			Complete(request, STATUS_INVALID_PARAMETER);
			return;
		}
		// A format the codec in use cannot play is refused instead of stored, because that entry
		// is the one the endpoint is pinned to and a stream would have nothing to open at. The
		// other entries only say what the sink should use if their codec is ever negotiated, and
		// the listing already ignores one the sink cannot serve, so they are stored as asked: a
		// setting for a codec the sink is not running cannot fail because of what it holds.
		// Before the first discovery nothing is known yet.
		const bool sinkKnown = InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0) != 0;
		const ULONG inUseCodec = sinkKnown ? AudioPinnedCodec(ctx) : Ioctl::kCodecCount;
		if (inUseCodec < Ioctl::kCodecCount)
		{
			const ULONG rate = in->sampleRate[inUseCodec];
			if (rate != 0 && !AudioSinkSupportsEndpointFormat(
									  ctx, inUseCodec, rate, static_cast<USHORT>(in->channels[inUseCodec])))
			{
				OA2DP_WARN(ctx, "Service", "the sink cannot play %u Hz %u ch with %s, the codec in use, so "
						   "that format is refused",
						   rate, in->channels[inUseCodec], CodecValueName(inUseCodec));
				Complete(request, STATUS_NOT_SUPPORTED);
				return;
			}
		}
		bool changed = false;
		{
			KIRQL irql;
			KeAcquireSpinLock(&ctx->AudioLock, &irql);
			for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
			{
				if (ctx->CodecSampleRate[codec] != in->sampleRate[codec] ||
					ctx->CodecChannels[codec] != in->channels[codec])
				{
					changed = true;
				}
				ctx->CodecSampleRate[codec] = in->sampleRate[codec];
				ctx->CodecChannels[codec] = static_cast<USHORT>(in->channels[codec]);
			}
			KeReleaseSpinLock(&ctx->AudioLock, irql);
		}
		if (changed)
		{
			for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
			{
				if (in->sampleRate[codec] != 0)
				{
					OA2DP_INFO(ctx, "Service", "endpoint format for %s set to %u Hz %u ch", CodecValueName(codec),
							   in->sampleRate[codec], in->channels[codec]);
				}
			}
			ConfigSaveSettings(ctx);
			// What the endpoint offered before the write: a stored format for a codec that is not
			// in use leaves the listing exactly as it was.
			const ULONG listedBefore = ctx->FormatsListed;
			const ULONG firstBefore = ctx->FirstListedIndex;
			// A changed list makes Windows drop its render stream at the old format; it does not
			// always build a new one, and while none runs the pin format moves without a stream
			// report. The work item compares the pin with the negotiated configuration either way
			// and renegotiates the stream that is still playing at the old one.
			const NTSTATUS formatStatus = AudioApplyEndpointFormats(ctx);
			if (!NT_SUCCESS(formatStatus))
			{
				OA2DP_WARN(ctx, "Service", "applying the endpoint formats failed 0x%08X", formatStatus);
			}
			// Windows keeps the format it cached for the endpoint, so only a rebuilt endpoint
			// offers the new one: without this, a player that starts afterwards asks for the old
			// format and is refused the stream, and nothing is heard. A write that moved no
			// listed format does not touch the stream: the setting belongs to a codec that is
			// not running, and the audio being heard must not drop for it.
			if (ctx->FormatsListed != listedBefore || ctx->FirstListedIndex != firstBefore)
			{
				AudioRestartRenderStream(ctx);
			}
		}
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 0);
		return;
	}

	case Ioctl::kIoctlResetCounters:
	{
		// Every counter status reports is cleared here and nowhere else: the loss counts, the
		// latency peak and the worst gaps all accumulate across streams, so a new stream cannot
		// hide an earlier fault and one reset starts every measurement window. The stream, its
		// configuration and its settings are untouched.
		InterlockedExchange64(&ctx->FramesDropped, 0);
		InterlockedExchange64(&ctx->FramesTrimmed, 0);
		InterlockedExchange64(&ctx->Underruns, 0);
		InterlockedExchange64(&ctx->SendFailures, 0);
		InterlockedExchange(&ctx->RefusedStreams, 0);
		InterlockedExchange(&ctx->JackFlaps, 0);
		// These are written by their own producer with plain increments (the audio timer under
		// AudioLock) or plain maxima (the audio and media paths), so a reset for them is a store:
		// the worst case is losing the one observation that is in flight, which is what a reset
		// means anyway.
		ctx->AudioTrimmedBytes = 0;
		ctx->PacketsSent = 0;
		ctx->AudioDroppedBytes = 0;
		ctx->AudioRunDroppedBytes = 0;
		InterlockedExchange64(&ctx->ServiceSilenceSamples, 0);
		ctx->LatencyPeakMs = 0;
		ctx->WorstRenderGapMs = 0;
		ctx->WorstServiceGapMs = 0;
		ctx->WorstSendMs = 0;
		ctx->WorstSendTransportMs = 0;
		ctx->WorstSendResumeMs = 0;
		OA2DP_INFO(ctx, "Service", "counters reset");
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 0);
		return;
	}

	default:
		Complete(request, STATUS_INVALID_DEVICE_REQUEST);
		return;
	}
}
} // namespace OpenA2dpDriver::Service
