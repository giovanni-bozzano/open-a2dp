#include "SetupRegistry.h"

#include "SetupEnvironment.h"
#include "SetupNames.h"
#include "SetupService.h"

#include <windows.h>

#include <vector>

namespace OpenA2dp::Setup {
namespace {

constexpr REGSAM kRegistryView = KEY_WOW64_64KEY;

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

bool ReadString(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring& value)
{
	value.clear();
	HKEY handle = nullptr;
	if (RegOpenKeyExW(root, key, 0, KEY_READ | kRegistryView, &handle) != ERROR_SUCCESS)
	{
		return false;
	}
	DWORD type = 0;
	DWORD size = 0;
	LONG status = RegQueryValueExW(handle, name, nullptr, &type, nullptr, &size);
	if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
		size < sizeof(wchar_t))
	{
		RegCloseKey(handle);
		return false;
	}
	std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
	status = RegQueryValueExW(handle, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &size);
	RegCloseKey(handle);
	if (status != ERROR_SUCCESS)
	{
		return false;
	}
	value.assign(buffer.data());
	return true;
}

bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value)
{
	const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
	return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes) ==
		   ERROR_SUCCESS;
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value)
{
	return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
		   ERROR_SUCCESS;
}

bool CreateKey(HKEY root, const wchar_t* path, HKEY& key, std::wstring& error)
{
	const LONG status =
		RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE | kRegistryView, nullptr, &key, nullptr);
	if (status != ERROR_SUCCESS)
	{
		error = L"Could not open " + std::wstring(path) + L": " + SystemMessage(static_cast<DWORD>(status));
		return false;
	}
	return true;
}

bool ReadInstallDirFromRegistry(std::wstring& directory)
{
	return ReadString(HKEY_LOCAL_MACHINE, kAppKey, L"InstallDir", directory) && !directory.empty();
}

} // namespace

bool ReadInstallInfo(InstallInfo& info)
{
	info = {};
	if (!ReadInstallDirFromRegistry(info.installDir))
	{
		std::wstring serviceDirectory;
		if (QueryEncoderServiceDirectory(serviceDirectory))
		{
			info.installDir = serviceDirectory;
		}
	}
	if (info.installDir.empty())
	{
		return false;
	}
	ReadString(HKEY_LOCAL_MACHINE, kAppKey, L"Version", info.version);
	ReadString(HKEY_LOCAL_MACHINE, kAppKey, L"TrustedCertificate", info.trustedCertificate);
	if (info.version.empty())
	{
		info.version = ReadDriverVersion(info.installDir + L"\\" + kInfName);
	}
	return true;
}

bool WriteInstallInfo(const InstallInfo& info, ULONGLONG estimatedSize, std::wstring& error)
{
	error.clear();
	HKEY app = nullptr;
	if (!CreateKey(HKEY_LOCAL_MACHINE, kAppKey, app, error))
	{
		return false;
	}
	bool ok = WriteString(app, L"InstallDir", info.installDir) &&
			  WriteString(app, L"Version", info.version.empty() ? std::wstring(L"1.0") : info.version);
	if (info.trustedCertificate.empty())
	{
		(void)RegDeleteValueW(app, L"TrustedCertificate");
	}
	else
	{
		ok = WriteString(app, L"TrustedCertificate", info.trustedCertificate) && ok;
	}
	RegCloseKey(app);
	if (!ok)
	{
		error = L"The install information could not be written to the registry.";
		return false;
	}

	HKEY uninstall = nullptr;
	if (!CreateKey(HKEY_LOCAL_MACHINE, kUninstallKey, uninstall, error))
	{
		return false;
	}
	SYSTEMTIME now = {};
	GetLocalTime(&now);
	wchar_t date[16] = {};
	swprintf_s(date, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
	const DWORD sizeKb = estimatedSize / 1024 > 0xFFFFFFFFull ? 0xFFFFFFFFu
															  : static_cast<DWORD>(estimatedSize / 1024);
	ok = WriteString(uninstall, L"DisplayName", kProductName) &&
		 WriteString(uninstall, L"DisplayVersion", info.version.empty() ? std::wstring(L"1.0") : info.version) &&
		 WriteString(uninstall, L"Publisher", kProductName) &&
		 WriteString(uninstall, L"InstallLocation", info.installDir) &&
		 WriteString(uninstall, L"DisplayIcon", Quoted(info.installDir + L"\\" + kConfigExeName) + L",0") &&
		 WriteString(uninstall, L"UninstallString",
					 Quoted(info.installDir + L"\\" + kSetupExeName) + L" /uninstall") &&
		 WriteDword(uninstall, L"NoModify", 1) && WriteDword(uninstall, L"NoRepair", 1) &&
		 WriteDword(uninstall, L"EstimatedSize", sizeKb) && WriteString(uninstall, L"InstallDate", date);
	RegCloseKey(uninstall);
	if (!ok)
	{
		error = L"The Settings > Apps entry could not be written to the registry.";
	}
	return ok;
}

bool RemoveInstallInfo(std::wstring& error)
{
	error.clear();
	const LONG app = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kAppKey);
	const LONG uninstall = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
	const auto missing = [](LONG status) {
		return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
			   status == ERROR_PATH_NOT_FOUND;
	};
	if (!missing(app) || !missing(uninstall))
	{
		error = L"The OpenA2DP registry entries could not be removed.";
		return false;
	}
	return true;
}

} // namespace OpenA2dp::Setup
