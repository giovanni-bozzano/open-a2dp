#pragma once

#include "../Driver.h"
#include "../Service.h"

#include <OpenA2dp/AacConfig.h>

using namespace OpenA2dp;

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::Media
{
constexpr ULONG MediaPeriodMs = 10;
// Service-encoded: no encoded frame for this long counts as an underrun (logged once per
// second).
// Diagnostic only: encoder loss is handled by its own events (file cleanup, ring fault).
constexpr ULONGLONG UnderrunTimeout = 200ull * 10000ull;
constexpr ULONGLONG UnderrunLogInterval = 10000000ull;
// How long a media send may stay pending in the transport before it is logged
// (diagnostic only; link loss arrives as a transport completion or disconnect
// indication). The tick count follows the period so the two cannot drift apart.
constexpr ULONG MediaBusyLimitMs = 3000;
// Most buffered audio the frame ring may hold *beyond the pacing lead* before the driver
// discards the oldest records. The driver drains the ring eagerly, so its steady fill is what
// the service produced ahead of the clock, which is the lead itself: counting the lead as
// backlog trimmed every long-lead stream back to this constant on its first packet, and logged
// a trim that had not gone stale. The slack covers a couple of records, so granularity never
// trims the lead either; past it a stall costs one gap instead of a latency step.
constexpr ULONG FrameSlackMs = 40;
constexpr ULONG MediaBusyLimitTicks = MediaBusyLimitMs / MediaPeriodMs;

EVT_WDF_TIMER OnMediaTimer;
EVT_WDF_WORKITEM OnMediaWorkItem;

void OnMediaTimer(_In_ WDFTIMER timer);
bool MediaSendReady(DEVICE_CONTEXT* ctx, bool tick);
bool SubmitPacket(DEVICE_CONTEXT* ctx, ULONG length);
ULONG BuildSbcPacket(DEVICE_CONTEXT* ctx, uint32_t& samples);
ULONG BuildServicePacket(DEVICE_CONTEXT* ctx, uint32_t& samples);
void TickSbc(DEVICE_CONTEXT* ctx, bool tick);
void TickService(DEVICE_CONTEXT* ctx, bool tick);
void OnMediaWorkItem(_In_ WDFWORKITEM workItem);
void StartAac(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
void StartAptx(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
void StartAptxHd(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
} // namespace OpenA2dpDriver::Media
