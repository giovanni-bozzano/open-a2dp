#include "ControlClient.h"

#include "common/Text.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace OpenA2dp::ConfigUi {
namespace {

// What the caller can do about a failed IOCTL, appended to the system message.
const char* Guidance(DWORD code)
{
	switch (code)
	{
	case ERROR_ACCESS_DENIED:
		return " (the control device is limited to Administrators; start OpenA2dpConfig elevated)";
	case ERROR_NOT_SUPPORTED:
		return " (the sink cannot serve that setting)";
	case ERROR_INVALID_PARAMETER:
		return " (the driver refused the value)";
	case ERROR_REVISION_MISMATCH:
		return " (driver and tool ABI differ; install both from the same build)";
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return " (the sink disconnected; press Refresh)";
	default:
		return "";
	}
}

// The system message, then what the window's reader can do about it.
std::string Failure(const char* what, DWORD code)
{
	return Tools::Win32Error(what, code) + Guidance(code);
}

bool CallIoctl(HANDLE handle, ULONG code, const void* in, DWORD inLength, void* out, DWORD outLength,
			   DWORD& bytes, std::string& error)
{
	bytes = 0;
	if (!DeviceIoControl(handle, code, const_cast<void*>(in), inLength, out, outLength, &bytes, nullptr))
	{
		error = Failure("DeviceIoControl", GetLastError());
		return false;
	}
	return true;
}

template <typename T>
bool ReadIoctl(HANDLE handle, ULONG code, T& out, std::string& error)
{
	DWORD bytes = 0;
	if (!CallIoctl(handle, code, nullptr, 0, &out, static_cast<DWORD>(sizeof(out)), bytes, error))
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

std::string AbiMismatch(uint32_t driverAbi)
{
	return "the driver reports ABI " + std::to_string(driverAbi) + " but this tool expects " +
		   std::to_string(OpenA2dp::Ioctl::kAbiVersion) + "; install both from the same build";
}

} // namespace

HANDLE OpenControl(const Device& device, std::string& error)
{
	const HANDLE handle = CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
									  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		error = Failure("CreateFile", GetLastError());
	}
	return handle;
}

bool ReadVersion(HANDLE handle, Ioctl::VersionInfo& version, std::string& error)
{
	if (!ReadIoctl(handle, OpenA2dp::Ioctl::kIoctlGetVersion, version, error))
	{
		return false;
	}
	if (version.abiVersion != OpenA2dp::Ioctl::kAbiVersion)
	{
		error = AbiMismatch(version.abiVersion);
		return false;
	}
	return true;
}

bool ReadStatus(HANDLE handle, Ioctl::Status& status, std::string& error)
{
	if (!ReadIoctl(handle, OpenA2dp::Ioctl::kIoctlGetStatus, status, error))
	{
		return false;
	}
	if (status.abiVersion != OpenA2dp::Ioctl::kAbiVersion)
	{
		error = AbiMismatch(status.abiVersion);
		return false;
	}
	return true;
}

bool ReadPreference(HANDLE handle, Ioctl::Preference& preference, std::string& error)
{
	if (!ReadIoctl(handle, OpenA2dp::Ioctl::kIoctlGetPreference, preference, error))
	{
		return false;
	}
	if (preference.abiVersion != OpenA2dp::Ioctl::kAbiVersion)
	{
		error = AbiMismatch(preference.abiVersion);
		return false;
	}
	return true;
}

bool WritePreference(HANDLE handle, const Ioctl::Preference& preference, std::string& error)
{
	Ioctl::Preference request = preference;
	request.abiVersion = OpenA2dp::Ioctl::kAbiVersion;
	DWORD bytes = 0;
	return CallIoctl(handle, OpenA2dp::Ioctl::kIoctlSetPreference, &request,
					 static_cast<DWORD>(sizeof(request)), nullptr, 0, bytes, error);
}

bool ReadEndpointFormat(HANDLE handle, Ioctl::EndpointFormat& format, std::string& error)
{
	if (!ReadIoctl(handle, OpenA2dp::Ioctl::kIoctlGetEndpointFormat, format, error))
	{
		return false;
	}
	if (format.abiVersion != OpenA2dp::Ioctl::kAbiVersion)
	{
		error = AbiMismatch(format.abiVersion);
		return false;
	}
	return true;
}

bool WriteEndpointFormat(HANDLE handle, const Ioctl::EndpointFormat& format, std::string& error)
{
	Ioctl::EndpointFormat request = format;
	request.abiVersion = OpenA2dp::Ioctl::kAbiVersion;
	DWORD bytes = 0;
	return CallIoctl(handle, OpenA2dp::Ioctl::kIoctlSetEndpointFormat, &request,
					 static_cast<DWORD>(sizeof(request)), nullptr, 0, bytes, error);
}

} // namespace OpenA2dp::ConfigUi
