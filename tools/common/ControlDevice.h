#pragma once

// The per-sink control devices both tools talk to. The driver names each one after the sink
// address, so finding them is the same walk of the MS-DOS device list for the CLI and for the
// settings window, and it is one copy instead of two.

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

} // namespace OpenA2dp::Tools
