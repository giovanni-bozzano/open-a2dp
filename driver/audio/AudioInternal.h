#pragma once

#include "../Driver.h"

#include <OpenA2dp/PcmBacklog.h>

using namespace OpenA2dp;

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::Audio
{
constexpr ULONG AudioPeriodMs = 10;
// Most PCM the ring may hold before the oldest audio is dropped. It has to cover the encoder's
// pacing lead (Ioctl::kPrerollMs) plus a packet, because both are read ahead of the clock;
// beyond that the ring only adds latency to whatever is heard.
constexpr ULONG AudioPcmBacklogMs = 80;
// How long the jack reports unplugged while Windows is made to rebuild the endpoint after a
// format change; long enough for the audio engine to close the stream it was using.
constexpr ULONG AudioJackFlapMs = 80;
constexpr ULONG AudioMaxPacketCount = 2;
constexpr ULONG StreamingPinId = 0;
constexpr ULONG BridgePinId = 1;

DECLARE_CONST_UNICODE_STRING(RenderCircuitName, L"Speaker0");

struct STREAM_CONTEXT
{
	DEVICE_CONTEXT* Device;
	// The format this stream was created with. It becomes the endpoint format only once the
	// stream runs (AudioSetPinFormat), because Windows creates one stream per format it
	// enumerates before it opens the one it plays.
	ULONG SampleRate;
	USHORT Channels;
	USHORT ValidBits;
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
EVT_WDF_TIMER OnJackFlapTimer;
EVT_WDF_WORKITEM OnJackFlapWorkItem;
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
// Unplugs the jack for AudioJackFlapTicks so Windows closes the render stream and rebuilds
// the endpoint, which is the only way it re-reads the offered formats; the audio timer plugs
// it back in. No-op without a jack.
void AudioRestartRenderStream(_In_ DEVICE_CONTEXT* ctx);
// Creates the ACX format objects for RenderFormats, parented to the streaming pin, so
// they can be listed and unlisted as the sink capabilities change.
NTSTATUS CreateRenderFormats(_In_ DEVICE_CONTEXT* ctx, _In_ ACXPIN pin);
NTSTATUS AudioValidatePinFormat(_In_ DEVICE_CONTEXT* ctx, _In_ ACXDATAFORMAT format);
// Valid bits of a WAVEFORMATEX-shaped stream format, which is the depth it must run at. A
// plain WAVEFORMATEX has no valid-bits field, so it reports the container depth.
USHORT PcmValidBits(const KSDATAFORMAT_WAVEFORMATEX& wave);
void AudioSetPinFormat(_In_ DEVICE_CONTEXT* ctx, ULONG sampleRate, USHORT channels, USHORT validBits);
const GUID* EndpointCategory(_In_ BTH_COD cod);
} // namespace OpenA2dpDriver::Audio
