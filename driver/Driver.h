#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <sdpnode.h>
#include <bthddi.h>
#include <windef.h>
#define NOBITMAP
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <acx.h>

#include <OpenA2dp/AvdtpSource.h>
#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/Rtp.h>
#include <OpenA2dp/SbcFixed.h>

#define OA2DP_LOG(fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "OpenA2dp: " fmt "\n", __VA_ARGS__)

inline constexpr ULONG OpenA2dpPoolTag = 'pdAO';
inline constexpr USHORT AvdtpPsm = 0x0019;
inline constexpr USHORT MediaMaxMtu = 895;

// Render endpoint format: 16-bit stereo 44.1 kHz (mandatory for A2DP SBC sinks).
inline constexpr ULONG AudioSampleRate = 44100;
inline constexpr USHORT AudioChannels = 2;
inline constexpr USHORT AudioBytesPerFrame = AudioChannels * sizeof(INT16);
// About 370 ms of PCM between the ACX stream and the SBC encoder.
inline constexpr ULONG PcmRingBytes = 64 * 1024;

inline constexpr LONG ChannelClosed = 0;
inline constexpr LONG ChannelOpening = 1;
inline constexpr LONG ChannelOpen = 2;
inline constexpr LONG ChannelClosing = 3;

struct DEVICE_CONTEXT
{
	WDFDEVICE Device;
	WDFIOTARGET IoTarget;

	BTH_PROFILE_DRIVER_INTERFACE Profile;
	BOOLEAN ProfileValid;
	BTH_ADDR RemoteAddress;
	// Remote device name (UTF-16) from BTHENUM, empty if unknown.
	WCHAR RemoteName[BTH_MAX_NAME_SIZE + 1];
	// Remote Class of Device from BTHENUM, 0 if unknown.
	BTH_COD RemoteClass;

	// AVDTP signaling channel.
	volatile LONG SignalingState;
	L2CAP_CHANNEL_HANDLE SignalingChannel;
	USHORT SignalingOutMtu;

	// Preallocated open request; OpenIdle is signaled while no open is in flight.
	WDFREQUEST OpenRequest;
	WDFMEMORY OpenBrbMemory;
	struct _BRB_L2CA_OPEN_CHANNEL OpenBrb;
	KEVENT OpenIdle;

	WDFWORKITEM DisconnectWorkItem;

	// L2CAP server for headset-initiated connections on the AVDTP PSM. Incoming
	// signaling connections are accepted through OpenRequest; others are
	// refused through RejectRequest (RejectBusy guards it).
	L2CAP_SERVER_HANDLE ServerHandle;
	WDFREQUEST RejectRequest;
	WDFMEMORY RejectBrbMemory;
	struct _BRB_L2CA_OPEN_CHANNEL RejectBrb;
	volatile LONG RejectBusy;
	// Set while the sink-configured stream is Open without a media channel: the
	// next incoming channel is accepted (through RejectRequest) as media transport.
	volatile LONG MediaAcceptAllowed;
	volatile LONG MediaAcceptPending;
	volatile LONG StartPending;

	// AVDTP session (Signaling.cpp). SessionLock serializes the session, the
	// write buffer and read resubmission; all of it runs at PASSIVE_LEVEL.
	WDFWAITLOCK SessionLock;
	OpenA2dp::Avdtp::SourceSession Session;
	OpenA2dp::Avdtp::SessionState LoggedState;
	WDFWORKITEM SignalingWorkItem;
	WDFTIMER ResponseTimer;
	volatile LONG ConnectPending;
	volatile LONG ReadCompleted;
	volatile LONG TimeoutPending;
	volatile LONG SuspendPending;

	// Media transport channel, opened from the signaling work item.
	L2CAP_CHANNEL_HANDLE MediaChannel;
	USHORT MediaOutMtu;

	// Media streaming (Media.cpp). MediaLock serializes the encoder, buffers and sends.
	WDFWAITLOCK MediaLock;
	WDFTIMER MediaTimer;
	WDFWORKITEM MediaWorkItem;
	BOOLEAN MediaActive;
	ULONGLONG MediaStartTime;
	ULONGLONG MediaSamplesSent;
	USHORT RtpSequence;
	ULONG RtpTimestamp;
	OpenA2dp::SbcFixed::Encoder Encoder;
	OpenA2dp::SbcFixed::ToneGenerator Tone;
	INT16 PcmBuffer[16 * 8 * 2];
	UCHAR MediaBuffer[MediaMaxMtu];

	// ACX render stream (Audio.cpp). AudioLock (spin lock) guards the RT buffer
	// and position state; the PCM ring is SPSC (audio timer -> media work item).
	KSPIN_LOCK AudioLock;
	WDFTIMER AudioTimer;
	ACXSTREAM AudioStream;
	PMDL AudioMdl;
	PUCHAR AudioBuffer;
	ULONG AudioPacketSize;
	ULONG AudioRtPacketCount;
	BOOLEAN AudioRunning;
	ULONGLONG AudioStartQpc;
	ULONGLONG AudioBasePosition;
	ULONGLONG AudioCopiedPosition;
	ULONGLONG AudioCompletedPackets;
	ULONGLONG AudioDroppedBytes;
	OpenA2dp::RingHeader PcmRingHeader;
	UCHAR PcmRingData[PcmRingBytes];

	WDFREQUEST ReadRequest;
	WDFMEMORY ReadBrbMemory;
	struct _BRB_L2CA_ACL_TRANSFER ReadBrb;
	KEVENT ReadIdle;
	UCHAR ReadBuffer[L2CAP_DEFAULT_MTU];
	UCHAR WriteBuffer[OpenA2dp::Avdtp::kMaxSignalPacket];
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// L2cap.cpp
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS L2capInitialize(_In_ DEVICE_CONTEXT* ctx);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS L2capRetrieveRemoteAddress(_In_ DEVICE_CONTEXT* ctx);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS L2capOpenSignalingChannel(_In_ DEVICE_CONTEXT* ctx);

// Registers/unregisters the L2CAP server for incoming AVDTP connections.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS L2capRegisterServer(_In_ DEVICE_CONTEXT* ctx);

_IRQL_requires_(PASSIVE_LEVEL)
void L2capUnregisterServer(_In_ DEVICE_CONTEXT* ctx);

_IRQL_requires_(PASSIVE_LEVEL)
void L2capCloseSignalingChannel(_In_ DEVICE_CONTEXT* ctx);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS L2capSend(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE channel, _In_reads_bytes_(length) PVOID data, _In_ ULONG length);

// Media transport channel (L2cap.cpp)
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS L2capOpenMediaChannel(_In_ DEVICE_CONTEXT* ctx);

_IRQL_requires_(PASSIVE_LEVEL)
void L2capCloseMediaChannel(_In_ DEVICE_CONTEXT* ctx);

// Signaling.cpp
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SignalingInitialize(_In_ DEVICE_CONTEXT* ctx);

// Called when the signaling channel opened; starts discovery.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingOnChannelOpen(_In_ DEVICE_CONTEXT* ctx);

// Called when the headset opened the media channel (acceptor role); sends START if needed.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingOnMediaChannelOpen(_In_ DEVICE_CONTEXT* ctx);

// Called before the signaling channel is closed; stops reads and the timer.
_IRQL_requires_(PASSIVE_LEVEL)
void SignalingStop(_In_ DEVICE_CONTEXT* ctx);

// Asks the signaling work item to SUSPEND the stream (e.g. after a media send failure).
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingRequestSuspend(_In_ DEVICE_CONTEXT* ctx);

// Audio.cpp
// Creates the ACX render circuit (speaker endpoint) for this device.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AudioInitialize(_In_ DEVICE_CONTEXT* ctx);

// Reads up to frames of stereo PCM rendered by Windows; pads the rest with
// silence. Returns the number of frames that came from the endpoint.
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG AudioReadPcm(_In_ DEVICE_CONTEXT* ctx, _Out_writes_(frames * AudioChannels) INT16* pcm, _In_ ULONG frames);

// Media.cpp
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS MediaInitialize(_In_ DEVICE_CONTEXT* ctx);

// Starts the SBC stream (endpoint PCM) for a configured SBC IE.
_IRQL_requires_(PASSIVE_LEVEL)
void MediaStart(_In_ DEVICE_CONTEXT* ctx, _In_reads_bytes_(ieLength) const UCHAR* ie, _In_ SIZE_T ieLength);

// Stops the stream and waits for in-flight sends.
_IRQL_requires_(PASSIVE_LEVEL)
void MediaStop(_In_ DEVICE_CONTEXT* ctx);
