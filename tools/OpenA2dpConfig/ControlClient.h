// Private to OpenA2dpConfig: finds the per-sink control devices and issues the IOCTLs in
// OpenA2dp/Ioctl.h, so the window code only deals with its controls. Errors come back as
// text that already names what the user can do about them.
#pragma once

#include "common/ControlDevice.h"

#include <OpenA2dp/Ioctl.h>

#include <windows.h>

#include <string>
#include <vector>

namespace OpenA2dp::ConfigUi {

// Every sink the driver owns has a control device named after its address; finding them and
// naming them is shared with the CLI, the IOCTL wrappers below are the window's own.
using Tools::Device;
using Tools::EnumerateDevices;
using Tools::FormatAddress;

HANDLE OpenControl(const Device& device, std::string& error);
bool ReadVersion(HANDLE handle, Ioctl::VersionInfo& version, std::string& error);
bool ReadStatus(HANDLE handle, Ioctl::Status& status, std::string& error);
bool ReadPreference(HANDLE handle, Ioctl::Preference& preference, std::string& error);
bool WritePreference(HANDLE handle, const Ioctl::Preference& preference, std::string& error);
bool ReadEndpointFormat(HANDLE handle, Ioctl::EndpointFormat& format, std::string& error);
bool WriteEndpointFormat(HANDLE handle, const Ioctl::EndpointFormat& format, std::string& error);

} // namespace OpenA2dp::ConfigUi
