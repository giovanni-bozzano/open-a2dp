#pragma once

// The UTF-8 conversions every tool needs: the ioctl structures carry the UTF-16 names the
// Bluetooth stack reported, while the report and the console are UTF-8. One implementation,
// so a sink name is converted the same way everywhere.

#include <string>

namespace OpenA2dp::Tools {

// UTF-8 to UTF-16; empty when the text is empty or cannot be converted.
std::wstring Widen(const std::string& text);

// UTF-16 to UTF-8; null and empty give an empty string.
std::string Utf8(const wchar_t* text);
std::string Utf8(const std::wstring& text);

// ASCII case folding, which is all a device address or an option needs.
std::string Upper(const std::string& text);
std::string Lower(const std::string& text);

} // namespace OpenA2dp::Tools
