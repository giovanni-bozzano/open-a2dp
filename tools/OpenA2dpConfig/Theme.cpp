#include "Theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace OpenA2dp::ConfigUi {
namespace {

// The theme is read once, at startup: a window that lives through a theme change is a
// different problem, and re-reading the palette would leave the measured layout stale.
Colors g_colors = {};
HBRUSH g_background = nullptr;
bool g_dark = false;

// Windows 10 1903+ keeps a separate set of colours for applications that opt in, reached
// through these uxtheme ordinals, which are what the shell itself calls. They are
// undocumented, so a missing entry point leaves the window light rather than failing.
constexpr int kSetPreferredAppModeOrdinal = 135;
constexpr int kFlushMenuThemesOrdinal = 136;

// DWMWA_USE_IMMERSIVE_DARK_MODE, renumbered for Windows 10 20H1.
constexpr DWORD kImmersiveDarkMode = 20;
constexpr DWORD kImmersiveDarkModePre20H1 = 19;

enum class PreferredAppMode : int {
	Default,
	AllowDark,
	ForceDark,
	ForceLight,
};

// The colours the settings window uses in each theme. The dark pair is the one the Windows
// settings pages use, which is what makes the window look native next to them.
constexpr Colors kLightColors = {
	RGB(0xFF, 0xFF, 0xFF), // background
	RGB(0x00, 0x00, 0x00), // text
	RGB(0x6D, 0x6D, 0x6D), // muted
	RGB(0xC8, 0xC8, 0xC8), // border
	RGB(0xF0, 0xF0, 0xF0), // section
	RGB(0xF7, 0xF8, 0xFA), // alternateRow
	RGB(0xCC, 0xE4, 0xF7), // selection
	RGB(0x0E, 0x6B, 0x2E), // good
	RGB(0x9A, 0x5B, 0x00), // warning
	RGB(0xB0, 0x1B, 0x1B), // error
};

constexpr Colors kDarkColors = {
	RGB(0x20, 0x20, 0x20), // background
	RGB(0xFF, 0xFF, 0xFF), // text
	RGB(0x9A, 0x9A, 0x9A), // muted
	RGB(0x3F, 0x3F, 0x3F), // border
	RGB(0x2B, 0x2B, 0x2B), // section
	RGB(0x2A, 0x2A, 0x2A), // alternateRow
	RGB(0x2C, 0x40, 0x56), // selection
	RGB(0x6C, 0xCB, 0x8F), // good
	RGB(0xF0, 0xB0, 0x50), // warning
	RGB(0xF0, 0x7C, 0x7C), // error
};

bool DarkAppsRequested()
{
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
			KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
	{
		return false;
	}
	DWORD light = 1;
	DWORD bytes = sizeof(light);
	const bool dark = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
						 reinterpret_cast<LPBYTE>(&light),
						 &bytes) == ERROR_SUCCESS &&
		light == 0;
	RegCloseKey(key);
	return dark;
}

// False when this Windows does not offer the opt-in, which also means the title bar and the
// themed controls cannot be drawn dark. The module is held for the lifetime of the process
// rather than unloaded, because the theme the setting installs lives inside it.
bool EnableDarkMode()
{
	const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (uxtheme == nullptr)
	{
		return false;
	}
	using SetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
	const auto setPreferred = reinterpret_cast<SetPreferredAppMode>(
		GetProcAddress(uxtheme, MAKEINTRESOURCEA(kSetPreferredAppModeOrdinal)));
	if (setPreferred == nullptr)
	{
		return false;
	}
	(void)setPreferred(PreferredAppMode::ForceDark);
	using FlushMenuThemes = void(WINAPI*)();
	if (const auto flush =
			reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(kFlushMenuThemesOrdinal))))
	{
		flush();
	}
	return true;
}

} // namespace

// The brush belongs to the module: both the window class and every paint use it, so it is
// created once and left to the process to release at exit.
void SelectTheme()
{
	g_dark = DarkAppsRequested() && EnableDarkMode();
	g_colors = g_dark ? kDarkColors : kLightColors;
	g_background = CreateSolidBrush(g_colors.background);
}

bool DarkTheme()
{
	return g_dark;
}

const Colors& ThemeColors()
{
	return g_colors;
}

HBRUSH BackgroundBrush()
{
	return g_background;
}

void ApplyTheme(HWND window)
{
	if (!g_dark)
	{
		return;
	}
	const BOOL dark = TRUE;
	if (FAILED(DwmSetWindowAttribute(window, kImmersiveDarkMode, &dark, sizeof(dark))))
	{
		(void)DwmSetWindowAttribute(window, kImmersiveDarkModePre20H1, &dark, sizeof(dark));
	}
}

} // namespace OpenA2dp::ConfigUi
