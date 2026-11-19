#pragma once

#include "SetupTypes.h"

#include <string>
#include <vector>

namespace OpenA2dp::Setup {

// Every package in the driver store whose original INF is OpenA2dp.inf, as oemNN.inf names.
bool ListOpenA2dpPackages(std::vector<std::wstring>& published, const Log& log, std::wstring& error);

// Adds the package to the driver store without installing it on any device. This is the call
// that keeps setup from switching a sink: /install is deliberately absent.
bool StageDriverPackage(const std::wstring& infPath, const Log& log, std::wstring& error);

// Removes every OpenA2dp.inf package and the devices using it. `/uninstall` hands those devices
// back to Windows, `/force` removes the package even while a node still refers to it.
bool DeleteDriverPackages(bool& restartRequired, const Log& log, std::wstring& error);

// Removes one package by its published name, for rolling back a failed install.
bool DeleteDriverPackage(const std::wstring& published, bool& restartRequired, const Log& log,
						 std::wstring& error);

// Deletes the kernel driver service entry the INF created, after its devices are gone.
bool DeleteDriverService(const Log& log, std::wstring& error);

// Deletes the copy under System32\drivers, or schedules it when the loaded image is still held.
bool RemoveDriverImage(bool& restartRequired, const Log& log, std::wstring& error);

} // namespace OpenA2dp::Setup
