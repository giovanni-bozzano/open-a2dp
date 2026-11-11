#pragma once

// The one place the settings window takes its colours from, and the one decision that picks
// them. The process opts into the dark theme through uxtheme so the title bar and the system
// controls follow, but GetSysColor keeps answering with the light palette in a process that
// only opted in, so the palette is carried here instead of read back from the system.

#include <windows.h>

namespace OpenA2dp::ConfigUi {

struct Colors
{
	COLORREF background;   // the window and every label, group caption and checkbox on it
	COLORREF text;
	COLORREF muted;        // a disabled label
	COLORREF border;       // the frame of a group, drawn by the window
	COLORREF section;      // a pressed owner-drawn button's face
	COLORREF alternateRow; // every second row of the session panel
	COLORREF selection;    // the rows selected there, so they can be copied
	COLORREF good;
	COLORREF warning;
	COLORREF error;
};

// Reads the apps theme and opts the process into it. Must run before the window class is
// registered, because the class background brush comes from the palette.
void SelectTheme();

bool DarkTheme();

const Colors& ThemeColors();

// The palette's background as a brush; owned by this module and never freed by the caller.
HBRUSH BackgroundBrush();

// The dark title bar. Call once the window exists; the controls are the window's to theme,
// because it is the one that knows which they are.
void ApplyTheme(HWND window);

} // namespace OpenA2dp::ConfigUi
