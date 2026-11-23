#include "SetupService.h"

#include "SetupEnvironment.h"
#include "SetupNames.h"

#include <windows.h>

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

SC_HANDLE OpenManager(DWORD access, std::wstring& error)
{
	const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, access);
	if (manager == nullptr)
	{
		error = L"Could not open the service control manager: " +
				SystemMessage(GetLastError());
	}
	return manager;
}

bool WaitForState(SC_HANDLE service, DWORD wanted, DWORD timeoutMs, DWORD& processId)
{
	DWORD waited = 0;
	while (waited <= timeoutMs)
	{
		SERVICE_STATUS_PROCESS status = {};
		DWORD needed = 0;
		if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
								  sizeof(status), &needed))
		{
			return false;
		}
		if (status.dwCurrentState == wanted)
		{
			return true;
		}
		if (status.dwCurrentState == SERVICE_STOPPED && wanted != SERVICE_STOPPED)
		{
			return false;
		}
		processId = status.dwProcessId;
		Sleep(200);
		waited += 200;
	}
	return false;
}

} // namespace

bool EncoderServiceExists()
{
	std::wstring error;
	const SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT, error);
	if (manager == nullptr)
	{
		return false;
	}
	const SC_HANDLE service =
		OpenServiceW(manager, kEncoderServiceName, SERVICE_QUERY_STATUS);
	const bool exists = service != nullptr;
	if (service != nullptr)
	{
		CloseServiceHandle(service);
	}
	CloseServiceHandle(manager);
	return exists;
}

bool QueryEncoderServiceDirectory(std::wstring& directory)
{
	directory.clear();
	std::wstring error;
	const SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT, error);
	if (manager == nullptr)
	{
		return false;
	}
	const SC_HANDLE service =
		OpenServiceW(manager, kEncoderServiceName, SERVICE_QUERY_CONFIG);
	if (service == nullptr)
	{
		CloseServiceHandle(manager);
		return false;
	}

	DWORD needed = 0;
	(void)QueryServiceConfigW(service, nullptr, 0, &needed);
	if (needed == 0)
	{
		CloseServiceHandle(service);
		CloseServiceHandle(manager);
		return false;
	}
	std::vector<BYTE> buffer(needed);
	const auto* config = reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(buffer.data());
	if (!QueryServiceConfigW(service, const_cast<LPQUERY_SERVICE_CONFIGW>(config), needed, &needed))
	{
		CloseServiceHandle(service);
		CloseServiceHandle(manager);
		return false;
	}
	std::wstring path = config->lpBinaryPathName != nullptr ? config->lpBinaryPathName : L"";
	if (path.size() >= 2 && path.front() == L'"')
	{
		const size_t close = path.find(L'"', 1);
		path = close == std::wstring::npos ? path.substr(1) : path.substr(1, close - 1);
	}
	const size_t slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
	{
		CloseServiceHandle(service);
		CloseServiceHandle(manager);
		return false;
	}
	directory = path.substr(0, slash);
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	return !directory.empty();
}

bool StopEncoderService(const Log& log, std::wstring& error, bool keepRecovery)
{
	error.clear();
	const SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT, error);
	if (manager == nullptr)
	{
		return false;
	}
	const SC_HANDLE service = OpenServiceW(manager, kEncoderServiceName,
										   SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
	if (service == nullptr)
	{
		const DWORD code = GetLastError();
		CloseServiceHandle(manager);
		if (code == ERROR_SERVICE_DOES_NOT_EXIST)
		{
			log(L"The encoder service is not installed.");
			return true;
		}
		error = L"Could not open the encoder service: " + SystemMessage(code);
		return false;
	}

	if (!keepRecovery)
	{
		// A stop must not be undone by the restart-on-failure policy while the files are replaced.
		SERVICE_FAILURE_ACTIONSW noActions = {};
		noActions.dwResetPeriod = 0;
		noActions.cActions = 0;
		(void)ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &noActions);
	}

	SERVICE_STATUS status = {};
	DWORD processId = 0;
	if (!ControlService(service, SERVICE_CONTROL_STOP, &status))
	{
		const DWORD code = GetLastError();
		if (code != ERROR_SERVICE_NOT_ACTIVE)
		{
			error = L"The encoder service could not be stopped: " + SystemMessage(code);
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			return false;
		}
	}
	if (!WaitForState(service, SERVICE_STOPPED, 15000, processId))
	{
		if (processId != 0)
		{
			log(L"The encoder service did not stop in time; ending its process.");
			const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
			if (process != nullptr)
			{
				(void)TerminateProcess(process, 1);
				(void)WaitForSingleObject(process, 5000);
				CloseHandle(process);
			}
		}
		if (!WaitForState(service, SERVICE_STOPPED, 5000, processId))
		{
			error = L"The encoder service did not stop; close anything using it and try again.";
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			return false;
		}
	}
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	log(L"Encoder service stopped.");
	return true;
}

bool InstallEncoderService(const std::wstring& exePath, const Log& log, std::wstring& error)
{
	error.clear();
	const SC_HANDLE manager = OpenManager(SC_MANAGER_CREATE_SERVICE, error);
	if (manager == nullptr)
	{
		return false;
	}
	const std::wstring binary = Quoted(exePath);
	SC_HANDLE service = CreateServiceW(
		manager, kEncoderServiceName, kEncoderServiceDisplayName, SERVICE_ALL_ACCESS,
		SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binary.c_str(), nullptr,
		nullptr, nullptr, nullptr, nullptr);
	const bool created = service != nullptr;
	if (service == nullptr)
	{
		if (GetLastError() != ERROR_SERVICE_EXISTS)
		{
			error = L"The encoder service could not be created: " + SystemMessage(GetLastError());
			CloseServiceHandle(manager);
			return false;
		}
		service = OpenServiceW(manager, kEncoderServiceName, SERVICE_ALL_ACCESS);
		if (service == nullptr)
		{
			error = L"The existing encoder service could not be opened: " + SystemMessage(GetLastError());
			CloseServiceHandle(manager);
			return false;
		}
		if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
								  binary.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr,
								  kEncoderServiceDisplayName))
		{
			error = L"The encoder service could not be updated: " + SystemMessage(GetLastError());
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			return false;
		}
	}

	SERVICE_DESCRIPTIONW description = {};
	description.lpDescription = const_cast<LPWSTR>(
		L"Encodes AAC-LC, aptX, aptX LL, aptX HD and LDAC for the OpenA2DP A2DP source driver.");
	(void)ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

	SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000},
							{SC_ACTION_RESTART, 5000}};
	SERVICE_FAILURE_ACTIONSW failure = {};
	failure.dwResetPeriod = 86400;
	failure.cActions = ARRAYSIZE(actions);
	failure.lpsaActions = actions;
	(void)ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	log(created ? L"Encoder service created." : L"Encoder service updated.");
	return true;
}

bool StartEncoderService(const Log& log, std::wstring& error)
{
	error.clear();
	const SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT, error);
	if (manager == nullptr)
	{
		return false;
	}
	const SC_HANDLE service =
		OpenServiceW(manager, kEncoderServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
	if (service == nullptr)
	{
		error = L"The encoder service is not installed: " + SystemMessage(GetLastError());
		CloseServiceHandle(manager);
		return false;
	}
	if (!StartServiceW(service, 0, nullptr))
	{
		const DWORD code = GetLastError();
		if (code != ERROR_SERVICE_ALREADY_RUNNING)
		{
			error = L"The encoder service could not start: " + SystemMessage(code);
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			return false;
		}
	}

	DWORD waited = 0;
	SERVICE_STATUS_PROCESS status = {};
	while (waited <= 15000)
	{
		DWORD needed = 0;
		if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
								  sizeof(status), &needed))
		{
			error = L"The encoder service state could not be read: " + SystemMessage(GetLastError());
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			return false;
		}
		if (status.dwCurrentState == SERVICE_RUNNING)
		{
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			log(L"Encoder service started.");
			return true;
		}
		if (status.dwCurrentState == SERVICE_STOPPED)
		{
			error = L"The encoder service stopped while starting (error " +
					std::to_wstring(status.dwWin32ExitCode) + L"). See the Windows event log.";
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			return false;
		}
		Sleep(200);
		waited += 200;
	}
	error = L"The encoder service did not reach the running state within 15 seconds.";
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	return false;
}

bool DeleteEncoderService(const Log& log, std::wstring& error)
{
	error.clear();
	const SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT, error);
	if (manager == nullptr)
	{
		return false;
	}
	const SC_HANDLE service = OpenServiceW(manager, kEncoderServiceName, DELETE | SERVICE_QUERY_STATUS);
	if (service == nullptr)
	{
		const DWORD code = GetLastError();
		CloseServiceHandle(manager);
		if (code == ERROR_SERVICE_DOES_NOT_EXIST)
		{
			return true;
		}
		error = L"The encoder service could not be opened for removal: " + SystemMessage(code);
		return false;
	}
	const bool deleted = DeleteService(service) != FALSE;
	const DWORD code = GetLastError();
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	if (!deleted && code != ERROR_SERVICE_MARKED_FOR_DELETE && code != ERROR_SERVICE_DOES_NOT_EXIST)
	{
		error = L"The encoder service could not be removed: " + SystemMessage(code);
		return false;
	}
	log(L"Encoder service removed.");
	return true;
}

} // namespace OpenA2dp::Setup
