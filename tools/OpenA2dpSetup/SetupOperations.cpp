#include "SetupOperations.h"

#include "SetupDriverStore.h"
#include "SetupEnvironment.h"
#include "SetupNames.h"
#include "SetupPayload.h"
#include "SetupRegistry.h"
#include "SetupService.h"
#include "SetupShortcut.h"
#include "SetupSignature.h"

#include "common/ControlDevice.h"
#include "common/SinkDriver.h"
#include "common/Text.h"

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <vector>

namespace OpenA2dp::Setup {
namespace {

std::wstring SystemMessage(DWORD code)
{
	wchar_t* text = nullptr;
	const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
											FORMAT_MESSAGE_IGNORE_INSERTS,
										nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
	std::wstring message = length != 0 && text != nullptr ? std::wstring(text, length) : std::wstring();
	if (text != nullptr)
	{
		LocalFree(text);
	}
	while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
	{
		message.pop_back();
	}
	return message.empty() ? L"Windows error " + std::to_wstring(code) : message;
}

std::wstring LastErrorText(const std::wstring& what)
{
	return what + L": " + SystemMessage(GetLastError());
}

std::wstring Combine(const std::wstring& directory, const std::wstring& name)
{
	return directory + L"\\" + name;
}

std::wstring PublishedInfPath(const std::wstring& published)
{
	wchar_t windows[MAX_PATH] = {};
	const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
	if (length == 0 || length >= ARRAYSIZE(windows))
	{
		return {};
	}
	return std::wstring(windows, length) + L"\\INF\\" + published;
}

// The packages in the store whose published INF is not this build. A package whose version cannot
// be read counts as old, but if no package matches the new version at all the caller leaves the
// store alone rather than risk deleting the package it just staged.
std::vector<std::wstring> OldDriverPackages(const std::vector<std::wstring>& packages,
											const std::wstring& currentVersion, bool& foundCurrent)
{
	std::vector<std::wstring> old;
	foundCurrent = false;
	for (const std::wstring& package : packages)
	{
		const std::wstring path = PublishedInfPath(package);
		if (!path.empty() &&
			CompareStringOrdinal(ReadDriverVersion(path).c_str(), -1, currentVersion.c_str(), -1, TRUE) ==
				CSTR_EQUAL)
		{
			foundCurrent = true;
			continue;
		}
		old.push_back(package);
	}
	return old;
}

std::wstring SinkName(const Tools::SinkNode& sink)
{
	return Tools::Widen(Tools::FormatAddress(sink.reference));
}

ULONGLONG InstalledSize(const std::wstring& directory)
{
	const wchar_t* names[] = {kSysName,      kInfName,      kCatName,
							  kServiceExeName, kControlExeName, kConfigExeName, kSetupExeName};
	ULONGLONG total = 0;
	for (const wchar_t* name : names)
	{
		WIN32_FILE_ATTRIBUTE_DATA data = {};
		if (GetFileAttributesExW(Combine(directory, name).c_str(), GetFileExInfoStandard, &data))
		{
			total += (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
		}
	}
	return total;
}

bool CopyInstallFiles(const std::wstring& installDir, const std::wstring& payloadDir,
					  const std::wstring& setupExePath, std::vector<std::wstring>& copied,
					  bool& createdDirectory, const Log& log, std::wstring& error)
{
	copied.clear();
	createdDirectory = false;
	const int created = SHCreateDirectoryExW(nullptr, installDir.c_str(), nullptr);
	if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS)
	{
		error = L"The install folder could not be created (" + std::to_wstring(created) + L").";
		return false;
	}
	createdDirectory = created == ERROR_SUCCESS;

	const wchar_t* names[] = {kSysName, kInfName, kCatName, kServiceExeName, kControlExeName,
							  kConfigExeName};
	for (const wchar_t* name : names)
	{
		const std::wstring target = Combine(installDir, name);
		if (!CopyFileW(Combine(payloadDir, name).c_str(), target.c_str(), FALSE))
		{
			error = LastErrorText(L"Could not copy " + std::wstring(name));
			return false;
		}
		copied.push_back(target);
	}
	const std::wstring installedSetup = Combine(installDir, kSetupExeName);
	if (!PathEqualsNoCase(setupExePath, installedSetup))
	{
		if (!CopyFileW(setupExePath.c_str(), installedSetup.c_str(), FALSE))
		{
			error = LastErrorText(L"Could not copy " + std::wstring(kSetupExeName));
			return false;
		}
		copied.push_back(installedSetup);
	}
	log(L"Files copied to " + installDir + L".");
	return true;
}

void RollbackInstall(bool updating, const std::wstring& installDir,
					 const std::vector<std::wstring>& copied,
					 const std::vector<std::wstring>& addedPackages, bool serviceExisted,
					 bool serviceCreated, bool createdDirectory, const PackageTrust& trust,
					 const Log& log)
{
	log(L"Rolling back the changes...");
	std::wstring error;
	for (const std::wstring& package : addedPackages)
	{
		bool restart = false;
		(void)DeleteDriverPackage(package, restart, log, error);
	}
	if (updating)
	{
		if (serviceCreated)
		{
			(void)DeleteEncoderService(log, error);
		}
		else if (serviceExisted)
		{
			(void)StartEncoderService(log, error);
		}
	}
	else
	{
		(void)DeleteEncoderService(log, error);
		(void)RemoveStartMenuShortcut(error);
		(void)RemoveInstallInfo(error);
		for (const std::wstring& file : copied)
		{
			if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
			{
				log(L"warning: could not delete " + file);
			}
		}
		if (createdDirectory)
		{
			(void)RemoveDirectoryW(installDir.c_str());
		}
	}
	if (trust.added)
	{
		(void)RemoveTrustedCertificate(trust.thumbprint, error);
	}
}

std::wstring FailureMessage(const std::wstring& error, bool updating)
{
	return L"Setup failed: " + error +
		   (updating ? L"\n\nThe existing installation may be partially updated. Run setup again to repair it."
					 : L"\n\nThe changes were rolled back.");
}

} // namespace

std::vector<std::wstring> CurrentOpenA2dpSinks(std::wstring& error)
{
	error.clear();
	std::vector<Tools::SinkNode> nodes;
	std::string readError;
	if (!Tools::EnumerateSinkNodes(nodes, readError))
	{
		error = Tools::Widen(readError);
		return {};
	}
	std::vector<std::wstring> sinks;
	for (const Tools::SinkNode& node : nodes)
	{
		if (node.driver == Tools::SinkDriver::OpenA2dp)
		{
			sinks.push_back(SinkName(node));
		}
	}
	return sinks;
}

Result Install(const std::wstring& installDir, const std::wstring& setupExePath, HWND owner,
			   const Log& log)
{
	// The Start Menu shortcut needs COM on this worker thread, not the UI thread that owns the
	// wizard.
	const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	struct ComGuard
	{
		HRESULT result;
		~ComGuard()
		{
			if (SUCCEEDED(result))
			{
				CoUninitialize();
			}
		}
	} comGuard{com};

	Result result;
	InstallInfo existing;
	const bool registered = ReadInstallInfo(existing);
	if (registered && !PathEqualsNoCase(existing.installDir, installDir))
	{
		result.message = L"OpenA2DP is already installed in " + existing.installDir +
						 L".\n\nRemove it before installing to another folder.";
		return result;
	}
	const bool updating = registered;

	std::wstring running;
	if (IsToolRunning(running))
	{
		result.message = running + L" is running. Close the OpenA2DP tools, then run setup again.";
		return result;
	}

	std::wstring error;
	std::wstring payloadDir;
	if (!CreatePayloadDirectory(payloadDir, error))
	{
		result.message = error;
		return result;
	}
	struct TempGuard
	{
		std::wstring directory;
		~TempGuard()
		{
			if (!directory.empty())
			{
				(void)RemoveDirectoryTree(directory);
			}
		}
	} tempGuard{payloadDir};

	log(L"Extracting the driver package, service and tools...");
	if (!ExtractPayload(payloadDir, error))
	{
		result.message = error;
		return result;
	}

	log(L"Checking the driver package signature...");
	PackageTrust trust;
	if (!PrepareDriverPackage(payloadDir, owner, trust, log, error))
	{
		result.message = error;
		return result;
	}

	const auto abortTrust = [&trust] {
		if (trust.added)
		{
			std::wstring ignored;
			(void)RemoveTrustedCertificate(trust.thumbprint, ignored);
		}
	};

	std::vector<std::wstring> packagesBefore;
	if (!ListOpenA2dpPackages(packagesBefore, log, error))
	{
		abortTrust();
		result.message = error;
		return result;
	}

	std::vector<Tools::SinkNode> sinksBefore;
	{
		std::string readError;
		if (!Tools::EnumerateSinkNodes(sinksBefore, readError))
		{
			log(L"warning: the A2DP sink list could not be read: " + Tools::Widen(readError));
		}
	}

	const bool serviceExisted = EncoderServiceExists();
	if (serviceExisted)
	{
		log(L"Stopping the existing encoder service...");
		if (!StopEncoderService(log, error))
		{
			abortTrust();
			result.message = error;
			return result;
		}
	}

	std::vector<std::wstring> copied;
	bool createdDirectory = false;
	if (!CopyInstallFiles(installDir, payloadDir, setupExePath, copied, createdDirectory, log, error))
	{
		RollbackInstall(updating, installDir, copied, {}, serviceExisted, false, createdDirectory,
						trust, log);
		result.message = FailureMessage(error, updating);
		return result;
	}

	log(L"Adding the driver package to the driver store; no headset is switched...");
	if (!StageDriverPackage(Combine(payloadDir, kInfName), log, error))
	{
		RollbackInstall(updating, installDir, copied, {}, serviceExisted, false, createdDirectory,
						trust, log);
		result.message = FailureMessage(error, updating);
		return result;
	}

	std::vector<std::wstring> packagesAfter;
	std::vector<std::wstring> addedPackages;
	if (ListOpenA2dpPackages(packagesAfter, log, error))
	{
		for (const std::wstring& package : packagesAfter)
		{
			if (std::find(packagesBefore.begin(), packagesBefore.end(), package) == packagesBefore.end())
			{
				addedPackages.push_back(package);
			}
		}
	}

	const bool serviceCreated = !serviceExisted;
	if (!InstallEncoderService(Combine(installDir, kServiceExeName), log, error))
	{
		RollbackInstall(updating, installDir, copied, addedPackages, serviceExisted, serviceCreated,
						createdDirectory, trust, log);
		result.message = FailureMessage(error, updating);
		return result;
	}
	if (!StartEncoderService(log, error))
	{
		RollbackInstall(updating, installDir, copied, addedPackages, serviceExisted, serviceCreated,
						createdDirectory, trust, log);
		result.message = FailureMessage(error, updating);
		return result;
	}

	if (!CreateStartMenuShortcut(Combine(installDir, kConfigExeName), installDir, error))
	{
		RollbackInstall(updating, installDir, copied, addedPackages, serviceExisted, serviceCreated,
						createdDirectory, trust, log);
		result.message = FailureMessage(error, updating);
		return result;
	}

	InstallInfo info;
	info.installDir = installDir;
	info.version = ReadDriverVersion(Combine(payloadDir, kInfName));
	if (trust.added || (!existing.trustedCertificate.empty() &&
						CompareStringOrdinal(existing.trustedCertificate.c_str(), -1,
											 trust.thumbprint.c_str(), -1, TRUE) == CSTR_EQUAL))
	{
		info.trustedCertificate = trust.thumbprint;
	}
	if (!WriteInstallInfo(info, InstalledSize(installDir), error))
	{
		RollbackInstall(updating, installDir, copied, addedPackages, serviceExisted, serviceCreated,
						createdDirectory, trust, log);
		result.message = FailureMessage(error, updating);
		return result;
	}

	if (!existing.trustedCertificate.empty() &&
		CompareStringOrdinal(existing.trustedCertificate.c_str(), -1, trust.thumbprint.c_str(), -1,
							 TRUE) != CSTR_EQUAL)
	{
		std::wstring ignored;
		if (RemoveTrustedCertificate(existing.trustedCertificate, ignored))
		{
			log(L"The previous driver certificate was removed from the machine Root store.");
		}
		else
		{
			log(L"warning: " + ignored);
		}
	}

	std::vector<std::wstring> installedPackages;
	if (!ListOpenA2dpPackages(installedPackages, log, error) || installedPackages.empty())
	{
		if (error.empty())
		{
			error = L"The driver package was not found after installation.";
		}
		RollbackInstall(updating, installDir, copied, addedPackages, serviceExisted, serviceCreated,
						createdDirectory, trust, log);
		result.message = FailureMessage(error, updating);
		return result;
	}

	std::vector<Tools::SinkNode> sinksAfter;
	{
		std::string readError;
		if (Tools::EnumerateSinkNodes(sinksAfter, readError))
		{
			for (const Tools::SinkNode& before : sinksBefore)
			{
				if (before.driver != Tools::SinkDriver::Windows)
				{
					continue;
				}
				for (const Tools::SinkNode& after : sinksAfter)
				{
					if (after.reference == before.reference &&
						after.driver == Tools::SinkDriver::OpenA2dp)
					{
						log(L"warning: " + SinkName(after) +
							L" moved to OpenA2DP during setup; setup does not switch headsets.");
					}
				}
			}
		}
	}

	int reloaded = 0;
	int reloadFailed = 0;
	bool serviceWarning = false;
	bool reloadNeeded = false;
	for (const Tools::SinkNode& sink : sinksBefore)
	{
		if (sink.driver == Tools::SinkDriver::OpenA2dp)
		{
			reloadNeeded = true;
			break;
		}
	}
	if (updating && reloadNeeded)
	{
		// The encoder service opens the driver's control devices. Stop it while the new package is
		// applied, or the old image stays loaded and the sinks come back on the previous driver.
		log(L"Stopping the encoder service so the driver can unload...");
		if (!StopEncoderService(log, error, true))
		{
			log(L"warning: " + error);
		}
	}
	if (updating)
	{
		for (const Tools::SinkNode& sink : sinksBefore)
		{
			if (sink.driver != Tools::SinkDriver::OpenA2dp)
			{
				continue;
			}
			Tools::SwitchReport report;
			std::string switchError;
			const Tools::SwitchOutcome outcome =
				Tools::ReloadSinkDriver(sink.reference, report, switchError);
			if (outcome == Tools::SwitchOutcome::Switched)
			{
				++reloaded;
				log(SinkName(sink) + L" restarted on the updated driver.");
				if (!report.pausedPlayer.empty())
				{
					log(L"Paused " + Tools::Widen(report.pausedPlayer) +
						L" while the headset restarted.");
				}
			}
			else
			{
				++reloadFailed;
				log(L"warning: " + SinkName(sink) + L" kept the previous driver: " +
					Tools::Widen(switchError) + L". Reconnect the headset after setup.");
			}
		}
	}
	if (updating && reloadNeeded)
	{
		log(L"Starting the encoder service...");
		std::wstring serviceError;
		if (!StartEncoderService(log, serviceError))
		{
			log(L"warning: " + serviceError);
			serviceWarning = true;
		}
	}

	bool oldPackagesRemoved = false;
	bool oldPackageWarning = false;
	bool oldPackageRestart = false;
	if (updating && info.version != L"1.0")
	{
		bool currentFound = false;
		const std::vector<std::wstring> oldPackages =
			OldDriverPackages(installedPackages, info.version, currentFound);
		if (!currentFound)
		{
			log(L"warning: the current driver package version could not be identified; the driver "
				L"store was left unchanged.");
			oldPackageWarning = true;
		}
		else if (!oldPackages.empty())
		{
			log(L"Removing the previous driver package from the driver store...");
			for (const std::wstring& package : oldPackages)
			{
				bool restart = false;
				if (!DeleteDriverPackageFromStore(package, restart, log, error))
				{
					log(L"warning: " + error);
					oldPackageWarning = true;
				}
				oldPackageRestart = oldPackageRestart || restart;
			}

			std::vector<std::wstring> remaining;
			if (!ListOpenA2dpPackages(remaining, log, error))
			{
				log(L"warning: " + error);
				oldPackageWarning = true;
			}
			else
			{
				bool remainingCurrent = false;
				const std::vector<std::wstring> left =
					OldDriverPackages(remaining, info.version, remainingCurrent);
				if (!remainingCurrent || !left.empty())
				{
					log(L"warning: the previous driver package is still in the driver store.");
					oldPackageWarning = true;
				}
				else
				{
					oldPackagesRemoved = true;
					oldPackageRestart = false;
				}
			}
		}
	}
	else if (updating)
	{
		log(L"warning: the driver package version could not be read; the driver store was left "
			L"unchanged.");
		oldPackageWarning = true;
	}
	result.restartRequired = oldPackageRestart;

	result.success = true;
	if (updating)
	{
		result.message = L"OpenA2DP was updated.";
		if (reloaded > 0)
		{
			result.message += L"\n\n" + std::to_wstring(reloaded) +
							  L" headset(s) already using OpenA2DP were restarted to load the new driver.";
		}
		if (reloadFailed > 0)
		{
			result.message += L"\n\n" + std::to_wstring(reloadFailed) +
							  L" headset(s) kept the previous driver; see the log and reconnect them.";
		}
		if (serviceWarning)
		{
			result.message += L"\n\nThe encoder service could not be restarted; start it or restart "
							  L"Windows.";
		}
		if (oldPackagesRemoved)
		{
			result.message += L"\n\nThe previous driver package was removed from the driver store.";
		}
		else if (oldPackageWarning)
		{
			result.message += L"\n\nThe previous driver package could not be removed from the driver "
							  L"store; see the log above.";
		}
		if (oldPackageRestart)
		{
			result.message += L"\n\nRestart Windows to finish removing the previous driver package.";
		}
		result.message += L"\n\nHeadsets on the Windows driver were not touched.";
	}
	else
	{
		result.message = L"OpenA2DP is installed. No headset was switched; use OpenA2DP Settings "
						 L"to choose the headsets you want on it.";
	}
	log(updating ? L"Update completed." : L"Installation completed.");
	return result;
}

Result Uninstall(const std::wstring& installDir, bool removeSettings, const Log& log)
{
	Result result;
	InstallInfo info;
	const bool registered = ReadInstallInfo(info);
	const std::wstring directory = !info.installDir.empty() ? info.installDir : installDir;
	if (directory.empty())
	{
		result.message = L"OpenA2DP is not installed.";
		return result;
	}

	std::wstring running;
	if (IsToolRunning(running))
	{
		result.message = running + L" is running. Close the OpenA2DP tools, then run setup again.";
		return result;
	}

	std::wstring error;
	log(L"Stopping the encoder service...");
	if (!StopEncoderService(log, error))
	{
		result.message = error;
		return result;
	}

	log(L"Returning headsets using OpenA2DP to the Windows driver...");
	std::vector<Tools::SinkNode> sinks;
	std::string readError;
	if (!Tools::EnumerateSinkNodes(sinks, readError))
	{
		log(L"warning: the A2DP sink list could not be read: " + Tools::Widen(readError));
	}
	else
	{
		for (const Tools::SinkNode& sink : sinks)
		{
			if (sink.driver != Tools::SinkDriver::OpenA2dp)
			{
				continue;
			}
			Tools::SwitchReport report;
			std::string switchError;
			const Tools::SwitchOutcome outcome =
				Tools::SwitchSinkDriver(sink.reference, Tools::SinkDriver::Windows, report, switchError);
			if (outcome == Tools::SwitchOutcome::Switched)
			{
				log(SinkName(sink) + L" is back on the Windows driver.");
				if (!report.pausedPlayer.empty())
				{
					log(L"Paused " + Tools::Widen(report.pausedPlayer) + L" while its headset moved back.");
				}
			}
			else
			{
				log(L"warning: " + SinkName(sink) + L" could not be moved back: " +
					Tools::Widen(switchError));
			}
		}
	}

	log(L"Removing OpenA2DP from the driver store...");
	bool restartRequired = false;
	if (!DeleteDriverPackages(restartRequired, log, error))
	{
		if (registered && FileExists(Combine(directory, kServiceExeName)))
		{
			std::wstring ignored;
			(void)StartEncoderService(log, ignored);
		}
		result.message = L"OpenA2DP could not be removed: " + error +
						 L"\n\nThe installation was kept so you can retry. Close anything using the "
						 L"driver, then run setup again.";
		result.restartRequired = restartRequired;
		return result;
	}

	std::wstring cleanupError;
	bool cleanupWarning = false;
	if (!DeleteEncoderService(log, cleanupError))
	{
		log(L"warning: " + cleanupError);
		cleanupWarning = true;
	}
	if (!DeleteDriverService(log, cleanupError))
	{
		log(L"warning: " + cleanupError);
		cleanupWarning = true;
	}
	bool imageRestart = false;
	if (!RemoveDriverImage(imageRestart, log, cleanupError))
	{
		log(L"warning: " + cleanupError);
		cleanupWarning = true;
	}
	restartRequired = restartRequired || imageRestart;

	int remaining = 0;
	std::vector<Tools::SinkNode> sinksAfter;
	if (Tools::EnumerateSinkNodes(sinksAfter, readError))
	{
		for (const Tools::SinkNode& sink : sinksAfter)
		{
			if (sink.driver == Tools::SinkDriver::OpenA2dp)
			{
				++remaining;
				log(L"warning: " + SinkName(sink) +
					L" is still on OpenA2DP; Windows will finish the move back after a restart.");
			}
		}
	}
	if (remaining > 0)
	{
		restartRequired = true;
	}

	if (!RemoveStartMenuShortcut(cleanupError))
	{
		log(L"warning: " + cleanupError);
		cleanupWarning = true;
	}
	const std::wstring certificate = info.trustedCertificate;
	if (!RemoveInstallInfo(cleanupError))
	{
		log(L"warning: " + cleanupError);
		cleanupWarning = true;
	}
	if (!certificate.empty())
	{
		if (RemoveTrustedCertificate(certificate, cleanupError))
		{
			log(L"Driver signing certificate removed from the machine Root store.");
		}
		else
		{
			log(L"warning: " + cleanupError);
			cleanupWarning = true;
		}
	}

	const wchar_t* names[] = {kSysName, kInfName, kCatName, kServiceExeName, kControlExeName,
							  kConfigExeName};
	for (const wchar_t* name : names)
	{
		const std::wstring path = Combine(directory, name);
		if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
		{
			continue;
		}
		if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
		{
			restartRequired = true;
			log(L"warning: " + std::wstring(name) + L" is held open; removal is scheduled for the next restart.");
		}
		else
		{
			log(L"warning: could not delete " + path);
			cleanupWarning = true;
		}
	}

	if (removeSettings)
	{
		log(L"Removing saved headset settings...");
		if (RemoveDirectoryTree(ProgramDataDirectory()))
		{
			log(L"Saved headset settings removed.");
		}
		else
		{
			log(L"warning: the saved headset settings could not be removed.");
			cleanupWarning = true;
		}
	}
	else
	{
		log(L"Saved headset settings kept.");
	}

	result.success = true;
	result.restartRequired = restartRequired;
	result.message = restartRequired
		? L"OpenA2DP was removed. Restart Windows to finish removing the driver package."
		: L"OpenA2DP was removed. Every connected headset is back on the Windows driver.";
	if (remaining > 0)
	{
		result.message += L"\n\nA headset could not be moved back while it was in use; it will use the "
						  L"Windows driver after the restart.";
	}
	if (cleanupWarning)
	{
		result.message += L"\n\nSome cleanup steps could not be completed; see the log above.";
	}
	log(result.message);
	return result;
}

bool CleanupInstalledFiles(const std::wstring& installDir, const Log& log)
{
	if (installDir.empty())
	{
		return true;
	}
	const wchar_t* names[] = {kSysName,      kInfName,         kCatName,
							  kServiceExeName, kControlExeName, kConfigExeName, kSetupExeName};
	bool ok = true;
	for (const wchar_t* name : names)
	{
		const std::wstring path = Combine(installDir, name);
		if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
		{
			continue;
		}
		if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
		{
			log(L"warning: " + std::wstring(name) + L" is held open; removal is scheduled for the next restart.");
			continue;
		}
		ok = false;
		log(L"warning: could not delete " + path);
	}
	if (!RemoveDirectoryW(installDir.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND &&
		GetLastError() != ERROR_DIR_NOT_EMPTY)
	{
		ok = false;
		log(L"warning: the install folder could not be removed: " + installDir);
	}
	return ok;
}

} // namespace OpenA2dp::Setup
