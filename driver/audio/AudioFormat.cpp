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

// Makes the format object the default of both mode lists. The engine builds its next stream
// from that default and ACX answers its own format queries from it. List order, not this
// default, decides the Windows stream (see AudioApplyEndpointFormats).
void AdoptFormatIndex(_In_ DEVICE_CONTEXT* ctx, ULONG index, _In_z_ const char* source)
{
	const ACXDATAFORMATLIST list =
		ctx->StreamingPin != nullptr ? AcxPinGetRawDataFormatList(ctx->StreamingPin) : nullptr;
	const ACXDATAFORMATLIST defaultList = ctx->DefaultModeFormats;
	if (index >= AudioRenderFormatCount || list == nullptr || defaultList == nullptr ||
		ctx->FormatLock == nullptr)
	{
		return;
	}

	WdfWaitLockAcquire(ctx->FormatLock, nullptr);
	NTSTATUS status = STATUS_SUCCESS;
	bool adopted = false;
	if ((ctx->FormatsListed & (1u << index)) == 0)
	{
		status = STATUS_NOT_FOUND;
	}
	else
	{
		ACXDATAFORMAT current = nullptr;
		if (!NT_SUCCESS(AcxDataFormatListRetrieveDefaultDataFormat(list, &current)) || current == nullptr ||
			!AcxDataFormatIsEqual(current, ctx->FormatObjects[index]))
		{
			status = AcxDataFormatListAssignDefaultDataFormat(list, ctx->FormatObjects[index]);
			if (NT_SUCCESS(status))
			{
				status = AcxDataFormatListAssignDefaultDataFormat(defaultList, ctx->FormatObjects[index]);
			}
			adopted = NT_SUCCESS(status);
		}
	}
	WdfWaitLockRelease(ctx->FormatLock);
	if (adopted)
	{
		OA2DP_INFO(ctx, "Audio", "endpoint default follows %s: %u Hz %u ch", source,
			AudioRenderRates[index / AudioRenderChannelCount],
			static_cast<unsigned>(AudioRenderChannels[index % AudioRenderChannelCount]));
	}
	else if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Audio", "adopting %s as the endpoint default failed 0x%08X", source, status);
	}
}

// Finds the WAVEFORMATEX in a KSDATAFORMAT_WAVEFORMATEX blob or in a bare WAVEFORMATEX. The
// rate and channel bounds reject the uninitialized output buffer of a plain GET.
const WAVEFORMATEX* FindWaveFormat(_In_reads_bytes_opt_(bytes) const void* blob, ULONG bytes)
{
	constexpr USHORT TagPcm = 1;
	constexpr USHORT TagExtensible = 0xFFFE;
	const ULONG offsets[] = {FIELD_OFFSET(KSDATAFORMAT_WAVEFORMATEX, WaveFormatEx), 0};
	if (blob == nullptr)
	{
		return nullptr;
	}
	const auto* base = static_cast<const UCHAR*>(blob);
	for (const ULONG offset : offsets)
	{
		if (bytes < offset + sizeof(WAVEFORMATEX))
		{
			continue;
		}
		const auto* wave = reinterpret_cast<const WAVEFORMATEX*>(base + offset);
		if ((wave->wFormatTag == TagPcm || wave->wFormatTag == TagExtensible) &&
			wave->nSamplesPerSec >= 8000 && wave->nSamplesPerSec <= 192000 &&
			wave->nChannels >= 1 && wave->nChannels <= 8)
		{
			return wave;
		}
	}
	return nullptr;
}
// True the first time a given pin property id is seen; the KSPROPERTY_PIN_* ids are all
// below 32.
bool PinPropertyFirstSeen(_In_ DEVICE_CONTEXT* ctx, ULONG id)
{
	if (id >= 32)
	{
		return false;
	}
	const LONG bit = static_cast<LONG>(1UL << id);
	return (InterlockedOr(&ctx->PinPropsLogged, bit) & bit) == 0;
}

// Logs a property request once, naming the format it carries when the value holds a
// WAVEFORMATEX (the data intersection candidate does; a packet-size query does not). The
// checks reject the output buffer of a plain GET, which is uninitialized.
void LogPropertyRequest(_In_ DEVICE_CONTEXT* ctx, ULONG id, ULONG verb, ULONG controlCb, ULONG valueCb,
	const void* value)
{
	if (const auto* wave = FindWaveFormat(value, value != nullptr ? valueCb : 0); wave != nullptr)
	{
		OA2DP_INFO(ctx, "Audio", "pin property %u verb %u input %u B output %u B carries %u Hz %u ch %u bit",
			id, verb, controlCb, valueCb, wave->nSamplesPerSec,
			static_cast<unsigned>(wave->nChannels), static_cast<unsigned>(wave->wBitsPerSample));
		return;
	}
	if (value != nullptr && valueCb >= sizeof(ULONG) * 8)
	{
		const auto* words = static_cast<const ULONG*>(value);
		OA2DP_INFO(ctx, "Audio",
			"pin property %u verb %u input %u B output %u B begins %08X %08X %08X %08X %08X %08X %08X %08X",
			id, verb, controlCb, valueCb, words[0], words[1], words[2], words[3], words[4], words[5],
			words[6], words[7]);
		return;
	}
	if (value != nullptr && valueCb >= sizeof(ULONG) * 4)
	{
		const auto* words = static_cast<const ULONG*>(value);
		OA2DP_INFO(ctx, "Audio", "pin property %u verb %u input %u B output %u B begins %08X %08X %08X %08X",
			id, verb, controlCb, valueCb, words[0], words[1], words[2], words[3]);
		return;
	}
	OA2DP_INFO(ctx, "Audio", "pin property %u verb %u input %u B output %u B", id, verb, controlCb, valueCb);
}
} // namespace

void OnPropertyRequest(_In_ ACXOBJECT object, _In_ ACXCONTEXT context, _In_ WDFREQUEST request)
{
	auto* ctx = static_cast<DEVICE_CONTEXT*>(context);
	ACX_REQUEST_PARAMETERS parameters;
	ACX_REQUEST_PARAMETERS_INIT(&parameters);
	AcxRequestGetParameters(request, &parameters);
	const auto& property = parameters.Parameters.Property;
	if (parameters.Type == AcxRequestTypeProperty && IsEqualGUID(property.Set, KSPROPSETID_Pin))
	{
		if (PinPropertyFirstSeen(ctx, property.Id))
		{
			LogPropertyRequest(ctx, property.Id, static_cast<ULONG>(property.Verb), property.ControlCb,
				property.ValueCb, property.Value);
		}
		if (property.Id == KSPROPERTY_PIN_PROPOSEDATAFORMAT && property.Verb == AcxPropertyVerbSet)
		{
			// Windows proposes the pin format here when the endpoint comes up and never asks for
			// the preferred-mode-format property, so this is the Sound-settings selection reaching
			// the driver. Adopting it keeps the engine's next stream and the codec on it.
			PVOID buffer = nullptr;
			size_t bytes = 0;
			if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(request, 0, &buffer, &bytes)))
			{
				buffer = property.Value;
				bytes = property.ValueCb;
			}
			// Windows sweeps every rate it knows through this property while it validates the
			// endpoint, so only the four formats this driver lists count as a proposal; anything
			// else is ignored and never touches the default or the encoder.
			if (const auto* wave = FindWaveFormat(buffer, static_cast<ULONG>(bytes)); wave != nullptr)
			{
				const ULONG index = RenderFormatIndex(wave->nSamplesPerSec, wave->nChannels);
				if (index < AudioRenderFormatCount)
				{
					AdoptFormatIndex(ctx, index, "the proposed format");
				}
			}
		}
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

// Called when a stream starts running, not when it is created: Windows creates a stream per
// format it enumerates, so only a stream that actually carries audio may commit the endpoint
// format. Every committed change invalidates the negotiated configuration, and committing the
// probes tore the session down once per probe until the shared-mode stream ended up at the
// first listed format again.
void AudioSetPinFormat(_In_ DEVICE_CONTEXT* ctx, ULONG sampleRate, USHORT channels)
{
	OA2DP_INFO(ctx, "Audio", "endpoint format %u Hz %u ch (actual stream, 16-bit PCM)", sampleRate,
		static_cast<unsigned>(channels));
	AdoptFormatIndex(ctx, RenderFormatIndex(sampleRate, channels), "the stream format");
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

// Only driver and sink capabilities determine this list; preferences never narrow it.
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
	const ULONG desired = DesiredFormats(sinkFormats);
	if (desired == 0)
	{
		WdfWaitLockRelease(ctx->FormatLock);
		OA2DP_ERROR(ctx, "Audio", "sink SBC formats 0x%02X have no driver-supported PCM format", sinkFormats);
		return STATUS_NOT_SUPPORTED;
	}
	// The selection leads both lists: Windows opens the shared-mode stream with the first
	// listed format, so the order is what carries the Sound-settings format to the engine.
	// The stored selection comes first, the live stream format is only a fallback.
	ULONG startIndex = ctx->StoredSampleRate != 0
		? RenderFormatIndex(ctx->StoredSampleRate, ctx->StoredChannels) : AudioRenderFormatCount;
	KIRQL irql;
	if (startIndex >= AudioRenderFormatCount)
	{
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		startIndex = RenderFormatIndex(ctx->PinSampleRate, ctx->PinChannels);
		KeReleaseSpinLock(&ctx->AudioLock, irql);
	}
	if (startIndex >= AudioRenderFormatCount || (desired & (1u << startIndex)) == 0)
	{
		startIndex = FirstListed(desired);
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
	if (NT_SUCCESS(defaultStatus))
	{
		OA2DP_INFO(ctx, "Audio", "advertised default %u Hz %u ch; Windows selects the stream format",
			AudioRenderRates[defaultIndex / AudioRenderChannelCount],
			static_cast<unsigned>(AudioRenderChannels[defaultIndex % AudioRenderChannelCount]));
	}
	return NT_SUCCESS(status) ? defaultStatus : status;
}
