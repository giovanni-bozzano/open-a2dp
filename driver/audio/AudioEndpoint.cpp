#include "AudioInternal.h"

using namespace OpenA2dpDriver::Audio;

void AudioNotifyLinkState(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->SinkJack != nullptr)
	{
		AcxJackChangeStateNotification(ctx->SinkJack);
	}
}

NTSTATUS AudioInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeSpinLock(&ctx->AudioLock);
	// Until Windows opens a stream the endpoint reports the default format.
	ctx->PinSampleRate = AudioSampleRate;
	ctx->PinChannels = AudioChannels;
	ctx->PinBytesPerFrame = AudioBytesPerFrame;
	BuildRenderFormats();
	// The PCM ring lives in the service block and is initialized by ServiceInitialize.

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;
	WDF_TIMER_CONFIG timerConfig;
	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, OnAudioTimer, AudioPeriodMs);
	NTSTATUS status = WdfTimerCreate(&timerConfig, &attrs, &ctx->AudioTimer);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	PACXCIRCUIT_INIT circuitInit = AcxCircuitInitAllocate(ctx->Device);
	if (circuitInit == nullptr)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	// {5A1E3C2B-8F4D-4B6A-9C1E-0A2D0B5E7F31}
	static const GUID RenderComponentId = {0x5a1e3c2b, 0x8f4d, 0x4b6a, {0x9c, 0x1e, 0x0a, 0x2d, 0x0b, 0x5e, 0x7f, 0x31}};
	AcxCircuitInitSetComponentId(circuitInit, &RenderComponentId);
	AcxCircuitInitSetCircuitType(circuitInit, AcxCircuitTypeRender);

	ACXCIRCUIT circuit = nullptr;
	status = AcxCircuitInitAssignName(circuitInit, &RenderCircuitName);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "AcxCircuitInitAssignName failed 0x%08X", status);
	}
	if (NT_SUCCESS(status))
	{
		status = AcxCircuitInitAssignAcxCreateStreamCallback(circuitInit, OnCreateStream);
		if (!NT_SUCCESS(status))
		{
			OA2DP_ERROR(ctx, "Audio", "AcxCircuitInitAssignAcxCreateStreamCallback failed 0x%08X", status);
		}
	}
	if (NT_SUCCESS(status))
	{
		// Clears circuitInit on success.
		WDF_OBJECT_ATTRIBUTES circuitAttrs;
		WDF_OBJECT_ATTRIBUTES_INIT(&circuitAttrs);
		circuitAttrs.ParentObject = ctx->Device;
		status = AcxCircuitCreate(ctx->Device, &circuitAttrs, &circuitInit, &circuit);
		if (!NT_SUCCESS(status))
		{
			OA2DP_ERROR(ctx, "Audio", "AcxCircuitCreate failed 0x%08X", status);
		}
	}
	if (circuitInit != nullptr)
	{
		AcxCircuitInitFree(circuitInit);
	}
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "circuit creation failed 0x%08X", status);
		return status;
	}

	ACX_PIN_CONFIG pinConfig;
	ACX_PIN_CONFIG_INIT_ID(&pinConfig, StreamingPinId);
	pinConfig.Type = AcxPinTypeSink;
	pinConfig.Communication = AcxPinCommunicationSink;
	pinConfig.Category = &KSCATEGORY_AUDIO;
	pinConfig.MaxStreams = 1;
	WDF_OBJECT_ATTRIBUTES pinAttrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&pinAttrs);
	pinAttrs.ParentObject = circuit;
	ACXPIN pins[2];
	status = AcxPinCreate(circuit, &pinAttrs, &pinConfig, &pins[0]);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "creating the streaming pin failed 0x%08X", status);
		return status;
	}
	// Every supported format is listed so Windows may open any of them; the one it picks
	// is what AVDTP negotiates once audio starts.
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		status = AddFormat(ctx->Device, pins[0], RenderFormats[i]);
		if (!NT_SUCCESS(status))
		{
			return status;
		}
	}

	ACX_PIN_CONFIG_INIT_ID(&pinConfig, BridgePinId);
	pinConfig.Type = AcxPinTypeSource;
	pinConfig.Communication = AcxPinCommunicationNone;
	pinConfig.Category = EndpointCategory(ctx->RemoteClass);
	status = AcxPinCreate(circuit, &pinAttrs, &pinConfig, &pins[1]);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "creating the bridge pin failed 0x%08X", status);
		return status;
	}

	status = AcxCircuitAddPins(circuit, pins, ARRAYSIZE(pins));
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "AcxCircuitAddPins failed 0x%08X", status);
		return status;
	}

	status = AddSinkJack(ctx, pins[1]);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = AcxDeviceAddCircuit(ctx->Device, circuit);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "AcxDeviceAddCircuit failed 0x%08X", status);
	}
	return status;
}
