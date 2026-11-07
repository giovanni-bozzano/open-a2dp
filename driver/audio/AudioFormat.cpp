#include "AudioInternal.h"

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
	ctx->PinSampleRate = sampleRate;
	ctx->PinChannels = channels;
	ctx->PinBytesPerFrame = static_cast<USHORT>(channels * sizeof(INT16));
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	OA2DP_INFO(ctx, "Audio", "endpoint format %u Hz %u ch", sampleRate, channels);
}

NTSTATUS AddFormat(_In_ WDFDEVICE device, _In_ ACXPIN pin, _In_ RENDER_FORMAT& renderFormat)
{
	ACX_DATAFORMAT_CONFIG config;
	ACX_DATAFORMAT_CONFIG_INIT_KS(&config, &renderFormat);
	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = pin;
	ACXDATAFORMAT format;
	NTSTATUS status = AcxDataFormatCreate(device, &attrs, &config, &format);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(GetDeviceContext(device), "Audio", "AcxDataFormatCreate failed 0x%08X", status);
		return status;
	}
	status = AcxDataFormatListAddDataFormat(AcxPinGetRawDataFormatList(pin), format);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(GetDeviceContext(device), "Audio", "AcxDataFormatListAddDataFormat failed 0x%08X", status);
	}
	return status;
}
} // namespace OpenA2dpDriver::Audio
