// OpenA2dpCtl.exe - diagnostic CLI for the OpenA2dp A2DP source driver.
//
// Enumerates the per-device control interfaces (one per sink) and uses the
// IOCTLs in common/include/OpenA2dp/Ioctl.h to report the negotiated codec and
// to set the codec preference. The tool is the diagnostic and script path; the
// end-user path is the settings window (tools/OpenA2dpConfig), which shows the
// same report through tools/common/StatusReport.

#include <OpenA2dp/Ioctl.h>

#include "common/ControlDevice.h"
#include "common/SinkDriver.h"
#include "common/StatusReport.h"
#include "common/Text.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <io.h>
#include <string>
#include <vector>

namespace {

using namespace OpenA2dp;
using namespace OpenA2dp::Tools;

constexpr const char* kUsage = R"(OpenA2dpCtl - inspect and configure the OpenA2dp A2DP source driver.

Usage:
  OpenA2dpCtl [status] [--device <mac|index>] [--verbose]
                       [--watch [--interval <ms>]]
  OpenA2dpCtl list
  OpenA2dpCtl codec [<auto|sbc|aac|aptx|aptxhd|ldac>] [--bitrate <bps>]
                     [--bitpool <n>] [--blocks <4|8|12|16>] [--subbands <4|8>]
                     [--allocation <loudness|snr>] [--preroll <ms>] [--pause-grace <ms>]
                     [--frames <n>] [--ldac-quality <high|standard|mobile>]
                     [--device <mac|index>]
  OpenA2dpCtl format [<codec> [<auto|16000|32000|44100|48000|88200|96000> [<1|2>]]] [--device <mac|index>]
  OpenA2dpCtl driver [<opena2dp|windows>] [--device <mac|index>]
  OpenA2dpCtl reset [--device <mac|index>]
  OpenA2dpCtl reset-counters [--device <mac|index>]
  OpenA2dpCtl version [--device <mac|index>]
  OpenA2dpCtl help

Commands:
  status    Show the AVDTP/media state, codec, preference and counters (default).
  list      List the present OpenA2dp devices.
  codec     Set the codec preference, stored for this sink and restored
            when the device starts. If a stream is active it is renegotiated
            immediately; otherwise the preference applies at the next AVDTP
            negotiation. "auto" prefers LDAC, then aptX HD, then aptX, then AAC
            while an encoder service is registered, then SBC. --bitrate and
            --bitpool cap the AAC bitrate and the SBC bitpool; --blocks,
            --subbands and --allocation pin the SBC frame shape; --preroll shortens
            the lead the stream is sent ahead of the clock and --frames caps how
            many SBC frames one packet carries, which lowers latency while the sink
            still buffers reliably; --pause-grace sets how long a paused stream
            is kept before it is suspended, so a short break resumes without a
            START; --ldac-quality picks LDAC's 990, 660 or
            330 kbps mode. 0 restores the driver default
            (--allocation default), and the sink's advertised limits still apply.
            Without a codec name the stored selection is kept and only the
            flags given are applied.
            Codec settings never narrow the supported endpoint format list.
  format    Show or set the PCM format each codec's streams are played at.
            Without arguments every codec's entry is printed. Each codec keeps
            its own: "auto" offers every format that codec can play, a rate and
            a channel count pin it to that one format, which is then the only
            entry in the Windows format list while that codec runs. Windows
            plays the first format the endpoint lists and ignores the pick,
            so pinning is the only way to force one; the sink has to advertise
            it. Pinning also limits WASAPI exclusive-mode players to it.
  driver    Show which A2DP source driver this sink is bound to, or move it to
            the other one: `driver opena2dp` takes the sink over from the inbox
            BthA2dp, `driver windows` hands it back. Only this sink's BTHENUM
            A2DP node is rebound, so other sinks keep their driver; the node
            restarts and any open stream goes with it. A program that renders
            to the sink holds the node open, so it is paused first: Windows
            cannot restart the node while a stream is open. A program that
            cannot be paused makes the switch refuse (exit 3) and nothing is
            changed. The OpenA2dp package has to be deployed once
            (scripts/deploy-vm.ps1) for the forward switch, and the inbox
            driver has to still be a candidate for the node for the way back.
  reset     Clear this sink's stored codec preference and endpoint format, so
            the automatic codec order and every sink-supported PCM format
            apply again.
  reset-counters
            Clear every counter status reports - packets, drops, underruns, send
            failures, trimmed audio, refused streams, jack flaps, the latency
            peak and the worst gaps - so a measurement window starts from zero.
            They otherwise accumulate across streams; the stream and its
            settings are untouched, so this can be done while the sink is
            playing.
  version   Show the driver ABI and version.

Options:
  --device <spec>   Select a device by Bluetooth address (AA:BB:CC:DD:EE:FF),
                    1-based list index, or a partial address.
  --bitrate <bps>   AAC bitrate cap for `codec` (0 = driver default).
  --bitpool <n>     SBC bitpool cap for `codec`, 0 or 2..250 (0 = driver
                    default). The sink's minimum and maximum still win.
  --ldac-quality <q>
                    LDAC mode for `codec`: high (990 kbps), standard (660) or
                    mobile (330). It is not negotiated, so both ends just use it.
  --preroll <ms>    Pacing lead for `codec`, 0 (default, 60 ms) or 20..200.
                    Every codec is sent this far ahead of the media clock; a
                    shorter lead is less latency but less margin against the
                    sink running dry.
  --pause-grace <ms>
                    How long a paused stream is kept before it is suspended, 0
                    (default, 3000 ms) or 1000..60000. A pause shorter than this
                    resumes at once; a longer one pays for a START.
  --frames <n>      SBC frames per media packet for `codec`, 0 (fill the
                    packet) or 1..15. Fewer frames is a shorter packet, less
                    audio buffered per packet and more packets per second.
                    AAC and aptX send one access unit per packet regardless.
  --watch           Refresh status until interrupted.
  --interval <ms>   Refresh period for --watch (default 1000).
  --verbose         Append every Status and Preference field with its decimal
                    and hex value. The settings window shows the same report,
                    and its Copy button puts this text on the clipboard.

The tool must run elevated: the control device only allows SYSTEM and
Administrators.
)";

bool IsDecimal(const std::string& text)
{
	if (text.empty())
	{
		return false;
	}
	for (const char c : text)
	{
		if (c < '0' || c > '9')
		{
			return false;
		}
	}
	return true;
}

std::string DeviceLabel(const Device& device)
{
	return "Device " + FormatAddress(device.reference);
}

bool FindDevice(const std::vector<Device>& devices, const std::string& spec, size_t& index, std::string& error)
{
	if (devices.empty())
	{
		error = "no OpenA2dp devices found (is the driver bound to a paired sink?)";
		return false;
	}
	if (spec.empty())
	{
		if (devices.size() == 1)
		{
			index = 0;
			return true;
		}
		error = "multiple OpenA2dp devices; select one with --device";
		return false;
	}
	std::string normalized;
	for (const char c : spec)
	{
		if (c != ':' && c != '-')
		{
			normalized.push_back(c);
		}
	}
	if (IsHex12(normalized))
	{
		const std::string wanted = Lower(normalized);
		for (size_t i = 0; i < devices.size(); ++i)
		{
			if (Lower(devices[i].reference) == wanted)
			{
				index = i;
				return true;
			}
		}
		error = "no device with address " + FormatAddress(normalized);
		return false;
	}
	if (IsDecimal(spec))
	{
		const unsigned long number = std::strtoul(spec.c_str(), nullptr, 10);
		if (number >= 1 && number <= devices.size())
		{
			index = static_cast<size_t>(number - 1);
			return true;
		}
		error = "device index out of range (1-" + std::to_string(devices.size()) + ")";
		return false;
	}

	const std::string wanted = Lower(normalized);
	size_t match = devices.size();
	for (size_t i = 0; i < devices.size(); ++i)
	{
		if (Lower(devices[i].reference).find(wanted) != std::string::npos)
		{
			if (match != devices.size())
			{
				error = "partial address matches more than one device; use the full address or index";
				return false;
			}
			match = i;
		}
	}
	if (match == devices.size())
	{
		error = "no device matches '" + spec + "'";
		return false;
	}
	index = match;
	return true;
}

std::string EndpointFormatText(const Ioctl::EndpointFormat& format)
{
	std::string text;
	for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
	{
		if (format.sampleRate[codec] == 0)
		{
			continue;
		}
		if (!text.empty())
		{
			text += ", ";
		}
		text += std::string(CodecName(codec)) + " " + std::to_string(format.sampleRate[codec]) + " Hz " +
				std::to_string(format.channels[codec]) + " ch";
	}
	return text.empty() ? std::string("automatic (each codec plays every format it can)") : text;
}

// The codec an argument names, or -1.
int CodecIndexOf(const std::string& name)
{
	if (name == "sbc")
	{
		return static_cast<int>(Ioctl::kCodecSbc);
	}
	if (name == "aac")
	{
		return static_cast<int>(Ioctl::kCodecAac);
	}
	if (name == "aptx")
	{
		return static_cast<int>(Ioctl::kCodecAptx);
	}
	if (name == "aptxhd")
	{
		return static_cast<int>(Ioctl::kCodecAptxHd);
	}
	if (name == "aptxll")
	{
		return static_cast<int>(Ioctl::kCodecAptxLl);
	}
	return name == "ldac" ? static_cast<int>(Ioctl::kCodecLdac) : -1;
}

// Prints the report the settings window shows, so a paste from either one is comparable.
void PrintStatus(const Device& device, HANDLE handle, const Ioctl::Status& status,
				 const Ioctl::Preference& preference, bool verbose)
{
	// The version block is informational: a failed read never hides the status.
	Ioctl::VersionInfo version = {};
	std::string ignored;
	const bool hasVersion = ReadVersion(handle, version, ignored);

	Tools::StatusReportInput input;
	input.title = DeviceLabel(device);
	input.status = &status;
	input.preference = &preference;
	input.version = hasVersion ? &version : nullptr;
	input.verbose = verbose;
	const std::string report = Tools::BuildStatusReport(input);
	std::fputs(report.c_str(), stdout);
}

int CommandStatus(const std::vector<Device>& devices, const std::string& spec, bool watch, unsigned interval,
				  bool verbose)
{
	size_t index = 0;
	std::string error;
	if (!FindDevice(devices, spec, index, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 2;
	}
	const Device& device = devices[index];
	const HANDLE handle = OpenControl(device, error);
	if (handle == INVALID_HANDLE_VALUE)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	const bool clear = watch && _isatty(_fileno(stdout)) != 0;
	int result = 0;
	do
	{
		Ioctl::Status status = {};
		Ioctl::Preference preference = {};
		if (!ReadStatus(handle, status, error) || !ReadPreference(handle, preference, error))
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			result = 1;
			break;
		}
		if (clear)
		{
			std::printf("\x1b[2J\x1b[H");
		}
		PrintStatus(device, handle, status, preference, verbose);
		if (watch)
		{
			std::fflush(stdout);
			Sleep(interval);
		}
	} while (watch);

	CloseHandle(handle);
	return result;
}

int CommandList(const std::vector<Device>& devices)
{
	if (devices.empty())
	{
		std::printf("No OpenA2dp devices found.\n");
		return 1;
	}

	// One row per sink: "<name> (<address>)" like the configuration tool, then the link
	// state and the codec, so a name reads the same in both places.
	std::vector<std::string> sinks;
	std::vector<std::string> states;
	std::vector<std::string> codecs;
	size_t sinkWidth = std::strlen("sink");
	size_t stateWidth = std::strlen("AVDTP");
	for (const Device& device : devices)
	{
		std::string sink = FormatAddress(device.reference);
		std::string state = "unavailable";
		std::string codec = "-";
		std::string error;
		const HANDLE handle = OpenControl(device, error);
		if (handle != INVALID_HANDLE_VALUE)
		{
			Ioctl::Status status = {};
			if (ReadStatus(handle, status, error))
			{
				if (status.sinkName[0] != L'\0')
				{
					sink = Utf8(status.sinkName) + " (" + sink + ")";
				}
				state = AvdtpStateName(status.avdtpState);
				if (status.mediaState == Ioctl::MediaState::Streaming && status.codec == 0)
				{
					codec = "SBC";
				}
				else if (status.codec != 0)
				{
					codec = CodecName(status.codec);
				}
				else if (status.negotiatedCodec != 0)
				{
					codec = std::string(CodecName(status.negotiatedCodec)) + "*";
				}
				else
				{
					codec = "none";
				}
			}
			CloseHandle(handle);
		}
		sinkWidth = (std::max)(sinkWidth, sink.size());
		stateWidth = (std::max)(stateWidth, state.size());
		sinks.push_back(std::move(sink));
		states.push_back(std::move(state));
		codecs.push_back(std::move(codec));
	}

	std::printf("%-3s %-*s %-*s %s\n", "#", static_cast<int>(sinkWidth), "sink", static_cast<int>(stateWidth),
				"AVDTP", "codec");
	for (size_t i = 0; i < sinks.size(); ++i)
	{
		std::printf("%-3u %-*s %-*s %s\n", static_cast<unsigned>(i + 1), static_cast<int>(sinkWidth),
					sinks[i].c_str(), static_cast<int>(stateWidth), states[i].c_str(), codecs[i].c_str());
	}
	std::printf("\n* negotiated but not streaming\n");
	return 0;
}

int CommandVersion(const std::vector<Device>& devices, const std::string& spec)
{
	size_t index = 0;
	std::string error;
	if (!FindDevice(devices, spec, index, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 2;
	}
	const Device& device = devices[index];
	const HANDLE handle = OpenControl(device, error);
	if (handle == INVALID_HANDLE_VALUE)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	Ioctl::VersionInfo version = {};
	const bool ok = ReadVersion(handle, version, error);
	CloseHandle(handle);
	if (!ok)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	std::printf("%s\n", DeviceLabel(device).c_str());
	std::printf("  driver          %u.%u.%u\n", (version.driverVersion >> 24) & 0xFF,
				(version.driverVersion >> 16) & 0xFF, version.driverVersion & 0xFFFF);
	std::printf("  ABI             %u (tool expects %u)\n", version.abiVersion, Ioctl::kAbiVersion);
	std::printf("  max frame       %u B\n", version.maxFramePayload);
	std::printf("  rings           PCM %u B, frames %u B\n", version.maxPcmRingBytes, version.maxFrameRingBytes);
	return 0;
}

// The parameters a `codec` command names; anything it leaves out keeps its stored value.
struct PreferenceOverrides {
	bool hasBitrate = false;
	uint32_t bitrate = 0;
	bool hasBitpool = false;
	uint32_t bitpool = 0;
	bool hasBlocks = false;
	uint32_t blocks = 0;
	bool hasSubbands = false;
	uint32_t subbands = 0;
	bool hasAllocation = false;
	uint32_t allocation = 0;
	bool hasPreroll = false;
	uint32_t preroll = 0;
	bool hasPauseGrace = false;
	uint32_t pauseGrace = 0;
	bool hasFrames = false;
	uint32_t frames = 0;
	bool hasLdacQuality = false;
	uint32_t ldacQuality = 0;

	bool ParameterChange() const
	{
		return hasBitrate || hasBitpool || hasBlocks || hasSubbands || hasAllocation || hasPreroll ||
			   hasFrames || hasLdacQuality || hasPauseGrace;
	}

};

int CommandCodec(const std::vector<Device>& devices, const std::string& spec, const std::string& codecArg,
				 const PreferenceOverrides& overrides)
{
	const bool isAptxHd = codecArg == "aptxhd" || codecArg == "aptx-hd";
	const bool isAptx = codecArg == "aptx";
	const bool isLdac = codecArg == "ldac";
	if (!codecArg.empty() && codecArg != "auto" && codecArg != "sbc" && codecArg != "aac" && !isAptx &&
		!isAptxHd && !isLdac)
	{
		std::fprintf(stderr, "unknown codec '%s'; use auto, sbc, aac, aptx, aptxhd or ldac\n",
			codecArg.c_str());
		return 2;
	}

	size_t index = 0;
	std::string error;
	if (!FindDevice(devices, spec, index, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 2;
	}
	const Device& device = devices[index];
	const HANDLE handle = OpenControl(device, error);
	if (handle == INVALID_HANDLE_VALUE)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	// What the sink reported in the last discovery decides which of the requested values
	// can work at all. A codec or format it does not advertise is refused here, with what
	// it does advertise, rather than stored as a preference the driver could never select.
	Ioctl::Status sink = {};
	if (!ReadStatus(handle, sink, error))
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if (codecArg == "aac" && sink.sinkCodecs != 0 && (sink.sinkCodecs & Ioctl::kSinkCodecAac) == 0)
	{
		CloseHandle(handle);
		std::fprintf(stderr,
					 "the sink does not advertise AAC (it offers %s); use `codec auto` or `codec sbc`. "
					 "No preference was stored.\n",
				CodecListText(sink.sinkCodecs).c_str());
		return 1;
	}
	if (isAptx && sink.sinkCodecs != 0 && (sink.sinkCodecs & Ioctl::kSinkCodecAptx) == 0)
	{
		CloseHandle(handle);
		std::fprintf(stderr,
					 "the sink does not advertise aptX (it offers %s); use `codec auto` or `codec sbc`. "
					 "No preference was stored.\n",
				CodecListText(sink.sinkCodecs).c_str());
		return 1;
	}
	if (isAptxHd && sink.sinkCodecs != 0 && (sink.sinkCodecs & Ioctl::kSinkCodecAptxHd) == 0)
	{
		CloseHandle(handle);
		std::fprintf(stderr,
					 "the sink does not advertise aptX HD (it offers %s); use `codec auto` or `codec sbc`. "
					 "No preference was stored.\n",
				CodecListText(sink.sinkCodecs).c_str());
		return 1;
	}
	if (codecArg == "aptxll" && sink.sinkCodecs != 0 && (sink.sinkCodecs & Ioctl::kSinkCodecAptxLl) == 0)
	{
		CloseHandle(handle);
		std::fprintf(stderr,
					 "the sink does not advertise aptX LL (it offers %s); use `codec auto` or `codec sbc`. "
					 "No preference was stored.\n",
				CodecListText(sink.sinkCodecs).c_str());
		return 1;
	}
	if (isLdac && sink.sinkCodecs != 0 && (sink.sinkCodecs & Ioctl::kSinkCodecLdac) == 0)
	{
		CloseHandle(handle);
		std::fprintf(stderr,
					 "the sink does not advertise LDAC (it offers %s); use `codec auto` or `codec sbc`. "
					 "No preference was stored.\n",
				CodecListText(sink.sinkCodecs).c_str());
		return 1;
	}

	// Start from the stored preference so a codec change keeps the parameters that
	// the command does not name.
	Ioctl::Preference preference = {};
	if (!ReadPreference(handle, preference, error))
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	preference.abiVersion = Ioctl::kAbiVersion;
	// No codec name keeps the stored selection.
	if (codecArg == "auto")
	{
		preference.flags = 0;
		preference.codec = 0;
	}
	else if (!codecArg.empty())
	{
		preference.flags = Ioctl::kPreferenceForceCodec;
		preference.codec = isLdac ? Ioctl::kCodecLdac
			: isAptxHd ? Ioctl::kCodecAptxHd
			: isAptx ? Ioctl::kCodecAptx
			: codecArg == "aptxll" ? Ioctl::kCodecAptxLl
			: codecArg == "aac"
			? Ioctl::kCodecAac
			: Ioctl::kCodecSbc;
	}
	if (overrides.hasBitrate)
	{
		preference.aacBitrate = overrides.bitrate;
	}
	if (overrides.hasBitpool)
	{
		preference.sbcBitpool = overrides.bitpool;
	}
	if (overrides.hasBlocks)
	{
		preference.sbcBlocks = overrides.blocks;
	}
	if (overrides.hasSubbands)
	{
		preference.sbcSubbands = overrides.subbands;
	}
	if (overrides.hasAllocation)
	{
		preference.sbcAllocation = overrides.allocation;
	}
	if (overrides.hasLdacQuality)
	{
		preference.ldacQuality = overrides.ldacQuality;
	}
	if (overrides.hasPreroll)
	{
		preference.prerollMs = overrides.preroll;
	}
	if (overrides.hasPauseGrace)
	{
		preference.pauseGraceMs = overrides.pauseGrace;
	}
	if (overrides.hasFrames)
	{
		preference.sbcFramesPerPacket = overrides.frames;
	}

	const bool set = WritePreference(handle, preference, error);
	Ioctl::Status status = {};
	const bool read = set && ReadStatus(handle, status, error);
	if (!set || !read)
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	const bool force = (preference.flags & Ioctl::kPreferenceForceCodec) != 0;
	std::printf("Preference set to %s on %s.\n",
			!force ? "auto (LDAC > aptX HD > aptX > AAC > SBC)"
				   : preference.codec == Ioctl::kCodecLdac   ? "LDAC"
				   : preference.codec == Ioctl::kCodecAptxHd ? "aptX HD"
				   : preference.codec == Ioctl::kCodecAptx ? "aptX"
				   : preference.codec == Ioctl::kCodecAptxLl ? "aptX LL"
				   : preference.codec == Ioctl::kCodecAac     ? "AAC"
																 : "SBC",
			DeviceLabel(device).c_str());
	PrintStatus(device, handle, status, preference, false);

	const bool aacRegistered = (status.encoderRegistered & Ioctl::EncoderBit(Ioctl::kCodecAac)) != 0;
	const bool aptxRegistered = (status.encoderRegistered & Ioctl::EncoderBit(Ioctl::kCodecAptx)) != 0;
	const bool hdRegistered = (status.encoderRegistered & Ioctl::EncoderBit(Ioctl::kCodecAptxHd)) != 0;
	const bool ldacRegistered = (status.encoderRegistered & Ioctl::EncoderBit(Ioctl::kCodecLdac)) != 0;
	if (force && preference.codec == Ioctl::kCodecAac && !aacRegistered)
	{
		std::printf("\nnote: no AAC encoder is registered, so AAC is not offered and SBC will be used.\n");
	}
	if (force && preference.codec == Ioctl::kCodecAptx && !aptxRegistered)
	{
		std::printf("\nnote: no aptX encoder is registered, so aptX is not offered.\n");
	}
	if (force && preference.codec == Ioctl::kCodecAptxLl)
	{
		const bool llRegistered = (status.encoderRegistered & Ioctl::EncoderBit(Ioctl::kCodecAptxLl)) != 0;
		if (!llRegistered)
		{
			std::printf("\nnote: no aptX LL encoder is registered, so aptX LL is not offered.\n");
		}
	}
	if (force && preference.codec == Ioctl::kCodecAptxHd && !hdRegistered)
	{
		std::printf("\nnote: no aptX HD encoder is registered, so aptX HD is not offered.\n");
	}
	if (force && preference.codec == Ioctl::kCodecLdac && !ldacRegistered)
	{
		std::printf("\nnote: no LDAC encoder is registered, so LDAC is not offered.\n");
	}
	const bool streaming = status.mediaState == Ioctl::MediaState::Streaming;
	const bool wantsLdac = force ? preference.codec == Ioctl::kCodecLdac : ldacRegistered;
	const bool wantsHd = force ? preference.codec == Ioctl::kCodecAptxHd : hdRegistered && !ldacRegistered;
	const bool wantsAptx =
		force ? preference.codec == Ioctl::kCodecAptx : aptxRegistered && !hdRegistered && !ldacRegistered;
	const bool wantsAptxLl = force && preference.codec == Ioctl::kCodecAptxLl;
	const bool wantsAac = force ? preference.codec == Ioctl::kCodecAac
								: aacRegistered && !hdRegistered && !aptxRegistered && !ldacRegistered;
	const bool wantsSbc = force && preference.codec == Ioctl::kCodecSbc;
	const bool codecChanges = (wantsLdac && status.codec != Ioctl::kCodecLdac) ||
							  (wantsHd && status.codec != Ioctl::kCodecAptxHd) ||
							  (wantsAptx && status.codec != Ioctl::kCodecAptx) ||
							  (wantsAptxLl && status.codec != Ioctl::kCodecAptxLl) ||
							  (wantsAac && status.codec != Ioctl::kCodecAac) ||
							  (wantsSbc && status.codec != Ioctl::kCodecSbc);
	if (streaming && (codecChanges || overrides.ParameterChange()))
	{
		std::printf("\nnote: the active stream renegotiates to apply the preference "
					"(the status above may lag until the sink answers).\n");
	}
	CloseHandle(handle);
	return 0;
}

int CommandFormat(const std::vector<Device>& devices, const std::string& spec, const std::string& codecArg,
				  const std::string& rateArg, const std::string& channelsArg)
{
	size_t index = 0;
	std::string error;
	if (!FindDevice(devices, spec, index, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 2;
	}
	const Device& device = devices[index];
	const HANDLE handle = OpenControl(device, error);
	if (handle == INVALID_HANDLE_VALUE)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	Ioctl::EndpointFormat format = {};
	if (!ReadEndpointFormat(handle, format, error))
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if (codecArg.empty())
	{
		std::printf("%s: endpoint format %s.\n", DeviceLabel(device).c_str(),
					EndpointFormatText(format).c_str());
		CloseHandle(handle);
		return 0;
	}

	const int codec = CodecIndexOf(codecArg);
	if (codec < 0 || rateArg.empty())
	{
		CloseHandle(handle);
		std::fprintf(stderr, "format takes a codec (sbc, aac, aptx, aptxhd, ldac) and auto, or a codec, a rate "
							 "(16000, 32000, 44100, 48000, 88200, 96000) and a channel count (1, 2)\n");
		return 2;
	}
	Ioctl::EndpointFormat requested = format;
	requested.sampleRate[codec] = 0;
	requested.channels[codec] = 0;
	if (rateArg != "auto")
	{
		if (!IsDecimal(rateArg) || !IsDecimal(channelsArg))
		{
			CloseHandle(handle);
			std::fprintf(stderr, "format takes auto, or a rate (16000, 32000, 44100, 48000, 88200, 96000) "
								 "and a channel count (1, 2)\n");
			return 2;
		}
		requested.sampleRate[codec] = static_cast<uint32_t>(std::strtoul(rateArg.c_str(), nullptr, 10));
		requested.channels[codec] = static_cast<uint32_t>(std::strtoul(channelsArg.c_str(), nullptr, 10));
	}
	if (!Ioctl::ValidEndpointFormat(requested))
	{
		CloseHandle(handle);
		std::fprintf(stderr, "format takes auto, or a rate (16000, 32000, 44100, 48000, 88200, 96000) and a "
							 "channel count (1, 2)\n");
		return 2;
	}

	// SBC's advertisement is the one the status reports per codec, so only that entry can be
	// checked here; the driver refuses a rate the sink or the codec cannot carry.
	Ioctl::Status status = {};
	if (!ReadStatus(handle, status, error))
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	const uint32_t rate = requested.sampleRate[codec];
	const uint32_t rateBit = rate == 44100 ? 1u : rate == 48000 ? 2u : rate == 32000 ? 4u : rate == 16000 ? 8u : 0u;
	if (codec == static_cast<int>(Ioctl::kCodecSbc) && rate != 0 && status.sinkRates != 0 && rateBit != 0)
	{
		const uint32_t channelBit = requested.channels[codec] == 1 ? 1u : 2u;
		if ((status.sinkRates & rateBit) == 0 || (status.sinkChannels & channelBit) == 0)
		{
			CloseHandle(handle);
			std::fprintf(stderr,
						 "the sink does not advertise %u Hz %u ch (it offered %u Hz and %u ch sets); nothing "
						 "was stored. Use `format sbc auto`.\n",
						 rate, requested.channels[codec], status.sinkRates, status.sinkChannels);
			return 1;
		}
	}

	const bool set = WriteEndpointFormat(handle, requested, error);
	Ioctl::EndpointFormat applied = {};
	const bool read = set && ReadEndpointFormat(handle, applied, error);
	if (!set || !read)
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	std::printf("%s: endpoint format set to %s.\n", DeviceLabel(device).c_str(),
				EndpointFormatText(applied).c_str());
	if (applied.sampleRate[codec] != 0)
	{
		std::printf("note: while %s runs, this is the only format the endpoint offers, so it is also the\r\n"
					"only entry in the Windows format list. Windows rebuilds the shared-mode stream to\r\n"
					"it; a WASAPI exclusive-mode player is limited to it as well.\r\n",
					CodecName(static_cast<uint32_t>(codec)));
	}
	else
	{
		std::printf("note: %s plays every format it can again; Windows uses the first one the endpoint\r\n"
					"lists.\r\n",
					CodecName(static_cast<uint32_t>(codec)));
	}
	CloseHandle(handle);
	return 0;
}

std::string AddressDigits(const std::string& text)
{
	std::string digits;
	for (const char character : text)
	{
		if (character == ':' || character == '-' || character == ' ')
		{
			continue;
		}
		if (isxdigit(static_cast<unsigned char>(character)) == 0)
		{
			return {};
		}
		digits += static_cast<char>(toupper(static_cast<unsigned char>(character)));
	}
	return digits.size() == 12 ? digits : std::string();
}

// Shows or changes which A2DP source driver the sink's BTHENUM node is bound to. Unlike the
// other commands this one never opens the control device: while the sink is on BthA2dp there is
// no control device to open, which is exactly the state it is here to get out of.
int CommandDriver(const std::vector<Device>& devices, const std::string& spec, const std::string& targetArg)
{
	const std::string choice = Lower(targetArg);
	SinkDriver target = SinkDriver::Unknown;
	if (choice == "opena2dp")
	{
		target = SinkDriver::OpenA2dp;
	}
	else if (choice == "windows" || choice == "btha2dp")
	{
		target = SinkDriver::Windows;
	}
	else if (!choice.empty())
	{
		std::fprintf(stderr, "driver takes opena2dp or windows\n");
		return 2;
	}

	// Which sink: the usual selection, a bare address, or the only sink there is. The address
	// form is what makes a sink on BthA2dp reachable, because it has no control device to pick.
	std::string reference;
	std::string error;
	if (!spec.empty())
	{
		size_t index = 0;
		if (FindDevice(devices, spec, index, error))
		{
			reference = devices[index].reference;
		}
		else
		{
			reference = AddressDigits(spec);
			if (reference.empty())
			{
				std::fprintf(stderr, "%s\n", error.c_str());
				return 2;
			}
		}
	}

	std::vector<SinkNode> nodes;
	if (!EnumerateSinkNodes(nodes, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if (reference.empty())
	{
		if (nodes.size() == 1)
		{
			reference = nodes.front().reference;
		}
		else
		{
			if (nodes.empty())
			{
				std::printf("no A2DP sink is connected: pair and connect one, then try again.\n");
				return 1;
			}
			std::printf("%u A2DP sinks; name one with --device <address>:\n",
						static_cast<unsigned>(nodes.size()));
			for (const SinkNode& node : nodes)
			{
				std::printf("  %s  %s\n", FormatAddress(node.reference).c_str(),
							SinkDriverName(node.driver));
			}
			return 0;
		}
	}

	SinkDriverState state;
	if (!ReadSinkDriver(reference, state, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	if (target == SinkDriver::Unknown)
	{
		std::printf("%s: %s%s\n", FormatAddress(reference).c_str(), SinkDriverName(state.current),
					state.restartPending ? " (installed, waiting for the node to restart)" : "");
		std::printf("  node      %s\n", state.instanceId.c_str());
		std::printf("  switch    %s\n",
					state.switchable ? "`driver opena2dp` or `driver windows` moves this sink"
									 : state.problem.c_str());
		return 0;
	}

	if (state.current == target && !state.restartPending)
	{
		if (!state.running)
		{
			// The right driver on a node that is not running plays nothing; "nothing to change"
			// would hide exactly the silence the user is asking about.
			std::fprintf(stderr,
						 "%s: %s is installed on the sink's A2DP node but the node is not running; "
						 "reconnect the sink or restart its device\n",
						 FormatAddress(reference).c_str(), SinkDriverName(target));
			return 1;
		}
		std::printf("%s: already on %s, nothing to change.\n", FormatAddress(reference).c_str(),
					SinkDriverName(target));
		return 0;
	}

	SwitchReport report;
	const SwitchOutcome outcome = SwitchSinkDriver(reference, target, report, error);
	if (!report.pausedPlayer.empty())
	{
		std::printf("paused %s, which was rendering to the sink\n", report.pausedPlayer.c_str());
	}
	if (outcome != SwitchOutcome::Switched)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return outcome == SwitchOutcome::PlaysAudio ? 3 : 1;
	}
	std::printf("%s: %s installed on the sink's A2DP node.\n", FormatAddress(reference).c_str(),
				SinkDriverName(target));

	// The node restarts as part of the install; reading it back says what Windows settled on.
	SinkDriverState applied;
	if (ReadSinkDriver(reference, applied, error))
	{
		std::printf("  node      %s\n  driver    %s\n", applied.instanceId.c_str(),
					SinkDriverName(applied.current));
	}
	else
	{
		std::printf("  the node is restarting: %s\n", error.c_str());
	}
	return 0;
}
// Starts a new measurement window: the counters status reports are cleared and nothing else is
// touched, so the sink keeps playing and the report printed afterwards is the one that follows
// the reset.
int CommandResetCounters(const std::vector<Device>& devices, const std::string& spec)
{
	size_t index = 0;
	std::string error;
	if (!FindDevice(devices, spec, index, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 2;
	}
	const Device& device = devices[index];
	const HANDLE handle = OpenControl(device, error);
	if (handle == INVALID_HANDLE_VALUE)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	Ioctl::Status status = {};
	Ioctl::Preference preference = {};
	const bool reset = ResetCounters(handle, error);
	const bool read = reset && ReadStatus(handle, status, error) && ReadPreference(handle, preference, error);
	if (!reset || !read)
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	std::printf("%s: counters reset; the stream was not touched.\n\n", DeviceLabel(device).c_str());
	PrintStatus(device, handle, status, preference, false);
	CloseHandle(handle);
	return 0;
}

int CommandReset(const std::vector<Device>& devices, const std::string& spec)
{
	size_t index = 0;
	std::string error;
	if (!FindDevice(devices, spec, index, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 2;
	}
	const Device& device = devices[index];
	const HANDLE handle = OpenControl(device, error);
	if (handle == INVALID_HANDLE_VALUE)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	// Every field at its default: the automatic codec order and an endpoint that offers every
	// format the sink advertises. Each SET is a change for the driver, so a running stream
	// renegotiates once.
	Ioctl::EndpointFormat format = {};
	Ioctl::Preference preference = {};
	const bool formatSet = WriteEndpointFormat(handle, format, error);
	const bool preferenceSet = formatSet && WritePreference(handle, preference, error);
	Ioctl::Status status = {};
	Ioctl::Preference applied = {};
	Ioctl::EndpointFormat appliedFormat = {};
	const bool read = preferenceSet && ReadStatus(handle, status, error) &&
					  ReadPreference(handle, applied, error) && ReadEndpointFormat(handle, appliedFormat, error);
	if (!formatSet || !preferenceSet || !read)
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	const bool forced = (applied.flags & Ioctl::kPreferenceForceCodec) != 0;
	std::printf("%s: codec preference reset to %s, endpoint format reset to %s.\n",
			DeviceLabel(device).c_str(),
			!forced ? "auto (LDAC > aptX HD > aptX > AAC > SBC)"
				   : applied.codec == Ioctl::kCodecAptxHd ? "aptX HD"
				   : applied.codec == Ioctl::kCodecAptx ? "aptX"
				   : applied.codec == Ioctl::kCodecAac     ? "AAC"
																	 : "SBC",
			EndpointFormatText(appliedFormat).c_str());
	if (status.mediaState == Ioctl::MediaState::Streaming)
	{
		std::printf("\nnote: the active stream renegotiates to apply this (the status above may lag "
					"until the sink answers).\n");
	}
	PrintStatus(device, handle, status, applied, false);
	CloseHandle(handle);
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	std::vector<std::string> args(argv + 1, argv + argc);
	std::string command = "status";
	std::string deviceSpec;
	std::string codecArg;
	std::string formatCodecArg;
	std::string formatRateArg;
	std::string formatChannelsArg;
	std::string driverArg;
	PreferenceOverrides overrides;
	bool watch = false;
	bool verbose = false;
	unsigned interval = 1000;
	size_t position = 0;

	if (!args.empty() && !args[0].empty() && args[0][0] != '-')
	{
		command = args[0];
		position = 1;
	}
	if (command == "codec" && position < args.size() && args[position][0] != '-')
	{
		codecArg = args[position];
		++position;
	}
	if (command == "driver" && position < args.size() && args[position][0] != '-')
	{
		driverArg = args[position];
		++position;
	}
	if (command == "format" && position < args.size() && args[position][0] != '-')
	{
		formatCodecArg = args[position];
		++position;
		if (position < args.size() && args[position][0] != '-')
		{
			formatRateArg = args[position];
			++position;
			if (position < args.size() && args[position][0] != '-')
			{
				formatChannelsArg = args[position];
				++position;
			}
		}
	}

	for (; position < args.size(); ++position)
	{
		const std::string& arg = args[position];
		if (arg == "--device" && position + 1 < args.size())
		{
			deviceSpec = args[++position];
		}
		else if (arg == "--bitrate" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			const unsigned long parsed = IsDecimal(value) ? std::strtoul(value.c_str(), nullptr, 10) : 1000001;
			if (!IsDecimal(value) || parsed > 1000000)
			{
				std::fprintf(stderr, "--bitrate takes bits per second, 0 to restore the default\n");
				return 2;
			}
			overrides.hasBitrate = true;
			overrides.bitrate = static_cast<uint32_t>(parsed);
		}
		else if (arg == "--bitpool" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			if (!IsDecimal(value))
			{
				std::fprintf(stderr, "--bitpool takes 0 (default) or %u..%u\n", Ioctl::kSbcMinBitpool,
						Ioctl::kSbcMaxBitpool);
				return 2;
			}
			const unsigned long parsed = std::strtoul(value.c_str(), nullptr, 10);
			if (parsed != 0 && (parsed < Ioctl::kSbcMinBitpool || parsed > Ioctl::kSbcMaxBitpool))
			{
				std::fprintf(stderr, "--bitpool takes 0 (default) or %u..%u\n", Ioctl::kSbcMinBitpool,
						Ioctl::kSbcMaxBitpool);
				return 2;
			}
			overrides.hasBitpool = true;
			overrides.bitpool = static_cast<uint32_t>(parsed);
		}
		else if (arg == "--ldac-quality" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			if (value != "high" && value != "standard" && value != "mobile")
			{
				std::fprintf(stderr, "--ldac-quality takes high, standard or mobile\n");
				return 2;
			}
			overrides.hasLdacQuality = true;
			overrides.ldacQuality =
				value == "high" ? 0u : value == "standard" ? 1u : 2u;
		}
		else if (arg == "--preroll" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			if (!IsDecimal(value))
			{
				std::fprintf(stderr, "--preroll takes 0 (default) or %u..%u ms\n", Ioctl::kMinPrerollMs,
						Ioctl::kMaxPrerollMs);
				return 2;
			}
			const unsigned long parsed = std::strtoul(value.c_str(), nullptr, 10);
			if (parsed != 0 && (parsed < Ioctl::kMinPrerollMs || parsed > Ioctl::kMaxPrerollMs))
			{
				std::fprintf(stderr, "--preroll takes 0 (default) or %u..%u ms\n", Ioctl::kMinPrerollMs,
						Ioctl::kMaxPrerollMs);
				return 2;
			}
			overrides.hasPreroll = true;
			overrides.preroll = static_cast<uint32_t>(parsed);
		}
		else if (arg == "--pause-grace" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			if (!IsDecimal(value))
			{
				std::fprintf(stderr, "--pause-grace takes 0 (default) or %u..%u ms\n",
						Ioctl::kMinPauseGraceMs, Ioctl::kMaxPauseGraceMs);
				return 2;
			}
			const unsigned long parsed = std::strtoul(value.c_str(), nullptr, 10);
			if (parsed != 0 && (parsed < Ioctl::kMinPauseGraceMs || parsed > Ioctl::kMaxPauseGraceMs))
			{
				std::fprintf(stderr, "--pause-grace takes 0 (default) or %u..%u ms\n",
						Ioctl::kMinPauseGraceMs, Ioctl::kMaxPauseGraceMs);
				return 2;
			}
			overrides.hasPauseGrace = true;
			overrides.pauseGrace = static_cast<uint32_t>(parsed);
		}
		else if (arg == "--frames" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			if (!IsDecimal(value))
			{
				std::fprintf(stderr, "--frames takes 0 (fill the packet) or %u..%u\n",
						Ioctl::kSbcMinFramesPerPacket, Ioctl::kSbcMaxFramesPerPacket);
				return 2;
			}
			const unsigned long parsed = std::strtoul(value.c_str(), nullptr, 10);
			if (parsed != 0 &&
				(parsed < Ioctl::kSbcMinFramesPerPacket || parsed > Ioctl::kSbcMaxFramesPerPacket))
			{
				std::fprintf(stderr, "--frames takes 0 (fill the packet) or %u..%u\n",
						Ioctl::kSbcMinFramesPerPacket, Ioctl::kSbcMaxFramesPerPacket);
				return 2;
			}
			overrides.hasFrames = true;
			overrides.frames = static_cast<uint32_t>(parsed);
		}
		else if (arg == "--blocks" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			const unsigned long parsed = IsDecimal(value) ? std::strtoul(value.c_str(), nullptr, 10) : 1;
			if (!IsDecimal(value) ||
				(parsed != 0 && parsed != 4 && parsed != 8 && parsed != 12 && parsed != 16))
			{
				std::fprintf(stderr, "--blocks takes 0 (default) or 4, 8, 12 or 16\n");
				return 2;
			}
			overrides.hasBlocks = true;
			overrides.blocks = static_cast<uint32_t>(parsed);
		}
		else if (arg == "--subbands" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			const unsigned long parsed = IsDecimal(value) ? std::strtoul(value.c_str(), nullptr, 10) : 1;
			if (!IsDecimal(value) || (parsed != 0 && parsed != 4 && parsed != 8))
			{
				std::fprintf(stderr, "--subbands takes 0 (default), 4 or 8\n");
				return 2;
			}
			overrides.hasSubbands = true;
			overrides.subbands = static_cast<uint32_t>(parsed);
		}
		else if (arg == "--allocation" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			uint32_t allocation = 0;
			if (value == "loudness")
			{
				allocation = Ioctl::kSbcAllocationLoudness;
			}
			else if (value == "snr")
			{
				allocation = Ioctl::kSbcAllocationSnr;
			}
			else if (value != "default")
			{
				std::fprintf(stderr, "--allocation takes default, loudness or snr\n");
				return 2;
			}
			overrides.hasAllocation = true;
			overrides.allocation = allocation;
		}
		else if (arg == "--rate" || arg == "--channels")
		{
			std::fprintf(stderr, "%s was removed; use `format <rate> <channels>` to pin the endpoint "
				"format instead.\n", arg.c_str());
			return 2;
		}
		else if (arg == "--watch")
		{
			watch = true;
		}
		else if (arg == "--verbose")
		{
			verbose = true;
		}
		else if (arg == "--interval" && position + 1 < args.size())
		{
			const unsigned long value = std::strtoul(args[++position].c_str(), nullptr, 10);
			interval = value != 0 ? static_cast<unsigned>(value) : 1000;
		}
		else if (arg == "--help" || arg == "-h")
		{
			command = "help";
		}
		else
		{
			std::fprintf(stderr, "unknown argument '%s'\n\n%s", arg.c_str(), kUsage);
			return 2;
		}
	}

	if (command == "help")
	{
		std::printf("%s", kUsage);
		return 0;
	}
	if (command != "list" && command != "status" && command != "codec" && command != "version" &&
		command != "format" && command != "driver" && command != "reset" &&
		command != "reset-counters")
	{
		std::fprintf(stderr, "unknown command '%s'\n\n%s", command.c_str(), kUsage);
		return 2;
	}
	if (command == "codec" && codecArg.empty() && !overrides.ParameterChange())
	{
		std::fprintf(stderr, "codec needs a codec (auto, sbc or aac) or at least one parameter flag\n\n%s", kUsage);
		return 2;
	}
	if (command != "codec" && overrides.ParameterChange())
	{
		std::fprintf(stderr, "the parameter flags apply to `codec`\n\n%s", kUsage);
		return 2;
	}

	std::string error;
	const std::vector<Device> devices = EnumerateDevices(error);
	if (!error.empty())
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	if (command == "list")
	{
		return CommandList(devices);
	}
	if (command == "version")
	{
		return CommandVersion(devices, deviceSpec);
	}
	if (command == "codec")
	{
		return CommandCodec(devices, deviceSpec, codecArg, overrides);
	}
	if (command == "format")
	{
		return CommandFormat(devices, deviceSpec, formatCodecArg, formatRateArg, formatChannelsArg);
	}
	if (command == "driver")
	{
		return CommandDriver(devices, deviceSpec, driverArg);
	}
	if (command == "reset")
	{
		return CommandReset(devices, deviceSpec);
	}
	if (command == "reset-counters")
	{
		return CommandResetCounters(devices, deviceSpec);
	}
	return CommandStatus(devices, deviceSpec, watch, interval, verbose);
}
