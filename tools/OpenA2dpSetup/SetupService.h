#pragma once

#include "SetupTypes.h"

#include <string>

namespace OpenA2dp::Setup {

bool EncoderServiceExists();
bool QueryEncoderServiceDirectory(std::wstring& directory);

bool StopEncoderService(const Log& log, std::wstring& error);
bool InstallEncoderService(const std::wstring& exePath, const Log& log, std::wstring& error);
bool StartEncoderService(const Log& log, std::wstring& error);
bool DeleteEncoderService(const Log& log, std::wstring& error);

} // namespace OpenA2dp::Setup
