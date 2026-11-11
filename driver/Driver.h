#pragma once

#include <ntifs.h>
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

#include <OpenA2dp/Aptx.h>
#include <OpenA2dp/AptxHd.h>
#include <OpenA2dp/AvdtpSource.h>
#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/Rtp.h>
#include <OpenA2dp/SbcFixed.h>

// Log format: "OpenA2dp <LEVEL> [<sink name> (<address>)] <Category>: <message>".
// ctx may be nullptr before a device exists (the tag is then "-"). Every level is
// emitted at DPFLTR_INFO_LEVEL on purpose: a debug-filter mask that shows the
// driver's messages at all must never hide the warnings and errors among them; the
// level is part of the text instead. The tag is plain ANSI in the device context, so
// it is safe to print at any IRQL (completion routines run at DISPATCH_LEVEL).
#define OA2DP_TAG(ctx) OA2dpTagOf(ctx)
#define OA2DP_LOG_AT(levelName, ctx, category, fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "OpenA2dp " levelName " [%s] " category ": " fmt "\n", \
		OA2DP_TAG(ctx), __VA_ARGS__)
#define OA2DP_INFO(ctx, category, fmt, ...) OA2DP_LOG_AT("INFO ", ctx, category, fmt, __VA_ARGS__)
#define OA2DP_WARN(ctx, category, fmt, ...) OA2DP_LOG_AT("WARN ", ctx, category, fmt, __VA_ARGS__)
#define OA2DP_ERROR(ctx, category, fmt, ...) OA2DP_LOG_AT("ERROR", ctx, category, fmt, __VA_ARGS__)

inline constexpr ULONG OpenA2dpPoolTag = 'pdAO';
inline constexpr USHORT AvdtpPsm = OpenA2dp::Avdtp::kPsm;
// Defined in OpenA2dp/Ioctl.h so the driver and the encoder service agree.
inline constexpr USHORT MediaMaxMtu = static_cast<USHORT>(OpenA2dp::Ioctl::kMediaMaxMtu);

// Render endpoint formats: PCM at 44.1 and 48 kHz, mono and stereo, in the two depths the
// codecs can consume. Those are the two rates every A2DP sink supports and the channel
// counts both codecs can encode, so whichever format Windows opens the stream with can be
// negotiated. AudioSampleRate and AudioChannels stay the defaults the endpoint reports
// before a stream exists.
inline constexpr ULONG AudioSampleRate = 44100;
inline constexpr USHORT AudioChannels = 2;
inline constexpr USHORT AudioBytesPerFrame = AudioChannels * sizeof(INT16);
inline constexpr ULONG AudioRenderRates[] = {44100, 48000};
inline constexpr ULONG AudioRenderRateCount = ARRAYSIZE(AudioRenderRates);
inline constexpr USHORT AudioRenderChannels[] = {2, 1};
inline constexpr ULONG AudioRenderChannelCount = ARRAYSIZE(AudioRenderChannels);
// Valid bits per sample. 24 is offered as 24-in-32, the Windows convention: a 32-bit
// container with the valid bits left-justified and the low byte zero, so the ring always
// holds whole samples. Only an encoder that takes 24-bit input lists those formats.
inline constexpr USHORT AudioRenderValidBits[] = {16, 24};
inline constexpr ULONG AudioRenderDepthCount = ARRAYSIZE(AudioRenderValidBits);
// Index layout: rate major, then channel count, then depth (the fastest-varying).
inline constexpr ULONG AudioRenderFormatsPerRate = AudioRenderChannelCount * AudioRenderDepthCount;
inline constexpr ULONG AudioRenderFormatCount = AudioRenderRateCount * AudioRenderFormatsPerRate;
constexpr ULONG AudioRenderRateAt(ULONG index)
{
	return AudioRenderRates[index / AudioRenderFormatsPerRate];
}
constexpr USHORT AudioRenderChannelsAt(ULONG index)
{
	return AudioRenderChannels[(index / AudioRenderDepthCount) % AudioRenderChannelCount];
}
constexpr USHORT AudioRenderValidBitsAt(ULONG index)
{
	return AudioRenderValidBits[index % AudioRenderDepthCount];
}
// Bytes one sample occupies in the stream: 24 valid bits travel in a 32-bit container.
constexpr USHORT AudioRenderSampleBytes(USHORT validBits)
{
	return validBits > 16 ? 4 : 2;
}
// The pin depth an aptX codec can run from. HD reads 24-in-32 only; classic takes 16-bit and
// narrows a 24-in-32 pin in the service with the kernel SBC path's rounding.
constexpr bool AptxPinDepthUsable(USHORT pinValidBits, USHORT codecBits) noexcept
{
	return pinValidBits == codecBits || (codecBits == OpenA2dp::Aptx::kBitsPerSample &&
										 pinValidBits == OpenA2dp::AptxHd::kBitsPerSample);
}
// About 370 ms of PCM between the ACX stream and the SBC encoder; shared with the
// encoder service (defined in OpenA2dp/Ioctl.h).
inline constexpr ULONG PcmRingBytes = OpenA2dp::Ioctl::kPcmRingBytes;

// Media modes (ctx->MediaMode). Service mode means the user-mode service encodes the codec
// (AAC or aptX HD) into the frame ring and the driver only packetizes it.
inline constexpr LONG MediaModeNone = 0;
inline constexpr LONG MediaModeSbc = 1;
inline constexpr LONG MediaModeService = 2;

// Signaling-channel reopens allowed when a sink answers a codec-switch CLOSE by
// dropping the A2DP connection; zeroed whenever a new switch sends its CLOSE.
inline constexpr ULONG CodecSwitchReconnectLimit = 3;

// Steps of a parameter-only renegotiation (same codec): SUSPEND, RECONFIGURE, START.
// Each phase waits for the response of the command it sent, then moves on.
inline constexpr LONG ReconfigureNone = 0;
inline constexpr LONG ReconfigureSuspending = 1;
inline constexpr LONG ReconfigureConfiguring = 2;
inline constexpr LONG ReconfigureStarting = 3;

inline constexpr LONG ChannelClosed = 0;
inline constexpr LONG ChannelOpening = 1;
inline constexpr LONG ChannelOpen = 2;
inline constexpr LONG ChannelClosing = 3;

// The names live with the enums in common/include, so the driver and the tools cannot
// disagree; these only fill in the word for a value the shared table does not know.
inline const char* AvdtpStateName(OpenA2dp::Avdtp::SessionState state)
{
	const char* const name = OpenA2dp::Avdtp::SessionStateName(state);
	return name != nullptr ? name : "?";
}

inline const char* CodecName(OpenA2dp::Avdtp::CodecType codec)
{
	const char* const name = OpenA2dp::Avdtp::CodecTypeName(codec);
	return name != nullptr ? name : "other";
}

// The name of an Ioctl::kCodec* value. A codec type cannot name a vendor codec, because
// every vendor codec shares it and this driver runs two, so the status/preference values
// are the only names that can tell them apart without an IE at hand.
inline const char* CodecValueName(uint32_t codec)
{
	switch (codec)
	{
	case OpenA2dp::Ioctl::kCodecSbc: return "SBC";
	case OpenA2dp::Ioctl::kCodecAptx: return "aptX";
	case OpenA2dp::Ioctl::kCodecAac: return "AAC";
	case OpenA2dp::Ioctl::kCodecAptxHd: return "aptX HD";
	default: return "?";
	}
}

// The name for a selected codec and its configuration IE: a vendor type alone says nothing,
// because every vendor codec shares it.
inline const char* CodecLabel(OpenA2dp::Avdtp::CodecType codec, const uint8_t* ie, size_t ieLength)
{
	uint8_t capabilities = 0;
	if (codec != OpenA2dp::Avdtp::CodecType::Vendor)
	{
		return CodecName(codec);
	}
	if (OpenA2dp::AptxHd::ParseIe(ie, ieLength, capabilities))
	{
		return "aptX HD";
	}
	if (OpenA2dp::Aptx::ParseIe(ie, ieLength, capabilities))
	{
		return "aptX";
	}
	return CodecName(codec);
}

// Bounds on how long a thread of ours may wait for the Bluetooth transport. The
// transport does not always complete a cancelled request, so no wait on it may be
// unbounded and nothing it still owns may be freed or reused after a wait gave up.
inline constexpr ULONG BrbControlTimeoutMs = 3000;
// How long a close waits for a cancelled send to leave the transport.
inline constexpr ULONG SendCancelGraceMs = 100;
// AVDTP packets that can wait for the signaling send slot.
inline constexpr ULONG SignalQueueDepth = 4;

inline constexpr ULONG SendSlotBytes = MediaMaxMtu > OpenA2dp::Avdtp::kMaxSignalPacket
	? static_cast<ULONG>(MediaMaxMtu)
	: static_cast<ULONG>(OpenA2dp::Avdtp::kMaxSignalPacket);

// One asynchronous ACL send. The slot owns the request, the BRB and a copy of the
// data, so a wait that times out can leave the send in flight without leaving the
// transport a pointer into a stack frame or a buffer another thread rewrites.
// Idle is signaled while no send is in flight. Callers serialize access to a slot.
// CompletionWorkItem, when set, is enqueued after every completion (after Idle is set).
struct SEND_SLOT
{
	WDFREQUEST Request;
	WDFMEMORY BrbMemory;
	struct _BRB_L2CA_ACL_TRANSFER Brb;
	KEVENT Idle;
	WDFWORKITEM CompletionWorkItem;
	volatile LONG Status;
	UCHAR Buffer[SendSlotBytes];
};

struct SIGNAL_PACKET
{
	ULONG Length;
	UCHAR Data[OpenA2dp::Avdtp::kMaxSignalPacket];
};

struct DEVICE_CONTEXT
{
	WDFDEVICE Device;
	WDFIOTARGET IoTarget;

	BTH_PROFILE_DRIVER_INTERFACE Profile;
	BOOLEAN ProfileValid;
	BTH_ADDR RemoteAddress;
	// "<name> (<aa:bb:cc:dd:ee:ff>)" for log lines; ANSI so any IRQL may print it.
	char LogTag[BTH_MAX_NAME_SIZE + 32];
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

	// Signaled once device teardown has begun, after the courtesy CLOSE. Waits that
	// would otherwise run to their timeout (control BRBs) end as soon as it is set,
	// so teardown never waits for them.
	KEVENT StopEvent;

	// Asynchronous sends: AVDTP signaling (SessionLock) and media (MediaLock).
	SEND_SLOT SignalSend;
	SEND_SLOT MediaSend;
	// AVDTP packets waiting for SignalSend, in order, under SessionLock. The send
	// completion enqueues the signaling work item, which submits the next one.
	SIGNAL_PACKET SignalQueue[SignalQueueDepth];
	ULONG SignalQueueHead;
	ULONG SignalQueueCount;
	// A signaling send was submitted and its completion status has not been logged yet.
	BOOLEAN SignalSendUnchecked;

	// Lock-free snapshot of the session for GET_STATUS, refreshed under SessionLock.
	volatile LONG AvdtpStateSnapshot;
	volatile LONG NegotiatedCodecSnapshot;
	// Set when the session reached Open as the initiator; the signaling work item then
	// opens the media channel after it has re-armed the read.
	volatile LONG MediaOpenPending;
	// The media channel open in flight (an L2CAP BRB_OP), owned by SessionLock holders
	// while the signaling channel is open and by its closer afterwards.
	PVOID MediaOpenOp;
	volatile LONG MediaOpenCompleted;
	// Initiator media-open attempts in this connection. A failed attempt ABORTs the
	// stream and MediaOpenRetry restarts discovery when the ABORT response arrives.
	ULONG MediaOpenAttempts;
	BOOLEAN MediaOpenRetry;
	// Set by REGISTER_ENCODER or SET_PREFERENCE when a codec selection input changes.
	// The signaling work item then checks, once the session is settled, whether the
	// negotiated codec still matches the current preference (CodecSwitchWanted);
	// CodecSwitchClosing restarts discovery when the CLOSE it sent for that is answered.
	// Some sinks drop the whole signaling channel on that CLOSE, so CodecSwitchRecover
	// keeps the switch alive across the teardown and L2capCloseSignalingChannel reopens
	// the channel, at most CodecSwitchReconnectLimit times per switch.
	volatile LONG CodecSwitchPending;
	// Set when the registered providers changed in a context that must not touch ACX; the
	// work item rebuilds the endpoint format listing before it reconsiders the codec.
	volatile LONG FormatsPending;
	// Set by the encoder service's cleanup. The work item arms ProviderLossTimer when the
	// configured stream really was served by the provider that left, so the stream is rebuilt
	// around a codec the driver encodes itself only when the service stays away.
	volatile LONG ProviderLossCheck;
	// Set when that timer expired: DesiredCodec may then override a forced codec, because the
	// provider's absence, not a change of mind, is what makes it unusable. Cleared by a
	// registration or a new preference, which switch back to it.
	volatile LONG ProviderFallback;
	WDFTIMER ProviderLossTimer;
	BOOLEAN CodecSwitchWanted;
	BOOLEAN CodecSwitchClosing;
	BOOLEAN CodecSwitchRecover;
	ULONG CodecSwitchReconnects;
	// The CLOSE in flight belongs to an endpoint format change: AVDTP cannot change the
	// sampling frequency or the channel count with RECONFIGURE, so the stream is closed and
	// configured again on the same sink SEP instead of being rediscovered.
	BOOLEAN FormatSwitchPending;
	// Bumped by SET_PREFERENCE whenever a stored parameter changes. The work item
	// compares it with the value the running stream was negotiated with, so a bitpool
	// or bitrate change renegotiates the codec without one being forced.
	volatile LONG PreferenceParameters;
	LONG AppliedPreferenceParameters;
	// Parameter-only change in progress; see the Reconfigure* steps. Applied in place
	// through RECONFIGURE so the sink connection and the codec stay as they are.
	LONG ReconfigurePhase;
	// Set when the AAC bitrate negotiated before the media channel opened does not fit the
	// MTU it actually reports. The work item reconfigures once at a bitrate the link
	// carries (BeginReconfigure consumes the flag); the attempt counter keeps that to one
	// try per connection, since the same session cannot yield a different configuration.
	volatile LONG MtuRenegotiate;
	volatile LONG MtuRenegotiationAttempts;
	// Consecutive media ticks that found the previous send still pending (diagnostic).
	ULONG MediaBusyTicks;
	// Set by the media timer so the work item can tell clock ticks from send completions.
	volatile LONG MediaTickPending;
	// A media send was submitted and its completion status has not been checked yet.
	// Only the media work item touches it, under MediaLock.
	BOOLEAN MediaSendUnchecked;

	// L2CAP server for sink-initiated connections on the AVDTP PSM. Incoming
	// signaling connections are accepted through OpenRequest; others are
	// refused through RejectRequest (RejectBusy guards it).
	L2CAP_SERVER_HANDLE ServerHandle;
	WDFREQUEST RejectRequest;
	WDFMEMORY RejectBrbMemory;
	struct _BRB_L2CA_OPEN_CHANNEL RejectBrb;
	volatile LONG RejectBusy;
	// Signaled while RejectBusy is clear, so the media close waits for an in-flight
	// accept instead of polling it.
	KEVENT RejectIdle;
	// Set while the sink-configured stream is Open without a media channel: the
	// next incoming channel is accepted (through RejectRequest) as media transport.
	volatile LONG MediaAcceptAllowed;
	volatile LONG MediaAcceptPending;
	volatile LONG StartPending;

	// AVDTP session (signaling/). SessionLock serializes the session, the
	// write buffer and read resubmission; all of it runs at PASSIVE_LEVEL.
	WDFWAITLOCK SessionLock;
	OpenA2dp::Avdtp::SourceSession Session;
	OpenA2dp::Avdtp::SessionState LoggedState;
	WDFWORKITEM SignalingWorkItem;
	WDFTIMER ResponseTimer;
	ULONGLONG ResponseDeadline;
	volatile LONG ConnectPending;
	volatile LONG ReadCompleted;
	volatile LONG TimeoutPending;
	volatile LONG SuspendPending;
	// Set after every session transition (Transmit, ResetSession). Teardown clears it,
	// re-checks the state under SessionLock and waits on it instead of polling.
	KEVENT SessionChanged;
	// The thread currently running OnSignalingWorkItem (it holds SessionLock then).
	// Code reached from inside the work item, such as the media-open failure
	// teardown, must neither flush that work item nor take SessionLock again; a
	// thread identity answers that reliably, where a plain flag would also fire
	// for other threads.
	PKTHREAD SignalingWorkItemThread;

	// True when the sink opened the signaling channel, false when we did. A
	// sink that connects to us usually starts its own AVDTP discovery and
	// configuration at the same moment, so we let it go first and only discover
	// once Windows runs the audio stream while the session is still Idle.
	volatile LONG SignalingIncoming;
	// Set by the ACX stream run (SignalingOnAudioRun) for the signaling work item.
	volatile LONG AudioRunPending;
	// Set by an endpoint pause or resume (SignalingOnAudioPause/Run). The work item makes the
	// AVDTP stream follow the endpoint and keeps this set until the session can do that, because
	// SUSPEND only exists from Streaming and START only from Open.
	volatile LONG EndpointStreamPending;
	// Byte 0 of the SBC capabilities the sink advertised in the last discovery (rates and
	// channel modes), 0 until one ran. Determines PCM availability independently of preferences.
	volatile LONG SinkSbcCapabilities;
	// The sample-rate/channel-mode byte of the aptX HD capabilities the sink advertised in the
	// last discovery, 0 until one ran. The endpoint's 24-bit formats follow it.
	volatile LONG SinkHdCapabilities;
	// The same byte for classic aptX; its 16-bit formats follow it.
	volatile LONG SinkAptxCapabilities;
	// Ioctl::kSinkCodec* bits for the codecs the sink advertised in the last discovery,
	// written by the selector so SET_PREFERENCE and the CLI can refuse a codec the sink
	// does not offer. Cleared when a new connection starts a fresh discovery.
	volatile LONG SinkCodecs;

	// Media transport channel, opened from the signaling work item.
	L2CAP_CHANNEL_HANDLE MediaChannel;
	// MTU the media channel last reported; kept for the rest of the connection so a
	// rediscovery caps the AAC bitrate the same way, and cleared when a connection starts.
	USHORT MediaOutMtu;

	// Media streaming (media/). MediaLock serializes the encoder, buffers and sends.
	WDFWAITLOCK MediaLock;
	WDFTIMER MediaTimer;
	WDFWORKITEM MediaWorkItem;
	volatile BOOLEAN MediaActive;
	ULONGLONG MediaStartTime;
	ULONGLONG MediaSamplesSent;
	// The RTP field is 16 bits and wraps there, so the packet count status reports is kept
	// apart from it: a stream sends 2^16 packets in minutes.
	USHORT RtpSequence;
	ULONGLONG PacketsSent;
	ULONG RtpTimestamp;
	OpenA2dp::SbcFixed::Encoder Encoder;
	INT16 PcmBuffer[16 * 8 * 2];
	UCHAR MediaBuffer[MediaMaxMtu];

	// ACX render stream (audio/). AudioLock (spin lock) guards the RT buffer
	// and position state; the PCM ring is SPSC (audio timer -> media work item).
	KSPIN_LOCK AudioLock;
	WDFTIMER AudioTimer;
	ACXSTREAM AudioStream;
	// PCM format of the stream that runs (44.1 or 48 kHz, mono or stereo, 16 or 24 valid bits).
	// The encoder input, the position math and the negotiated configuration follow it; it is
	// committed at stream run, so the probe streams Windows creates per format leave it alone.
	// Without a running stream it holds the default.
	ULONG PinSampleRate;
	USHORT PinChannels;
	USHORT PinValidBits;
	// Container bytes per frame, so 24 valid bits in a 32-bit container count as four.
	USHORT PinBytesPerFrame;
	// PCM format this endpoint offers Windows, set from SET_ENDPOINT_FORMAT and stored per
	// sink. 0 means automatic: every format the sink supports is listed. A pinned format is
	// listed alone, because Windows ignores the Sound-settings pick and plays the first
	// listed format (AudioApplyEndpointFormats).
	ULONG SelectedSampleRate;
	USHORT SelectedChannels;
	// Plug-state jack on the render sink pin. Windows shows the endpoint as unplugged
	// while the sink link is down (AudioNotifyLinkState).
	ACXJACK SinkJack;
	// Windows only re-reads an endpoint's formats when it rebuilds the endpoint, so a format
	// change while a stream runs unplugs the jack for AudioJackFlapMs and plugs it back in
	// (AudioRestartRenderStream). JackFlap is set for that window, JackFlapTimer ends it and
	// JackFlapWorkItem does the passive-level work that ending it needs.
	BOOLEAN JackFlap;
	WDFTIMER JackFlapTimer;
	WDFWORKITEM JackFlapWorkItem;
	// The streaming pin and the render format objects listed on it. A changed endpoint
	// format is applied by adding and removing those objects in place
	// (AudioApplyEndpointFormats); FormatLock serializes the rebuilds and FormatsListed
	// records which RenderFormats indices are listed now (one bit per index). The DEFAULT
	// mode list holds the same objects as the pin's built-in RAW list.
	ACXPIN StreamingPin;
	ACXDATAFORMATLIST DefaultModeFormats;
	ACXDATAFORMAT FormatObjects[AudioRenderFormatCount];
	WDFWAITLOCK FormatLock;
	ULONG FormatsListed;
	// Index of the format that leads both lists. Windows opens the shared-mode stream with
	// the first listed format, so the selection has to come first (AudioApplyEndpointFormats).
	ULONG FirstListedIndex;
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
	// Audio the producer discarded from the head of the PCM ring to keep the stream near live;
	// counted apart from the drops above, which mean new audio could not be written at all.
	ULONGLONG AudioTrimmedBytes;
	OpenA2dp::RingHeader* PcmRingHeader;
	UCHAR* PcmRingData;

	// Encoder service (service/): one shared block holds the PCM ring (driver ->
	// service) and the frame ring (service -> driver). See docs/architecture/AAC-Service-Boundary.md.
	WDFDEVICE ControlDevice;
	WDFMEMORY ServiceMemory;
	PVOID ServiceRawBase;
	UCHAR* ServiceBase;
	ULONG ServiceBlockBytes;
	OpenA2dp::RingHeader* FrameRingHeader;
	UCHAR* FrameRingData;
	OpenA2dp::Ioctl::SharedRings Rings;
	KSPIN_LOCK WaitLock;
	WDFREQUEST WaitRequest;
	OpenA2dp::Ioctl::WaitEventOut WaitState;
	uint32_t PendingEvents;
	// RingLock serializes MapRings/UnmapRings; RingOwner is the file object that mapped
	// the rings, so only that handle's cleanup unmaps them.
	KMUTEX RingLock;
	PVOID RingOwner;
	PMDL RingMdl;
	PVOID RingUserVa;
	PVOID RingProcess;
	volatile LONG EncoderRegistered;
	volatile LONG RingsMapped;
	volatile LONG ServiceActive;
	// Codec preference. Written by SET_PREFERENCE and read by the negotiation under
	// PreferenceLock (a spin lock), so neither side ever waits on SessionLock.
	KSPIN_LOCK PreferenceLock;
	OpenA2dp::Ioctl::Preference Preference;

	// Active media configuration (GET_STATUS/WAIT_EVENT) and failure counters. The media
	// work item sets the Active* fields under MediaLock; GET_STATUS reads them without it, so
	// they stay volatile: a status poll must not queue behind a media tick.
	volatile LONG MediaMode;
	volatile uint32_t ActiveCodec;
	volatile uint32_t ActiveSampleRate;
	volatile uint32_t ActiveChannels;
	volatile uint32_t ActiveBitrate;
	volatile uint32_t ActiveObjectType;
	volatile LONG StreamFault;
	// Counters a long stream keeps counting past what 32 bits hold: status reports them as
	// they are, and a soak is the case that reads them.
	volatile LONG64 FramesDropped;
	// Buffered records the driver discarded to keep the stream near live after a stall, and the
	// interrupt time of the last such event so the log stays bounded.
	volatile LONG64 FramesTrimmed;
	ULONGLONG FrameTrimLogTime;
	volatile LONG64 Underruns;
	volatile LONG64 SendFailures;
	volatile LONG LastMediaError;
	// How far ahead of the media clock this stream is sent, from the stored preference at
	// MediaStart. The kernel pacer and the service both use it, and the presentation position
	// subtracts it.
	volatile ULONG PacingLeadMs;
	// Worst driver-side latency (lead plus both ring fills) seen since this stream started,
	// sampled on the audio copy tick. A transport or service stall shows up here even after the
	// stream trimmed back to live.
	volatile LONG LatencyPeakMs;
	// Most SBC frames one packet carries for this stream, from the stored preference at
	// MediaStart; 0 lets the MTU decide. Shorter packets carry less audio each.
	volatile ULONG SbcFramesPerPacket;
	// Service-encoded pacing: interrupt time of the last sent frame and of the last underrun
	// log.
	ULONGLONG ServiceLastFrameTime;
	ULONGLONG ServiceLastUnderrunLog;
	// SBC packets the PCM ring could not fill since the last underrun log, and that log's
	// interrupt time. Taken under MediaLock with the rest of the pacer state.
	ULONG SbcUnderrunPackets;
	ULONGLONG SbcUnderrunLogTime;
	// The pacer has reached its target once for this stream. Until then it is still filling the
	// lead, and the silence that takes is not an underrun.
	BOOLEAN MediaCaughtUp;
	UCHAR FrameRecord[OpenA2dp::Ioctl::kMaxFrameRecordBytes];

	WDFREQUEST ReadRequest;
	WDFMEMORY ReadBrbMemory;
	struct _BRB_L2CA_ACL_TRANSFER ReadBrb;
	KEVENT ReadIdle;
	UCHAR ReadBuffer[L2CAP_DEFAULT_MTU];
	UCHAR WriteBuffer[OpenA2dp::Avdtp::kMaxSignalPacket];
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

inline const char* OA2dpTagOf(const DEVICE_CONTEXT* ctx)
{
	return ctx != nullptr ? ctx->LogTag : "-";
}

// l2cap/
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

// Copies the data into the slot and submits it without waiting. On success the result
// arrives in slot->Status once Idle is signaled, followed by CompletionWorkItem.
// STATUS_DEVICE_BUSY when the slot is not idle; on any failure the slot stays idle.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS L2capSendAsync(_In_ DEVICE_CONTEXT* ctx, _In_ SEND_SLOT* slot, _In_ L2CAP_CHANNEL_HANDLE channel,
	_In_reads_bytes_(length) const VOID* data, _In_ ULONG length);

_IRQL_requires_max_(DISPATCH_LEVEL)
bool L2capSendIdle(_In_ SEND_SLOT* slot);

// Cancels the send in flight, if any, so the transport releases the channel.
_IRQL_requires_max_(DISPATCH_LEVEL)
void L2capCancelSend(_In_ SEND_SLOT* slot);

// True when a failed send means the sink went away, not a driver fault.
bool IsLinkLoss(_In_ DEVICE_CONTEXT* ctx, _In_ NTSTATUS status);

// Media transport channel (l2cap/)
// Submits the media channel open and returns STATUS_PENDING; the completion sets
// MediaOpenCompleted and enqueues the signaling work item, which calls
// L2capFinishMediaOpen. Caller holds SessionLock with the signaling channel open.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS L2capStartMediaOpen(_In_ DEVICE_CONTEXT* ctx);

// Collects a completed media open: adopts the channel on success. False while no open
// has completed. Caller holds SessionLock with the signaling channel open.
_IRQL_requires_(PASSIVE_LEVEL)
bool L2capFinishMediaOpen(_In_ DEVICE_CONTEXT* ctx, _Out_ NTSTATUS* status);

// Closes the media channel; an open still in flight is cancelled and either adopted
// and closed or left to close its own channel when it completes.
_IRQL_requires_(PASSIVE_LEVEL)
void L2capCloseMediaChannel(_In_ DEVICE_CONTEXT* ctx);

// signaling/
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SignalingInitialize(_In_ DEVICE_CONTEXT* ctx);

// Called when the signaling channel opened; starts discovery.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingOnChannelOpen(_In_ DEVICE_CONTEXT* ctx);

// Forgets the AVDTP session after the signaling channel closed, so status and the
// next connection start from Idle instead of the stale Open/Streaming state. Safe to
// call from inside the signaling work item (which already holds SessionLock).
_IRQL_requires_(PASSIVE_LEVEL)
void SignalingResetSession(_In_ DEVICE_CONTEXT* ctx);

// Called when the sink opened the media channel (acceptor role); sends START if needed.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingOnMediaChannelOpen(_In_ DEVICE_CONTEXT* ctx);

// Called before the signaling channel is closed; stops reads and the timer.
_IRQL_requires_(PASSIVE_LEVEL)
void SignalingStop(_In_ DEVICE_CONTEXT* ctx);

// Best-effort AVDTP CLOSE (ABORT as fallback) before teardown, so the sink frees its SEP.
// Bounded. Returns false when the session lock stayed busy and nothing was sent.
_IRQL_requires_(PASSIVE_LEVEL)
bool SignalingAbortSession(_In_ DEVICE_CONTEXT* ctx);

// Asks the signaling work item to SUSPEND the stream (e.g. after a media send failure).
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingRequestSuspend(_In_ DEVICE_CONTEXT* ctx);

// The endpoint paused: no stream should keep running for it. The work item suspends once the
// session can take a SUSPEND; a resume before then starts it again instead.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingOnAudioPause(_In_ DEVICE_CONTEXT* ctx);

// The negotiated AAC bitrate does not fit the media channel's real MTU (it was chosen
// before the channel opened): reconfigures in place once per connection at a bitrate the
// link carries instead of leaving the stream silent.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingRequestMtuRenegotiation(_In_ DEVICE_CONTEXT* ctx);

// Called when the encoder service registered; may renegotiate the stream if that
// makes a different codec selectable. Rebuilds the endpoint formats, so PASSIVE only.
_IRQL_requires_(PASSIVE_LEVEL)
void SignalingOnEncoderRegistered(_In_ DEVICE_CONTEXT* ctx);

// Called when the encoder service closed; drops the endpoint formats only that provider
// could serve. Safe to call with the control lock held, which is why the rebuild is
// deferred to the signaling work item instead of happening here.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingOnEncoderUnregistered(_In_ DEVICE_CONTEXT* ctx);

// Called when SET_PREFERENCE replaced the stored selection. Rebuilds the endpoint formats
// the preference controls, then renegotiates an active stream instead of waiting for the
// sink to reconnect. PASSIVE only, like the rebuild it performs.
_IRQL_requires_(PASSIVE_LEVEL)
void SignalingOnPreferenceChanged(_In_ DEVICE_CONTEXT* ctx);

// Called when the endpoint format Windows opened replaced the pinned one; renegotiates an
// active stream when that changes which codec can run.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingOnStreamFormatChanged(_In_ DEVICE_CONTEXT* ctx);

// Called when Windows runs the render stream; discovers if no stream is configured.
_IRQL_requires_max_(DISPATCH_LEVEL)
void SignalingOnAudioRun(_In_ DEVICE_CONTEXT* ctx);

// audio/
// Reports the sink link state to Windows through the sink jack. Safe at PASSIVE_LEVEL.
void AudioNotifyLinkState(_In_ DEVICE_CONTEXT* ctx);

// Rebuilds the Windows endpoint by unplugging its jack briefly, which is the only way
// Windows re-reads the formats the endpoint offers. Safe at PASSIVE_LEVEL.
void AudioRestartRenderStream(_In_ DEVICE_CONTEXT* ctx);

// Fills the render format table once, from DriverEntry, so endpoints added concurrently
// never rewrite format state another endpoint is listing.
_IRQL_requires_(PASSIVE_LEVEL)
void BuildRenderFormats();

// Creates the ACX render circuit (speaker endpoint) for this device.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AudioInitialize(_In_ DEVICE_CONTEXT* ctx);

// Rebuilds the streaming pin's format list from discovered sink capabilities in place, and
// from the selected endpoint format when one is set.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AudioApplyEndpointFormats(_In_ DEVICE_CONTEXT* ctx);

// True when the sink's advertised SBC capabilities carry this PCM format. A selection the
// sink cannot play would leave the endpoint without a stream, so it is refused.
bool AudioSinkSupportsEndpointFormat(_In_ DEVICE_CONTEXT* ctx, ULONG sampleRate, USHORT channels);

// Reads up to frames of stereo PCM rendered by Windows; pads the rest with
// silence. Returns the number of frames that came from the endpoint. Reads whole 16-bit
// frames; a 24-in-32 stream is not encodable yet, so it reads nothing.
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG AudioReadPcm(_In_ DEVICE_CONTEXT* ctx, _Out_writes_(frames * AudioChannels) INT16* pcm, _In_ ULONG frames);

// True while Windows is running the render stream. It is the media clock: while it is paused
// nothing is due, so the silence a stream sends then is a pause and not a gap.
_IRQL_requires_max_(DISPATCH_LEVEL)
bool AudioStreamRunning(_In_ DEVICE_CONTEXT* ctx);

// media/
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS MediaInitialize(_In_ DEVICE_CONTEXT* ctx);

// Starts the stream for the negotiated codec. SBC encodes from the endpoint PCM in
// the kernel; AAC drains the frame ring filled by the encoder service.
_IRQL_requires_(PASSIVE_LEVEL)
void MediaStart(_In_ DEVICE_CONTEXT* ctx, OpenA2dp::Avdtp::CodecType codec, _In_reads_bytes_(ieLength) const UCHAR* ie,
	_In_ SIZE_T ieLength);

// Stops the stream and waits for in-flight sends.
_IRQL_requires_(PASSIVE_LEVEL)
void MediaStop(_In_ DEVICE_CONTEXT* ctx);

// config/
// Restores the stored codec preference: the sink's own key first, then the global
// default. Call once the sink address is known, before negotiation.
_IRQL_requires_(PASSIVE_LEVEL)
void ConfigLoadPreference(_In_ DEVICE_CONTEXT* ctx);

// Stores the current codec preference under the sink's configuration key.
_IRQL_requires_(PASSIVE_LEVEL)
void ConfigSavePreference(_In_ DEVICE_CONTEXT* ctx);

// Restores the endpoint format selected for this sink; 0 Hz means automatic.
_IRQL_requires_(PASSIVE_LEVEL)
void ConfigLoadEndpointFormat(_In_ DEVICE_CONTEXT* ctx);

// Stores the endpoint format selected for this sink.
_IRQL_requires_(PASSIVE_LEVEL)
void ConfigSaveEndpointFormat(_In_ DEVICE_CONTEXT* ctx);
