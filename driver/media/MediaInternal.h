#pragma once

#include "../Driver.h"
#include "../Service.h"

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/LatencyBudget.h>

using namespace OpenA2dp;

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::Media
{
// Fallback wake of the one-shot media timer, and the unit the service wake cadence is
// counted in. A pacing stream is driven by send completions, so this is not a stream clock.
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
// The service paces itself against the clock it is handed, so a wake that arrives with no
// access unit due costs a context switch and no audio. Its records say how long one access
// unit lasts; the skip is capped because the encoder flushes several records per wake.
constexpr ULONG ServiceWakeMaxTicks = 3;

EVT_WDF_TIMER OnMediaTimer;
EVT_WDF_WORKITEM OnMediaWorkItem;

void OnMediaTimer(_In_ WDFTIMER timer);
bool MediaSendReady(DEVICE_CONTEXT* ctx, bool tick);
bool SubmitPacket(DEVICE_CONTEXT* ctx, ULONG length);
ULONG BuildSbcPacket(DEVICE_CONTEXT* ctx, uint32_t dueSamples, uint32_t& samples);
ULONG BuildServicePacket(DEVICE_CONTEXT* ctx, uint32_t& samples);
void TickSbc(DEVICE_CONTEXT* ctx, bool tick);
void TickService(DEVICE_CONTEXT* ctx, bool tick);
void OnMediaWorkItem(_In_ WDFWORKITEM workItem);
void StartAac(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
void StartAptx(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
void StartAptxHd(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
} // namespace OpenA2dpDriver::Media
