#pragma once

#include <functional>
#include <string>

namespace OpenA2dp::Setup {

// One line for the progress page and the log file. Called from the worker thread.
using Log = std::function<void(const std::wstring& line)>;

struct Result
{
	bool success = false;
	bool restartRequired = false;
	std::wstring message;
};

struct InstallInfo
{
	std::wstring installDir;
	std::wstring version;
	std::wstring trustedCertificate;
};

} // namespace OpenA2dp::Setup
