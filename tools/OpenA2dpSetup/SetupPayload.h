#pragma once

#include <string>

namespace OpenA2dp::Setup {

// A fresh, empty directory under the user's temporary folder. The caller removes it.
bool CreatePayloadDirectory(std::wstring& directory, std::wstring& error);

// Writes the driver package, the service and the tools that are embedded in this executable
// into `directory`, keeping the names the INF and the service expect.
bool ExtractPayload(const std::wstring& directory, std::wstring& error);

} // namespace OpenA2dp::Setup
