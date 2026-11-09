#include "AudioInternal.h"

#include <ntstrsafe.h>

#include <OpenA2dp/SbcConfig.h>

namespace OpenA2dpDriver::Audio
{
NTSTATUS OnRetrieveJackPresence(_In_ ACXJACK jack, _Out_ PBOOLEAN isConnected)
{
	*isConnected = GetSinkJackContext(jack)->Device->SignalingState == ChannelOpen ? TRUE : FALSE;
	return STATUS_SUCCESS;
}

// The jack is what Windows uses to show the endpoint as plugged or unplugged. The
// endpoint itself lives as long as the device, so without this it stays listed as
// available after the sink disconnects.
NTSTATUS AddSinkJack(_In_ DEVICE_CONTEXT* ctx, _In_ ACXPIN pin)
{
	ACX_JACK_CALLBACKS callbacks;
	ACX_JACK_CALLBACKS_INIT(&callbacks);
	callbacks.EvtAcxJackRetrievePresenceState = OnRetrieveJackPresence;

	ACX_JACK_CONFIG config;
	ACX_JACK_CONFIG_INIT(&config);
	config.Flags = AcxJackConfigJackDetection;
	config.Callbacks = &callbacks;
	config.Description.ChannelMapping = KSAUDIO_SPEAKER_STEREO;
	config.Description.ConnectionType = AcxConnTypeUnknown;
	config.Description.GeoLocation = AcxGeoLocNotApplicable;
	config.Description.GenLocation = AcxGenLocSeparate;
	config.Description.PortConnection = AcxPortConnJack;

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, SINK_JACK_CONTEXT);
	attrs.ParentObject = pin;
	NTSTATUS status = AcxJackCreate(pin, &attrs, &config, &ctx->SinkJack);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "AcxJackCreate failed 0x%08X", status);
		return status;
	}
	GetSinkJackContext(ctx->SinkJack)->Device = ctx;

	status = AcxPinAddJacks(pin, &ctx->SinkJack, 1);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "AcxPinAddJacks failed 0x%08X", status);
	}
	return status;
}

namespace
{
// The encoder follows the actual stream, not the Windows mix or advertised default.
void RecordEndpointFormat(_In_ DEVICE_CONTEXT* ctx, _In_ ULONG sampleRate, _In_ USHORT channels)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const ULONG previousRate = ctx->PinSampleRate;
	const USHORT previousChannels = ctx->PinChannels;
	ctx->PinSampleRate = sampleRate;
	ctx->PinChannels = channels;
	ctx->PinBytesPerFrame = static_cast<USHORT>(channels * sizeof(INT16));
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	if (sampleRate != previousRate || channels != previousChannels)
	{
		SignalingOnPreferenceChanged(ctx);
	}
}
} // namespace

void OnProposeModeFormat(_In_ ACXOBJECT object, _In_ ACXCONTEXT, _In_ WDFREQUEST request)
{
	ACX_REQUEST_PARAMETERS parameters;
	ACX_REQUEST_PARAMETERS_INIT(&parameters);
	AcxRequestGetParameters(request, &parameters);
	if (parameters.Parameters.Property.ItemType == AcxItemTypePin &&
		parameters.Parameters.Property.ItemId == StreamingPinId)
	{
		// PROPOSEDATAFORMAT2 overrides the user's mix format: Windows always opens the
		// preferred mode format. No DSP here requires one, so leave selection to Windows.
		WdfRequestComplete(request, STATUS_NOT_SUPPORTED);
		return;
	}
	const NTSTATUS status = AcxCircuitDispatchAcxRequest(static_cast<ACXCIRCUIT>(object), request);
	if (!NT_SUCCESS(status))
	{
		WdfRequestComplete(request, status);
	}
}

NTSTATUS AudioValidatePinFormat(_In_ DEVICE_CONTEXT* ctx, _In_ ACXDATAFORMAT format)
{
	if (format == nullptr)
	{
		return STATUS_INVALID_PARAMETER;
	}
	const auto* wave = static_cast<const KSDATAFORMAT_WAVEFORMATEX*>(AcxDataFormatGetKsDataFormat(format));
	if (wave == nullptr ||
		wave->DataFormat.FormatSize < FIELD_OFFSET(KSDATAFORMAT_WAVEFORMATEX, WaveFormatEx) + sizeof(WAVEFORMATEX) ||
		!IsEqualGUID(wave->DataFormat.Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
	{
		OA2DP_WARN(ctx, "Audio", "stream format missing or not WAVEFORMATEX; refusing it");
		return STATUS_NOT_SUPPORTED;
	}
	OA2DP_INFO(ctx, "Audio", "stream format %u B, WAVEFORMATEX, tag %u, cbSize %u, %u Hz %u ch %u bit",
		wave->DataFormat.FormatSize, static_cast<unsigned>(wave->WaveFormatEx.wFormatTag),
		static_cast<unsigned>(wave->WaveFormatEx.cbSize), wave->WaveFormatEx.nSamplesPerSec,
		static_cast<unsigned>(wave->WaveFormatEx.nChannels),
		static_cast<unsigned>(wave->WaveFormatEx.wBitsPerSample));
	NTSTATUS status = STATUS_NOT_SUPPORTED;
	const WAVEFORMATEX& pcm = wave->WaveFormatEx;
	bool pcm16 = IsEqualGUID(wave->DataFormat.MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
		IsEqualGUID(wave->DataFormat.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
		pcm.wBitsPerSample == 16 && pcm.nBlockAlign == pcm.nChannels * sizeof(INT16) &&
		pcm.nAvgBytesPerSec == pcm.nSamplesPerSec * pcm.nBlockAlign;
	if (pcm.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	{
		pcm16 = pcm16 && pcm.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
			wave->DataFormat.FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);
		if (pcm16)
		{
			const auto& extended = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&pcm);
			pcm16 = IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
				extended.Samples.wValidBitsPerSample == 16 &&
				(extended.dwChannelMask == 0 || extended.dwChannelMask ==
					static_cast<ULONG>(pcm.nChannels == 1 ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO));
		}
	}
	else
	{
		pcm16 = pcm16 && pcm.wFormatTag == WAVE_FORMAT_PCM;
	}
	const uint8_t sinkFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0));
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		if (pcm16 &&
			pcm.nSamplesPerSec == AudioRenderRates[i / AudioRenderChannelCount] &&
			pcm.nChannels == AudioRenderChannels[i % AudioRenderChannelCount] &&
			(sinkFormats == 0 || SbcConfig::SupportsFormat(sinkFormats, pcm.nSamplesPerSec,
				static_cast<uint8_t>(pcm.nChannels))))
		{
			status = STATUS_SUCCESS;
			break;
		}
	}
	if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Audio", "stream format is not in the supported endpoint list; refusing it");
	}
	return status;
}

void AudioSetPinFormat(_In_ DEVICE_CONTEXT* ctx, _In_ ACXDATAFORMAT format)
{
	const auto* wave = static_cast<const KSDATAFORMAT_WAVEFORMATEX*>(AcxDataFormatGetKsDataFormat(format));
	OA2DP_INFO(ctx, "Audio", "endpoint format %u Hz %u ch (actual stream, 16-bit PCM)",
		wave->WaveFormatEx.nSamplesPerSec, static_cast<unsigned>(wave->WaveFormatEx.nChannels));
	RecordEndpointFormat(ctx, wave->WaveFormatEx.nSamplesPerSec, wave->WaveFormatEx.nChannels);
}

namespace
{
constexpr ULONG AllFormatsMask = (1u << AudioRenderFormatCount) - 1u;

// Index of the first listed format, used as the endpoint default.
ULONG FirstListed(_In_ ULONG listed)
{
	ULONG index = 0;
	while (index < AudioRenderFormatCount && (listed & (1u << index)) == 0)
	{
		++index;
	}
	return index < AudioRenderFormatCount ? index : 0;
}

ULONG DesiredFormats(_In_ uint8_t sinkFormats)
{
	if (sinkFormats == 0)
	{
		return AllFormatsMask;
	}
	ULONG supported = 0;
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		if (SbcConfig::SupportsFormat(sinkFormats, AudioRenderRates[i / AudioRenderChannelCount],
			static_cast<uint8_t>(AudioRenderChannels[i % AudioRenderChannelCount])))
		{
			supported |= 1u << i;
		}
	}
	return supported;
}
} // namespace

NTSTATUS CreateRenderFormats(_In_ DEVICE_CONTEXT* ctx, _In_ ACXPIN pin)
{
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		ACX_DATAFORMAT_CONFIG config;
		ACX_DATAFORMAT_CONFIG_INIT_KS(&config, &RenderFormats[i]);
		WDF_OBJECT_ATTRIBUTES attrs;
		WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
		attrs.ParentObject = pin;
		const NTSTATUS status = AcxDataFormatCreate(ctx->Device, &attrs, &config, &ctx->FormatObjects[i]);
		if (!NT_SUCCESS(status))
		{
			OA2DP_ERROR(ctx, "Audio", "AcxDataFormatCreate failed 0x%08X", status);
			return status;
		}
	}
	return STATUS_SUCCESS;
}

} // namespace OpenA2dpDriver::Audio

// Declared in Driver.h with the rest of the driver-wide audio interface, so it lives
// outside the subsystem namespace like AudioInitialize.
using namespace OpenA2dpDriver::Audio;

// Only driver and sink capabilities determine this list; preferences never narrow it.
NTSTATUS AudioApplyEndpointFormats(_In_ DEVICE_CONTEXT* ctx)
{
	const ACXPIN pin = ctx->StreamingPin;
	ACXDATAFORMATLIST list = pin != nullptr ? AcxPinGetRawDataFormatList(pin) : nullptr;
	if (pin == nullptr || list == nullptr || ctx->FormatLock == nullptr)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	NTSTATUS status = STATUS_SUCCESS;
	WdfWaitLockAcquire(ctx->FormatLock, nullptr);
	const uint8_t sinkFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0));
	const ULONG desired = DesiredFormats(sinkFormats);
	if (desired == 0)
	{
		WdfWaitLockRelease(ctx->FormatLock);
		OA2DP_ERROR(ctx, "Audio", "sink SBC formats 0x%02X have no driver-supported PCM format", sinkFormats);
		return STATUS_NOT_SUPPORTED;
	}
	const ULONG listedBefore = ctx->FormatsListed;
	if (listedBefore == desired)
	{
		WdfWaitLockRelease(ctx->FormatLock);
		return STATUS_SUCCESS;
	}
	const bool firstBuild = listedBefore == 0;
	ULONG listed = listedBefore;
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		const ULONG bit = 1u << i;
		const bool wanted = (desired & bit) != 0;
		if (wanted == ((listed & bit) != 0))
		{
			continue;
		}
		const NTSTATUS result = wanted ? AcxDataFormatListAddDataFormat(list, ctx->FormatObjects[i])
									  : AcxDataFormatListRemoveDataFormat(list, ctx->FormatObjects[i]);
		if (!NT_SUCCESS(result))
		{
			OA2DP_ERROR(ctx, "Audio", "%s render format %u failed 0x%08X", wanted ? "adding" : "removing", i, result);
			if (NT_SUCCESS(status))
			{
				status = result;
			}
			continue;
		}
		listed ^= bit;
	}
	ctx->FormatsListed = listed;
	if (listed == 0)
	{
		OA2DP_ERROR(ctx, "Audio", "no render format is listed; the endpoint cannot be opened");
		WdfWaitLockRelease(ctx->FormatLock);
		return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
	}

	// This is the advertised initial default, not Windows' persisted mix format.
	ACXDATAFORMAT currentDefault = nullptr;
	ULONG currentDefaultIndex = AudioRenderFormatCount;
	if (NT_SUCCESS(AcxDataFormatListRetrieveDefaultDataFormat(list, &currentDefault)) && currentDefault != nullptr)
	{
		for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
		{
			if (AcxDataFormatIsEqual(currentDefault, ctx->FormatObjects[i]))
			{
				currentDefaultIndex = i;
				break;
			}
		}
	}
	const bool keepDefault = currentDefaultIndex < AudioRenderFormatCount &&
		(listed & (1u << currentDefaultIndex)) != 0;
	const NTSTATUS defaultStatus = keepDefault
		? STATUS_SUCCESS
		: AcxDataFormatListAssignDefaultDataFormat(list, ctx->FormatObjects[FirstListed(listed)]);
	const ULONG defaultIndex = keepDefault ? currentDefaultIndex : FirstListed(listed);
	if (NT_SUCCESS(defaultStatus))
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		if (ctx->AudioStream == nullptr)
		{
			ctx->PinSampleRate = AudioRenderRates[defaultIndex / AudioRenderChannelCount];
			ctx->PinChannels = AudioRenderChannels[defaultIndex % AudioRenderChannelCount];
			ctx->PinBytesPerFrame = static_cast<USHORT>(ctx->PinChannels * sizeof(INT16));
		}
		KeReleaseSpinLock(&ctx->AudioLock, irql);
	}
	WdfWaitLockRelease(ctx->FormatLock);
	if (!NT_SUCCESS(defaultStatus))
	{
		if (firstBuild)
		{
			OA2DP_INFO(ctx, "Audio", "the endpoint default format was not assigned (0x%08X)", defaultStatus);
		}
		else
		{
			OA2DP_WARN(ctx, "Audio", "assigning the endpoint default format failed 0x%08X", defaultStatus);
		}
	}

	char formats[96];
	formats[0] = '\0';
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		if ((listed & (1u << i)) == 0)
		{
			continue;
		}
		char entry[24];
		(void)RtlStringCchPrintfA(entry, ARRAYSIZE(entry), "%s%u Hz %u ch", formats[0] != '\0' ? ", " : "",
			AudioRenderRates[i / AudioRenderChannelCount],
			static_cast<unsigned>(AudioRenderChannels[i % AudioRenderChannelCount]));
		(void)RtlStringCchCatA(formats, ARRAYSIZE(formats), entry);
	}
	OA2DP_INFO(ctx, "Audio", "endpoint formats %s (16-bit PCM, %s)", formats,
		sinkFormats == 0 ? "provisional; sink not discovered" : "sink SBC capabilities");
	if (NT_SUCCESS(defaultStatus))
	{
		OA2DP_INFO(ctx, "Audio", "advertised default %u Hz %u ch; Windows selects the stream format",
			AudioRenderRates[defaultIndex / AudioRenderChannelCount],
			static_cast<unsigned>(AudioRenderChannels[defaultIndex % AudioRenderChannelCount]));
	}
	return NT_SUCCESS(status) ? defaultStatus : status;
}
