#include "SetupEnvironment.h"

#include "SetupNames.h"

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <cctype>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <vector>

namespace OpenA2dp::Setup {
namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
	PWSTR path = nullptr;
	if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) || path == nullptr)
	{
		return {};
	}
	std::wstring result(path);
	CoTaskMemFree(path);
	return result;
}

std::wstring Trim(const std::wstring& text)
{
	size_t begin = 0;
	while (begin < text.size() && iswspace(text[begin]) != 0)
	{
		++begin;
	}
	size_t end = text.size();
	while (end > begin && iswspace(text[end - 1]) != 0)
	{
		--end;
	}
	return text.substr(begin, end - begin);
}

bool HasPrefixNoCase(const std::wstring& text, const std::wstring& prefix)
{
	if (prefix.size() > text.size())
	{
		return false;
	}
	return CompareStringOrdinal(text.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
								static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

bool IsAbsoluteDrivePath(const std::wstring& text)
{
	return text.size() >= 3 && iswalpha(text[0]) != 0 && text[1] == L':' &&
		   (text[2] == L'\\' || text[2] == L'/');
}

} // namespace

std::wstring DefaultInstallDir()
{
	const std::wstring programFiles = KnownFolder(FOLDERID_ProgramFiles);
	return programFiles.empty() ? std::wstring() : programFiles + L"\\" + kProductName;
}

std::wstring ProgramDataDirectory()
{
	const std::wstring programData = KnownFolder(FOLDERID_ProgramData);
	return programData.empty() ? std::wstring() : programData + L"\\OpenA2dp";
}

std::wstring StartMenuDirectory()
{
	const std::wstring programs = KnownFolder(FOLDERID_CommonPrograms);
	return programs.empty() ? std::wstring() : programs + L"\\" + kStartMenuFolder;
}

bool IsElevated()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
	{
		return false;
	}
	TOKEN_ELEVATION elevation = {};
	DWORD size = 0;
	const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE &&
						  elevation.TokenIsElevated != 0;
	CloseHandle(token);
	return elevated;
}

bool ValidateInstallDir(const std::wstring& text, std::wstring& normalized, std::wstring& error)
{
	normalized.clear();
	const std::wstring trimmed = Trim(text);
	if (trimmed.empty())
	{
		error = L"Choose a folder to install OpenA2DP in.";
		return false;
	}
	if (trimmed.find(L'"') != std::wstring::npos)
	{
		error = L"The install folder cannot contain a quotation mark.";
		return false;
	}
	for (const wchar_t character : trimmed)
	{
		if (character < 0x20)
		{
			error = L"The install folder contains an invalid character.";
			return false;
		}
	}
	if (PathIsRelativeW(trimmed.c_str()) || !IsAbsoluteDrivePath(trimmed))
	{
		error = L"Choose a full path, for example the Program Files folder followed by OpenA2DP.";
		return false;
	}

	std::vector<wchar_t> buffer(MAX_PATH);
	DWORD length = 0;
	for (;;)
	{
		length = GetFullPathNameW(trimmed.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
		if (length == 0)
		{
			error = L"The install folder is not a valid path.";
			return false;
		}
		if (length < buffer.size())
		{
			break;
		}
		buffer.resize(length + 1);
	}
	std::wstring full(buffer.data(), length);
	while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/'))
	{
		full.pop_back();
	}
	if (full.size() <= 3)
	{
		error = L"Choose a folder, not the root of a drive.";
		return false;
	}

	wchar_t volume[MAX_PATH] = {};
	if (!GetVolumePathNameW(full.c_str(), volume, ARRAYSIZE(volume)))
	{
		error = L"The install folder is not on a readable volume.";
		return false;
	}
	const UINT driveType = GetDriveTypeW(volume);
	if (driveType != DRIVE_FIXED)
	{
		error = L"Choose a folder on a local fixed drive; a service cannot run from a network or removable drive.";
		return false;
	}

	const DWORD attributes = GetFileAttributesW(full.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
	{
		error = L"That path is a file, not a folder.";
		return false;
	}

	wchar_t windows[MAX_PATH] = {};
	const UINT windowsLength = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
	if (windowsLength > 0 && windowsLength < ARRAYSIZE(windows))
	{
		const std::wstring windowsDir(windows, windowsLength);
		if (PathEqualsNoCase(full, windowsDir) || HasPrefixNoCase(full, windowsDir + L"\\"))
		{
			error = L"Choose a folder outside the Windows directory.";
			return false;
		}
	}
	const std::wstring programFiles = KnownFolder(FOLDERID_ProgramFiles);
	const std::wstring programData = KnownFolder(FOLDERID_ProgramData);
	if ((!programFiles.empty() && PathEqualsNoCase(full, programFiles)) ||
		(!programData.empty() && PathEqualsNoCase(full, programData)))
	{
		error = L"Choose a folder of its own inside that location, not the location itself.";
		return false;
	}

	normalized = full;
	error.clear();
	return true;
}

bool IsToolRunning(std::wstring& name)
{
	name.clear();
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry))
	{
		do
		{
			if (CompareStringOrdinal(entry.szExeFile, -1, kConfigExeName, -1, TRUE) == CSTR_EQUAL ||
				CompareStringOrdinal(entry.szExeFile, -1, kControlExeName, -1, TRUE) == CSTR_EQUAL)
			{
				name = entry.szExeFile;
				break;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return !name.empty();
}

bool PathEqualsNoCase(const std::wstring& first, const std::wstring& second)
{
	return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool FileExists(const std::wstring& path)
{
	const DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool RemoveDirectoryTree(const std::wstring& directory)
{
	if (directory.empty())
	{
		return false;
	}
	bool ok = true;
	WIN32_FIND_DATAW data = {};
	const std::wstring pattern = directory + L"\\*";
	const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE)
	{
		const DWORD code = GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
	}
	do
	{
		if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
		{
			continue;
		}
		const std::wstring child = directory + L"\\" + data.cFileName;
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
			(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
		{
			if (!RemoveDirectoryTree(child))
			{
				ok = false;
			}
		}
		else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			if (!RemoveDirectoryW(child.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
			{
				ok = false;
			}
		}
		else
		{
			SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
			if (!DeleteFileW(child.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
			{
				ok = false;
			}
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	if (!RemoveDirectoryW(directory.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
	{
		ok = false;
	}
	return ok;
}

std::wstring ReadDriverVersion(const std::wstring& infPath)
{
	const HANDLE file = CreateFileW(infPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
									FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return L"1.0";
	}
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024)
	{
		CloseHandle(file);
		return L"1.0";
	}
	std::string text(static_cast<size_t>(size.QuadPart), '\0');
	DWORD read = 0;
	const bool ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) != FALSE;
	CloseHandle(file);
	if (!ok)
	{
		return L"1.0";
	}
	text.resize(read);

	size_t begin = 0;
	while (begin < text.size())
	{
		const size_t end = text.find('\n', begin);
		std::string line = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		begin = end == std::string::npos ? text.size() : end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
		{
			line.pop_back();
		}
		size_t first = 0;
		while (first < line.size() && (line[first] == ' ' || line[first] == '\t'))
		{
			++first;
		}
		if (line.size() - first < 9 || _strnicmp(line.c_str() + first, "DriverVer", 9) != 0)
		{
			continue;
		}
		const size_t equals = line.find('=', first);
		if (equals == std::string::npos)
		{
			continue;
		}
		std::string value = line.substr(equals + 1);
		const size_t comment = value.find(';');
		if (comment != std::string::npos)
		{
			value.resize(comment);
		}
		const size_t comma = value.find(',');
		if (comma != std::string::npos)
		{
			value = value.substr(comma + 1);
		}
		size_t valueBegin = 0;
		while (valueBegin < value.size() && (value[valueBegin] == ' ' || value[valueBegin] == '\t'))
		{
			++valueBegin;
		}
		size_t valueEnd = value.size();
		while (valueEnd > valueBegin && (value[valueEnd - 1] == ' ' || value[valueEnd - 1] == '\t'))
		{
			--valueEnd;
		}
		value = value.substr(valueBegin, valueEnd - valueBegin);
		if (value.empty())
		{
			break;
		}
		return std::wstring(value.begin(), value.end());
	}
	return L"1.0";
}

std::wstring Quoted(const std::wstring& path)
{
	return L"\"" + path + L"\"";
}

} // namespace OpenA2dp::Setup
