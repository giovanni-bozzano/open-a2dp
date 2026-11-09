#pragma once

// The session view of the settings window: a two-column panel instead of a wall of text.
// Rows are grouped under section headers, alternating rows are shaded and a value that
// reports a failure is coloured, so the state is readable without parsing every line.
//
// It is painted by this window and not by a common control: an Explorer-themed list marks
// the row under the cursor, and this view is a read-only report - nothing in it can be
// clicked or selected.
//
// The text behind it is still Tools::BuildStatusReport - the caller builds the rows from
// that report, so the panel and `OpenA2dpCtl status` cannot disagree.

#include <windows.h>

#include <string>
#include <vector>

namespace OpenA2dp::ConfigUi {

// Colour of a value; section rows ignore it.
enum class RowTone {
	Normal,
	Good,    // streaming, registered
	Warning, // degraded but running (suspended, underruns)
	Error,   // failed or a non-zero error
};

struct StatusRow
{
	bool section = false; // a header for the rows below it
	std::wstring label;
	std::wstring value;
	RowTone tone = RowTone::Normal;
};

// Creates the panel. Returns false if the window could not be created.
bool CreateStatusView(HWND parent, int id, int x, int y, int width, int height, HFONT font);

// Releases the state the view owns; call from WM_DESTROY.
void DestroyStatusView(HWND parent, int id);

// Replaces the rows, keeping the scroll position when the shape did not change. `emptyText`
// is shown as the only row when `rows` is empty.
void UpdateStatusView(HWND parent, int id, const std::vector<StatusRow>& rows, const std::wstring& emptyText);

// Height of one row, so the caller can size the panel to a whole number of them.
int StatusViewRowHeight(HWND parent, int id);

} // namespace OpenA2dp::ConfigUi
