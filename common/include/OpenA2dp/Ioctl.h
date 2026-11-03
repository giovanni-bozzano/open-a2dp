#pragma once

// Driver <-> user-mode encoder service ABI (AAC).
// Kernel-safe and user-mode safe: no STL, no exceptions, no allocation.
// Layouts and IOCTL codes are versioned by kAbiVersion; the lifecycle,
// threading and failure rules are in docs/architecture/AAC-Service-Boundary.md.

#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/Types.h>

#if defined(_KERNEL_MODE)
#include <wdm.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

namespace OpenA2dp::Ioctl {

// 2: Status carries the negotiated codec and the current preference.
constexpr uint32_t kAbiVersion = 2;

// Every OpenA2dp device instance (one per device) exposes a control device.
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
// AAC/LATM payload that fits in one RTP packet. Keep the MTU in sync with
// MediaMaxMtu in driver/Driver.h (it is defined here so both sides agree).
constexpr uint32_t kMediaMaxMtu = 895;
constexpr uint32_t kRtpHeaderBytes = 12;
constexpr uint32_t kMaxFramePayload = kMediaMaxMtu - kRtpHeaderBytes;

// Ring capacities, bytes (powers of two). PCM holds ~370 ms of 44.1 kHz stereo;
// the frame ring holds ~1.6 s at 320 kbps.
constexpr uint32_t kPcmRingBytes = 64 * 1024;
constexpr uint32_t kFrameRingBytes = 64 * 1024;

// A record is header + payload; size is exact, so records are never padded.
constexpr uint32_t kMaxFrameRecordBytes = 2048;

// AAC-LC: 1024 PCM samples per raw_data_block.
constexpr uint32_t kAacSamplesPerFrame = 1024;

// Codec values match Avdtp::CodecType.
constexpr uint32_t kCodecSbc = 0x00;
constexpr uint32_t kCodecAac = 0x02;

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
constexpr uint32_t kEventCodecChange = 1u << 3; // reconfigured while streaming
constexpr uint32_t kEventDeviceGone = 1u << 4;  // device removing; close the handle

// SET_PREFERENCE flags.
constexpr uint32_t kPreferenceForceCodec = 1u << 0;

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
	uint32_t codec;            // kCodecAac
	uint32_t maxFramePayload;  // service limit; 0 = no limit (driver clamps)
	uint32_t reserved;
};

struct RegisterEncoderOut {
	uint32_t abiVersion;
	uint32_t codec;
	uint32_t maxFramePayload; // authoritative limit for the frame payload
	uint32_t pcmSampleRate;   // 44100 until M5b adds a multi-rate endpoint
	uint32_t pcmChannels;     // 2
	uint32_t reserved[3];
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
	uint32_t reserved[2];
};

struct Status {
	uint32_t abiVersion;
	uint32_t avdtpState; // Avdtp::SessionState
	MediaState mediaState;
	uint32_t codec;
	uint32_t sampleRate;
	uint32_t channels;
	uint32_t bitrate;
	uint32_t objectType;
	uint32_t encoderRegistered;
	uint32_t pcmBufferedBytes;
	uint32_t pcmCapacity;
	uint32_t frameBufferedBytes;
	uint32_t frameCapacity;
	uint32_t packetsSent;
	uint32_t framesDropped;
	uint32_t underruns; // SBC: PCM underrun; AAC: no record available on a tick
	uint32_t sendFailures;
	uint32_t lastError; // NTSTATUS of the last media failure, 0 if none
	// Codec chosen by the last AVDTP negotiation (0 = none yet). It is valid
	// before streaming starts, unlike `codec`, which is set by MediaStart.
	uint32_t negotiatedCodec;
	uint32_t preferenceFlags; // kPreference*
	uint32_t preferenceCodec; // kCodec*
};

struct Preference {
	uint32_t abiVersion;
	uint32_t flags;         // kPreference*
	uint32_t codec;         // kCodec*
	uint32_t aacBitrate;    // 0 = driver default (highest supported <= sink max)
	uint32_t aacSampleRate; // 0 = driver default
	uint32_t reserved[3];
};

static_assert(sizeof(SharedRings) == 48, "SharedRings layout is part of the driver/service ABI");
static_assert(sizeof(FrameRecordHeader) == 12, "FrameRecordHeader layout is part of the ABI");
static_assert(sizeof(Status) == 84, "Status layout is part of the driver/CLI ABI");
static_assert(kMaxFrameRecordBytes >= kMaxFramePayload + sizeof(FrameRecordHeader),
			  "a maximum-size frame must fit in one record");

} // namespace OpenA2dp::Ioctl
