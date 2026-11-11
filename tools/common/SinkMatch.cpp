#include "SinkMatch.h"

#include "Text.h"

#include <cctype>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace OpenA2dp::Tools {

bool ContainsNoCase(const std::wstring& text, const wchar_t* needle)
{
	for (size_t i = 0; i + 1 <= text.size(); ++i)
	{
		size_t j = 0;
		for (; needle[j] != L'\0'; ++j)
		{
			if (i + j >= text.size() || towupper(text[i + j]) != towupper(needle[j]))
			{
				break;
			}
		}
		if (needle[j] == L'\0')
		{
			return true;
		}
	}
	return false;
}

SinkDriver DriverOfServiceName(const std::wstring& service)
{
	if (ContainsNoCase(service, L"OpenA2dp"))
	{
		return SinkDriver::OpenA2dp;
	}
	if (ContainsNoCase(service, L"BthA2dp"))
	{
		return SinkDriver::Windows;
	}
	return SinkDriver::Unknown;
}

bool MatchesSinkAddress(const std::wstring& instanceId, const std::string& reference)
{
	const std::string address = Upper(reference);
	if (address.size() != 12)
	{
		return false;
	}
	const std::string id = Upper(Utf8(instanceId));
	if (id.find(address) != std::string::npos)
	{
		return true;
	}
	std::string reversed;
	for (int pair = 5; pair >= 0; --pair)
	{
		reversed += address.substr(static_cast<size_t>(pair) * 2, 2);
	}
	return id.find(reversed) != std::string::npos;
}

std::string AddressInInstanceId(const std::wstring& instanceId)
{
	const std::string id = Upper(Utf8(instanceId));
	std::string address;
	for (size_t i = 0; i < id.size();)
	{
		size_t length = 0;
		while (i + length < id.size() && isxdigit(static_cast<unsigned char>(id[i + length])) != 0)
		{
			++length;
		}
		if (length == 12)
		{
			address = id.substr(i, 12);
		}
		i += length == 0 ? 1 : length;
	}
	return address;
}

bool IsA2dpServiceId(const std::wstring& id)
{
	return !id.empty() && id.back() == L'}' && ContainsNoCase(id, kA2dpNodeMarker);
}

namespace {

std::string LowerAscii(std::string text)
{
	for (char& character : text)
	{
		if (character >= 'A' && character <= 'Z')
		{
			character = static_cast<char>(character - 'A' + 'a');
		}
	}
	return text;
}

// The image name without its extension: an app id carries the name in one form or another, so
// the stem is what both sides are compared on.
std::string PlayerStem(const std::string& player)
{
	std::string name = LowerAscii(player);
	constexpr const char* kExe = ".exe";
	if (name.size() > 4 && name.compare(name.size() - 4, 4, kExe) == 0)
	{
		name.resize(name.size() - 4);
	}
	return name;
}

} // namespace

std::string PlayerTitleFromAppId(const std::string& appId)
{
	std::string id = appId;
	const size_t bang = id.rfind('!');
	if (bang != std::string::npos)
	{
		id = id.substr(bang + 1);
	}
	const size_t separator = id.find_last_of("\\/");
	if (separator != std::string::npos)
	{
		id = id.substr(separator + 1);
	}
	return id;
}

bool AppIdMatchesPlayer(const std::string& appId, const std::vector<std::string>& players)
{
	const std::string id = LowerAscii(appId);
	for (const std::string& player : players)
	{
		const std::string name = PlayerStem(player);
		if (!name.empty() && id.find(name) != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

} // namespace OpenA2dp::Tools
