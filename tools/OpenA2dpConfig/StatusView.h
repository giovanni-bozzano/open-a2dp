#pragma once

// The session view of the settings window: a two-column list instead of a wall of text.
// Rows are grouped under section headers, alternating rows are shaded and a value that
// reports a failure is coloured, so the state is readable without parsing every line.
// The text behind it is still Tools::BuildStatusReport - the window parses that report into
// rows, so the list and `OpenA2dpCtl status` can never disagree.

#include <windows.h>

#include <string>
#include <vector>

namespace OpenA2dp::ConfigUi {

// Colour of a value; section rows ignore it.
enum class RowTone {
	Normal,
	Good,   // streaming, registered
	Warning,// degraded but running (suspended, underruns)
	Error,  // failed or a non-zero error
};

struct StatusRow
{
	bool section = false; // a header for the rows below it, spanning both columns
	std::wstring label;
	std::wstring value;
	RowTone tone = RowTone::Normal;
};

// Creates the list view. Returns false if the control could not be created.
bool CreateStatusView(HWND parent, int id, int x, int y, int width, int height, HFONT font);

// Releases the fonts the view owns; call from WM_DESTROY.
void DestroyStatusView(HWND parent, int id);

// Replaces the rows. The list is rebuilt only when its content changed, so the timer does
// not reset the scroll position or the selection. `emptyText` is shown as the only row when
// `rows` is empty.
void UpdateStatusView(HWND parent, int id, const std::vector<StatusRow>& rows, const std::wstring& emptyText);

// Handles NM_CUSTOMDRAW for the list, which is where sections, shading and value colours
// come from. Returns true when the notification was consumed.
bool OnStatusViewNotify(HWND parent, int id, LPARAM lParam, LRESULT& result);

} // namespace OpenA2dp::ConfigUi
