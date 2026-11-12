#pragma once

// Driver <-> user-mode encoder service ABI (AAC).
// Kernel-safe and user-mode safe: no STL, no exceptions, no allocation.
// Layouts and IOCTL codes are versioned by kAbiVersion; the lifecycle,
// threading and failure rules are in docs/architecture/AAC-Service-Boundary.md.

#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/Rtp.h>
#include <OpenA2dp/Types.h>

#if defined(_KERNEL_MODE)
#include <wdm.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

namespace OpenA2dp::Ioctl {

// Development baseline; driver, service and tools must come from the same build.
// No compatibility with earlier development layouts is provided.
constexpr uint32_t kAbiVersion = 5;

// Every OpenA2dp device instance (one per sink) exposes a control device.
// The SDDL limits access to SYSTEM and Administrators, so the rings cannot be
// mapped by other users. The device is named \Device\OpenA2dpCtl<address> and
// has the symbolic link \DosDevices\OpenA2dpCtl-<address>; the CLI and the
// service enumerate the links with QueryDosDevice and open \\.\<link>.
constexpr wchar_t kControlLinkPrefix[] = L"OpenA2dpCtl-";
constexpr uint32_t kControlAddressChars = 12;

// Device interface on the PnP device, enabled only while its control device exists
// (after the link is created, disabled before it goes, re-enabled after a resume). The
// service registers for its arrival instead of polling; it is never opened.
constexpr GUID kControlArrivalInterface = {
	0x9f710239, 0x3d1c, 0x472d, {0xac, 0xd1, 0x25, 0x9c, 0x64, 0xb0, 0x9b, 0x69}};

// A2DP media channel MTU (L2CAP out MTU used by the driver) and the largest
// AAC/LATM payload that fits in one RTP packet. driver/Driver.h derives its
// MediaMaxMtu from this value, so the two sides agree.
constexpr uint32_t kMediaMaxMtu = 895;
constexpr uint32_t kMaxFramePayload = kMediaMaxMtu - static_cast<uint32_t>(Rtp::kHeaderSize);

// Real time sent ahead of the clock so the sink's jitter buffer fills before it runs dry.
// The kernel paces SBC with it and the service's AAC pacing centres the encoder on it, so
// both sides of a stream start the same distance ahead. The stored preference can shorten it
// for a low-latency profile; 0 there means this default.
constexpr uint32_t kPrerollMs = 60;
constexpr uint32_t kMinPrerollMs = 20;
constexpr uint32_t kMaxPrerollMs = 200;
constexpr uint32_t EffectivePrerollMs(uint32_t preferred) noexcept
{
	return preferred == 0 ? kPrerollMs : preferred;
}

// Clock span the encoder service covers with one record (aptX, aptX HD); AAC writes one record
// per access unit instead. The driver holds its pacing lead above a record, because a lead
// shorter than the interval in which the producer hands audio over cannot be served.
constexpr uint32_t kServiceRecordMs = 10;

// Ring capacities, bytes (powers of two). PCM holds ~370 ms of 44.1 kHz stereo;
// the frame ring holds ~1.6 s at 320 kbps.
constexpr uint32_t kPcmRingBytes = 64 * 1024;
constexpr uint32_t kFrameRingBytes = 64 * 1024;

// A record is header + payload; size is exact, so records are never padded.
constexpr uint32_t kMaxFrameRecordBytes = 2048;

// AAC-LC: 1024 PCM samples per raw_data_block.
constexpr uint32_t kAacSamplesPerFrame = 1024;

// SBC and AAC values match Avdtp::CodecType. The aptX codecs cannot: AVDTP has one type for
// every vendor codec, so preferences and registration identify them by these values.
constexpr uint32_t kCodecSbc = 0x00;
constexpr uint32_t kCodecAptx = 0x01;
constexpr uint32_t kCodecAac = 0x02;
constexpr uint32_t kCodecAptxHd = 0x03;

enum class MediaState : uint32_t {
	Idle = 0,
	WaitingForEncoder = 1, // AAC configured; waiting for the first encoded frame
	Streaming = 2,
	Suspended = 3,
	Failed = 4,
};

// WAIT_EVENT event mask.
constexpr uint32_t kEventTick = 1u << 0;        // media timer tick while AAC streams
constexpr uint32_t kEventStreamStart = 1u << 1; // AAC configured; params are valid
constexpr uint32_t kEventStreamStop = 1u << 2;  // stream stopped or suspended
constexpr uint32_t kEventDeviceGone = 1u << 3;  // device removing; close the handle

// SET_PREFERENCE flags.
constexpr uint32_t kPreferenceForceCodec = 1u << 0;

// sbcBitpool is 0 (default) or within the SBC bitpool range the selector accepts.
constexpr uint32_t kSbcMinBitpool = 2;
constexpr uint32_t kSbcMaxBitpool = 250;

// SBC allocation bits, as carried in the configured IE (SbcConfig::kAllocation*).
constexpr uint32_t kSbcAllocationLoudness = 0x01;
constexpr uint32_t kSbcAllocationSnr = 0x02;

// The SBC frame parameters a stored preference may pin; 0 means the driver default.
constexpr bool ValidSbcFrameParameters(uint32_t blocks, uint32_t subbands, uint32_t allocation) noexcept
{
	const bool blocksOk = blocks == 0 || blocks == 4 || blocks == 8 || blocks == 12 || blocks == 16;
	const bool subbandsOk = subbands == 0 || subbands == 4 || subbands == 8;
	const bool allocationOk = allocation == 0 || allocation == kSbcAllocationLoudness ||
							  allocation == kSbcAllocationSnr;
	return blocksOk && subbandsOk && allocationOk;
}

// Status.sinkCodecs bits; the codec constants above cannot be used as bits because
// kCodecSbc is 0.
constexpr uint32_t kSinkCodecSbc = 1u << 0;
constexpr uint32_t kSinkCodecAac = 1u << 1;
constexpr uint32_t kSinkCodecAptxHd = 1u << 2;
constexpr uint32_t kSinkCodecAptx = 1u << 3;

// Status.encoderRegistered and DEVICE_CONTEXT::EncoderRegistered hold one bit per registered
// encoder codec. Registration is per codec because each provider supplies one codec, and a
// missing provider must keep that codec out of negotiation without hiding the others.
constexpr uint32_t EncoderBit(uint32_t codec) noexcept
{
	return codec < 32 ? 1u << codec : 0;
}

// Frame record flags.
constexpr uint32_t kFrameFlagDiscontinuity = 1u << 0; // first record after a gap
constexpr uint16_t kFrameRecordMagic = 0x4B46;        // 'FK'
constexpr uint32_t kRingsMagic = 0x3241504F;          // 'OPA2'

// IOCTLs (METHOD_BUFFERED; the interface SDDL, not the IOCTL access bits,
// restricts callers to SYSTEM and Administrators).
constexpr ULONG kIoctlGetVersion = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kIoctlRegisterEncoder = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kIoctlMapRings = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kIoctlWaitEvent = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kIoctlGetStatus = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kIoctlSetPreference = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kIoctlGetPreference = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kIoctlGetEndpointFormat = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kIoctlSetEndpointFormat = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS);

// How long a caller waits for one of these requests before cancelling it. The service and the
// tools each wrap DeviceIoControl differently, but they wait on the same driver, so the bound
// is one value here instead of a copy per caller.
constexpr ULONG kIoctlTimeoutMs = 5000;

// How long a caller waits after cancelling before it stops waiting and closes the handle. Long
// enough that the driver has finished writing into the caller's buffer, short enough that a
// wedged driver does not hold a tool request or a service stop. The kernel's transport grace
// (`SendCancelGraceMs`) is a different layer and much shorter.
constexpr ULONG kCancelGraceMs = 2000;

struct VersionInfo {
	uint32_t abiVersion;      // kAbiVersion
	uint32_t driverVersion;   // 0xMMmmpppp
	uint32_t maxFramePayload; // largest LATM payload accepted (kMaxFramePayload)
	uint32_t maxPcmRingBytes;
	uint32_t maxFrameRingBytes;
	uint32_t reserved[3];
};

struct RegisterEncoderIn {
	uint32_t abiVersion;       // must equal kAbiVersion
	uint32_t codec;            // kCodecAac, kCodecAptx or kCodecAptxHd
	uint32_t maxFramePayload;  // service limit; 0 = no limit (driver clamps)
	uint32_t reserved;
};

struct RegisterEncoderOut {
	uint32_t abiVersion;
	uint32_t codec;
	uint32_t maxFramePayload; // authoritative limit for the frame payload
	uint32_t pcmSampleRate;   // endpoint rate Windows opened (44100 or 48000)
	uint32_t pcmChannels;     // 1 or 2
	// Valid bits per sample in the PCM ring: 16 is int16, 24 is 24-in-32 (left-justified in
	// an int32, low byte zero). The ring carries the endpoint's depth, so a service encoder
	// that wants another depth converts it.
	uint32_t pcmValidBits;
	uint32_t reserved[2];
};

// Output of MAP_RINGS: one user mapping holding both rings and their headers.
// Offsets are relative to userBase. The driver keeps its own kernel VA.
struct SharedRings {
	uint32_t magic; // kRingsMagic
	uint32_t abiVersion;
	uint32_t totalBytes;
	uint32_t pcmHeaderOffset;
	uint32_t pcmDataOffset;
	uint32_t pcmCapacity;
	uint32_t frameHeaderOffset;
	uint32_t frameDataOffset;
	uint32_t frameCapacity;
	uint32_t reserved0;
	uint64_t userBase;
};

// One AAC/LATM AudioMuxElement per record and per RTP packet.
// Producer (service) writes header and payload in a single RingBuffer::Write.
// size is the exact record length (header + payload), so the payload length is
// size - sizeof(FrameRecordHeader).
struct FrameRecordHeader {
	uint16_t magic;   // kFrameRecordMagic
	uint16_t size;    // whole record including this header
	uint32_t samples; // PCM samples covered (kAacSamplesPerFrame)
	uint32_t flags;   // kFrameFlag*
};

struct WaitEventOut {
	uint32_t events; // kEvent*
	MediaState state;
	uint32_t codec;
	uint32_t sampleRate;
	uint32_t channels;
	uint32_t bitrate;
	uint32_t objectType;
	uint32_t pcmValidBits; // depth the PCM ring carries, as in RegisterEncoderOut
	// Pacing lead the service must centre its records on, from the stored preference. It is
	// the same distance the kernel paces SBC with, so the two paths behave alike.
	uint32_t prerollMs;
};

// Length of Status.sinkName, UTF-16 including the terminator. The driver captured the name
// from the sink once, the same one the endpoint is named after; a longer name is cut to fit.
constexpr uint32_t kSinkNameChars = 64;

struct Status {
	uint32_t abiVersion;
	uint32_t avdtpState; // Avdtp::SessionState
	MediaState mediaState;
	// 1 while Windows is clocking the render stream. A session can be Open while it does, and
	// Streaming only ever follows it, so this is the half of the state the AVDTP lines hide.
	uint32_t endpointRunning;
	// How many formats the endpoint lists now, and how many stream formats it refused since the
	// device started, with the last one refused. A player that is refused a stream is silent
	// without any error reaching the user, so both are part of the report.
	uint32_t listedFormats;
	uint32_t refusedStreams;
	uint32_t refusedRate;
	uint32_t refusedChannels;
	uint32_t refusedBits;
	// Endpoint rebuilds asked for by a format change, and whether the sink jack is reported
	// unplugged right now: Windows opens no stream at all while it is, and that is silence
	// with nothing in the session to show for it.
	uint32_t jackFlaps;
	uint32_t jackUnplugged;
	uint32_t codec;
	uint32_t sampleRate;
	uint32_t channels;
	uint32_t bitrate;
	uint32_t objectType;
	uint32_t encoderRegistered; // EncoderBit() per registered codec; 0 = none
	uint32_t pcmBufferedBytes;
	uint32_t pcmCapacity;
	uint32_t frameBufferedBytes;
	uint32_t frameCapacity;
	// Buffered media in milliseconds: how far the stream is behind live audio, derived from
	// the ring fills. `pcmTrimmedBytes` counts audio already discarded to keep that bounded,
	// so a non-zero value means the stream recovered from a stall instead of playing late.
	uint32_t pcmBufferedMs;
	uint32_t frameBufferedMs;
	// Pacing lead the running stream is sent ahead of the media clock by, and the worst
	// driver-side latency (lead plus both ring fills) seen since that stream started, so a
	// transient backlog spike is still visible after the stream trimmed back to live.
	uint32_t pacingLeadMs;
	uint32_t latencyPeakMs;
	// Worst gaps since this stream started, on the same reset as latencyPeakMs: the interval
	// between two PCM copies that carried audio, between two encoded records the pacer consumed,
	// and how long one media send stayed with the transport. They are the margin the lead needed
	// - a cause - where the counters below are what a margin that was too short cost.
	uint32_t worstRenderGapMs;
	uint32_t worstServiceGapMs;
	uint32_t worstSendMs;
	uint32_t lastError; // NTSTATUS of the last media failure, 0 if none
	// Codec chosen by the last AVDTP negotiation (0 = none yet). It is valid
	// before streaming starts, unlike `codec`, which is set by MediaStart.
	uint32_t negotiatedCodec;
	uint32_t preferenceFlags; // kPreference*
	uint32_t preferenceCodec; // kCodec*
	// Media channel MTU in use (the negotiated L2CAP MTU, or the largest the driver asks
	// for while no channel is open) and the AAC bitrate that MTU carries at the endpoint's
	// sample rate. The bitrate limit is independent of the codec in use.
	uint32_t mediaMtu;
	uint32_t bitrateLimit;
	// Formats the sink advertised in the last discovery, 0 until one ran: a bit per rate
	// (1 = 44100 Hz, 2 = 48000 Hz) and a bit per channel count (1 = mono, 2 = stereo). A
	// supported endpoint format has to be covered by both masks.
	uint32_t sinkRates;
	uint32_t sinkChannels;
	// kSinkCodec* bits for the codecs the sink advertised in the last discovery, 0 until
	// one ran. SBC is mandatory, so it is only ever clear before the first discovery.
	uint32_t sinkCodecs;
	// The endpoint format setting (SET/GET_ENDPOINT_FORMAT): 0/0 is automatic, otherwise the
	// one format the endpoint offers Windows. Mirrored here so a status report shows it
	// without a second IOCTL.
	uint32_t endpointRate;
	uint32_t endpointChannels;
	// Counters, 64-bit because they are never reset: they cover the endpoint's whole life, so a
	// soak reports audio discarded and packets sent from driver start. The wire RTP sequence
	// beside them is the 16-bit one and wraps in minutes. They are grouped past the byte,
	// format and setting fields so each keeps 8-byte alignment without padding the record.
	uint64_t pcmTrimmedBytes;
	uint64_t framesTrimmed;
	uint64_t packetsSent;
	uint64_t framesDropped;
	uint64_t underruns; // SBC: PCM underrun; AAC: no record available on a tick
	uint64_t sendFailures;
	// The sink's Bluetooth name, so a tool can show the sink as "name (address)" without
	// asking the Bluetooth stack; empty until the sink reported one.
	wchar_t sinkName[kSinkNameChars];
};

struct Preference {
	uint32_t abiVersion;
	uint32_t flags;            // kPreference*
	uint32_t codec;            // kCodec*
	uint32_t aacBitrate;       // 0 = driver default (highest supported <= sink max)
	uint32_t sbcBitpool;       // 0 = driver default; clamped to the sink's range
	uint32_t sbcBlocks;        // 0 = driver default; 4, 8, 12 or 16 blocks per frame
	uint32_t sbcSubbands;      // 0 = driver default; 4 or 8 subbands
	uint32_t sbcAllocation;    // 0 = driver default; kSbcAllocation*
	// Pacing lead in milliseconds: how far ahead of the media clock the stream is sent. 0 is
	// kPrerollMs, otherwise kMinPrerollMs..kMaxPrerollMs. A shorter lead lowers latency by
	// spending the sink's jitter margin.
	uint32_t prerollMs;
	// Most SBC frames one media packet carries: 0 fills the packet (the MTU decides), 1..15 caps
	// it so a packet carries less audio (shorter packets, more of them). AAC and the aptX codecs
	// send one access unit per packet and are unaffected.
	uint32_t sbcFramesPerPacket;
};

// The PCM format the render endpoint offers Windows. Windows opens the shared-mode stream
// with the first format the endpoint lists and ignores the Sound-settings pick, so a pinned
// format is offered alone: that is the only way the format the user wants is the one the
// engine plays. 0 Hz / 0 channels means every format the sink supports (the default), which
// also keeps every format available to a WASAPI exclusive-mode player.
struct EndpointFormat {
	uint32_t abiVersion;
	uint32_t sampleRate; // 0 = automatic, 44100 or 48000 otherwise
	uint32_t channels;   // 0 = automatic, 1 or 2 otherwise
	uint32_t reserved;
};

constexpr bool ValidEndpointFormat(const EndpointFormat& format) noexcept
{
	if (format.sampleRate == 0 || format.channels == 0)
	{
		return format.sampleRate == 0 && format.channels == 0;
	}
	const bool rateOk = format.sampleRate == 44100 || format.sampleRate == 48000;
	const bool channelsOk = format.channels == 1 || format.channels == 2;
	return rateOk && channelsOk;
}

static_assert(sizeof(EndpointFormat) == 16, "EndpointFormat layout is part of the driver/CLI ABI");

// Range SbcFramesPerPacket accepts (see SbcFrame::kMaxFramesPerPacket).
constexpr uint32_t kSbcMinFramesPerPacket = 1;
constexpr uint32_t kSbcMaxFramesPerPacket = 15;

// A hand-edited or stale store must not force a configuration the driver cannot serve.
// The driver and the CLI share this, so a rejected preference never leaves an endpoint
// without a render format.
constexpr bool ValidPreference(const Preference& preference) noexcept
{
	if ((preference.flags & ~kPreferenceForceCodec) != 0)
	{
		return false;
	}
	if (preference.sbcBitpool != 0 &&
		(preference.sbcBitpool < kSbcMinBitpool || preference.sbcBitpool > kSbcMaxBitpool))
	{
		return false;
	}
	if (!ValidSbcFrameParameters(preference.sbcBlocks, preference.sbcSubbands, preference.sbcAllocation))
	{
		return false;
	}
	if (preference.prerollMs != 0 &&
		(preference.prerollMs < kMinPrerollMs || preference.prerollMs > kMaxPrerollMs))
	{
		return false;
	}
	if (preference.sbcFramesPerPacket != 0 && (preference.sbcFramesPerPacket < kSbcMinFramesPerPacket ||
												 preference.sbcFramesPerPacket > kSbcMaxFramesPerPacket))
	{
		return false;
	}
	if ((preference.flags & kPreferenceForceCodec) == 0)
	{
		return true;
	}
	return preference.codec == kCodecSbc || preference.codec == kCodecAptx ||
		   preference.codec == kCodecAac || preference.codec == kCodecAptxHd;
}

static_assert(sizeof(Preference) == 40, "Preference layout is part of the driver/CLI ABI");

static_assert(sizeof(RegisterEncoderOut) == 32, "RegisterEncoderOut layout is part of the ABI");
static_assert(sizeof(WaitEventOut) == 36, "WaitEventOut layout is part of the ABI");
static_assert(sizeof(SharedRings) == 48, "SharedRings layout is part of the driver/service ABI");
static_assert(sizeof(FrameRecordHeader) == 12, "FrameRecordHeader layout is part of the ABI");
static_assert(sizeof(Status) == 336, "Status layout is part of the driver/CLI ABI");
static_assert(kMaxFrameRecordBytes >= kMaxFramePayload + sizeof(FrameRecordHeader),
			  "a maximum-size frame must fit in one record");

} // namespace OpenA2dp::Ioctl
