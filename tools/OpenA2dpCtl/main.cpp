// OpenA2dpCtl.exe - diagnostic CLI for the OpenA2dp A2DP source driver.
//
// Enumerates the per-device control interfaces (one per sink) and uses the
// IOCTLs in common/include/OpenA2dp/Ioctl.h to report the negotiated codec and
// to set the codec preference. The tool is the diagnostic and script path; the
// end-user path is the settings window (tools/OpenA2dpConfig), which shows the
// same report through tools/common/StatusReport.

#include <OpenA2dp/Ioctl.h>

#include "common/StatusReport.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
  OpenA2dpCtl codec [<auto|sbc|aac>] [--bitrate <bps>] [--bitpool <n>]
                     [--blocks <4|8|12|16>] [--subbands <4|8>]
                     [--allocation <loudness|snr>] [--rate <44100|48000>]
                     [--channels <1|2>] [--device <mac|index>]
  OpenA2dpCtl version [--device <mac|index>]
  OpenA2dpCtl help

Commands:
  status    Show the AVDTP/media state, codec, preference and counters (default).
  list      List the present OpenA2dp devices.
  codec     Set the codec preference, stored for this sink and restored
            when the device starts. If a stream is active it is renegotiated
            immediately; otherwise the preference applies at the next AVDTP
            negotiation. "auto" prefers AAC while an encoder service is
            registered, then SBC. --bitrate and --bitpool cap the AAC bitrate
            and the SBC bitpool; --blocks, --subbands and --allocation pin the
            SBC frame shape. 0 restores the driver default (--allocation
            default), and the sink's advertised limits still apply.
            Without a codec name the stored selection is kept and only the
            flags given are applied.
            --rate and --channels pin the endpoint format Windows opens,
            which is the rate the stream runs at. The endpoint's format
            list is rebuilt in place, so the change applies without a
            reconnect and an active stream renegotiates. 0 lets Windows
            choose among every served format.
  version   Show the driver ABI and version.

Options:
  --device <spec>   Select a device by Bluetooth address (AA:BB:CC:DD:EE:FF),
                    1-based list index, or a partial address.
  --bitrate <bps>   AAC bitrate cap for `codec` (0 = driver default).
  --bitpool <n>     SBC bitpool cap for `codec`, 0 or 2..250 (0 = driver
                    default). The sink's minimum and maximum still win.
  --rate <hz>       Endpoint sample rate for `codec`, 0, 44100 or 48000 Hz
                    (0 = every served rate).
  --channels <n>    Endpoint channel count for `codec`, 0, 1 or 2 (0 = both).
  --watch           Refresh status until interrupted.
  --interval <ms>   Refresh period for --watch (default 1000).
  --verbose         Append every Status and Preference field with its decimal
                    and hex value. The settings window shows the same report,
                    and its Copy button puts this text on the clipboard.

The tool must run elevated: the control device only allows SYSTEM and
Administrators.
)";

struct Device
{
	std::wstring path;
	std::string reference; // 12 hex digits (the driver's symbolic link suffix)
};

std::string Narrow(const wchar_t* text)
{
	if (text == nullptr)
	{
		return {};
	}
	const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (length <= 1)
	{
		return {};
	}
	std::vector<char> buffer(static_cast<size_t>(length));
	WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer.data(), length, nullptr, nullptr);
	return std::string(buffer.data(), static_cast<size_t>(length - 1));
}

std::string Win32Error(const char* what, DWORD code)
{
	char* text = nullptr;
	const DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
											FORMAT_MESSAGE_IGNORE_INSERTS,
										nullptr, code, 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
	std::string message;
	if (length != 0 && text != nullptr)
	{
		message.assign(text, length);
		while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
		{
			message.pop_back();
		}
	}
	LocalFree(text);

	char suffix[32];
	std::snprintf(suffix, sizeof(suffix), " (0x%08lX)", static_cast<unsigned long>(code));
	return std::string(what) + ": " + (message.empty() ? "error" : message) + suffix;
}

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

bool IsHex12(const std::string& text)
{
	if (text.size() != 12)
	{
		return false;
	}
	for (const char c : text)
	{
		const bool digit = c >= '0' && c <= '9';
		const bool lower = c >= 'a' && c <= 'f';
		const bool upper = c >= 'A' && c <= 'F';
		if (!digit && !lower && !upper)
		{
			return false;
		}
	}
	return true;
}

char LowerAscii(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lower(const std::string& text)
{
	std::string result = text;
	for (char& c : result)
	{
		c = LowerAscii(c);
	}
	return result;
}

std::string Upper(const std::string& text)
{
	std::string result = text;
	for (char& c : result)
	{
		c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
	}
	return result;
}

// The driver creates \DosDevices\OpenA2dpCtl-<address> for every sink, so the
// MS-DOS device list is enough to find all control devices.
std::vector<Device> EnumerateDevices(std::string& error)
{
	std::vector<Device> devices;
	const std::wstring prefix = Ioctl::kControlLinkPrefix;
	DWORD size = 64 * 1024;
	std::vector<wchar_t> buffer;
	for (int attempt = 0; attempt < 4; ++attempt)
	{
		buffer.assign(size, L'\0');
		if (QueryDosDeviceW(nullptr, buffer.data(), size) == 0)
		{
			const DWORD code = GetLastError();
			if (code != ERROR_INSUFFICIENT_BUFFER)
			{
				error = Win32Error("QueryDosDevice", code);
				return devices;
			}
			size *= 2;
			continue;
		}

		for (const wchar_t* name = buffer.data(); *name != L'\0'; name += std::wcslen(name) + 1)
		{
			const std::wstring link(name);
			if (link.size() != prefix.size() + Ioctl::kControlAddressChars ||
				link.compare(0, prefix.size(), prefix) != 0)
			{
				continue;
			}
			const std::string reference = Upper(Narrow(link.c_str() + prefix.size()));
			if (!IsHex12(reference))
			{
				continue;
			}
			Device device;
			device.reference = reference;
			device.path = L"\\\\.\\" + link;
			devices.push_back(std::move(device));
		}

		std::sort(devices.begin(), devices.end(),
				  [](const Device& a, const Device& b) { return a.reference < b.reference; });
		return devices;
	}
	error = "the MS-DOS device name list is too large";
	return devices;
}

std::string FormatAddress(const std::string& reference)
{
	if (!IsHex12(reference))
	{
		return reference.empty() ? "unknown" : reference;
	}
	std::string result;
	for (size_t i = 0; i < reference.size(); i += 2)
	{
		if (i != 0)
		{
			result.push_back(':');
		}
		result.append(reference, i, 2);
	}
	return result;
}

std::string DeviceLabel(const Device& device)
{
	return "Device " + FormatAddress(device.reference);
}

HANDLE OpenDevice(const Device& device, std::string& error)
{
	const HANDLE handle = CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
									  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		const DWORD code = GetLastError();
		error = Win32Error("CreateFile", code);
		if (code == ERROR_ACCESS_DENIED)
		{
			error += " (run the tool as Administrator)";
		}
	}
	return handle;
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

bool IoctlSync(HANDLE handle, ULONG code, const void* in, DWORD inLength, void* out, DWORD outLength,
			   DWORD& bytes, std::string& error, DWORD timeoutMs = 5000)
{
	bytes = 0;
	OVERLAPPED overlapped = {};
	overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (overlapped.hEvent == nullptr)
	{
		error = Win32Error("CreateEvent", GetLastError());
		return false;
	}
	BOOL ok = DeviceIoControl(handle, code, const_cast<void*>(in), inLength, out, outLength, &bytes, &overlapped);
	if (!ok)
	{
		const DWORD gle = GetLastError();
		if (gle != ERROR_IO_PENDING)
		{
			error = Win32Error("DeviceIoControl", gle);
			CloseHandle(overlapped.hEvent);
			return false;
		}
		const DWORD wait = WaitForSingleObject(overlapped.hEvent, timeoutMs);
		if (wait == WAIT_TIMEOUT)
		{
			CancelIoEx(handle, &overlapped);
			// Give the cancel a moment to land so the driver is not left writing into this frame.
			WaitForSingleObject(overlapped.hEvent, 1000);
			GetOverlappedResult(handle, &overlapped, &bytes, FALSE);
			error = "DeviceIoControl timed out after " + std::to_string(timeoutMs) + " ms";
			CloseHandle(overlapped.hEvent);
			return false;
		}
		if (!GetOverlappedResult(handle, &overlapped, &bytes, FALSE))
		{
			error = Win32Error("DeviceIoControl", GetLastError());
			CloseHandle(overlapped.hEvent);
			return false;
		}
	}
	CloseHandle(overlapped.hEvent);
	if (bytes < outLength && out != nullptr && outLength != 0)
	{
		// Some IOCTLs legitimately return 0 bytes; only enforce for GET_STATUS/VERSION
		// where a short buffer is always wrong. Callers that expect 0 pass outLength 0.
	}
	return true;
}

template <typename T>
bool ReadIoctl(HANDLE handle, ULONG code, T& out, std::string& error)
{
	DWORD bytes = 0;
	if (!IoctlSync(handle, code, nullptr, 0, &out, static_cast<DWORD>(sizeof(out)), bytes, error))
	{
		return false;
	}
	if (bytes < sizeof(out))
	{
		error = "DeviceIoControl returned a short buffer";
		return false;
	}
	return true;
}

bool ReadStatus(HANDLE handle, Ioctl::Status& status, std::string& error)
{
	if (!ReadIoctl(handle, Ioctl::kIoctlGetStatus, status, error))
	{
		return false;
	}
	if (status.abiVersion != Ioctl::kAbiVersion)
	{
		error = "driver ABI " + std::to_string(status.abiVersion) + " does not match the tool ABI " +
				std::to_string(Ioctl::kAbiVersion) + "; update both from the same build";
		return false;
	}
	return true;
}

bool ReadPreference(HANDLE handle, Ioctl::Preference& preference, std::string& error)
{
	if (!ReadIoctl(handle, Ioctl::kIoctlGetPreference, preference, error))
	{
		return false;
	}
	if (preference.abiVersion != Ioctl::kAbiVersion)
	{
		error = "driver ABI " + std::to_string(preference.abiVersion) + " does not match the tool ABI " +
				std::to_string(Ioctl::kAbiVersion) + "; update both from the same build";
		return false;
	}
	return true;
}

uint32_t RateBit(uint32_t rate)
{
	return rate == 44100 ? 1u : (rate == 48000 ? 2u : 0u);
}

uint32_t ChannelBit(uint32_t channels)
{
	return channels == 1 ? 1u : (channels == 2 ? 2u : 0u);
}

// Prints the report the settings window shows, so a paste from either one is comparable.
void PrintStatus(const Device& device, HANDLE handle, const Ioctl::Status& status,
				 const Ioctl::Preference& preference, bool verbose)
{
	// The version block is informational: a failed read never hides the status.
	Ioctl::VersionInfo version = {};
	std::string ignored;
	const bool hasVersion = ReadIoctl(handle, Ioctl::kIoctlGetVersion, version, ignored);

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
	const HANDLE handle = OpenDevice(device, error);
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

	std::printf("%-3s %-17s %-18s %s\n", "#", "address", "AVDTP", "codec");
	for (size_t i = 0; i < devices.size(); ++i)
	{
		std::string state = "unavailable";
		std::string codec = "-";
		std::string error;
		const HANDLE handle = OpenDevice(devices[i], error);
		if (handle != INVALID_HANDLE_VALUE)
		{
			Ioctl::Status status = {};
			if (ReadStatus(handle, status, error))
			{
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
		std::printf("%-3u %-17s %-18s %s\n", static_cast<unsigned>(i + 1),
					FormatAddress(devices[i].reference).c_str(), state.c_str(), codec.c_str());
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
	const HANDLE handle = OpenDevice(device, error);
	if (handle == INVALID_HANDLE_VALUE)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	Ioctl::VersionInfo version = {};
	const bool ok = ReadIoctl(handle, Ioctl::kIoctlGetVersion, version, error);
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
	bool hasRate = false;
	uint32_t rate = 0;
	bool hasChannels = false;
	uint32_t channels = 0;

	bool ParameterChange() const
	{
		return hasBitrate || hasBitpool || hasBlocks || hasSubbands || hasAllocation;
	}

	// The endpoint formats are rebuilt in place, so an endpoint change renegotiates an
	// active stream like a parameter change does.
	bool EndpointChange() const
	{
		return hasRate || hasChannels;
	}
};

int CommandCodec(const std::vector<Device>& devices, const std::string& spec, const std::string& codecArg,
				 const PreferenceOverrides& overrides)
{
	if (!codecArg.empty() && codecArg != "auto" && codecArg != "sbc" && codecArg != "aac")
	{
		std::fprintf(stderr, "unknown codec '%s'; use auto, sbc or aac\n", codecArg.c_str());
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
	const HANDLE handle = OpenDevice(device, error);
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
	if (overrides.hasRate && overrides.rate != 0 && sink.sinkRates != 0 &&
		(sink.sinkRates & RateBit(overrides.rate)) == 0)
	{
		const std::string advertised = FormatListText(sink.sinkRates, sink.sinkChannels);
		CloseHandle(handle);
		std::fprintf(stderr, "the sink does not advertise %u Hz (it offers %s). No preference was stored.\n",
					 overrides.rate, advertised.c_str());
		return 1;
	}
	if (overrides.hasChannels && overrides.channels != 0 && sink.sinkChannels != 0 &&
		(sink.sinkChannels & ChannelBit(overrides.channels)) == 0)
	{
		const std::string advertised = FormatListText(sink.sinkRates, sink.sinkChannels);
		CloseHandle(handle);
		std::fprintf(stderr,
					 "the sink does not advertise %s (it offers %s). No preference was stored.\n",
					 overrides.channels == 1 ? "mono" : "stereo", advertised.c_str());
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
	// No codec name keeps the stored selection, so `codec --rate 48000` pins the endpoint
	// format without changing the codec that was chosen before.
	if (codecArg == "auto")
	{
		preference.flags = 0;
		preference.codec = 0;
	}
	else if (!codecArg.empty())
	{
		preference.flags = Ioctl::kPreferenceForceCodec;
		preference.codec = codecArg == "aac" ? Ioctl::kCodecAac : Ioctl::kCodecSbc;
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
	if (overrides.hasRate)
	{
		preference.endpointRate = overrides.rate;
	}
	if (overrides.hasChannels)
	{
		preference.endpointChannels = overrides.channels;
	}

	DWORD bytes = 0;
	const bool set = IoctlSync(handle, Ioctl::kIoctlSetPreference, &preference, static_cast<DWORD>(sizeof(preference)),
							  nullptr, 0, bytes, error);
	if (!set && error.empty())
	{
		error = Win32Error("SET_PREFERENCE", GetLastError());
	}
	Ioctl::Status status = {};
	const bool read = set && ReadStatus(handle, status, error);
	CloseHandle(handle);
	if (!set || !read)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		if (!set && overrides.EndpointChange())
		{
			std::fprintf(stderr,
						 "the sink does not advertise that endpoint format; `OpenA2dpCtl status` lists the "
						 "formats it reported in the last discovery.\n");
		}
		return 1;
	}

	const bool force = (preference.flags & Ioctl::kPreferenceForceCodec) != 0;
	std::printf("Preference set to %s on %s.\n",
			!force ? "auto (AAC > SBC)" : (preference.codec == Ioctl::kCodecAac ? "AAC" : "SBC"),
			DeviceLabel(device).c_str());
	PrintStatus(device, handle, status, preference, false);

	if (force && preference.codec == Ioctl::kCodecAac && status.encoderRegistered == 0)
	{
		std::printf("\nnote: no encoder service is registered, so AAC is not offered and SBC will be used.\n");
	}
	const bool streaming = status.mediaState == Ioctl::MediaState::Streaming;
	const bool wantsAac = force ? preference.codec == Ioctl::kCodecAac : status.encoderRegistered != 0;
	const bool wantsSbc = force ? preference.codec == Ioctl::kCodecSbc : status.encoderRegistered == 0;
	const bool codecChanges = (wantsAac && status.codec != Ioctl::kCodecAac) ||
							  (wantsSbc && status.codec == Ioctl::kCodecAac);
	if (streaming && (codecChanges || overrides.ParameterChange() || overrides.EndpointChange()))
	{
		std::printf("\nnote: the active stream renegotiates to apply the preference "
					"(the status above may lag until the sink answers).\n");
	}
	if (overrides.EndpointChange())
	{
		std::printf("\nnote: the endpoint formats were rebuilt in place, so no reconnect is needed; "
					"Windows offers only the pinned rate and channel count from now on.\n");
	}
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	std::vector<std::string> args(argv + 1, argv + argc);
	std::string command = "status";
	std::string deviceSpec;
	std::string codecArg;
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
		else if (arg == "--rate" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			const unsigned long parsed = IsDecimal(value) ? std::strtoul(value.c_str(), nullptr, 10) : 1;
			if (!IsDecimal(value) || !Ioctl::ValidEndpointRate(static_cast<uint32_t>(parsed)))
			{
				std::fprintf(stderr, "--rate takes 0 (every rate), 44100 or 48000\n");
				return 2;
			}
			overrides.hasRate = true;
			overrides.rate = static_cast<uint32_t>(parsed);
		}
		else if (arg == "--channels" && position + 1 < args.size())
		{
			const std::string& value = args[++position];
			const unsigned long parsed = IsDecimal(value) ? std::strtoul(value.c_str(), nullptr, 10) : 3;
			if (!IsDecimal(value) || !Ioctl::ValidEndpointChannels(static_cast<uint32_t>(parsed)))
			{
				std::fprintf(stderr, "--channels takes 0 (every channel count), 1 or 2\n");
				return 2;
			}
			overrides.hasChannels = true;
			overrides.channels = static_cast<uint32_t>(parsed);
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
	if (command != "list" && command != "status" && command != "codec" && command != "version")
	{
		std::fprintf(stderr, "unknown command '%s'\n\n%s", command.c_str(), kUsage);
		return 2;
	}
	if (command == "codec" && codecArg.empty() && !overrides.ParameterChange() && !overrides.EndpointChange())
	{
		std::fprintf(stderr, "codec needs a codec (auto, sbc or aac) or at least one parameter flag\n\n%s", kUsage);
		return 2;
	}
	if (command != "codec" && (overrides.ParameterChange() || overrides.EndpointChange()))
	{
		std::fprintf(stderr, "the parameter and endpoint flags apply to `codec`\n\n%s", kUsage);
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
	return CommandStatus(devices, deviceSpec, watch, interval, verbose);
}
