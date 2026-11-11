#include "SinkDriver.h"

#include "ControlDevice.h"
#include "Text.h"

#include <windows.h>

#include <newdev.h>
#include <setupapi.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace OpenA2dp::Tools {
namespace {

// The AudioSink service UUID: the only BTHENUM node a sink's A2DP source driver can be
// replaced on. The radio and the other profile nodes are never candidates.
constexpr wchar_t kA2dpNodeMarker[] = L"{0000110B-";

bool ContainsNoCase(const std::wstring& text, const wchar_t* needle)
{
	for (size_t i = 0; i + 1 <= text.size(); ++i)
	{
		size_t j = 0;
		for (; needle[j] != L'\0'; ++j)
		{
			if (i + j >= text.size() || towupper(text[i + j]) != towupper(needle[j]))
			{
				break;
			}
		}
		if (needle[j] == L'\0')
		{
			return true;
		}
	}
	return false;
}

std::wstring InstanceIdOf(HDEVINFO set, SP_DEVINFO_DATA& dev)
{
	DWORD required = 0;
	(void)SetupDiGetDeviceInstanceIdW(set, &dev, nullptr, 0, &required);
	if (required == 0)
	{
		return {};
	}
	std::vector<wchar_t> buffer(required, L'\0');
	if (!SetupDiGetDeviceInstanceIdW(set, &dev, buffer.data(), required, nullptr))
	{
		return {};
	}
	return std::wstring(buffer.data());
}

// The first string of a registry property: every property this file reads is a single string
// of a multi-string, and the first one is the one that identifies the device or its service.
std::wstring StringProperty(HDEVINFO set, SP_DEVINFO_DATA& dev, DWORD property)
{
	DWORD type = 0;
	DWORD required = 0;
	if (SetupDiGetDeviceRegistryPropertyW(set, &dev, property, &type, nullptr, 0, &required) ||
		GetLastError() != ERROR_INSUFFICIENT_BUFFER || required < sizeof(wchar_t))
	{
		return {};
	}
	std::vector<UCHAR> buffer(required + sizeof(wchar_t), 0);
	if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, property, &type, buffer.data(), required, nullptr))
	{
		return {};
	}
	return std::wstring(reinterpret_cast<const wchar_t*>(buffer.data()));
}

// A BTHENUM instance id carries the remote address, in the order the stack wrote it: some
// builds write it as the address reads, others byte-reversed, so both forms identify a sink.
bool MatchesAddress(const std::wstring& instanceId, const std::string& reference)
{
	const std::string address = Upper(reference);
	if (address.size() != 12)
	{
		return false;
	}
	const std::string id = Upper(Utf8(instanceId));
	if (id.find(address) != std::string::npos)
	{
		return true;
	}
	std::string reversed;
	for (int pair = 5; pair >= 0; --pair)
	{
		reversed += address.substr(static_cast<size_t>(pair) * 2, 2);
	}
	return id.find(reversed) != std::string::npos;
}

// The INF a driver entry installs from. The driver store reports these as bare names
// ("oem12.inf", "bth.inf"), and the install API wants a path.
std::wstring FullInfPath(const std::wstring& inf)
{
	if (inf.size() > 2 && inf[1] == L':')
	{
		return inf;
	}
	wchar_t windows[MAX_PATH] = {};
	const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
	{
		return inf;
	}
	return std::wstring(windows) + L"\\INF\\" + inf;
}

struct DriverEntry
{
	std::wstring infPath;
	bool ours = false;
};

// The driver packages this node could bind, and which of them is this project's: the INF's
// manufacturer is the one identifier a package carries that does not depend on its oemNN name.
void CollectDriverEntries(HDEVINFO set, SP_DEVINFO_DATA& dev, std::vector<DriverEntry>& entries)
{
	if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_COMPATDRIVER))
	{
		return;
	}
	for (DWORD index = 0;; ++index)
	{
		SP_DRVINFO_DATA_W driver = {};
		driver.cbSize = sizeof(driver);
		if (!SetupDiEnumDriverInfoW(set, &dev, SPDIT_COMPATDRIVER, index, &driver))
		{
			break;
		}
		// The detail structure ends in a variable-length field, so it is given room to spare.
		std::vector<UCHAR> buffer(sizeof(SP_DRVINFO_DETAIL_DATA_W) + MAX_PATH * sizeof(wchar_t) + 512, 0);
		auto* detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buffer.data());
		detail->cbSize = sizeof(*detail);
		DWORD required = 0;
		(void)SetupDiGetDriverInfoDetailW(set, &dev, &driver, detail, static_cast<DWORD>(buffer.size()),
										&required);
		if (detail->InfFileName[0] == L'\0')
		{
			continue;
		}
		entries.push_back({FullInfPath(detail->InfFileName),
						   ContainsNoCase(driver.MfgName, L"OpenA2dp")});
	}
	SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER);
}

// Opens the A2DP node for a sink. `error` is set only when the enumeration itself fails.
bool FindNode(const std::string& reference, HDEVINFO& set, SP_DEVINFO_DATA& dev, std::string& error)
{
	set = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
	if (set == INVALID_HANDLE_VALUE)
	{
		error = Win32Error("SetupDiGetClassDevs", GetLastError());
		return false;
	}
	for (DWORD index = 0;; ++index)
	{
		dev = {};
		dev.cbSize = sizeof(dev);
		if (!SetupDiEnumDeviceInfo(set, index, &dev))
		{
			break;
		}
		const std::wstring instanceId = InstanceIdOf(set, dev);
		if (instanceId.empty() || !ContainsNoCase(instanceId, kA2dpNodeMarker) ||
			!MatchesAddress(instanceId, reference))
		{
			continue;
		}
		return true;
	}
	SetupDiDestroyDeviceInfoList(set);
	set = INVALID_HANDLE_VALUE;
	error = "no A2DP source node carries " + FormatAddress(reference) +
			". Pair and connect the sink, then try again.";
	return false;
}

} // namespace

const char* SinkDriverName(SinkDriver driver)
{
	switch (driver)
	{
	case SinkDriver::Windows:
		return "BthA2dp";
	case SinkDriver::OpenA2dp:
		return "OpenA2dp";
	default:
		return "unknown";
	}
}

SinkDriver OtherDriver(SinkDriver driver)
{
	switch (driver)
	{
	case SinkDriver::Windows:
		return SinkDriver::OpenA2dp;
	case SinkDriver::OpenA2dp:
		return SinkDriver::Windows;
	default:
		return SinkDriver::Unknown;
	}
}

namespace {

SinkDriver DriverOfService(const std::wstring& service)
{
	if (ContainsNoCase(service, L"OpenA2dp"))
	{
		return SinkDriver::OpenA2dp;
	}
	if (ContainsNoCase(service, L"BthA2dp"))
	{
		return SinkDriver::Windows;
	}
	return SinkDriver::Unknown;
}

// The address a BTHENUM id carries, as the last run of 12 hex digits: the same form the
// control device is named after, in the order the stack wrote it.
std::string AddressOf(const std::wstring& instanceId)
{
	const std::string id = Upper(Utf8(instanceId));
	std::string address;
	for (size_t i = 0; i < id.size();)
	{
		size_t length = 0;
		while (i + length < id.size() && isxdigit(static_cast<unsigned char>(id[i + length])) != 0)
		{
			++length;
		}
		if (length == 12)
		{
			address = id.substr(i, 12);
		}
		i += length == 0 ? 1 : length;
	}
	return address;
}

} // namespace

bool ReadSinkDriver(const std::string& reference, SinkDriverState& state, std::string& error)
{
	state = {};
	error.clear();

	HDEVINFO set = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA dev = {};
	if (!FindNode(reference, set, dev, error))
	{
		return false;
	}

	std::vector<DriverEntry> entries;
	CollectDriverEntries(set, dev, entries);
	const std::wstring instanceId = InstanceIdOf(set, dev);
	const std::wstring service = StringProperty(set, dev, SPDRP_SERVICE);
	SetupDiDestroyDeviceInfoList(set);

	state.instanceId = Utf8(instanceId);
	state.current = DriverOfService(service);
	for (const DriverEntry& entry : entries)
	{
		if (entry.ours && state.openA2dpInf.empty())
		{
			state.openA2dpInf = Utf8(entry.infPath);
		}
		else if (!entry.ours && state.windowsInf.empty())
		{
			state.windowsInf = Utf8(entry.infPath);
		}
	}

	// The reader is told what stands in the way, in the terms of the sink in front of them.
	if (state.current == SinkDriver::Unknown)
	{
		state.problem = "the node's service is neither BthA2dp nor OpenA2dp";
	}
	else if (state.current == SinkDriver::OpenA2dp && state.windowsInf.empty())
	{
		state.problem = "the inbox A2DP driver is not a candidate for this node";
	}
	else if (state.current == SinkDriver::Windows && state.openA2dpInf.empty())
	{
		state.problem = "the OpenA2dp package is not in the driver store; deploy it first";
	}
	else
	{
		state.switchable = true;
	}
	return true;
}

bool EnumerateSinkNodes(std::vector<SinkNode>& nodes, std::string& error)
{
	nodes.clear();
	error.clear();
	HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
	if (set == INVALID_HANDLE_VALUE)
	{
		error = Win32Error("SetupDiGetClassDevs", GetLastError());
		return false;
	}
	for (DWORD index = 0;; ++index)
	{
		SP_DEVINFO_DATA dev = {};
		dev.cbSize = sizeof(dev);
		if (!SetupDiEnumDeviceInfo(set, index, &dev))
		{
			break;
		}
		const std::wstring instanceId = InstanceIdOf(set, dev);
		if (instanceId.empty() || !ContainsNoCase(instanceId, kA2dpNodeMarker))
		{
			continue;
		}
		SinkNode node;
		node.reference = AddressOf(instanceId);
		if (node.reference.empty())
		{
			continue;
		}
		node.driver = DriverOfService(StringProperty(set, dev, SPDRP_SERVICE));
		node.instanceId = Utf8(instanceId);
		nodes.push_back(node);
	}
	SetupDiDestroyDeviceInfoList(set);
	std::sort(nodes.begin(), nodes.end(),
			  [](const SinkNode& first, const SinkNode& second) { return first.reference < second.reference; });
	return true;
}

bool SwitchSinkDriver(const std::string& reference, SinkDriver target, bool& rebootRequired,
					  std::string& error)
{
	rebootRequired = false;
	SinkDriverState state;
	if (!ReadSinkDriver(reference, state, error))
	{
		return false;
	}
	if (target == SinkDriver::Unknown)
	{
		error = "choose opena2dp or windows";
		return false;
	}
	if (state.current == target)
	{
		return true;
	}
	const std::string& inf = target == SinkDriver::OpenA2dp ? state.openA2dpInf : state.windowsInf;
	if (inf.empty())
	{
		error = state.problem.empty() ? "the driver package for that choice is not available" : state.problem;
		return false;
	}

	HDEVINFO set = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA dev = {};
	if (!FindNode(reference, set, dev, error))
	{
		return false;
	}
	// The instance's own hardware id carries the sink address, so the install reaches this node
	// and no other one; a generic id would move every sink.
	const std::wstring hardwareId = StringProperty(set, dev, SPDRP_HARDWAREID);
	SetupDiDestroyDeviceInfoList(set);
	if (hardwareId.empty())
	{
		error = "the A2DP node reported no hardware id";
		return false;
	}

	BOOL reboot = FALSE;
	const std::wstring path = Widen(inf);
	if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hardwareId.c_str(), path.c_str(),
											INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &reboot))
	{
		const DWORD code = GetLastError();
		error = code == ERROR_ACCESS_DENIED
					? "switching a sink's driver needs an elevated prompt"
					: Win32Error("UpdateDriverForPlugAndPlayDevices", code);
		return false;
	}
	rebootRequired = reboot != FALSE;
	return true;
}

} // namespace OpenA2dp::Tools
