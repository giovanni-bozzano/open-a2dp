// OpenA2dpCtl.exe - diagnostic CLI for the OpenA2dp A2DP source driver.
//
// Enumerates the per-device control interfaces (one per sink) and uses the
// IOCTLs in common/include/OpenA2dp/Ioctl.h to report the negotiated codec and
// to set the codec preference. The tool is the diagnostic and script path; the
// end-user path is the settings window (tools/OpenA2dpConfig), which shows the
// same report through tools/common/StatusReport.

#include <OpenA2dp/Ioctl.h>

#include "common/ControlDevice.h"
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
  OpenA2dpCtl codec [<auto|sbc|aac|aptx|aptxhd>] [--bitrate <bps>] [--bitpool <n>]
                     [--blocks <4|8|12|16>] [--subbands <4|8>]
                     [--allocation <loudness|snr>] [--device <mac|index>]
  OpenA2dpCtl format [<auto|44100|48000> [<1|2>]] [--device <mac|index>]
  OpenA2dpCtl reset [--device <mac|index>]
  OpenA2dpCtl version [--device <mac|index>]
  OpenA2dpCtl help

Commands:
  status    Show the AVDTP/media state, codec, preference and counters (default).
  list      List the present OpenA2dp devices.
  codec     Set the codec preference, stored for this sink and restored
            when the device starts. If a stream is active it is renegotiated
            immediately; otherwise the preference applies at the next AVDTP
            negotiation. "auto" prefers aptX HD, then aptX, then AAC while an
            encoder service is registered, then SBC. --bitrate and --bitpool cap
            the AAC bitrate and the SBC bitpool; --blocks, --subbands and
            --allocation pin the SBC frame shape. 0 restores the driver default
            (--allocation default), and the sink's advertised limits still apply.
            Without a codec name the stored selection is kept and only the
            flags given are applied.
            Codec settings never narrow the supported endpoint format list.
  format    Show or set the PCM format this sink's endpoint offers Windows.
            "auto" (the default) offers every format the sink supports. A rate
            and a channel count pin the endpoint to that one format, which is
            then the only entry in the Windows format list. Windows plays the
            first format the endpoint lists and ignores the Sound-settings pick,
            so pinning is the only way to force one; the sink has to advertise
            it. Pinning also limits WASAPI exclusive-mode players to it.
  reset     Clear this sink's stored codec preference and endpoint format, so
            the automatic codec order and every sink-supported PCM format
            apply again.
  version   Show the driver ABI and version.

Options:
  --device <spec>   Select a device by Bluetooth address (AA:BB:CC:DD:EE:FF),
                    1-based list index, or a partial address.
  --bitrate <bps>   AAC bitrate cap for `codec` (0 = driver default).
  --bitpool <n>     SBC bitpool cap for `codec`, 0 or 2..250 (0 = driver
                    default). The sink's minimum and maximum still win.
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
	if (format.sampleRate == 0)
	{
		return "automatic (every format the sink supports)";
	}
	return std::to_string(format.sampleRate) + " Hz " + std::to_string(format.channels) + " ch";
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

	bool ParameterChange() const
	{
		return hasBitrate || hasBitpool || hasBlocks || hasSubbands || hasAllocation;
	}

};

int CommandCodec(const std::vector<Device>& devices, const std::string& spec, const std::string& codecArg,
				 const PreferenceOverrides& overrides)
{
	const bool isAptxHd = codecArg == "aptxhd" || codecArg == "aptx-hd";
	const bool isAptx = codecArg == "aptx";
	if (!codecArg.empty() && codecArg != "auto" && codecArg != "sbc" && codecArg != "aac" && !isAptx &&
		!isAptxHd)
	{
		std::fprintf(stderr, "unknown codec '%s'; use auto, sbc, aac, aptx or aptxhd\n", codecArg.c_str());
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
					 "the sink does not advertise aptX (it offers %s); use `codec auto`. "
					 "No preference was stored.\n",
				CodecListText(sink.sinkCodecs).c_str());
		return 1;
	}
	if (isAptxHd && sink.sinkCodecs != 0 && (sink.sinkCodecs & Ioctl::kSinkCodecAptxHd) == 0)
	{
		CloseHandle(handle);
		std::fprintf(stderr,
					 "the sink does not advertise aptX HD (it offers %s); use `codec auto`. "
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
		preference.codec = isAptxHd ? Ioctl::kCodecAptxHd
			: isAptx ? Ioctl::kCodecAptx
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
			!force ? "auto (aptX HD > aptX > AAC > SBC)"
				   : preference.codec == Ioctl::kCodecAptxHd ? "aptX HD"
				   : preference.codec == Ioctl::kCodecAptx ? "aptX"
				   : preference.codec == Ioctl::kCodecAac     ? "AAC"
																 : "SBC",
			DeviceLabel(device).c_str());
	PrintStatus(device, handle, status, preference, false);

	const bool aacRegistered = (status.encoderRegistered & Ioctl::EncoderBit(Ioctl::kCodecAac)) != 0;
	const bool aptxRegistered = (status.encoderRegistered & Ioctl::EncoderBit(Ioctl::kCodecAptx)) != 0;
	const bool hdRegistered = (status.encoderRegistered & Ioctl::EncoderBit(Ioctl::kCodecAptxHd)) != 0;
	if (force && preference.codec == Ioctl::kCodecAac && !aacRegistered)
	{
		std::printf("\nnote: no AAC encoder is registered, so AAC is not offered and SBC will be used.\n");
	}
	if (force && preference.codec == Ioctl::kCodecAptx && !aptxRegistered)
	{
		std::printf("\nnote: no aptX encoder is registered, so aptX is not offered.\n");
	}
	if (force && preference.codec == Ioctl::kCodecAptxHd && !hdRegistered)
	{
		std::printf("\nnote: no aptX HD encoder is registered, so aptX HD is not offered.\n");
	}
	const bool streaming = status.mediaState == Ioctl::MediaState::Streaming;
	const bool wantsHd = force ? preference.codec == Ioctl::kCodecAptxHd : hdRegistered;
	const bool wantsAptx = force ? preference.codec == Ioctl::kCodecAptx : aptxRegistered && !hdRegistered;
	const bool wantsAac =
		force ? preference.codec == Ioctl::kCodecAac : aacRegistered && !hdRegistered && !aptxRegistered;
	const bool wantsSbc = force && preference.codec == Ioctl::kCodecSbc;
	const bool codecChanges = (wantsHd && status.codec != Ioctl::kCodecAptxHd) ||
							  (wantsAptx && status.codec != Ioctl::kCodecAptx) ||
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

int CommandFormat(const std::vector<Device>& devices, const std::string& spec, const std::string& rateArg,
				  const std::string& channelsArg)
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
	if (rateArg.empty())
	{
		std::printf("%s: endpoint format %s.\n", DeviceLabel(device).c_str(),
					EndpointFormatText(format).c_str());
		CloseHandle(handle);
		return 0;
	}

	Ioctl::EndpointFormat requested = {};
	requested.abiVersion = Ioctl::kAbiVersion;
	if (rateArg != "auto")
	{
		if (!IsDecimal(rateArg) || !IsDecimal(channelsArg))
		{
			CloseHandle(handle);
			std::fprintf(stderr, "format takes auto, or a rate (44100, 48000) and a channel count (1, 2)\n");
			return 2;
		}
		requested.sampleRate = static_cast<uint32_t>(std::strtoul(rateArg.c_str(), nullptr, 10));
		requested.channels = static_cast<uint32_t>(std::strtoul(channelsArg.c_str(), nullptr, 10));
	}
	if (!Ioctl::ValidEndpointFormat(requested))
	{
		CloseHandle(handle);
		std::fprintf(stderr, "format takes auto, or a rate (44100, 48000) and a channel count (1, 2)\n");
		return 2;
	}

	// The endpoint only offers what the sink advertised in the last discovery, so a format it
	// cannot carry is named here rather than left as a refused SET.
	Ioctl::Status status = {};
	if (!ReadStatus(handle, status, error))
	{
		CloseHandle(handle);
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if (requested.sampleRate != 0 && status.sinkRates != 0)
	{
		const uint32_t rateBit = requested.sampleRate == 44100 ? 1u : 2u;
		const uint32_t channelBit = requested.channels == 1 ? 1u : 2u;
		if ((status.sinkRates & rateBit) == 0 || (status.sinkChannels & channelBit) == 0)
		{
			CloseHandle(handle);
			std::fprintf(stderr,
						 "the sink does not advertise %u Hz %u ch (it offered %u Hz and %u ch sets); "
						 "nothing was stored. Use `format auto`.\n",
						 requested.sampleRate, requested.channels, status.sinkRates, status.sinkChannels);
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
	if (applied.sampleRate != 0)
	{
		std::printf("note: this is now the only format the endpoint offers, so it is also the only\r\n"
					"entry in the Windows format list. Windows rebuilds the shared-mode stream to it;\r\n"
					"a WASAPI exclusive-mode player is limited to it as well.\r\n");
	}
	else
	{
		std::printf("note: Windows plays the first format the endpoint lists for the shared-mode\r\n"
					"stream; the Sound-settings pick does not reach the driver.\r\n");
	}
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
			!forced ? "auto (aptX HD > aptX > AAC > SBC)"
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
	std::string formatRateArg;
	std::string formatChannelsArg;
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
	if (command == "format" && position < args.size() && args[position][0] != '-')
	{
		formatRateArg = args[position];
		++position;
		if (position < args.size() && args[position][0] != '-')
		{
			formatChannelsArg = args[position];
			++position;
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
		command != "format" && command != "reset")
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
		return CommandFormat(devices, deviceSpec, formatRateArg, formatChannelsArg);
	}
	if (command == "reset")
	{
		return CommandReset(devices, deviceSpec);
	}
	return CommandStatus(devices, deviceSpec, watch, interval, verbose);
}
