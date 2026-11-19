#include "SetupShortcut.h"

#include "SetupEnvironment.h"
#include "SetupNames.h"

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace OpenA2dp::Setup {
namespace {

std::wstring HresultText(HRESULT result)
{
	wchar_t text[16] = {};
	swprintf_s(text, L"0x%08lX", static_cast<unsigned long>(result));
	return text;
}

} // namespace

bool CreateStartMenuShortcut(const std::wstring& target, const std::wstring& workingDirectory,
							 std::wstring& error)
{
	error.clear();
	const std::wstring directory = StartMenuDirectory();
	if (directory.empty())
	{
		error = L"The common Start Menu folder is not available.";
		return false;
	}
	const int created = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
	if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS)
	{
		error = L"The Start Menu folder could not be created (" + std::to_wstring(created) + L").";
		return false;
	}

	IShellLinkW* link = nullptr;
	HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
									  reinterpret_cast<void**>(&link));
	if (FAILED(result) || link == nullptr)
	{
		error = L"The Start Menu shortcut could not be created (" + HresultText(result) + L").";
		return false;
	}
	link->SetPath(target.c_str());
	link->SetWorkingDirectory(workingDirectory.c_str());
	link->SetDescription(L"Open the OpenA2DP settings window");
	link->SetIconLocation(target.c_str(), 0);

	IPersistFile* file = nullptr;
	result = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
	if (SUCCEEDED(result) && file != nullptr)
	{
		const std::wstring path = directory + L"\\" + kShortcutName;
		result = file->Save(path.c_str(), TRUE);
		file->Release();
	}
	link->Release();
	if (FAILED(result))
	{
		error = L"The Start Menu shortcut could not be saved (" + HresultText(result) + L").";
		return false;
	}
	return true;
}

bool RemoveStartMenuShortcut(std::wstring& error)
{
	error.clear();
	const std::wstring directory = StartMenuDirectory();
	if (directory.empty())
	{
		return true;
	}
	const std::wstring path = directory + L"\\" + kShortcutName;
	if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
	{
		error = L"The Start Menu shortcut could not be removed.";
		return false;
	}
	if (!RemoveDirectoryW(directory.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND &&
		GetLastError() != ERROR_DIR_NOT_EMPTY)
	{
		error = L"The OpenA2DP Start Menu folder could not be removed.";
		return false;
	}
	return true;
}

} // namespace OpenA2dp::Setup
