#include "SetupPayload.h"

#include "SetupNames.h"
#include "SetupResources.h"

#include <windows.h>

#include <string>

namespace OpenA2dp::Setup {
namespace {

struct PayloadEntry
{
	int id;
	const wchar_t* name;
};

constexpr PayloadEntry kPayload[] = {
	{IDR_PAYLOAD_SYS, kSysName},
	{IDR_PAYLOAD_INF, kInfName},
	{IDR_PAYLOAD_CAT, kCatName},
	{IDR_PAYLOAD_SERVICE, kServiceExeName},
	{IDR_PAYLOAD_CONTROL, kControlExeName},
	{IDR_PAYLOAD_CONFIG, kConfigExeName},
};

std::wstring LastErrorText(const wchar_t* what)
{
	const DWORD code = GetLastError();
	wchar_t* text = nullptr;
	const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
											FORMAT_MESSAGE_IGNORE_INSERTS,
										nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
	std::wstring message = length != 0 && text != nullptr ? std::wstring(text, length) : std::wstring();
	if (text != nullptr)
	{
		LocalFree(text);
	}
	while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
	{
		message.pop_back();
	}
	return std::wstring(what) + L": " + (message.empty() ? L"error " + std::to_wstring(code) : message);
}

} // namespace

bool CreatePayloadDirectory(std::wstring& directory, std::wstring& error)
{
	directory.clear();
	wchar_t temp[MAX_PATH + 1] = {};
	const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
	if (length == 0 || length >= ARRAYSIZE(temp))
	{
		error = L"The temporary folder is not available.";
		return false;
	}
	for (unsigned attempt = 0; attempt < 16; ++attempt)
	{
		wchar_t name[64] = {};
		swprintf_s(name, L"OpenA2dpSetup.%lu.%llu", GetCurrentProcessId(),
				   static_cast<unsigned long long>(GetTickCount64()) + attempt);
		const std::wstring candidate = std::wstring(temp) + name;
		if (CreateDirectoryW(candidate.c_str(), nullptr))
		{
			directory = candidate;
			error.clear();
			return true;
		}
		if (GetLastError() != ERROR_ALREADY_EXISTS)
		{
			break;
		}
	}
	error = LastErrorText(L"The temporary payload folder could not be created");
	return false;
}

bool ExtractPayload(const std::wstring& directory, std::wstring& error)
{
	error.clear();
	const HMODULE module = GetModuleHandleW(nullptr);
	for (const PayloadEntry& entry : kPayload)
	{
		const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(entry.id), RT_RCDATA);
		if (resource == nullptr)
		{
			error = std::wstring(L"The setup payload is missing ") + entry.name +
					L"; rebuild OpenA2dpSetup.exe from the full solution.";
			return false;
		}
		const HGLOBAL loaded = LoadResource(module, resource);
		const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
		const DWORD size = SizeofResource(module, resource);
		if (data == nullptr || size == 0)
		{
			error = std::wstring(L"The setup payload for ") + entry.name + L" is empty.";
			return false;
		}
		const std::wstring path = directory + L"\\" + entry.name;
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
										FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			error = LastErrorText((std::wstring(L"Could not write ") + entry.name).c_str());
			return false;
		}
		const BYTE* bytes = static_cast<const BYTE*>(data);
		DWORD remaining = size;
		bool written = true;
		while (remaining > 0)
		{
			DWORD count = 0;
			if (!WriteFile(file, bytes, remaining, &count, nullptr) || count == 0)
			{
				written = false;
				break;
			}
			bytes += count;
			remaining -= count;
		}
		CloseHandle(file);
		if (!written)
		{
			error = LastErrorText((std::wstring(L"Could not write ") + entry.name).c_str());
			return false;
		}
	}
	return true;
}

} // namespace OpenA2dp::Setup
