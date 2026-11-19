#pragma once

#include <windows.h>

#include <string>

namespace OpenA2dp::Setup {

std::wstring DefaultInstallDir();
std::wstring ProgramDataDirectory();
std::wstring StartMenuDirectory();

bool IsElevated();

// Normalizes an install folder and rejects the choices that cannot hold a service: a relative
// path, a drive root, a network or removable volume, or a folder inside the Windows directory.
// `normalized` is the full path with no trailing separator.
bool ValidateInstallDir(const std::wstring& text, std::wstring& normalized, std::wstring& error);

// The first OpenA2DP settings window or CLI that is still running, or an empty string.
bool IsToolRunning(std::wstring& name);

bool PathEqualsNoCase(const std::wstring& first, const std::wstring& second);
bool FileExists(const std::wstring& path);

// Deletes a directory and everything below it. Used for the installer's temporary payload and,
// when the user asks for it, the per-sink settings folder.
bool RemoveDirectoryTree(const std::wstring& directory);

// The version field of the INF's DriverVer line, or "1.0" when it is missing.
std::wstring ReadDriverVersion(const std::wstring& infPath);

std::wstring Quoted(const std::wstring& path);

} // namespace OpenA2dp::Setup
