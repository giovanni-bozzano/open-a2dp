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

// Records the PCM format of the ACX stream Windows opened. The encoder input and the
// position math follow it, so a pin at 48 kHz is negotiated and encoded at 48 kHz.
void AudioSetPinFormat(_In_ DEVICE_CONTEXT* ctx, _In_ ACXDATAFORMAT format)
{
	ULONG sampleRate = AudioSampleRate;
	USHORT channels = AudioChannels;
	if (format != nullptr)
	{
		const auto* wave = static_cast<const KSDATAFORMAT_WAVEFORMATEX*>(AcxDataFormatGetKsDataFormat(format));
		if (wave != nullptr && wave->WaveFormatEx.nSamplesPerSec != 0 && wave->WaveFormatEx.nChannels != 0)
		{
			sampleRate = wave->WaveFormatEx.nSamplesPerSec;
			channels = wave->WaveFormatEx.nChannels;
		}
	}

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const ULONG previousRate = ctx->PinSampleRate;
	const USHORT previousChannels = ctx->PinChannels;
	ctx->PinSampleRate = sampleRate;
	ctx->PinChannels = channels;
	ctx->PinBytesPerFrame = static_cast<USHORT>(channels * sizeof(INT16));
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	OA2DP_INFO(ctx, "Audio", "endpoint format %u Hz %u ch", sampleRate, channels);
	// Windows can open the endpoint at another format at any time (a Sound settings change,
	// or the format list this driver rebuilt under it). The A2DP stream is still configured
	// at the previous one, so ask for a renegotiation; the signaling work item drops the
	// request while no stream is configured.
	if (sampleRate != previousRate || channels != previousChannels)
	{
		SignalingOnPreferenceChanged(ctx);
	}
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

// The RenderFormats indices Windows may open: the ones the stored preference admits and
// the sink advertised. Hiding what the sink cannot play keeps its unsupported rates out of
// Sound settings and out of the negotiation. A sink whose capabilities are unknown leaves
// the preference's own set, and a stored pin that the sink no longer covers leaves the
// sink's set: either way the endpoint stays openable and negotiable.
ULONG DesiredFormats(_In_ const Ioctl::Preference& preference, _In_ uint8_t sinkFormats)
{
	ULONG allowed = 0;
	ULONG covered = 0;
	ULONG sinkCovered = 0;
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		const bool sinkHas = sinkFormats != 0 &&
			SbcConfig::SupportsFormat(sinkFormats, AudioRenderRates[i / AudioRenderChannelCount],
									 static_cast<uint8_t>(AudioRenderChannels[i % AudioRenderChannelCount]));
		if (sinkHas)
		{
			sinkCovered |= 1u << i;
		}
		if (!RenderFormatAllowed(preference, i))
		{
			continue;
		}
		allowed |= 1u << i;
		if (sinkFormats == 0 || sinkHas)
		{
			covered |= 1u << i;
		}
	}
	if (covered != 0)
	{
		return covered;
	}
	// The pin is not one the sink advertises (its capabilities changed, or the pin was stored
	// before the first discovery): listing it would give Windows a format nothing can
	// configure, so the sink's own formats are listed and the stream still plays.
	if (sinkCovered != 0)
	{
		return sinkCovered;
	}
	return allowed != 0 ? allowed : AllFormatsMask;
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

// The pin's raw format list is what Windows offers as the endpoint's format and what it
// opens a stream with, so a stored rate or channel mode reaches the audio stack by
// rebuilding that list in place: ACX fires the pin's format-change event after the list
// changes and Windows re-queries, so the device does not have to restart.
NTSTATUS AudioApplyEndpointFormats(_In_ DEVICE_CONTEXT* ctx)
{
	const ACXPIN pin = ctx->StreamingPin;
	ACXDATAFORMATLIST list = pin != nullptr ? AcxPinGetRawDataFormatList(pin) : nullptr;
	if (pin == nullptr || list == nullptr || ctx->FormatLock == nullptr)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	Ioctl::Preference preference;
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		preference = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	}
	const ULONG desired =
		DesiredFormats(preference, static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0)));

	NTSTATUS status = STATUS_SUCCESS;
	WdfWaitLockAcquire(ctx->FormatLock, nullptr);
	// The first call builds the list while the pin is not part of the circuit yet, so no
	// client is listening for the change and the list order alone decides the default.
	const ULONG listedBefore = ctx->FormatsListed;
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

	// The pinned format becomes the endpoint default, so Windows reports and opens it
	// instead of falling back to whatever the list happens to start with.
	const NTSTATUS defaultStatus =
		AcxDataFormatListAssignDefaultDataFormat(list, ctx->FormatObjects[FirstListed(listed)]);
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
	if (!firstBuild)
	{
		// ACX already notifies Windows of a list change; the explicit call only makes sure a
		// client re-queries, and a pin without one (a device that has not started yet)
		// says so with an error status.
		const NTSTATUS notifyStatus = AcxPinNotifyDataFormatChange(pin);
		if (!NT_SUCCESS(notifyStatus))
		{
			OA2DP_INFO(ctx, "Audio", "the format change notification returned 0x%08X", notifyStatus);
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
	OA2DP_INFO(ctx, "Audio", "endpoint formats %s", formats);

	// The endpoint reports the pinned format until Windows opens a stream; with a stream
	// the opened format stays authoritative and the renegotiation below follows it.
	if (ctx->AudioStream == nullptr)
	{
		const ULONG first = FirstListed(listed);
		KIRQL irql;
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		ctx->PinSampleRate = AudioRenderRates[first / AudioRenderChannelCount];
		ctx->PinChannels = AudioRenderChannels[first % AudioRenderChannelCount];
		ctx->PinBytesPerFrame = static_cast<USHORT>(ctx->PinChannels * sizeof(INT16));
		KeReleaseSpinLock(&ctx->AudioLock, irql);
	}
	return status;
}
