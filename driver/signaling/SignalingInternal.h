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
// How long a stream keeps its provider-supplied codec after that provider left, before the
// driver rebuilds it around one it encodes itself. Long enough that a service restart costs
// no renegotiation, short enough that playback resumes on its own.
constexpr LONGLONG ProviderLossGraceMs = 2000;
constexpr uint8_t SbcMaxBitpool = 53;
// What the driver can actually play. The endpoint serves 16-bit PCM at 44.1 or 48 kHz,
// mono or stereo, and MediaStart refuses any other rate or channel count, so 16/32 kHz is
// not advertised: that would let a sink configure a stream we then cannot start.
constexpr uint8_t kSbcDriverCapabilities[SbcConfig::kIeLength] = {
	SbcConfig::kFs44100 | SbcConfig::kFs48000 | SbcConfig::kAllModes,
	SbcConfig::kAllBlocks | SbcConfig::kAllSubbands | SbcConfig::kAllAllocations,
	SbcConfig::kMinBitpool,
	SbcConfig::kMaxBitpool};
// Initiator media channel opens per connection before the signaling channel is torn down.
constexpr ULONG kMediaOpenAttempts = 3;

EVT_WDF_WORKITEM OnSignalingWorkItem;
EVT_WDF_TIMER OnResponseTimeout;
EVT_WDF_TIMER OnProviderLossTimeout;
EVT_WDF_REQUEST_COMPLETION_ROUTINE OnReadComplete;

const char* DiscoveryFailureName(Avdtp::DiscoveryFailure reason);
uint32_t SelectConfig(void* context, Avdtp::CodecType codec, const uint8_t* remoteIe, size_t remoteIeLength,
	uint8_t* configIe, size_t& configIeLength);
uint8_t ValidateConfig(void* context, Avdtp::CodecType codec, const uint8_t* ie, size_t ieLength);
// False when Windows opened the endpoint at another format than the stream was configured
// with, which is what a changed endpoint format looks like once the stream is settled.
bool SelectedFormatMatchesPin(DEVICE_CONTEXT* ctx);
// The Ioctl::kCodec* value of the configured stream, derived from its IE because the two
// aptX codecs share Avdtp::CodecType::Vendor. False when nothing is configured or the
// vendor IE is not one of ours.
bool SelectedCodecValue(DEVICE_CONTEXT* ctx, uint32_t& codecValue);
// The selected AAC configuration rewritten at the highest bitrate the media channel's MTU
// carries. False when the stream is not AAC or the configuration already fits.
bool BuildMtuCappedConfiguration(DEVICE_CONTEXT* ctx, uint8_t* ie, size_t& ieLength);
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
