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
			in->codec != Ioctl::kCodecAptxHd)
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
			in->codec == Ioctl::kCodecAptxHd ? "aptX HD"
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
		// Everything here comes from lock-free snapshots or short spin locks, so a
		// busy session can never make the CLI wait.
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
			out->mediaState = Ioctl::MediaState::Idle;
		}
		else if (mode == MediaModeService && ctx->RtpSequence == 0)
		{
			out->mediaState = Ioctl::MediaState::WaitingForEncoder;
		}
		else
		{
			out->mediaState = Ioctl::MediaState::Streaming;
		}
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
		out->packetsSent = ctx->RtpSequence;
		out->framesDropped = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->FramesDropped, 0, 0));
		out->underruns = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->Underruns, 0, 0));
		out->sendFailures = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->SendFailures, 0, 0));
		out->lastError = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->LastMediaError, 0, 0));
		// What the bitrate is allowed to be: one access unit per media packet, so the
		// ceiling follows the negotiated MTU and the endpoint's sample rate.
		KIRQL irql;
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		const ULONG sampleRate = ctx->PinSampleRate;
		KeReleaseSpinLock(&ctx->AudioLock, irql);
		out->mediaMtu = ctx->MediaOutMtu != 0 ? ctx->MediaOutMtu : MediaMaxMtu;
		out->bitrateLimit = AacConfig::MaxBitrateForMtu(out->mediaMtu - Rtp::kHeaderSize, sampleRate);
		// SBC PCM capabilities are the baseline, not the AAC capability set or Windows mix format.
		const uint8_t sinkFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0));
		out->sinkRates = SbcConfig::AdvertisedRates(sinkFormats);
		out->sinkChannels = SbcConfig::AdvertisedChannels(sinkFormats);
		out->sinkCodecs = static_cast<uint32_t>(InterlockedCompareExchange(&ctx->SinkCodecs, 0, 0));
		out->endpointRate = ctx->SelectedSampleRate;
		out->endpointChannels = ctx->SelectedChannels;
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
													   : 0;
		if ((in->flags & Ioctl::kPreferenceForceCodec) != 0 && requiredBit != 0 && sinkCodecs != 0 &&
			(sinkCodecs & requiredBit) == 0)
		{
			OA2DP_WARN(ctx, "Service", "the sink does not advertise %s, so forcing it is refused",
				in->codec == Ioctl::kCodecAptxHd ? "aptX HD"
				: in->codec == Ioctl::kCodecAptx   ? "aptX"
														 : "AAC");
			Complete(request, STATUS_NOT_SUPPORTED);
			return;
		}
		bool changed = false;
		bool selectionChanged = false;
		bool parametersChanged = false;
		{
			KIRQL irql;
			KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
			selectionChanged =
				ctx->Preference.flags != in->flags || ctx->Preference.codec != in->codec;
			parametersChanged = ctx->Preference.sbcBitpool != in->sbcBitpool ||
								ctx->Preference.sbcBlocks != in->sbcBlocks ||
								ctx->Preference.sbcSubbands != in->sbcSubbands ||
								ctx->Preference.sbcAllocation != in->sbcAllocation ||
								ctx->Preference.aacBitrate != in->aacBitrate;
			changed = selectionChanged || parametersChanged;
			ctx->Preference = *in;
			KeReleaseSpinLock(&ctx->PreferenceLock, irql);
		}
		if (changed)
		{
			if (parametersChanged)
			{
				// A parameter-only change keeps the codec, so the work item needs this
				// marker to tell that the running stream is stale.
				InterlockedIncrement(&ctx->PreferenceParameters);
			}
			OA2DP_INFO(ctx, "Service",
				"codec preference changed (flags 0x%08X, codec %u, SBC bitpool %u, blocks %u, subbands %u, "
				"allocation 0x%02X, AAC bitrate %u)",
				in->flags, static_cast<unsigned>(in->codec), in->sbcBitpool, in->sbcBlocks, in->sbcSubbands,
				in->sbcAllocation, in->aacBitrate);
			// Stored before the renegotiation so the switch survives a reload; the IOCTL
			// still succeeds in memory if the key cannot be written.
			ConfigSavePreference(ctx);
			if (selectionChanged || parametersChanged)
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
		out->sampleRate = ctx->SelectedSampleRate;
		out->channels = ctx->SelectedChannels;
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
		// A format the sink does not advertise would leave the endpoint with nothing to open,
		// so it is refused instead of stored. Before the first discovery nothing is known yet.
		const bool sinkKnown =
			InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0) != 0;
		if (in->sampleRate != 0 && sinkKnown &&
			!AudioSinkSupportsEndpointFormat(ctx, in->sampleRate, static_cast<USHORT>(in->channels)))
		{
			OA2DP_WARN(ctx, "Service",
				"the sink does not advertise %u Hz %u ch, so that endpoint format is refused",
				in->sampleRate, in->channels);
			Complete(request, STATUS_NOT_SUPPORTED);
			return;
		}
		KIRQL irql;
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		const bool changed = ctx->SelectedSampleRate != in->sampleRate ||
							 ctx->SelectedChannels != in->channels;
			ctx->SelectedSampleRate = in->sampleRate;
			ctx->SelectedChannels = static_cast<USHORT>(in->channels);
		KeReleaseSpinLock(&ctx->AudioLock, irql);
		if (changed)
		{
			if (in->sampleRate != 0)
			{
				OA2DP_INFO(ctx, "Service", "endpoint format set to %u Hz %u ch", in->sampleRate,
					in->channels);
			}
			else
			{
				OA2DP_INFO(ctx, "Service", "endpoint format set to automatic");
			}
			ConfigSaveEndpointFormat(ctx);
			// A changed list makes Windows drop its render stream at the old format; it does not
			// always build a new one, and while none runs the pin format moves without a stream
			// report. The work item compares the pin with the negotiated configuration either way
			// and renegotiates the stream that is still playing at the old one.
			const NTSTATUS formatStatus = AudioApplyEndpointFormats(ctx);
			if (!NT_SUCCESS(formatStatus))
			{
				OA2DP_WARN(ctx, "Service", "applying the endpoint format failed 0x%08X", formatStatus);
			}
			if (ctx->AudioStream != nullptr)
			{
				// A running stream keeps the format it was opened with, and Windows re-reads the
				// offered formats only when it rebuilds the endpoint; unplugging the jack briefly
				// makes it close the stream and open the new format.
				AudioRestartRenderStream(ctx);
			}
		}
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 0);
		return;
	}

	default:
		Complete(request, STATUS_INVALID_DEVICE_REQUEST);
		return;
	}
}
} // namespace OpenA2dpDriver::Service
