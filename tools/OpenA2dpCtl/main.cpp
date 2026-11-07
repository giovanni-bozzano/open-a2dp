// OpenA2dpCtl.exe - diagnostic CLI for the OpenA2dp A2DP source driver.
//
// Enumerates the per-device control interfaces (one per sink) and uses the
// IOCTLs in common/include/OpenA2dp/Ioctl.h to report the negotiated codec and
// to set the codec preference. The tool is a test/diagnostic aid: the end-user
// configuration path is still the desktop UI described in the plan.

#include <OpenA2dp/Ioctl.h>

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

constexpr const char* kUsage = R"(OpenA2dpCtl - inspect and configure the OpenA2dp A2DP source driver.

Usage:
  OpenA2dpCtl [status] [--device <mac|index>] [--watch [--interval <ms>]]
  OpenA2dpCtl list
  OpenA2dpCtl codec <auto|sbc|aac> [--bitrate <bps>] [--bitpool <n>]
                     [--blocks <4|8|12|16>] [--subbands <4|8>]
                     [--allocation <loudness|snr>] [--device <mac|index>]
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
  version   Show the driver ABI and version.

Options:
  --device <spec>   Select a device by Bluetooth address (AA:BB:CC:DD:EE:FF),
                    1-based list index, or a partial address.
  --bitrate <bps>   AAC bitrate cap for `codec` (0 = driver default).
  --bitpool <n>     SBC bitpool cap for `codec`, 0 or 2..250 (0 = driver
                    default). The sink's minimum and maximum still win.
  --watch           Refresh status until interrupted.
  --interval <ms>   Refresh period for --watch (default 1000).

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

const char* AvdtpStateName(uint32_t state)
{
	static const char* const names[] = {
		"Idle", "Discovering", "GettingCapabilities", "Configuring", "Configured", "Opening",
		"Open", "Starting", "Streaming", "Suspending", "Closing", "Aborting", "Failed",
	};
	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "Unknown";
}

const char* MediaStateName(uint32_t state)
{
	switch (static_cast<Ioctl::MediaState>(state))
	{
	case Ioctl::MediaState::Idle: return "Idle";
	case Ioctl::MediaState::WaitingForEncoder: return "WaitingForEncoder";
	case Ioctl::MediaState::Streaming: return "Streaming";
	case Ioctl::MediaState::Suspended: return "Suspended";
	case Ioctl::MediaState::Failed: return "Failed";
	default: return "Unknown";
	}
}

const char* CodecName(uint32_t codec)
{
	if (codec == Ioctl::kCodecSbc)
	{
		return "SBC";
	}
	if (codec == Ioctl::kCodecAac)
	{
		return "AAC";
	}
	return "none";
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

void PrintPreference(const Ioctl::Preference& preference)
{
	if ((preference.flags & Ioctl::kPreferenceForceCodec) != 0)
	{
		std::printf("  preference      force %s\n", CodecName(preference.codec));
	}
	else
	{
		std::printf("  preference      auto (AAC > SBC)\n");
	}
	if (preference.sbcBitpool != 0)
	{
		std::printf("  SBC bitpool     %u\n", preference.sbcBitpool);
	}
	if (preference.aacBitrate != 0)
	{
		std::printf("  AAC bitrate     %u bps\n", preference.aacBitrate);
	}
	if (preference.aacSampleRate != 0)
	{
		std::printf("  AAC rate        %u Hz (used once the endpoint is multi-rate)\n", preference.aacSampleRate);
	}
	if (preference.sbcBlocks != 0 || preference.sbcSubbands != 0 || preference.sbcAllocation != 0)
	{
		std::printf("  SBC frame       %u blocks, %u subbands, %s\n",
					preference.sbcBlocks != 0 ? preference.sbcBlocks : 16u,
					preference.sbcSubbands != 0 ? preference.sbcSubbands : 8u,
					preference.sbcAllocation == Ioctl::kSbcAllocationSnr ? "SNR" : "loudness");
	}
}

void PrintStatus(const Device& device, const Ioctl::Status& status, const Ioctl::Preference& preference)
{
	std::printf("%s\n", DeviceLabel(device).c_str());
	std::printf("  AVDTP           %s\n", AvdtpStateName(status.avdtpState));
	std::printf("  media           %s\n", MediaStateName(static_cast<uint32_t>(status.mediaState)));
	if (status.mediaState == Ioctl::MediaState::Streaming && status.codec == 0)
	{
		std::printf("  codec           SBC, %u Hz, %u ch, %u bps\n", status.sampleRate, status.channels,
					status.bitrate);
	}
	else if (status.codec != 0)
	{
		std::printf("  codec           %s, %u Hz, %u ch, %u bps\n", CodecName(status.codec), status.sampleRate,
					status.channels, status.bitrate);
	}
	else if (status.negotiatedCodec != 0)
	{
		std::printf("  codec           %s (negotiated, not streaming)\n", CodecName(status.negotiatedCodec));
	}
	else
	{
		std::printf("  codec           none\n");
	}
	// The bitrate ceiling the media MTU enforces, so a rate the encoder supports but one
	// packet cannot carry is visible instead of looking like a negotiation failure.
	if (status.mediaMtu != 0)
	{
		std::printf("  media MTU       %u B, %u B payload; AAC up to %u bps\n", status.mediaMtu,
					status.mediaMtu - Ioctl::kRtpHeaderBytes, status.bitrateLimit);
	}
	PrintPreference(preference);
	std::printf("  encoder         %s\n", status.encoderRegistered != 0 ? "registered" : "not registered");
	std::printf("  rings           PCM %u/%u B, frames %u/%u B\n", status.pcmBufferedBytes, status.pcmCapacity,
				status.frameBufferedBytes, status.frameCapacity);
	std::printf("  counters        packets %u, dropped %u, underruns %u, send failures %u\n", status.packetsSent,
				status.framesDropped, status.underruns, status.sendFailures);
	if (status.lastError != 0)
	{
		std::printf("  last error      0x%08X\n", status.lastError);
	}
}

int CommandStatus(const std::vector<Device>& devices, const std::string& spec, bool watch, unsigned interval)
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
		PrintStatus(device, status, preference);
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

	bool ParameterChange() const
	{
		return hasBitrate || hasBitpool || hasBlocks || hasSubbands || hasAllocation;
	}
};

int CommandCodec(const std::vector<Device>& devices, const std::string& spec, const std::string& codecArg,
				 const PreferenceOverrides& overrides)
{
	if (codecArg != "auto" && codecArg != "sbc" && codecArg != "aac")
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
	if (codecArg == "auto")
	{
		preference.flags = 0;
		preference.codec = 0;
	}
	else
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
		return 1;
	}

	std::printf("Preference set to %s on %s.\n",
				codecArg == "auto" ? "auto (AAC > SBC)" : (codecArg == "aac" ? "AAC" : "SBC"),
				DeviceLabel(device).c_str());
	PrintStatus(device, status, preference);

	if (preference.codec == Ioctl::kCodecAac && status.encoderRegistered == 0)
	{
		std::printf("\nnote: no encoder service is registered, so AAC is not offered and SBC will be used.\n");
	}
	const bool streaming = status.mediaState == Ioctl::MediaState::Streaming;
	const bool force = (preference.flags & Ioctl::kPreferenceForceCodec) != 0;
	const bool wantsAac = force ? preference.codec == Ioctl::kCodecAac : status.encoderRegistered != 0;
	const bool wantsSbc = force ? preference.codec == Ioctl::kCodecSbc : status.encoderRegistered == 0;
	const bool codecChanges = (wantsAac && status.codec != Ioctl::kCodecAac) ||
							  (wantsSbc && status.codec == Ioctl::kCodecAac);
	if (streaming && (codecChanges || overrides.ParameterChange()))
	{
		std::printf("\nnote: the active stream renegotiates to apply the preference "
					"(the status above may lag until the sink answers).\n");
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
		else if (arg == "--watch")
		{
			watch = true;
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
	if (command == "codec" && codecArg.empty())
	{
		std::fprintf(stderr, "codec needs an argument: auto, sbc or aac\n\n%s", kUsage);
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
	return CommandStatus(devices, deviceSpec, watch, interval);
}
