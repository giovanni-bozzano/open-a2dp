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
// Index in AudioRenderRates/AudioRenderChannels of a rate and channel count, or
// AudioRenderFormatCount when the pair is not one of the endpoint formats.
ULONG RenderFormatIndex(ULONG sampleRate, USHORT channels)
{
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		if (sampleRate == AudioRenderRates[i / AudioRenderChannelCount] &&
			channels == AudioRenderChannels[i % AudioRenderChannelCount])
		{
			return i;
		}
	}
	return AudioRenderFormatCount;
}

// Removes and re-adds the listed formats so startIndex comes first. Windows opens the
// shared-mode stream with the first listed format, so the selection has to lead both lists;
// the assigned default alone does not reach the engine. The caller holds FormatLock.
NTSTATUS RebuildListing(_In_ DEVICE_CONTEXT* ctx, ULONG desired, ULONG startIndex)
{
	ACXDATAFORMATLIST list =
		ctx->StreamingPin != nullptr ? AcxPinGetRawDataFormatList(ctx->StreamingPin) : nullptr;
	const ACXDATAFORMATLIST defaultList = ctx->DefaultModeFormats;
	if (list == nullptr || defaultList == nullptr)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	NTSTATUS status = STATUS_SUCCESS;
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		if ((ctx->FormatsListed & (1u << i)) == 0)
		{
			continue;
		}
		NTSTATUS result = AcxDataFormatListRemoveDataFormat(list, ctx->FormatObjects[i]);
		if (NT_SUCCESS(result))
		{
			result = AcxDataFormatListRemoveDataFormat(defaultList, ctx->FormatObjects[i]);
		}
		if (!NT_SUCCESS(result))
		{
			OA2DP_ERROR(ctx, "Audio", "removing render format %u failed 0x%08X", i, result);
			if (NT_SUCCESS(status))
			{
				status = result;
			}
		}
	}
	ctx->FormatsListed = 0;
	for (ULONG n = 0; n < AudioRenderFormatCount; ++n)
	{
		const ULONG i = (startIndex + n) % AudioRenderFormatCount;
		if ((desired & (1u << i)) == 0)
		{
			continue;
		}
		NTSTATUS result = AcxDataFormatListAddDataFormat(list, ctx->FormatObjects[i]);
		if (NT_SUCCESS(result))
		{
			result = AcxDataFormatListAddDataFormat(defaultList, ctx->FormatObjects[i]);
		}
		if (!NT_SUCCESS(result))
		{
			OA2DP_ERROR(ctx, "Audio", "adding render format %u failed 0x%08X", i, result);
			if (NT_SUCCESS(status))
			{
				status = result;
			}
			continue;
		}
		ctx->FormatsListed |= 1u << i;
	}
	return status;
}

} // namespace

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

// Called when a stream starts running, not when it is created: Windows creates a stream per
// format it enumerates, so only a stream that actually carries audio may commit the endpoint
// format. Every committed change invalidates the negotiated configuration, and committing the
// probes tore the session down once per probe until the shared-mode stream ended up at the
// first listed format again.
void AudioSetPinFormat(_In_ DEVICE_CONTEXT* ctx, ULONG sampleRate, USHORT channels)
{
	OA2DP_INFO(ctx, "Audio", "endpoint format %u Hz %u ch (actual stream)", sampleRate,
		static_cast<unsigned>(channels));
	RecordEndpointFormat(ctx, sampleRate, channels);
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

	// Sound settings' default format is the device format of the DEFAULT mode. A pin that
	// lists only RAW gives Windows no mode to apply it to, so it stays on the list default.
	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = pin;
	ACX_DATAFORMAT_LIST_CONFIG config;
	ACX_DATAFORMAT_LIST_CONFIG_INIT(&config);
	NTSTATUS status = AcxDataFormatListCreate(ctx->Device, &attrs, &config, &ctx->DefaultModeFormats);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "creating the DEFAULT mode format list failed 0x%08X", status);
		return status;
	}
	status = AcxPinAssignModeDataFormatList(pin, &AUDIO_SIGNALPROCESSINGMODE_DEFAULT, ctx->DefaultModeFormats);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "assigning the DEFAULT mode format list failed 0x%08X", status);
	}
	return status;
}

} // namespace OpenA2dpDriver::Audio

// Declared in Driver.h with the rest of the driver-wide audio interface, so it lives
// outside the subsystem namespace like AudioInitialize.
using namespace OpenA2dpDriver::Audio;

// The list is the sink's formats, narrowed to the selected one when the user pinned it.
NTSTATUS AudioApplyEndpointFormats(_In_ DEVICE_CONTEXT* ctx)
{
	const ACXPIN pin = ctx->StreamingPin;
	ACXDATAFORMATLIST list = pin != nullptr ? AcxPinGetRawDataFormatList(pin) : nullptr;
	const ACXDATAFORMATLIST defaultList = ctx->DefaultModeFormats;
	if (pin == nullptr || list == nullptr || defaultList == nullptr || ctx->FormatLock == nullptr)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	NTSTATUS status = STATUS_SUCCESS;
	WdfWaitLockAcquire(ctx->FormatLock, nullptr);
	const uint8_t sinkFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0));
	ULONG desired = DesiredFormats(sinkFormats);
	if (desired == 0)
	{
		WdfWaitLockRelease(ctx->FormatLock);
		OA2DP_ERROR(ctx, "Audio", "sink SBC formats 0x%02X have no driver-supported PCM format", sinkFormats);
		return STATUS_NOT_SUPPORTED;
	}
	// Windows opens the shared-mode stream with the first listed format and ignores the
	// Sound-settings pick, so a pinned format is offered alone: listing it first is enough
	// for the engine to use it but leaves the other formats selectable, which is how the pick
	// ends up ignored. With no selection every sink-supported format stays listed.
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const ULONG selectedRate = ctx->SelectedSampleRate;
	const USHORT selectedChannels = ctx->SelectedChannels;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	ULONG startIndex = FirstListed(desired);
	if (selectedRate != 0)
	{
		const ULONG index = RenderFormatIndex(selectedRate, selectedChannels);
		if (index < AudioRenderFormatCount && (desired & (1u << index)) != 0)
		{
			desired = 1u << index;
			startIndex = index;
		}
		else
		{
			OA2DP_WARN(ctx, "Audio",
				"the selected endpoint format %u Hz %u ch is not one the sink supports; offering "
				"every supported format instead",
				selectedRate, static_cast<unsigned>(selectedChannels));
		}
	}

	const ULONG listedBefore = ctx->FormatsListed;
	const bool firstBuild = listedBefore == 0;
	if (listedBefore == desired && ctx->FirstListedIndex == startIndex)
	{
		WdfWaitLockRelease(ctx->FormatLock);
		return STATUS_SUCCESS;
	}
	status = RebuildListing(ctx, desired, startIndex);
	ctx->FirstListedIndex = startIndex;
	const ULONG listed = ctx->FormatsListed;
	if (listed == 0)
	{
		OA2DP_ERROR(ctx, "Audio", "no render format is listed; the endpoint cannot be opened");
		WdfWaitLockRelease(ctx->FormatLock);
		return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
	}

	// The leading format is the advertised default too, so ACX's own answers agree with it.
	const ULONG defaultIndex = startIndex;
	NTSTATUS defaultStatus = AcxDataFormatListAssignDefaultDataFormat(list, ctx->FormatObjects[defaultIndex]);
	if (NT_SUCCESS(defaultStatus))
	{
		defaultStatus = AcxDataFormatListAssignDefaultDataFormat(defaultList, ctx->FormatObjects[defaultIndex]);
	}
	if (NT_SUCCESS(defaultStatus))
	{
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
	OA2DP_INFO(ctx, "Audio", "endpoint formats %s (16-bit PCM, RAW and DEFAULT modes, %s)", formats,
		sinkFormats == 0 ? "provisional; sink not discovered" : "sink SBC capabilities");
	return NT_SUCCESS(status) ? defaultStatus : status;
}

bool AudioSinkSupportsEndpointFormat(_In_ DEVICE_CONTEXT* ctx, ULONG sampleRate, USHORT channels)
{
	const uint8_t sinkFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0));
	const ULONG index = RenderFormatIndex(sampleRate, channels);
	return index < AudioRenderFormatCount && (DesiredFormats(sinkFormats) & (1u << index)) != 0;
}
