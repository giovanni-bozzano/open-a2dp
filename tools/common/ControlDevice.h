#pragma once

// The per-sink control devices both tools talk to. The driver names each one after the sink
// address, so finding them, opening them and issuing the IOCTLs in OpenA2dp/Ioctl.h is one
// implementation for the CLI and for the settings window.

#include <OpenA2dp/Ioctl.h>

#include <windows.h>

#include <string>
#include <vector>

namespace OpenA2dp::Tools {

struct Device
{
	std::wstring path;
	std::string reference; // 12 hex digits, the driver's symbolic link suffix
};

// True for the 12 hex digits the driver names a control device after.
bool IsHex12(const std::string& text);

// "AA:BB:CC:DD:EE:FF" for a reference, or the reference itself when it is not one.
std::string FormatAddress(const std::string& reference);

// Every control device the driver currently has, sorted by address. `error` is set when the
// device list cannot be read at all.
std::vector<Device> EnumerateDevices(std::string& error);

// "CreateFile: The system cannot find the file specified. (0x00000002)": the call that failed,
// the system message and the code, which is what a bug report needs to be actionable.
std::string Win32Error(const char* what, DWORD code);

// The same text with what the reader can do about it. Both tools show it, so it names neither
// the tool nor one of its buttons.
std::string ControlError(const char* what, DWORD code);

HANDLE OpenControl(const Device& device, std::string& error);

// One request and its answer: the wait covers a driver that takes the request and completes it
// later, a timeout cancels it rather than leaving the caller waiting, and a short reply is a
// failure instead of a half-filled structure.
bool IoctlSync(HANDLE handle, ULONG code, const void* in, DWORD inLength, void* out,
			   DWORD outLength, DWORD& bytes, std::string& error,
			   DWORD timeoutMs = Ioctl::kIoctlTimeoutMs);

// The requests the driver serves; a reply whose ABI is not this tool's is refused, because the
// buffer it filled would be laid out differently.
bool ReadVersion(HANDLE handle, Ioctl::VersionInfo& version, std::string& error);
bool ReadStatus(HANDLE handle, Ioctl::Status& status, std::string& error);
bool ReadPreference(HANDLE handle, Ioctl::Preference& preference, std::string& error);
bool WritePreference(HANDLE handle, const Ioctl::Preference& preference, std::string& error);
// Starts a new measurement window: the counters status reports, which otherwise cover the
// endpoint's whole life, are cleared. The stream and its settings are untouched.
bool ResetCounters(HANDLE handle, std::string& error);
bool ReadEndpointFormat(HANDLE handle, Ioctl::EndpointFormat& format, std::string& error);
bool WriteEndpointFormat(HANDLE handle, const Ioctl::EndpointFormat& format, std::string& error);

} // namespace OpenA2dp::Tools
