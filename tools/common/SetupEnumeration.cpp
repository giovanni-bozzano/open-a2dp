#include "SetupEnumeration.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace OpenA2dp::Tools {
namespace {

bool IsNameChar(char character)
{
	return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' ||
		   character == '-';
}

bool EqualsNoCase(const std::string& text, size_t offset, const char* expected)
{
	const size_t length = std::strlen(expected);
	if (offset + length > text.size())
	{
		return false;
	}
	for (size_t i = 0; i < length; ++i)
	{
		if (std::tolower(static_cast<unsigned char>(text[offset + i])) !=
			std::tolower(static_cast<unsigned char>(expected[i])))
		{
			return false;
		}
	}
	return true;
}

bool ContainsFileName(const std::string& line, const char* name)
{
	const size_t length = std::strlen(name);
	if (length == 0 || line.size() < length)
	{
		return false;
	}
	for (size_t i = 0; i + length <= line.size(); ++i)
	{
		if (!EqualsNoCase(line, i, name))
		{
			continue;
		}
		if (i > 0 && IsNameChar(line[i - 1]))
		{
			continue;
		}
		if (i + length < line.size() && IsNameChar(line[i + length]))
		{
			continue;
		}
		return true;
	}
	return false;
}

// The `oemNN.inf` value anywhere in the line, lower case, or empty. A pnputil block puts it on
// its own field, but matching the value instead of the label is what keeps this working when the
// label is localized.
std::wstring PublishedName(const std::string& line)
{
	for (size_t i = 0; i + 7 <= line.size(); ++i)
	{
		if (std::tolower(static_cast<unsigned char>(line[i])) != 'o' ||
			std::tolower(static_cast<unsigned char>(line[i + 1])) != 'e' ||
			std::tolower(static_cast<unsigned char>(line[i + 2])) != 'm')
		{
			continue;
		}
		if (i > 0 && IsNameChar(line[i - 1]))
		{
			continue;
		}
		size_t end = i + 3;
		if (end >= line.size() || std::isdigit(static_cast<unsigned char>(line[end])) == 0)
		{
			continue;
		}
		while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])) != 0)
		{
			++end;
		}
		if (end + 4 > line.size() || line[end] != '.' || !EqualsNoCase(line, end + 1, "inf"))
		{
			continue;
		}
		if (end + 4 < line.size() && IsNameChar(line[end + 4]))
		{
			continue;
		}
		std::string name = line.substr(i, end + 4 - i);
		for (char& character : name)
		{
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}
		return std::wstring(name.begin(), name.end());
	}
	return {};
}

} // namespace

std::vector<std::wstring> ParseOpenA2dpPackages(const std::string& output)
{
	std::vector<std::wstring> packages;
	std::wstring published;
	bool openA2dp = false;

	const auto flush = [&] {
		if (!published.empty() && openA2dp &&
			std::find(packages.begin(), packages.end(), published) == packages.end())
		{
			packages.push_back(published);
		}
		published.clear();
		openA2dp = false;
	};

	size_t begin = 0;
	while (begin <= output.size())
	{
		const size_t end = output.find('\n', begin);
		std::string line = output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		begin = end == std::string::npos ? output.size() + 1 : end + 1;

		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		const size_t first = line.find_first_not_of(" \t");
		if (first == std::string::npos)
		{
			flush();
			continue;
		}
		const size_t last = line.find_last_not_of(" \t");
		line = line.substr(first, last - first + 1);

		const std::wstring name = PublishedName(line);
		if (!name.empty())
		{
			// A block names one package. Seeing a second published name before a blank line
			// means the listing was not separated as expected, so close the previous block.
			if (!published.empty() && published != name)
			{
				flush();
			}
			published = name;
		}
		if (ContainsFileName(line, "opena2dp.inf"))
		{
			openA2dp = true;
		}
	}
	flush();
	return packages;
}

} // namespace OpenA2dp::Tools
