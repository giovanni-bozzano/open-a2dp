#include "ControlClient.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace OpenA2dp::ConfigUi {
namespace {

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
	return std::string(buffer.data());
}

std::string Upper(std::string text)
{
	for (char& c : text)
	{
		if (c >= 'a' && c <= 'z')
		{
			c = static_cast<char>(c - 'a' + 'A');
		}
	}
	return text;
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
	return std::string(what) + ": " + (message.empty() ? "error" : message) + suffix + Guidance(code);
}

// The control device answers without waiting on the session, so a plain synchronous call is
// enough; the status and preference IOCTLs only read snapshots or take a spin lock briefly.
bool CallIoctl(HANDLE handle, ULONG code, const void* in, DWORD inLength, void* out, DWORD outLength,
			   DWORD& bytes, std::string& error)
{
	bytes = 0;
	if (!DeviceIoControl(handle, code, const_cast<void*>(in), inLength, out, outLength, &bytes, nullptr))
	{
		error = Win32Error("DeviceIoControl", GetLastError());
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

std::vector<Device> EnumerateDevices(std::string& error)
{
	std::vector<Device> devices;
	const std::wstring prefix = OpenA2dp::Ioctl::kControlLinkPrefix;
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
			if (link.size() != prefix.size() + OpenA2dp::Ioctl::kControlAddressChars ||
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

HANDLE OpenControl(const Device& device, std::string& error)
{
	const HANDLE handle = CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
									  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		error = Win32Error("CreateFile", GetLastError());
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
