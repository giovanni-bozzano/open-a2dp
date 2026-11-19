#pragma once

#include <windows.h>

#include "SetupTypes.h"

#include <string>

namespace OpenA2dp::Setup {

// The install directory and its version from the app key, or from the encoder service when the
// installation came from the deploy script. False when nothing is installed.
bool ReadInstallInfo(InstallInfo& info);

// Writes the app key and the Settings > Apps entry. `estimatedSize` is in bytes.
bool WriteInstallInfo(const InstallInfo& info, ULONGLONG estimatedSize, std::wstring& error);

bool RemoveInstallInfo(std::wstring& error);

} // namespace OpenA2dp::Setup
