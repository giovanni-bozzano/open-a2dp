#pragma once

#include "SetupTypes.h"

#include <windows.h>

#include <string>
#include <vector>

namespace OpenA2dp::Setup {

// Addresses of the connected A2DP sinks currently bound to OpenA2dp, formatted for the
// uninstall confirmation page.
std::vector<std::wstring> CurrentOpenA2dpSinks(std::wstring& error);

// Stages the package, installs or updates the files and service, restarts only sinks that were
// already on OpenA2dp, removes the superseded package after an update, and registers the
// installation. Does not switch any sink.
Result Install(const std::wstring& installDir, const std::wstring& setupExePath, HWND owner,
			   const Log& log);

// Returns every OpenA2dp sink to BthA2dp, removes the package from the driver store, the encoder
// and kernel services, the shortcut, the registry entries and, optionally, the saved settings. The installed setup
// executable is removed by the cleanup helper after this process exits.
Result Uninstall(const std::wstring& installDir, bool removeSettings, const Log& log);

// Deletes the installer's own files from an install directory after the process that created
// them has exited. Used by the /cleanup helper so the folder can remove itself.
bool CleanupInstalledFiles(const std::wstring& installDir, const Log& log);

} // namespace OpenA2dp::Setup
