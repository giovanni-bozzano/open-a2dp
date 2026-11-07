#pragma once

#include "../Driver.h"

using namespace OpenA2dp;

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::Audio
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

struct SINK_JACK_CONTEXT
{
	DEVICE_CONTEXT* Device;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SINK_JACK_CONTEXT, GetSinkJackContext)

struct RENDER_FORMAT
{
	KSDATAFORMAT DataFormat;
	WAVEFORMATEXTENSIBLE WaveFormatExt;
};

extern RENDER_FORMAT RenderFormats[AudioRenderFormatCount];

EVT_ACX_CIRCUIT_CREATE_STREAM OnCreateStream;
EVT_ACX_JACK_RETRIEVE_PRESENCE_STATE OnRetrieveJackPresence;
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

ULONGLONG QpcNow(ULONGLONG* frequency);
ULONGLONG CurrentPositionLocked(DEVICE_CONTEXT* ctx, ULONGLONG* qpc);
void OnAudioTimer(_In_ WDFTIMER timer);
NTSTATUS OnCreateStream(_In_ WDFDEVICE device, _In_ ACXCIRCUIT circuit, _In_ ACXPIN, _In_ PACXSTREAM_INIT streamInit,
	_In_ ACXDATAFORMAT, _In_ const GUID*, _In_ ACXOBJECTBAG);
void OnStreamDestroy(_In_ WDFOBJECT object);
NTSTATUS OnStreamPrepareHardware(_In_ ACXSTREAM);
NTSTATUS OnStreamReleaseHardware(_In_ ACXSTREAM stream);
NTSTATUS OnStreamRun(_In_ ACXSTREAM stream);
NTSTATUS OnStreamPause(_In_ ACXSTREAM stream);
NTSTATUS OnAllocateRtPackets(_In_ ACXSTREAM stream, _In_ ULONG packetCount, _In_ ULONG packetSize, _Out_ PACX_RTPACKET* packets);
void OnFreeRtPackets(_In_ ACXSTREAM stream, _In_ PACX_RTPACKET packets, _In_ ULONG);
NTSTATUS OnGetHwLatency(_In_ ACXSTREAM, _Out_ ULONG* fifoSize, _Out_ ULONG* delay);
NTSTATUS OnSetRenderPacket(_In_ ACXSTREAM, _In_ ULONG, _In_ ULONG, _In_ ULONG);
NTSTATUS OnGetCurrentPacket(_In_ ACXSTREAM stream, _Out_ PULONG currentPacket);
NTSTATUS OnGetPresentationPosition(_In_ ACXSTREAM stream, _Out_ PULONGLONG positionInBlocks, _Out_ PULONGLONG qpcPosition);
NTSTATUS OnRetrieveJackPresence(_In_ ACXJACK jack, _Out_ PBOOLEAN isConnected);
NTSTATUS AddSinkJack(_In_ DEVICE_CONTEXT* ctx, _In_ ACXPIN pin);
void BuildRenderFormats();
NTSTATUS AddFormat(_In_ WDFDEVICE device, _In_ ACXPIN pin, _In_ RENDER_FORMAT& format);
void AudioSetPinFormat(_In_ DEVICE_CONTEXT* ctx, _In_ ACXDATAFORMAT format);
const GUID* EndpointCategory(_In_ BTH_COD cod);
} // namespace OpenA2dpDriver::Audio
