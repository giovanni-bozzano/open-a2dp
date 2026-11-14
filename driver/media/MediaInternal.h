#pragma once

#include "../Driver.h"
#include "../Service.h"

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/LatencyBudget.h>
#include <OpenA2dp/Rtp.h>

using namespace OpenA2dp;

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::Media
{
// Fallback wake of the one-shot media timer, and how often a service-encoded stream is
// drained. A pacing stream is driven by send completions, so this is not a stream clock.
constexpr ULONG MediaPeriodMs = 10;
// Service-encoded: no encoded frame for this long while the endpoint runs counts as an
// underrun (logged once per second).
// Diagnostic only: encoder loss is handled by its own events (file cleanup, ring fault).
constexpr ULONGLONG UnderrunTimeout = 200ull * 10000ull;
constexpr ULONGLONG UnderrunLogInterval = 10000000ull;
// How long a media send may stay pending in the transport before it is logged
// (diagnostic only; link loss arrives as a transport completion or disconnect
// indication). Measured in real time, because the watchdog wakes on demand.
constexpr ULONG MediaBusyLimitMs = 3000;
constexpr ULONGLONG MediaBusyLimit = MediaBusyLimitMs * 10000ull;
// A send half this long on either side is logged: the transport and the wait for the media work
// item are the two halves a hold-off can come from, and only the first is the Bluetooth link.
constexpr ULONG MediaSplitLogMs = 40;
// How long a stop waits for the media work item's lock before going ahead without it. A run is
// bounded, so this only elapses when the run itself is stuck, and waiting forever would hang the
// session that asked for the stop.
constexpr ULONG MediaStopLockLimitMs = 2000;
constexpr ULONGLONG MediaStopLockLimit = MediaStopLockLimitMs * 10000ull;
// A service-encoded stream that has produced no frame for this long cannot be kept: the sink
// would buffer silence behind a session that claims to play. Suspending leaves an Open session
// the endpoint edge starts again instead.
constexpr ULONGLONG ServiceSilenceLimit = 1000ull * 10000ull;

// Bytes one media packet can carry after the RTP header, or 0 before a channel MTU is known.
// Without the guard the subtraction wraps and the packet is sized for a link that is not there.
inline size_t MediaPayloadCapacity(const DEVICE_CONTEXT* ctx) noexcept
{
	return ctx->MediaOutMtu > Rtp::kHeaderSize
		? static_cast<size_t>(ctx->MediaOutMtu) - Rtp::kHeaderSize
		: 0;
}

// MediaActive gates runs from the timer, the audio copy and the send completions, all of which
// read it without a lock. Every write goes through one atomic so a stop is seen whole.
inline void SetMediaActive(DEVICE_CONTEXT* ctx, bool active) noexcept
{
	InterlockedExchange8(reinterpret_cast<volatile CHAR*>(&ctx->MediaActive), active ? 1 : 0);
}

// The endpoint format a stream has to match. The pin fields are written under AudioLock from the
// audio side while a start runs on the signaling path, so a start reads them once, as one set,
// instead of comparing against a format that changed underneath it.
struct PinFormat
{
	ULONG sampleRate;
	USHORT channels;
	USHORT validBits;
	USHORT bytesPerFrame;
};

inline PinFormat SnapshotPinFormat(DEVICE_CONTEXT* ctx) noexcept
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const PinFormat format{ctx->PinSampleRate, ctx->PinChannels, ctx->PinValidBits, ctx->PinBytesPerFrame};
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return format;
}

// Ends a stream the media path could not start. Without this the sink stays in Streaming with
// silence while the session claims to play; the suspend returns it to Open, where the endpoint
// edge starts it again (a bounded number of times) or leaves it suspended.
inline void AbandonStream(DEVICE_CONTEXT* ctx) noexcept
{
	SignalingRequestSuspend(ctx);
}

EVT_WDF_TIMER OnMediaTimer;
EVT_WDF_WORKITEM OnMediaWorkItem;

void OnMediaTimer(_In_ WDFTIMER timer);
bool MediaSendReady(DEVICE_CONTEXT* ctx, bool tick);
bool SubmitPacket(DEVICE_CONTEXT* ctx, ULONG length);
ULONG BuildSbcPacket(DEVICE_CONTEXT* ctx, uint32_t dueSamples, uint32_t& samples);
ULONG BuildServicePacket(DEVICE_CONTEXT* ctx, uint32_t& samples);
void TickSbc(DEVICE_CONTEXT* ctx, bool tick);
void TickService(DEVICE_CONTEXT* ctx, bool tick);
// Per-stream diagnostics start over with the stream: the peaks, the consecutive-over-limit count
// and the timestamps they are measured from all describe one stream. Caller holds MediaLock; the
// audio side resets its own pair under AudioLock.
inline void MediaStatsReset(DEVICE_CONTEXT* ctx)
{
	ctx->FrameTrimStrikes = 0;
	ctx->WorstServiceGapMs = 0;
	ctx->WorstSendMs = 0;
	ctx->WorstSendTransportMs = 0;
	ctx->WorstSendResumeMs = 0;
	ctx->LastRecordTime = 0;
	ctx->MediaSendPending = FALSE;
	ctx->MediaSendLogTime = 0;
	ctx->MediaSend.CompletedAt = 0;
}

// PCM samples (per channel) one SBC packet of this stream carries. Caller holds MediaLock.
uint32_t SbcPacketSamples(DEVICE_CONTEXT* ctx);
// Holds the pacing lead at one access unit of the negotiated codec plus the copy slack, so the
// pacer never asks for a packet the producer has not had the interval to fill. Caller holds
// MediaLock, and must call it before the service is told the stream started.
void ApplyLeadFloor(DEVICE_CONTEXT* ctx, uint32_t unitSamples);
void OnMediaWorkItem(_In_ WDFWORKITEM workItem);
void StartAac(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
void StartAptx(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
void StartAptxHd(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
void StartLdac(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
} // namespace OpenA2dpDriver::Media
