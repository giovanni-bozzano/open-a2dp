#include "ControlDevice.h"

#include "Text.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

// ntdll's own conversion, the one the I/O manager uses on the status a driver completes a request
// with. Declared here rather than pulled in with winternl.h, which the tools do not otherwise need.
extern "C" ULONG NTAPI RtlNtStatusToDosError(LONG status);
#pragma comment(lib, "ntdll.lib")

namespace OpenA2dp::Tools {
namespace {

// One typed read: the whole reply, or a failure that says why.
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

// A reply is only readable when the driver and this tool agree on the structure it filled.
bool CheckAbi(uint32_t driverAbi, std::string& error)
{
	if (driverAbi == OpenA2dp::Ioctl::kAbiVersion)
	{
		return true;
	}
	error = "driver ABI " + std::to_string(driverAbi) + " does not match the tool ABI " +
			std::to_string(OpenA2dp::Ioctl::kAbiVersion) + "; install both from the same build";
	return false;
}

} // namespace

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

	char suffix[32] = {};
	std::snprintf(suffix, sizeof(suffix), " (0x%08lX)", static_cast<unsigned long>(code));
	return std::string(what) + ": " + (message.empty() ? "error" : message) + suffix;
}

// The statuses the driver completes a control request with, which is what a failed
// DeviceIoControl has just turned into a Win32 error. The name is found by asking that same
// conversion which of them produces this error, so the answer is the one the driver gave and not
// a second table of Win32 codes that could drift from it.
const char* DriverStatusName(DWORD code)
{
	static const struct
	{
		ULONG status;
		const char* name;
	} statuses[] = {
		{0xC00000BBu, "STATUS_NOT_SUPPORTED"},
		{0xC000000Du, "STATUS_INVALID_PARAMETER"},
		{0xC0000059u, "STATUS_REVISION_MISMATCH"},
		{0xC0000010u, "STATUS_INVALID_DEVICE_REQUEST"},
		{0xC000009Au, "STATUS_INSUFFICIENT_RESOURCES"},
		{0xC0000001u, "STATUS_UNSUCCESSFUL"},
		{0xC00000A3u, "STATUS_DEVICE_NOT_READY"},
		{0xC0000120u, "STATUS_CANCELLED"},
		{0xC0000023u, "STATUS_BUFFER_TOO_SMALL"},
	};
	for (const auto& status : statuses)
	{
		if (RtlNtStatusToDosError(static_cast<LONG>(status.status)) == code)
		{
			return status.name;
		}
	}
	return nullptr;
}

std::string ControlError(const char* what, DWORD code)
{
	const char* advice = "";
	switch (code)
	{
	case ERROR_ACCESS_DENIED:
		advice = " (run the tool as Administrator)";
		break;
	case ERROR_NOT_SUPPORTED:
		advice = " (the sink cannot serve that setting)";
		break;
	case ERROR_INVALID_PARAMETER:
		advice = " (the driver refused the value)";
		break;
	case ERROR_REVISION_MISMATCH:
		advice = " (driver and tool ABI differ; install both from the same build)";
		break;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		advice = " (the sink disconnected; refresh the device list)";
		break;
	default:
		break;
	}
	std::string text = Win32Error(what, code);
	// A request the driver refused names the status it completed it with: that is the name the
	// driver's own log carries, which is what makes the two worth comparing.
	if (std::strcmp(what, "DeviceIoControl") == 0)
	{
		if (const char* status = DriverStatusName(code))
		{
			text += " [";
			text += status;
			text += ']';
		}
	}
	return text + advice;
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

// The driver creates \DosDevices\OpenA2dpCtl-<address> for every sink, so the MS-DOS device
// list is enough to find all control devices.
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
			const std::string reference = Upper(Utf8(link.c_str() + prefix.size()));
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

HANDLE OpenControl(const Device& device, std::string& error)
{
	const HANDLE handle = CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
									  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		error = ControlError("CreateFile", GetLastError());
	}
	return handle;
}

bool IoctlSync(HANDLE handle, ULONG code, const void* in, DWORD inLength, void* out, DWORD outLength,
			   DWORD& bytes, std::string& error, DWORD timeoutMs)
{
	bytes = 0;
	OVERLAPPED overlapped = {};
	overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (overlapped.hEvent == nullptr)
	{
		error = Win32Error("CreateEvent", GetLastError());
		return false;
	}

	// The handle is opened for synchronous use, so most requests complete inside this call; the
	// event covers a driver that takes the request and answers later.
	if (DeviceIoControl(handle, code, const_cast<void*>(in), inLength, out, outLength, &bytes,
						&overlapped) == FALSE)
	{
		const DWORD gle = GetLastError();
		if (gle != ERROR_IO_PENDING)
		{
			error = ControlError("DeviceIoControl", gle);
			CloseHandle(overlapped.hEvent);
			return false;
		}
		if (WaitForSingleObject(overlapped.hEvent, timeoutMs) == WAIT_TIMEOUT)
		{
			// Give the cancel a moment to land, so the driver is not left writing into `out`
			// while the caller is already gone.
			CancelIoEx(handle, &overlapped);
			WaitForSingleObject(overlapped.hEvent, Ioctl::kCancelGraceMs);
			GetOverlappedResult(handle, &overlapped, &bytes, FALSE);
			error = "DeviceIoControl timed out after " + std::to_string(timeoutMs) + " ms";
			CloseHandle(overlapped.hEvent);
			return false;
		}
		if (GetOverlappedResult(handle, &overlapped, &bytes, FALSE) == FALSE)
		{
			error = ControlError("DeviceIoControl", GetLastError());
			CloseHandle(overlapped.hEvent);
			return false;
		}
	}
	CloseHandle(overlapped.hEvent);
	return true;
}

bool ReadVersion(HANDLE handle, Ioctl::VersionInfo& version, std::string& error)
{
	return ReadIoctl(handle, Ioctl::kIoctlGetVersion, version, error) &&
		   CheckAbi(version.abiVersion, error);
}

bool ReadStatus(HANDLE handle, Ioctl::Status& status, std::string& error)
{
	return ReadIoctl(handle, Ioctl::kIoctlGetStatus, status, error) && CheckAbi(status.abiVersion, error);
}

bool ReadPreference(HANDLE handle, Ioctl::Preference& preference, std::string& error)
{
	return ReadIoctl(handle, Ioctl::kIoctlGetPreference, preference, error) &&
		   CheckAbi(preference.abiVersion, error);
}

bool WritePreference(HANDLE handle, const Ioctl::Preference& preference, std::string& error)
{
	Ioctl::Preference request = preference;
	request.abiVersion = Ioctl::kAbiVersion;
	DWORD bytes = 0;
	return IoctlSync(handle, Ioctl::kIoctlSetPreference, &request, static_cast<DWORD>(sizeof(request)),
					 nullptr, 0, bytes, error);
}

bool ResetCounters(HANDLE handle, std::string& error)
{
	DWORD bytes = 0;
	return IoctlSync(handle, Ioctl::kIoctlResetCounters, nullptr, 0, nullptr, 0, bytes, error);
}

bool ReadEndpointFormat(HANDLE handle, Ioctl::EndpointFormat& format, std::string& error)
{
	return ReadIoctl(handle, Ioctl::kIoctlGetEndpointFormat, format, error) &&
		   CheckAbi(format.abiVersion, error);
}

bool WriteEndpointFormat(HANDLE handle, const Ioctl::EndpointFormat& format, std::string& error)
{
	Ioctl::EndpointFormat request = format;
	request.abiVersion = Ioctl::kAbiVersion;
	DWORD bytes = 0;
	return IoctlSync(handle, Ioctl::kIoctlSetEndpointFormat, &request, static_cast<DWORD>(sizeof(request)),
					 nullptr, 0, bytes, error);
}

} // namespace OpenA2dp::Tools
