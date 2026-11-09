#include "Text.h"

#include <windows.h>

#include <vector>

namespace OpenA2dp::Tools {

std::wstring Widen(const std::string& text)
{
	if (text.empty())
	{
		return {};
	}
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	if (length <= 1)
	{
		return {};
	}
	std::vector<wchar_t> buffer(static_cast<size_t>(length));
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer.data(), length);
	return std::wstring(buffer.data());
}

std::string Utf8(const wchar_t* text)
{
	if (text == nullptr || text[0] == L'\0')
	{
		return {};
	}
	const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (length <= 1)
	{
		return {};
	}
	std::string result(static_cast<size_t>(length - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
	return result;
}

std::string Utf8(const std::wstring& text)
{
	return Utf8(text.c_str());
}

std::string Upper(const std::string& text)
{
	std::string result = text;
	for (char& c : result)
	{
		c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
	}
	return result;
}

std::string Lower(const std::string& text)
{
	std::string result = text;
	for (char& c : result)
	{
		c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}
	return result;
}

} // namespace OpenA2dp::Tools
