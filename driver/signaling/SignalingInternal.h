#pragma once

#include "../Driver.h"
#include "../Service.h"

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/SbcConfig.h>

using namespace OpenA2dp;

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::Signaling
{
constexpr LONGLONG ResponseTimeoutMs = 3000;
constexpr uint8_t SbcMaxBitpool = 53;
// What the driver can actually play. The endpoint is fixed at 44.1 kHz stereo and
// MediaStart refuses anything else, so advertising 16/32/48 kHz or mono only invites
// a device to configure a stream we then cannot start.
// Byte 0: 44.1 kHz (0x20) | dual channel (0x04) | stereo (0x02) | joint stereo (0x01).
constexpr uint8_t kSbcDriverCapabilities[SbcConfig::kIeLength] = {
	0x27, 0xFF, SbcConfig::kMinBitpool, SbcConfig::kMaxBitpool};
// Initiator media channel opens per connection before the signaling channel is torn down.
constexpr ULONG kMediaOpenAttempts = 3;

EVT_WDF_WORKITEM OnSignalingWorkItem;
EVT_WDF_TIMER OnResponseTimeout;
EVT_WDF_REQUEST_COMPLETION_ROUTINE OnReadComplete;

const char* DiscoveryFailureName(Avdtp::DiscoveryFailure reason);
uint32_t SelectConfig(void* context, Avdtp::CodecType codec, const uint8_t* remoteIe, size_t remoteIeLength,
	uint8_t* configIe, size_t& configIeLength);
uint8_t ValidateConfig(void* context, Avdtp::CodecType codec, const uint8_t* ie, size_t ieLength);
void PublishStatus(DEVICE_CONTEXT* ctx);
void ResetSession(DEVICE_CONTEXT* ctx);
void Transmit(DEVICE_CONTEXT* ctx, size_t length);
// Logs the last send's status and submits queued packets while the slot is idle.
void PumpSignalSends(DEVICE_CONTEXT* ctx);
void SubmitRead(DEVICE_CONTEXT* ctx);
void OnReadComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context);
void OnResponseTimeout(_In_ WDFTIMER timer);
void OnSignalingWorkItem(_In_ WDFWORKITEM workItem);
} // namespace OpenA2dpDriver::Signaling
