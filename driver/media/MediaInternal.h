#pragma once

#include "../Driver.h"
#include "../Service.h"

#include <OpenA2dp/AacConfig.h>

using namespace OpenA2dp;

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::Media
{
constexpr ULONG MediaPeriodMs = 10;
// Samples sent ahead of real time so the sink's jitter buffer fills.
constexpr uint32_t PrerollMs = 60;
constexpr int MaxPacketsPerTick = 8;
// AAC: no encoded frame for this long counts as an underrun (logged once per second);
// no frame for StallTimeoutMs stops the stream so it reconnects.
constexpr ULONGLONG UnderrunTimeout = 200ull * 10000ull;
constexpr ULONGLONG UnderrunLogInterval = 10000000ull;
constexpr ULONGLONG StallTimeout = 2000ull * 10000ull;
// Ticks (10 ms each) a media send may stay pending in the transport before the stream is stopped.
constexpr ULONG MediaBusyLimitTicks = 300;

EVT_WDF_TIMER OnMediaTimer;
EVT_WDF_WORKITEM OnMediaWorkItem;

void OnMediaTimer(_In_ WDFTIMER timer);
bool MediaSendReady(DEVICE_CONTEXT* ctx);
ULONG BuildSbcPacket(DEVICE_CONTEXT* ctx, uint32_t& samples);
ULONG BuildAacPacket(DEVICE_CONTEXT* ctx, uint32_t& samples);
void TickSbc(DEVICE_CONTEXT* ctx);
void TickAac(DEVICE_CONTEXT* ctx);
void OnMediaWorkItem(_In_ WDFWORKITEM workItem);
void StartAac(DEVICE_CONTEXT* ctx, const UCHAR* ie, SIZE_T ieLength);
} // namespace OpenA2dpDriver::Media
