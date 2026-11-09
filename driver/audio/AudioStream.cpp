#include "AudioInternal.h"

namespace OpenA2dpDriver::Audio
{
NTSTATUS OnCreateStream(_In_ WDFDEVICE device, _In_ ACXCIRCUIT circuit, _In_ ACXPIN, _In_ PACXSTREAM_INIT streamInit,
	_In_ ACXDATAFORMAT format, _In_ const GUID* processingMode, _In_ ACXOBJECTBAG)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	const char* mode = processingMode == nullptr ? "unspecified"
		: IsEqualGUID(*processingMode, AUDIO_SIGNALPROCESSINGMODE_RAW) ? "raw"
		: IsEqualGUID(*processingMode, AUDIO_SIGNALPROCESSINGMODE_DEFAULT) ? "default"
		: "other";
	OA2DP_INFO(ctx, "Audio", "creating stream, processing mode %s", mode);
	NTSTATUS status = AudioValidatePinFormat(ctx, format);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	ACX_STREAM_CALLBACKS streamCallbacks;
	ACX_STREAM_CALLBACKS_INIT(&streamCallbacks);
	streamCallbacks.EvtAcxStreamPrepareHardware = OnStreamPrepareHardware;
	streamCallbacks.EvtAcxStreamReleaseHardware = OnStreamReleaseHardware;
	streamCallbacks.EvtAcxStreamRun = OnStreamRun;
	streamCallbacks.EvtAcxStreamPause = OnStreamPause;
	status = AcxStreamInitAssignAcxStreamCallbacks(streamInit, &streamCallbacks);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	ACX_RT_STREAM_CALLBACKS rtCallbacks;
	ACX_RT_STREAM_CALLBACKS_INIT(&rtCallbacks);
	rtCallbacks.EvtAcxStreamGetHwLatency = OnGetHwLatency;
	rtCallbacks.EvtAcxStreamAllocateRtPackets = OnAllocateRtPackets;
	rtCallbacks.EvtAcxStreamFreeRtPackets = OnFreeRtPackets;
	rtCallbacks.EvtAcxStreamSetRenderPacket = OnSetRenderPacket;
	rtCallbacks.EvtAcxStreamGetCurrentPacket = OnGetCurrentPacket;
	rtCallbacks.EvtAcxStreamGetPresentationPosition = OnGetPresentationPosition;
	status = AcxStreamInitAssignAcxRtStreamCallbacks(streamInit, &rtCallbacks);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	AcxStreamInitSetAcxRtStreamSupportsNotifications(streamInit);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, STREAM_CONTEXT);
	attrs.EvtDestroyCallback = OnStreamDestroy;
	ACXSTREAM stream;
	status = AcxRtStreamCreate(device, circuit, &attrs, &streamInit, &stream);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(GetDeviceContext(device), "Audio", "AcxRtStreamCreate failed 0x%08X", status);
		return status;
	}

	GetStreamContext(stream)->Device = ctx;

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioStream = stream;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	AudioSetPinFormat(ctx, format);
	return STATUS_SUCCESS;
}

void OnStreamDestroy(_In_ WDFOBJECT object)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(static_cast<ACXSTREAM>(object))->Device;
	if (ctx == nullptr)
	{
		return;
	}
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	if (ctx->AudioStream == static_cast<ACXSTREAM>(object))
	{
		ctx->AudioStream = nullptr;
	}
	KeReleaseSpinLock(&ctx->AudioLock, irql);
}

NTSTATUS OnStreamPrepareHardware(_In_ ACXSTREAM)
{
	return STATUS_SUCCESS;
}

NTSTATUS OnStreamReleaseHardware(_In_ ACXSTREAM stream)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	WdfTimerStop(ctx->AudioTimer, TRUE);

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioRunning = FALSE;
	ctx->AudioBasePosition = 0;
	ctx->AudioCopiedPosition = 0;
	ctx->AudioCompletedPackets = 0;
	ctx->AudioStream = nullptr;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return STATUS_SUCCESS;
}

NTSTATUS OnStreamRun(_In_ ACXSTREAM stream)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioStartQpc = QpcNow(nullptr);
	ctx->AudioRunning = TRUE;
	ctx->AudioDroppedBytes = 0;
	const ULONG rate = ctx->PinSampleRate;
	const USHORT channels = ctx->PinChannels;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	WdfTimerStart(ctx->AudioTimer, 0);
	OA2DP_INFO(ctx, "Audio", "stream run");
	ConfigSaveEndpointFormat(ctx, rate, channels);
	SignalingOnAudioRun(ctx);
	return STATUS_SUCCESS;
}

NTSTATUS OnStreamPause(_In_ ACXSTREAM stream)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	WdfTimerStop(ctx->AudioTimer, TRUE);

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioBasePosition = CurrentPositionLocked(ctx, nullptr);
	ctx->AudioRunning = FALSE;
	const ULONGLONG dropped = ctx->AudioDroppedBytes;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	OA2DP_INFO(ctx, "Audio", "stream paused, %llu bytes dropped during the run", dropped);
	return STATUS_SUCCESS;
}
} // namespace OpenA2dpDriver::Audio
