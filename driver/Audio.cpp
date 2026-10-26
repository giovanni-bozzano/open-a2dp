#include "Driver.h"

using namespace OpenA2dp;

namespace
{

constexpr ULONG AudioPeriodMs = 10;
constexpr ULONG AudioMaxPacketCount = 2;
constexpr ULONG StreamingPinId = 0;
constexpr ULONG BridgePinId = 1;

DECLARE_CONST_UNICODE_STRING(RenderCircuitName, L"Speaker0");

struct STREAM_CONTEXT
{
	DEVICE_CONTEXT* Device;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(STREAM_CONTEXT, GetStreamContext)

struct RENDER_FORMAT
{
	KSDATAFORMAT DataFormat;
	WAVEFORMATEXTENSIBLE WaveFormatExt;
};

RENDER_FORMAT RenderFormat = {
	{
		sizeof(RENDER_FORMAT),
		0,
		0,
		0,
		STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
		STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
		STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX),
	},
	{
		{
			WAVE_FORMAT_EXTENSIBLE,
			AudioChannels,
			AudioSampleRate,
			AudioSampleRate * AudioBytesPerFrame,
			AudioBytesPerFrame,
			16,
			sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX),
		},
		{16},
		KSAUDIO_SPEAKER_STEREO,
		STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
	},
};

EVT_ACX_CIRCUIT_CREATE_STREAM OnCreateStream;
EVT_ACX_STREAM_PREPARE_HARDWARE OnStreamPrepareHardware;
EVT_ACX_STREAM_RELEASE_HARDWARE OnStreamReleaseHardware;
EVT_ACX_STREAM_RUN OnStreamRun;
EVT_ACX_STREAM_PAUSE OnStreamPause;
EVT_ACX_STREAM_ALLOCATE_RTPACKETS OnAllocateRtPackets;
EVT_ACX_STREAM_FREE_RTPACKETS OnFreeRtPackets;
EVT_ACX_STREAM_GET_HW_LATENCY OnGetHwLatency;
EVT_ACX_STREAM_SET_RENDER_PACKET OnSetRenderPacket;
EVT_ACX_STREAM_GET_CURRENT_PACKET OnGetCurrentPacket;
EVT_ACX_STREAM_GET_PRESENTATION_POSITION OnGetPresentationPosition;
EVT_WDF_TIMER OnAudioTimer;
EVT_WDF_OBJECT_CONTEXT_DESTROY OnStreamDestroy;

ULONGLONG QpcNow(ULONGLONG* frequency)
{
	LARGE_INTEGER freq;
	const LARGE_INTEGER now = KeQueryPerformanceCounter(&freq);
	if (frequency != nullptr)
	{
		*frequency = static_cast<ULONGLONG>(freq.QuadPart);
	}
	return static_cast<ULONGLONG>(now.QuadPart);
}

// Bytes the virtual device has consumed from the RT buffer. Caller holds AudioLock.
ULONGLONG CurrentPositionLocked(DEVICE_CONTEXT* ctx, ULONGLONG* qpc)
{
	ULONGLONG frequency;
	const ULONGLONG now = QpcNow(&frequency);
	if (qpc != nullptr)
	{
		*qpc = now;
	}
	if (!ctx->AudioRunning)
	{
		return ctx->AudioBasePosition;
	}
	const ULONGLONG frames = (now - ctx->AudioStartQpc) * AudioSampleRate / frequency;
	return ctx->AudioBasePosition + frames * AudioBytesPerFrame;
}

// Moves newly consumed bytes from the RT buffer into the PCM ring and reports
// completed packets to ACX.
void OnAudioTimer(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	if (!ctx->AudioRunning || ctx->AudioBuffer == nullptr)
	{
		KeReleaseSpinLock(&ctx->AudioLock, irql);
		return;
	}

	ULONGLONG qpc;
	const ULONGLONG position = CurrentPositionLocked(ctx, &qpc);
	const ULONG bufferSize = ctx->AudioPacketSize * ctx->AudioRtPacketCount;
	RingBufferView ring(&ctx->PcmRingHeader, ctx->PcmRingData);

	// Never copy more than one buffer: anything older was already overwritten.
	ULONGLONG copied = ctx->AudioCopiedPosition;
	if (position - copied > bufferSize)
	{
		ctx->AudioDroppedBytes += position - copied - bufferSize;
		copied = position - bufferSize;
	}
	while (copied < position)
	{
		const ULONG offset = static_cast<ULONG>(copied % bufferSize);
		ULONG chunk = bufferSize - offset;
		if (chunk > position - copied)
		{
			chunk = static_cast<ULONG>(position - copied);
		}
		// Drop audio when the encoder fell behind and the ring is full.
		if (!ring.Write(ctx->AudioBuffer + offset, chunk))
		{
			ctx->AudioDroppedBytes += position - copied;
			copied = position;
			break;
		}
		copied += chunk;
	}
	ctx->AudioCopiedPosition = copied;

	const ULONGLONG completed = position / ctx->AudioPacketSize;
	const BOOLEAN notify = completed > ctx->AudioCompletedPackets;
	ctx->AudioCompletedPackets = completed;
	ACXSTREAM stream = ctx->AudioStream;
	static ULONG ticks;
	const BOOLEAN report = (++ticks % 200) == 0;
	INT16 firstSample = 0;
	if (report)
	{
		RtlCopyMemory(&firstSample, ctx->AudioBuffer, sizeof(firstSample));
	}
	const ULONG readable = ring.ReadableBytes();
	const ULONGLONG dropped = ctx->AudioDroppedBytes;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	if (report)
	{
		OA2DP_LOG("audio: pos %llu ring %u dropped %llu sample0 %d", position, readable, dropped, firstSample);
	}

	if (notify && stream != nullptr)
	{
		(void)AcxRtStreamNotifyPacketComplete(stream, completed - 1, qpc);
	}
}

NTSTATUS OnCreateStream(_In_ WDFDEVICE device, _In_ ACXCIRCUIT circuit, _In_ ACXPIN, _In_ PACXSTREAM_INIT streamInit,
	_In_ ACXDATAFORMAT, _In_ const GUID*, _In_ ACXOBJECTBAG)
{
	ACX_STREAM_CALLBACKS streamCallbacks;
	ACX_STREAM_CALLBACKS_INIT(&streamCallbacks);
	streamCallbacks.EvtAcxStreamPrepareHardware = OnStreamPrepareHardware;
	streamCallbacks.EvtAcxStreamReleaseHardware = OnStreamReleaseHardware;
	streamCallbacks.EvtAcxStreamRun = OnStreamRun;
	streamCallbacks.EvtAcxStreamPause = OnStreamPause;
	NTSTATUS status = AcxStreamInitAssignAcxStreamCallbacks(streamInit, &streamCallbacks);
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
	OA2DP_LOG("audio: create stream 0x%08X", status);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	GetStreamContext(stream)->Device = ctx;

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioStream = stream;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
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
	OA2DP_LOG("audio: stream destroyed");
}

NTSTATUS OnStreamPrepareHardware(_In_ ACXSTREAM)
{
	OA2DP_LOG("audio: stream prepare hardware");
	return STATUS_SUCCESS;
}

NTSTATUS OnStreamReleaseHardware(_In_ ACXSTREAM stream)
{
	OA2DP_LOG("audio: stream release hardware");
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
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	WdfTimerStart(ctx->AudioTimer, 0);
	OA2DP_LOG("audio: stream run");
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

	OA2DP_LOG("audio: stream pause, %llu bytes dropped", dropped);
	return STATUS_SUCCESS;
}

NTSTATUS OnAllocateRtPackets(_In_ ACXSTREAM stream, _In_ ULONG packetCount, _In_ ULONG packetSize, _Out_ PACX_RTPACKET* packets)
{
	*packets = nullptr;
	OA2DP_LOG("audio: allocate %u packets of %u bytes", packetCount, packetSize);
	if (packetCount == 0 || packetCount > AudioMaxPacketCount || packetSize == 0 || packetSize % AudioBytesPerFrame != 0)
	{
		OA2DP_LOG("audio: allocate rejected");
		return STATUS_NOT_SUPPORTED;
	}

	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	const SIZE_T bufferSize = static_cast<SIZE_T>(packetSize) * packetCount;

	auto* rtPackets = static_cast<PACX_RTPACKET>(
		ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ACX_RTPACKET) * packetCount, OpenA2dpPoolTag));
	if (rtPackets == nullptr)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	PHYSICAL_ADDRESS low = {};
	PHYSICAL_ADDRESS high;
	high.QuadPart = MAXLONGLONG;
	PHYSICAL_ADDRESS skip = {};
	PMDL mdl = MmAllocatePagesForMdlEx(low, high, skip, bufferSize, MmCached, MM_ALLOCATE_FULLY_REQUIRED);
	if (mdl == nullptr)
	{
		OA2DP_LOG("audio: MDL allocation failed");
		ExFreePoolWithTag(rtPackets, OpenA2dpPoolTag);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	auto* buffer = static_cast<PUCHAR>(MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute));
	if (buffer == nullptr)
	{
		MmFreePagesFromMdl(mdl);
		ExFreePool(mdl);
		ExFreePoolWithTag(rtPackets, OpenA2dpPoolTag);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(buffer, bufferSize);

	for (ULONG i = 0; i < packetCount; ++i)
	{
		ACX_RTPACKET_INIT(&rtPackets[i]);
		WDF_MEMORY_DESCRIPTOR_INIT_MDL(&rtPackets[i].RtPacketBuffer, mdl, static_cast<ULONG>(bufferSize));
		rtPackets[i].RtPacketOffset = packetSize * i;
		rtPackets[i].RtPacketSize = packetSize;
	}

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioMdl = mdl;
	ctx->AudioBuffer = buffer;
	ctx->AudioPacketSize = packetSize;
	ctx->AudioRtPacketCount = packetCount;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	*packets = rtPackets;
	OA2DP_LOG("audio: allocate ok, buffer %Iu bytes", bufferSize);
	return STATUS_SUCCESS;
}

void OnFreeRtPackets(_In_ ACXSTREAM stream, _In_ PACX_RTPACKET packets, _In_ ULONG)
{
	OA2DP_LOG("audio: free rt packets");
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	WdfTimerStop(ctx->AudioTimer, TRUE);

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	PMDL mdl = ctx->AudioMdl;
	PUCHAR buffer = ctx->AudioBuffer;
	ctx->AudioMdl = nullptr;
	ctx->AudioBuffer = nullptr;
	ctx->AudioPacketSize = 0;
	ctx->AudioRtPacketCount = 0;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	if (mdl != nullptr)
	{
		if (buffer != nullptr)
		{
			MmUnmapLockedPages(buffer, mdl);
		}
		MmFreePagesFromMdl(mdl);
		ExFreePool(mdl);
	}
	ExFreePoolWithTag(packets, OpenA2dpPoolTag);
}

NTSTATUS OnGetHwLatency(_In_ ACXSTREAM, _Out_ ULONG* fifoSize, _Out_ ULONG* delay)
{
	OA2DP_LOG("audio: get hw latency");
	*fifoSize = 128;
	*delay = 0;
	return STATUS_SUCCESS;
}

NTSTATUS OnSetRenderPacket(_In_ ACXSTREAM, _In_ ULONG packet, _In_ ULONG flags, _In_ ULONG)
{
	static ULONG calls;
	if (calls++ < 4)
	{
		OA2DP_LOG("audio: set render packet %u flags 0x%X", packet, flags);
	}
	return STATUS_SUCCESS;
}

NTSTATUS OnGetCurrentPacket(_In_ ACXSTREAM stream, _Out_ PULONG currentPacket)
{
	static ULONG calls;
	if (calls++ < 4)
	{
		OA2DP_LOG("audio: get current packet");
	}
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const ULONGLONG position = CurrentPositionLocked(ctx, nullptr);
	*currentPacket = ctx->AudioPacketSize == 0 ? 0 : static_cast<ULONG>(position / ctx->AudioPacketSize);
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return STATUS_SUCCESS;
}

NTSTATUS OnGetPresentationPosition(_In_ ACXSTREAM stream, _Out_ PULONGLONG positionInBlocks, _Out_ PULONGLONG qpcPosition)
{
	static ULONG calls;
	if (calls++ < 4)
	{
		OA2DP_LOG("audio: get presentation position");
	}
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	*positionInBlocks = CurrentPositionLocked(ctx, qpcPosition) / AudioBytesPerFrame;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return STATUS_SUCCESS;
}

NTSTATUS AddFormat(_In_ WDFDEVICE device, _In_ ACXPIN pin)
{
	ACX_DATAFORMAT_CONFIG config;
	ACX_DATAFORMAT_CONFIG_INIT_KS(&config, &RenderFormat);
	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = pin;
	ACXDATAFORMAT format;
	NTSTATUS status = AcxDataFormatCreate(device, &attrs, &config, &format);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("audio: AcxDataFormatCreate 0x%08X", status);
		return status;
	}
	status = AcxDataFormatListAddDataFormat(AcxPinGetRawDataFormatList(pin), format);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("audio: AcxDataFormatListAddDataFormat 0x%08X", status);
	}
	return status;
}

} // namespace

NTSTATUS AudioInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeSpinLock(&ctx->AudioLock);
	RingBufferView::Initialize(&ctx->PcmRingHeader, PcmRingBytes);

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
		OA2DP_LOG("audio: AcxCircuitInitAssignName 0x%08X", status);
	}
	if (NT_SUCCESS(status))
	{
		status = AcxCircuitInitAssignAcxCreateStreamCallback(circuitInit, OnCreateStream);
		if (!NT_SUCCESS(status))
		{
			OA2DP_LOG("audio: AcxCircuitInitAssignAcxCreateStreamCallback 0x%08X", status);
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
			OA2DP_LOG("audio: AcxCircuitCreate 0x%08X", status);
		}
	}
	if (circuitInit != nullptr)
	{
		AcxCircuitInitFree(circuitInit);
	}
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("audio: circuit creation 0x%08X", status);
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
		OA2DP_LOG("audio: streaming pin 0x%08X", status);
		return status;
	}
	status = AddFormat(ctx->Device, pins[0]);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	ACX_PIN_CONFIG_INIT_ID(&pinConfig, BridgePinId);
	pinConfig.Type = AcxPinTypeSource;
	pinConfig.Communication = AcxPinCommunicationNone;
	pinConfig.Category = &KSNODETYPE_SPEAKER;
	status = AcxPinCreate(circuit, &pinAttrs, &pinConfig, &pins[1]);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("audio: bridge pin 0x%08X", status);
		return status;
	}

	status = AcxCircuitAddPins(circuit, pins, ARRAYSIZE(pins));
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("audio: AcxCircuitAddPins 0x%08X", status);
		return status;
	}

	status = AcxDeviceAddCircuit(ctx->Device, circuit);
	if (!NT_SUCCESS(status))
	{
		OA2DP_LOG("audio: AcxDeviceAddCircuit 0x%08X", status);
	}
	return status;
}

ULONG AudioReadPcm(_In_ DEVICE_CONTEXT* ctx, _Out_writes_(frames * AudioChannels) INT16* pcm, _In_ ULONG frames)
{
	RingBufferView ring(&ctx->PcmRingHeader, ctx->PcmRingData);
	const ULONG wanted = frames * AudioBytesPerFrame;
	ULONG available = ring.ReadableBytes();
	available -= available % AudioBytesPerFrame;
	const ULONG bytes = available < wanted ? available : wanted;
	if (bytes != 0)
	{
		(void)ring.Read(pcm, bytes);
	}
	if (bytes < wanted)
	{
		RtlZeroMemory(reinterpret_cast<PUCHAR>(pcm) + bytes, wanted - bytes);
	}
	return bytes / AudioBytesPerFrame;
}
