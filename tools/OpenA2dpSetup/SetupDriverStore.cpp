#include "SetupDriverStore.h"

#include "SetupEnvironment.h"
#include "SetupNames.h"

#include "common/SetupEnumeration.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <thread>
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

std::wstring System32(const wchar_t* name)
{
	wchar_t system[MAX_PATH] = {};
	const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
	if (length == 0 || length >= ARRAYSIZE(system))
	{
		return name;
	}
	return std::wstring(system, length) + L"\\" + name;
}

std::wstring WidenBytes(const std::string& text)
{
	if (text.empty())
	{
		return {};
	}
	int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
									 static_cast<int>(text.size()), nullptr, 0);
	UINT codePage = CP_UTF8;
	if (length <= 0)
	{
		codePage = CP_ACP;
		length = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	}
	if (length <= 0)
	{
		return {};
	}
	std::wstring result(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
	return result;
}

void LogOutput(const Log& log, const std::string& output)
{
	size_t begin = 0;
	while (begin < output.size())
	{
		const size_t end = output.find('\n', begin);
		std::string line = output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		begin = end == std::string::npos ? output.size() : end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
		{
			line.pop_back();
		}
		if (!line.empty())
		{
			log(L"    " + WidenBytes(line));
		}
	}
}

bool RunTool(const std::wstring& executable, const std::wstring& arguments, DWORD timeoutMs,
			 DWORD& exitCode, std::string& output, std::wstring& error)
{
	output.clear();
	exitCode = 0;
	SECURITY_ATTRIBUTES attributes = {};
	attributes.nLength = sizeof(attributes);
	attributes.bInheritHandle = TRUE;
	HANDLE read = nullptr;
	HANDLE write = nullptr;
	if (!CreatePipe(&read, &write, &attributes, 0))
	{
		error = L"The driver-store command could not be started: " + SystemMessage(GetLastError());
		return false;
	}
	SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startup.hStdOutput = write;
	startup.hStdError = write;

	PROCESS_INFORMATION process = {};
	std::wstring command = L"\"" + executable + L"\"";
	if (!arguments.empty())
	{
		command += L" " + arguments;
	}
	std::vector<wchar_t> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back(L'\0');
	const BOOL created =
		CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
					   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process);
	CloseHandle(write);
	if (!created)
	{
		error = L"The driver-store command could not be started: " + SystemMessage(GetLastError());
		CloseHandle(read);
		return false;
	}

	std::string captured;
	std::thread reader([&] {
		char buffer[4096];
		DWORD bytes = 0;
		while (ReadFile(read, buffer, sizeof(buffer), &bytes, nullptr) && bytes > 0)
		{
			captured.append(buffer, bytes);
		}
	});

	const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMs);
	const bool timedOut = wait == WAIT_TIMEOUT;
	if (timedOut)
	{
		(void)TerminateProcess(process.hProcess, 1);
		(void)WaitForSingleObject(process.hProcess, 5000);
	}
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	reader.join();
	CloseHandle(read);
	output = std::move(captured);
	if (timedOut)
	{
		error = L"The driver-store command did not finish within " + std::to_wstring(timeoutMs / 1000) +
				L" seconds.";
		return false;
	}
	return true;
}

bool ListPackages(std::vector<std::wstring>& published, const Log& log, std::wstring& error)
{
	DWORD exitCode = 0;
	std::string output;
	if (!RunTool(System32(L"pnputil.exe"), L"/enum-drivers", 60000, exitCode, output, error))
	{
		return false;
	}
	if (exitCode != 0)
	{
		LogOutput(log, output);
		error = L"pnputil /enum-drivers failed (exit " + std::to_wstring(exitCode) + L").";
		return false;
	}
	published = Tools::ParseOpenA2dpPackages(output);
	if (published.empty())
	{
		log(L"No OpenA2DP package is in the driver store.");
	}
	else
	{
		log(L"OpenA2DP packages in the driver store:");
		for (const std::wstring& package : published)
		{
			log(L"    " + package);
		}
	}
	return true;
}

bool DeleteOnePackage(const std::wstring& published, bool uninstallDevices, bool& restartRequired,
					  const Log& log, std::wstring& error)
{
	const std::wstring options = uninstallDevices ? L" /uninstall /force" : L" /force";
	const std::wstring arguments = L"/delete-driver " + published + options;
	log(L"pnputil /delete-driver " + published + options);
	DWORD exitCode = 0;
	std::string output;
	if (!RunTool(System32(L"pnputil.exe"), arguments, 180000, exitCode, output, error))
	{
		LogOutput(log, output);
		return false;
	}
	LogOutput(log, output);
	if (exitCode == 3010)
	{
		restartRequired = true;
	}
	if (exitCode != 0 && exitCode != 259 && exitCode != 3010)
	{
		error = L"The driver package " + published + L" could not be removed (exit " +
				std::to_wstring(exitCode) + L").";
		return false;
	}
	return true;
}

} // namespace

bool ListOpenA2dpPackages(std::vector<std::wstring>& published, const Log& log, std::wstring& error)
{
	return ListPackages(published, log, error);
}

bool StageDriverPackage(const std::wstring& infPath, const Log& log, std::wstring& error)
{
	const std::wstring arguments = L"/add-driver " + Quoted(infPath);
	log(L"pnputil /add-driver (staging only, no device is touched)");
	DWORD exitCode = 0;
	std::string output;
	if (!RunTool(System32(L"pnputil.exe"), arguments, 120000, exitCode, output, error))
	{
		LogOutput(log, output);
		return false;
	}
	LogOutput(log, output);
	if (exitCode != 0 && exitCode != 259 && exitCode != 3010)
	{
		error = L"The driver package could not be added to the driver store (exit " +
				std::to_wstring(exitCode) + L").";
		return false;
	}

	std::vector<std::wstring> packages;
	if (!ListPackages(packages, log, error))
	{
		return false;
	}
	if (packages.empty())
	{
		error = L"The driver package was accepted, but it is not in the driver store.";
		return false;
	}
	log(L"The driver package is staged. No headset was switched.");
	return true;
}

bool DeleteDriverPackage(const std::wstring& published, bool& restartRequired, const Log& log,
						 std::wstring& error)
{
	return DeleteOnePackage(published, true, restartRequired, log, error);
}

bool DeleteDriverPackageFromStore(const std::wstring& published, bool& restartRequired,
								  const Log& log, std::wstring& error)
{
	return DeleteOnePackage(published, false, restartRequired, log, error);
}

bool DeleteDriverPackages(bool& restartRequired, const Log& log, std::wstring& error)
{
	std::vector<std::wstring> packages;
	if (!ListPackages(packages, log, error))
	{
		return false;
	}
	if (packages.empty())
	{
		return true;
	}
	for (const std::wstring& package : packages)
	{
		if (!DeleteDriverPackage(package, restartRequired, log, error))
		{
			return false;
		}
	}

	std::vector<std::wstring> remaining;
	if (!ListPackages(remaining, log, error))
	{
		return false;
	}
	if (!remaining.empty())
	{
		if (restartRequired)
		{
			log(L"The package is marked for removal; a restart finishes it.");
			return true;
		}
		error = L"OpenA2DP package " + remaining.front() + L" is still in the driver store.";
		return false;
	}
	return true;
}

bool DeleteDriverService(const Log& log, std::wstring& error)
{
	error.clear();
	const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (manager == nullptr)
	{
		error = L"The service control manager could not be opened: " + SystemMessage(GetLastError());
		return false;
	}
	SC_HANDLE service = OpenServiceW(manager, kDriverServiceName,
									 DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
	if (service == nullptr)
	{
		const DWORD code = GetLastError();
		CloseServiceHandle(manager);
		if (code == ERROR_SERVICE_DOES_NOT_EXIST)
		{
			return true;
		}
		error = L"The OpenA2DP driver service could not be opened: " + SystemMessage(code);
		return false;
	}

	SERVICE_STATUS status = {};
	if (QueryServiceStatus(service, &status) &&
		(status.dwCurrentState == SERVICE_RUNNING || status.dwCurrentState == SERVICE_START_PENDING))
	{
		(void)ControlService(service, SERVICE_CONTROL_STOP, &status);
		for (DWORD waited = 0; waited < 10000; waited += 200)
		{
			if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED)
			{
				break;
			}
			Sleep(200);
		}
	}
	if (!DeleteService(service))
	{
		const DWORD code = GetLastError();
		if (code != ERROR_SERVICE_DOES_NOT_EXIST && code != ERROR_SERVICE_MARKED_FOR_DELETE)
		{
			error = L"The OpenA2DP driver service could not be removed: " + SystemMessage(code);
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			return false;
		}
	}
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	log(L"OpenA2DP driver service removed.");
	return true;
}

bool RemoveDriverImage(bool& restartRequired, const Log& log, std::wstring& error)
{
	error.clear();
	wchar_t system[MAX_PATH] = {};
	const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
	if (length == 0 || length >= ARRAYSIZE(system))
	{
		error = L"The Windows directory could not be located.";
		return false;
	}
	const std::wstring path = std::wstring(system, length) + L"\\drivers\\" + kSysName;
	if (!FileExists(path))
	{
		return true;
	}
	if (DeleteFileW(path.c_str()))
	{
		log(L"OpenA2dp.sys removed from the drivers folder.");
		return true;
	}
	const DWORD code = GetLastError();
	if ((code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) &&
		MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
	{
		restartRequired = true;
		log(L"OpenA2dp.sys is loaded; its removal is scheduled for the next restart.");
		return true;
	}
	error = L"OpenA2dp.sys could not be removed: " + SystemMessage(code);
	return false;
}

} // namespace OpenA2dp::Setup
