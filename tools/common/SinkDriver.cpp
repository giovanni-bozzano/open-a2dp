#include "SinkDriver.h"

#include "ControlDevice.h"
#include "SinkPlayback.h"
#include "Text.h"

#include <windows.h>

#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
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
	// SetupAPI owns the block this points into, so the driver list has to stay alive until the
	// entry has been installed.
	SP_DRVINFO_DATA_W info;
	std::wstring infPath;
	// The hardware id this entry matched. One INF describes several devices, so this is what
	// tells the model for this node's service from the rest of the package.
	std::wstring hardwareId;
	bool ours = false;
};

// The driver packages this node could bind, and which of them is this project's: the INF's
// manufacturer is the one identifier a package carries that does not depend on its oemNN name.
// The caller destroys the driver list, because SetupAPI owns the entries' blocks.
bool CollectDriverEntries(HDEVINFO set, SP_DEVINFO_DATA& dev, std::vector<DriverEntry>& entries)
{
	if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_COMPATDRIVER))
	{
		return false;
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
		if (!SetupDiGetDriverInfoDetailW(set, &dev, &driver, detail, static_cast<DWORD>(buffer.size()),
										 &required) ||
			detail->InfFileName[0] == L'\0')
		{
			continue;
		}
		DriverEntry entry = {};
		entry.info = driver;
		entry.infPath = FullInfPath(detail->InfFileName);
		entry.hardwareId = detail->HardwareID;
		entry.ours = ContainsNoCase(driver.MfgName, L"OpenA2dp");
		entries.push_back(entry);
	}
	return true;
}

// True for the A2DP source service itself as a driver entry's matching id: the UUID and nothing
// after its closing brace. The same node also offers `..._GENERIC`, which is what the Bluetooth
// enumerator's own package matches for every profile, and `..._LOCALMFG&nnnn`, which says what
// the sink is rather than which package drives it. Both carry an A2DP driver's name next to the
// real one in a node's compatible list, so only this form identifies the inbox A2DP source.
bool IsA2dpServiceId(const std::wstring& id)
{
	return !id.empty() && id.back() == L'}' && ContainsNoCase(id, kA2dpNodeMarker);
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

// Installs one of the node's compatible packages on that node and no other one.
//
// UpdateDriverForPlugAndPlayDevices cannot do this job. Both packages match the same generic A2DP
// hardware id, so by hardware id it would install on every sink, and with INSTALLFLAG_FORCE it
// installs the INF's first model even when none of that INF's models matches the device - for the
// inbox INF that is a different Bluetooth profile driver, which leaves the node without an A2DP
// driver instead of putting BthA2dp on it. DiInstallDevice installs the entry SetupAPI found
// compatible with this node and restarts that node.
bool InstallDriverEntry(HDEVINFO set, SP_DEVINFO_DATA& dev, const DriverEntry& entry,
						bool& rebootRequired, std::string& error)
{
	SP_DRVINFO_DATA_W info = entry.info;
	BOOL reboot = FALSE;
	if (!DiInstallDevice(nullptr, set, &dev, &info, DIIDFLAG_NOFINISHINSTALLUI, &reboot))
	{
		const DWORD code = GetLastError();
		error = code == ERROR_ACCESS_DENIED
				? "switching a sink's driver needs an elevated prompt"
				: Win32Error("DiInstallDevice", code);
		return false;
	}
	rebootRequired = reboot != FALSE;
	return true;
}

// Stops the node so the driver installed on it can be replaced. A restart asks every holder of
// the node for its consent first, and the audio engine refuses while it plays to the sink, which
// is what left a switch half applied; disabling does not consult anybody. Returns false and
// changes nothing when a holder refuses anyway.
bool StopNode(SP_DEVINFO_DATA& dev, std::string& error)
{
	const CONFIGRET status = CM_Disable_DevNode(dev.DevInst, CM_DISABLE_UI_NOT_OK);
	if (status == CR_SUCCESS)
	{
		return true;
	}
	error = status == CR_REMOVE_VETOED
			? std::string("the sink's A2DP node is still in use, so it was not restarted; close the "
						  "audio device and try again")
			: "the sink's A2DP node could not be stopped (CONFIGRET " + std::to_string(status) + ")";
	return false;
}

// Starts the node again, on the driver that is installed on it now.
bool StartNode(SP_DEVINFO_DATA& dev, std::string& error)
{
	const CONFIGRET status = CM_Enable_DevNode(dev.DevInst, 0);
	if (status == CR_SUCCESS)
	{
		return true;
	}
	// Never leave the node stopped: that takes the sink out of the stack entirely.
	(void)CM_Enable_DevNode(dev.DevInst, 0);
	error = "the sink's A2DP node was stopped but did not start again (CONFIGRET " +
			std::to_string(status) + "); reconnect the sink";
	return false;
}

// Restarts the node so the driver installed on it binds now.
bool RestartNode(SP_DEVINFO_DATA& dev, std::string& error)
{
	return StopNode(dev, error) && StartNode(dev, error);
}

// True when the node carries a driver it has not loaded yet: an install whose restart was refused.
bool NeedsRestart(const SP_DEVINFO_DATA& dev)
{
	ULONG status = 0;
	ULONG problem = 0;
	if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) != CR_SUCCESS)
	{
		return false;
	}
	return (status & DN_NEED_RESTART) != 0 || problem == CM_PROB_NEED_RESTART ||
		   problem == CM_PROB_DRIVER_FAILED_PRIOR_UNLOAD;
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
	(void)CollectDriverEntries(set, dev, entries);
	(void)SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER);
	const std::wstring instanceId = InstanceIdOf(set, dev);
	const std::wstring service = StringProperty(set, dev, SPDRP_SERVICE);
	state.restartPending = NeedsRestart(dev);
	SetupDiDestroyDeviceInfoList(set);

	state.instanceId = Utf8(instanceId);
	state.current = DriverOfService(service);
	// Only the entry matched by the service id is the A2DP source driver. A package the node
	// also lists through `..._GENERIC` is the enumerator's catch-all, and installing it is not a
	// move to the Windows driver, so it is not offered as one.
	for (const DriverEntry& entry : entries)
	{
		if (entry.ours)
		{
			if (state.openA2dpInf.empty())
			{
				state.openA2dpInf = Utf8(entry.infPath);
			}
		}
		else if (IsA2dpServiceId(entry.hardwareId) && state.windowsInf.empty())
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

// True while the driver reports a stream of its own on this sink. Only a sink on OpenA2dp has a
// control device to ask; on the inbox driver the audio session is the only signal.
bool DriverStreams(const std::string& reference)
{
	std::string error;
	for (const Device& device : EnumerateDevices(error))
	{
		if (device.reference != reference)
		{
			continue;
		}
		const HANDLE handle = OpenControl(device, error);
		if (handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		Ioctl::Status status = {};
		const bool read = ReadStatus(handle, status, error);
		CloseHandle(handle);
		return read && (status.mediaState == Ioctl::MediaState::Streaming ||
						status.mediaState == Ioctl::MediaState::WaitingForEncoder);
	}
	return false;
}

// How long the audio engine may take to close the node's pin after the program that rendered to
// it stops. Nothing reports that moment - the pin belongs to the engine, and PnP has no event
// for it - so it is waited out in short steps instead of the restart being vetoed.
constexpr int kSinkReleaseTimeoutMs = 3000;
constexpr int kSinkReleaseStepMs = 100;

bool SinkIdle(const std::string& reference, const std::string& instanceId)
{
	SinkPlayback playback;
	const bool sessionSays = ReadSinkPlayback(instanceId, playback);
	return (!sessionSays || !playback.playing) && !DriverStreams(reference);
}

bool WaitForSinkRelease(const std::string& reference, const std::string& instanceId)
{
	for (int waited = 0;; waited += kSinkReleaseStepMs)
	{
		if (SinkIdle(reference, instanceId))
		{
			return true;
		}
		if (waited >= kSinkReleaseTimeoutMs)
		{
			return false;
		}
		Sleep(kSinkReleaseStepMs);
	}
}

// "spotify.exe", "spotify.exe and chrome.exe": the programs a refusal has to name.
std::string PlayerNames(const std::vector<std::string>& players)
{
	std::string names;
	for (const std::string& player : players)
	{
		names += names.empty() ? player : " and " + player;
	}
	return names;
}

// Whether the node can be restarted right now, pausing the program that holds it when one does.
//
// The audio engine keeps the node's pin open for as long as a program renders to the sink, so
// pausing that program is the only way to release the node without ending its stream. Both
// signals are needed: the audio session covers either driver, while the driver still reports
// Streaming for the moment after a player stops, before it suspends.
bool EnsureSinkIdle(const std::string& reference, const std::string& instanceId, std::string& paused,
					std::string& error)
{
	SinkPlayback playback;
	const bool sessionSays = ReadSinkPlayback(instanceId, playback);
	if ((!sessionSays || !playback.playing) && !DriverStreams(reference))
	{
		return true;
	}
	const bool rendering = sessionSays && playback.playing;
	std::string pauseError;
	const bool asked = rendering && PauseSinkPlayback(playback.players, paused, pauseError);
	if ((!rendering || asked) && WaitForSinkRelease(reference, instanceId))
	{
		return true;
	}
	const std::string names = PlayerNames(playback.players);
	error.clear();
	const std::string holder = names.empty() ? std::string("a program") : names;
	if (!rendering)
	{
		error = "the sink is playing (" + holder + ") and the driver is still streaming to it";
	}
	else if (asked)
	{
		error = "the sink is playing (" + holder + ") and it did not let go after being paused";
	}
	else
	{
		error = "the sink is playing (" + holder + ") and it could not be paused" +
				(pauseError.empty() ? std::string() : ": " + pauseError);
	}
	error += ", so Windows cannot restart its A2DP node. Nothing was changed: stop playback, "
			 "then try again.";
	return false;
}

SwitchOutcome SwitchSinkDriver(const std::string& reference, SinkDriver target, SwitchReport& report,
							   std::string& error)
{
	report = {};
	SinkDriverState state;
	if (!ReadSinkDriver(reference, state, error))
	{
		return SwitchOutcome::Failed;
	}
	if (target == SinkDriver::Unknown)
	{
		error = "choose opena2dp or windows";
		return SwitchOutcome::Failed;
	}
	if (state.current == target)
	{
		// Already installed, but possibly not loaded: a previous switch whose restart was refused
		// finishes here, without a reboot.
		HDEVINFO set = INVALID_HANDLE_VALUE;
		SP_DEVINFO_DATA dev = {};
		if (!FindNode(reference, set, dev, error))
		{
			return SwitchOutcome::Failed;
		}
		SwitchOutcome outcome = SwitchOutcome::Switched;
		if (NeedsRestart(dev))
		{
			std::string paused;
			const bool idle = EnsureSinkIdle(reference, state.instanceId, paused, error);
			report.pausedPlayer = paused;
			if (!idle)
			{
				outcome = SwitchOutcome::PlaysAudio;
			}
			else if (!RestartNode(dev, error))
			{
				outcome = SwitchOutcome::Failed;
			}
		}
		SetupDiDestroyDeviceInfoList(set);
		return outcome;
	}
	const std::string& inf = target == SinkDriver::OpenA2dp ? state.openA2dpInf : state.windowsInf;
	if (inf.empty())
	{
		error = state.problem.empty() ? "the driver package for that choice is not available" : state.problem;
		return SwitchOutcome::Failed;
	}
	std::string paused;
	if (!EnsureSinkIdle(reference, state.instanceId, paused, error))
	{
		report.pausedPlayer = paused;
		return SwitchOutcome::PlaysAudio;
	}
	report.pausedPlayer = paused;
	HDEVINFO set = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA dev = {};
	if (!FindNode(reference, set, dev, error))
	{
		return SwitchOutcome::Failed;
	}
	std::vector<DriverEntry> entries;
	if (!CollectDriverEntries(set, dev, entries))
	{
		error = Win32Error("SetupDiBuildDriverInfoList", GetLastError());
		SetupDiDestroyDeviceInfoList(set);
		return SwitchOutcome::Failed;
	}
	// The package the store offered for this choice, and within it the model that matched the
	// service id: the same node also lists the enumerator's `_GENERIC` catch-all, and installing
	// that is what left the node without an A2DP driver.
	const std::wstring targetInf = Widen(inf);
	const DriverEntry* chosen = nullptr;
	for (const DriverEntry& entry : entries)
	{
		if (_wcsicmp(entry.infPath.c_str(), targetInf.c_str()) != 0)
		{
			continue;
		}
		if (IsA2dpServiceId(entry.hardwareId))
		{
			chosen = &entry;
			break;
		}
	}

	bool switched = false;
	if (chosen == nullptr)
	{
		error = "the package in the driver store has no A2DP source driver for this node; "
				"deploy the driver, then reconnect the sink and try again";
	}
	else
	{
		bool rebootRequired = false;
		switched = InstallDriverEntry(set, dev, *chosen, rebootRequired, error);
		// SetupAPI asks for a reboot when the node's restart was refused. The driver is on the
		// node already, so restarting the node ourselves finishes the switch now.
		if (switched && rebootRequired)
		{
			switched = RestartNode(dev, error);
		}
	}
	(void)SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER);
	SetupDiDestroyDeviceInfoList(set);
	return switched ? SwitchOutcome::Switched : SwitchOutcome::Failed;
}

} // namespace OpenA2dp::Tools
