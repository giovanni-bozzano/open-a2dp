#pragma once

#include <string>

namespace OpenA2dp::Setup {

bool CreateStartMenuShortcut(const std::wstring& target, const std::wstring& workingDirectory,
							 std::wstring& error);
bool RemoveStartMenuShortcut(std::wstring& error);

} // namespace OpenA2dp::Setup
