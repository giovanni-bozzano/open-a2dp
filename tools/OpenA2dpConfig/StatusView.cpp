#include "StatusView.h"

#include "Theme.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace OpenA2dp::ConfigUi {
namespace {

constexpr wchar_t kClassName[] = L"OpenA2dpStatusView";
constexpr int kRowHeight = 24;
// The label column has no width of its own: it takes the widest label's, so the values start as
// far left as the text allows. Only the room the value column keeps limits it below.
constexpr int kMinLabelColumnWidth = 96;
// What the value column keeps, so dragging the divider cannot squeeze it away.
constexpr int kMinValueColumnWidth = 140;
constexpr int kTextInset = 10;
constexpr int kColumnGap = 8;    // between the divider and the text on either side
constexpr int kSplitterGrab = 3; // pixels either side of the divider that start a drag
// Rows per wheel notch, like a list that scrolls by items.
constexpr int kWheelRows = 3;
// How long the pointer rests on a row before its explanation is shown: the pane is crossed on
// the way to the divider, and every row has one.
constexpr UINT_PTR kTipTimerId = 1;
constexpr UINT kTipDelayMs = 600;

struct ViewState
{
	std::vector<StatusRow> rows;
	std::vector<bool> selected; // one flag per row, so a report can be copied out
	std::wstring emptyText;
	int scrollTop = 0; // pixels; the view scrolls smoothly, so the last row is fully reachable
	int rowHeight = kRowHeight;
	int labelWidth = kMinLabelColumnWidth;
	// The user dragged the divider, so the column stops following the widest label.
	bool labelWidthChosen = false;
	bool dragging = false;
	bool dividerHot = false;
	// The row under the pointer, and whether the tooltip is up for it: rows are painted rather
	// than controls, so one tooltip covers the pane and this view switches it on and off.
	int hotRow = -1;
	bool tipShown = false;
	// The text the active tip shows. It is built when the tip is shown, because its first
	// lines are the value the row cannot fit and that value changes with every refresh; the
	// tooltip keeps the pointer, so it has to outlive this function's caller.
	std::wstring tipText;
	HWND tooltip = nullptr;
	HFONT normal = nullptr;
	HFONT bold = nullptr;
};

ViewState* StateOf(HWND window)
{
	return reinterpret_cast<ViewState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

COLORREF ToneColor(RowTone tone)
{
	switch (tone)
	{
	case RowTone::Good: return ThemeColors().good;
	case RowTone::Warning: return ThemeColors().warning;
	case RowTone::Error: return ThemeColors().error;
	default: return ThemeColors().text;
	}
}

void Fill(HDC dc, const RECT& rect, COLORREF color)
{
	SetDCBrushColor(dc, color);
	FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

int DividerX(const ViewState& state)
{
	return kTextInset + state.labelWidth;
}

int MaxLabelWidth(int clientWidth)
{
	const int maximum = clientWidth - (kTextInset + kColumnGap) - kMinValueColumnWidth;
	return maximum < kMinLabelColumnWidth ? kMinLabelColumnWidth : maximum;
}

void ClampLabelWidth(HWND window, ViewState& state)
{
	RECT client = {};
	GetClientRect(window, &client);
	const int maximum = MaxLabelWidth(client.right);
	if (state.labelWidth > maximum)
	{
		state.labelWidth = maximum;
	}
	if (state.labelWidth < kMinLabelColumnWidth)
	{
		state.labelWidth = kMinLabelColumnWidth;
	}
}

// The divider is only there when there are two columns to separate.
bool HasColumns(const ViewState& state)
{
	return !state.rows.empty() && state.emptyText.empty();
}

bool OnDivider(const ViewState& state, int x)
{
	if (!HasColumns(state))
	{
		return false;
	}
	const int divider = DividerX(state);
	return x >= divider - kSplitterGrab && x <= divider + kSplitterGrab;
}

// One row: section headers span the whole width as a band, a value row has two columns. Bands
// and values alike end on a hairline, so the pane reads as one table while the bands still name
// the groups in it.
void PaintRow(HDC dc, const ViewState& state, int index, int y, int width, int height)
{
	const StatusRow& row = state.rows[static_cast<size_t>(index)];
	const RECT strip = {0, y, width, y + state.rowHeight};
	// A value row under the pointer gets the band colour too, so the row being pointed at is
	// obvious in a list that has no hover highlight of its own.
	const bool hot = !row.section && index == state.hotRow;
	Fill(dc, strip, row.section || hot ? ThemeColors().section : ThemeColors().background);
	const bool selected = !row.section && index < static_cast<int>(state.selected.size()) &&
						  state.selected[index];
	if (selected)
	{
		Fill(dc, strip, ThemeColors().selection);
	}
	// The bottom row's rule would land on the pane's own border and thicken it, so that one row
	// is left to the frame.
	if (y + state.rowHeight < height)
	{
		const RECT rule = {0, y + state.rowHeight - 1, width, y + state.rowHeight};
		Fill(dc, rule, ThemeColors().border);
	}
	SelectObject(dc, row.section ? state.bold : state.normal);

	const int divider = DividerX(state);
	// A value row lines up with the section bands above it: one inset is the whole of the
	// left edge, so the first character of every line starts at the same place.
	const int left = kTextInset;
	RECT label = {left, y, divider - kColumnGap, y + state.rowHeight};
	RECT value = {divider + kColumnGap, y, width - kTextInset, y + state.rowHeight};
	const UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, ThemeColors().text);
	DrawTextW(dc, row.label.c_str(), -1, &label, format);
	if (!row.section && !row.value.empty())
	{
		SetTextColor(dc, ToneColor(row.tone));
		DrawTextW(dc, row.value.c_str(), -1, &value, format);
	}
}

// The empty pane says why it is empty in a sentence, and a sentence is not a column value: it
// wraps at the pane's own edge instead of being cut with an ellipsis, so a long reason - the
// error that closed the device, say - is readable to its last word.
void PaintEmpty(HDC dc, const ViewState& state, int width, int height)
{
	RECT text = {kTextInset, kTextInset, width - kTextInset, height - kTextInset};
	if (text.right <= text.left || text.bottom <= text.top)
	{
		return;
	}
	const HGDIOBJ previousFont = state.normal != nullptr ? SelectObject(dc, state.normal) : nullptr;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, ThemeColors().muted);
	DrawTextW(dc, state.emptyText.c_str(), -1, &text, DT_WORDBREAK | DT_NOPREFIX | DT_LEFT);
	if (previousFont != nullptr)
	{
		SelectObject(dc, previousFont);
	}
}

// The column edge itself: a line the whole height of the panel, so the two columns stay
// legible as columns and the divider is findable without a header row.
void PaintDivider(HDC dc, const ViewState& state, int width, int height)
{
	const int divider = DividerX(state);
	if (!HasColumns(state) || divider >= width)
	{
		return;
	}
	const COLORREF color = state.dragging || state.dividerHot ? ThemeColors().muted : ThemeColors().border;
	RECT line = {divider, 0, divider + 1, height};
	Fill(dc, line, color);
}

int RowAt(const ViewState& state, int y)
{
	const int index = (y + state.scrollTop) / state.rowHeight;
	if (index < 0 || index >= static_cast<int>(state.rows.size()))
	{
		return -1;
	}
	return index;
}

// The selection outlives a refresh that only changes values; a different shape starts it over,
// because the rows a flag belonged to are gone.
void ResizeSelection(ViewState& state)
{
	if (state.selected.size() != state.rows.size())
	{
		state.selected.assign(state.rows.size(), false);
	}
}

// A section band names the rows below it; only those rows are content a report can quote.
bool SelectableRow(const ViewState& state, int index)
{
	return index >= 0 && index < static_cast<int>(state.rows.size()) && !state.rows[static_cast<size_t>(index)].section;
}

bool HasSelection(const ViewState& state)
{
	return std::find(state.selected.begin(), state.selected.end(), true) != state.selected.end();
}

// What a bug report needs: the selected rows, or every row when nothing is selected - the pane
// is a report, so copying it whole is the common case.
std::wstring CopyText(const ViewState& state)
{
	const bool onlySelected = HasSelection(state);
	std::wstring text;
	for (size_t i = 0; i < state.rows.size(); ++i)
	{
		if (onlySelected && (i >= state.selected.size() || !state.selected[i]))
		{
			continue;
		}
		const StatusRow& row = state.rows[i];
		if (!text.empty())
		{
			text += L"\r\n";
		}
		text += row.label;
		if (!row.section && !row.value.empty())
		{
			text += L": ";
			text += row.value;
		}
	}
	return text;
}

void CopyToClipboard(HWND window, const std::wstring& text)
{
	if (text.empty() || OpenClipboard(window) == FALSE)
	{
		return;
	}
	EmptyClipboard();
	const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (memory != nullptr)
	{
		void* target = GlobalLock(memory);
		if (target != nullptr)
		{
			std::memcpy(target, text.c_str(), bytes);
			GlobalUnlock(memory);
			if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr)
			{
				memory = nullptr; // the clipboard owns it now
			}
		}
		if (memory != nullptr)
		{
			GlobalFree(memory);
		}
	}
	CloseClipboard();
}

void Paint(HWND window, ViewState& state)
{
	PAINTSTRUCT paint = {};
	const HDC dc = BeginPaint(window, &paint);
	RECT client = {};
	GetClientRect(window, &client);
	const int width = client.right;
	const int height = client.bottom;

	// Double buffered: a scroll redraws whole rows and would otherwise flicker.
	const HDC memory = CreateCompatibleDC(dc);
	const HBITMAP bitmap = memory != nullptr ? CreateCompatibleBitmap(dc, width, height) : nullptr;
	const HGDIOBJ previousBitmap = bitmap != nullptr ? SelectObject(memory, bitmap) : nullptr;
	if (memory != nullptr)
	{
		FillRect(memory, &client, BackgroundBrush());
		if (!state.emptyText.empty())
		{
			// Nothing to list: the placeholder row only carries the sentence, which is drawn
			// wrapped rather than as a row.
			PaintEmpty(memory, state, width, height);
		}
		else
		{
			// The offset is continuous, so the last row can be scrolled fully into view.
			const int first = state.scrollTop / state.rowHeight;
			int y = first * state.rowHeight - state.scrollTop;
			for (int index = first; index < static_cast<int>(state.rows.size()) && y < height;
				 ++index, y += state.rowHeight)
			{
				PaintRow(memory, state, index, y, width, height);
			}
			PaintDivider(memory, state, width, height);
		}
	}
	if (bitmap != nullptr)
	{
		BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
	}
	if (previousBitmap != nullptr)
	{
		SelectObject(memory, previousBitmap);
	}
	if (bitmap != nullptr)
	{
		DeleteObject(bitmap);
	}
	if (memory != nullptr)
	{
		DeleteDC(memory);
	}
	EndPaint(window, &paint);
}

void UpdateScrollBar(HWND window, const ViewState& state)
{
	RECT client = {};
	GetClientRect(window, &client);
	const int content = static_cast<int>(state.rows.size()) * state.rowHeight;
	SCROLLINFO info = {};
	info.cbSize = sizeof(info);
	info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	info.nMin = 0;
	info.nMax = content > 0 ? content - 1 : 0;
	info.nPage = static_cast<UINT>(client.bottom > 0 ? client.bottom : 1);
	info.nPos = state.scrollTop;
	SetScrollInfo(window, SB_VERT, &info, TRUE);
}

void ScrollTo(HWND window, ViewState& state, int scrollTop)
{
	RECT client = {};
	GetClientRect(window, &client);
	const int content = static_cast<int>(state.rows.size()) * state.rowHeight;
	const int maximum = content > client.bottom ? content - client.bottom : 0;
	if (scrollTop < 0)
	{
		scrollTop = 0;
	}
	if (scrollTop > maximum)
	{
		scrollTop = maximum;
	}
	if (scrollTop == state.scrollTop)
	{
		return;
	}
	state.scrollTop = scrollTop;
	UpdateScrollBar(window, state);
	InvalidateRect(window, nullptr, FALSE);
}

// The label column follows the widest label, so the values start as far left as the text
// allows instead of at a guessed coordinate. Once the divider has been dragged the width is
// the user's, and only the room left for the value column limits it.
void UpdateColumns(HWND window, ViewState& state)
{
	if (!state.labelWidthChosen)
	{
		int widest = 0;
		const HDC dc = GetDC(window);
		if (dc != nullptr)
		{
			const HGDIOBJ previous = state.normal != nullptr ? SelectObject(dc, state.normal) : nullptr;
			for (const StatusRow& row : state.rows)
			{
				SIZE size = {};
				if (GetTextExtentPoint32W(dc, row.label.c_str(), static_cast<int>(row.label.size()), &size) != FALSE &&
					size.cx > widest)
				{
					widest = size.cx;
				}
			}
			if (previous != nullptr)
			{
				SelectObject(dc, previous);
			}
			ReleaseDC(window, dc);
		}
		state.labelWidth = widest + kColumnGap;
	}
	ClampLabelWidth(window, state);
}

// Dragging the divider: the label column takes the x the pointer is at, minus the panel's inset,
// bounded so neither column can be squeezed away.
void DragLabelWidth(HWND window, ViewState& state, int x)
{
	RECT client = {};
	GetClientRect(window, &client);
	int width = x - kTextInset;
	const int maximum = MaxLabelWidth(client.right);
	if (width > maximum)
	{
		width = maximum;
	}
	if (width < kMinLabelColumnWidth)
	{
		width = kMinLabelColumnWidth;
	}
	if (width == state.labelWidth)
	{
		return;
	}
	state.labelWidth = width;
	state.labelWidthChosen = true;
	InvalidateRect(window, nullptr, FALSE);
}



// One tooltip for the whole pane. Rows are painted rather than controls, so the view switches
// it on for the row under the pointer and off when the pointer leaves: a tooltip that watched
// the window by itself showed up over the pane while it was busy elsewhere and stayed there
// after the pointer left it.
TTTOOLINFOW RowTool(HWND window)
{
	TTTOOLINFOW tool = {};
	tool.cbSize = sizeof(tool);
	tool.uFlags = TTF_TRACK;
	tool.hwnd = window;
	tool.uId = 1;
	return tool;
}

void EnsureRowTooltip(HWND window, ViewState& state)
{
	if (state.tooltip != nullptr)
	{
		return;
	}
	state.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
									WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
									CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (state.tooltip == nullptr)
	{
		return;
	}
	SendMessageW(state.tooltip, TTM_SETMAXTIPWIDTH, 0, 420);
	TTTOOLINFOW tool = RowTool(window);
	tool.lpszText = const_cast<LPWSTR>(L"");
	SendMessageW(state.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

void HideRowTip(HWND window, ViewState& state)
{
	KillTimer(window, kTipTimerId);
	if (state.tooltip == nullptr || !state.tipShown)
	{
		return;
	}
	TTTOOLINFOW tool = RowTool(window);
	SendMessageW(state.tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
	state.tipShown = false;
}

// True when the row's column has room for the whole string. DrawTextW cuts the rest off with
// DT_END_ELLIPSIS, so the same measurement against the same font says whether it did.
bool TextFits(HDC dc, HFONT font, const std::wstring& text, int width)
{
	if (text.empty())
	{
		return true;
	}
	if (dc == nullptr || width <= 0)
	{
		return false;
	}
	const HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
	SIZE size = {};
	const bool measured =
			GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size) != FALSE;
	if (previous != nullptr)
	{
		SelectObject(dc, previous);
	}
	return measured && size.cx <= width;
}

// What the row cannot show: a clipped label or value comes first, in full, then the explanation
// the row already carried. Empty when the row shows everything and has nothing to explain, in
// which case there is no tip to put up.
std::wstring RowTipText(HWND window, const ViewState& state, int index)
{
	if (index < 0 || index >= static_cast<int>(state.rows.size()))
	{
		return {};
	}
	const StatusRow& row = state.rows[static_cast<size_t>(index)];
	RECT client = {};
	GetClientRect(window, &client);
	const int divider = DividerX(state);
	const int labelRoom = divider - kColumnGap - kTextInset;
	const int valueRoom = client.right - kTextInset - divider - kColumnGap;
	std::wstring text;
	const HDC dc = GetDC(window);
	if (dc != nullptr)
	{
		if (!row.label.empty() && !TextFits(dc, state.normal, row.label, labelRoom))
		{
			text = row.label;
		}
		if (!row.section && !row.value.empty() && !TextFits(dc, state.normal, row.value, valueRoom))
		{
			if (!text.empty())
			{
				text += L"\n";
			}
			text += row.value;
		}
		ReleaseDC(window, dc);
	}
	if (!row.tip.empty())
	{
		if (!text.empty())
		{
			text += L"\n\n";
		}
		text += row.tip;
	}
	return text;
}

// Rebuilds the text of a tip that is already up, without moving or flashing it. Its clipped
// value is a live counter on some rows, so a shown tip is brought up to date with the refresh
// rather than left quoting the value it was opened with.
void UpdateRowTip(HWND window, ViewState& state, int index)
{
	if (state.tooltip == nullptr || !state.tipShown)
	{
		return;
	}
	state.tipText = RowTipText(window, state, index);
	if (state.tipText.empty())
	{
		HideRowTip(window, state);
		return;
	}
	TTTOOLINFOW tool = RowTool(window);
	tool.lpszText = state.tipText.data();
	SendMessageW(state.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
}

void ShowRowTip(HWND window, ViewState& state, int index)
{
	if (state.tooltip == nullptr)
	{
		HideRowTip(window, state);
		return;
	}
	state.tipText = RowTipText(window, state, index);
	if (state.tipText.empty())
	{
		HideRowTip(window, state);
		return;
	}
	TTTOOLINFOW tool = RowTool(window);
	tool.lpszText = state.tipText.data();
	SendMessageW(state.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
	// Under the row, not under the pointer: the tip stays put while the pointer moves across
	// the row it explains.
	POINT anchor = {kTextInset, index * state.rowHeight - state.scrollTop + state.rowHeight};
	ClientToScreen(window, &anchor);
	SendMessageW(state.tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(anchor.x, anchor.y));
	SendMessageW(state.tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
	state.tipShown = true;
}

void SetDividerHot(HWND window, ViewState& state, bool hot)
{
	if (state.dividerHot == hot)
	{
		return;
	}
	state.dividerHot = hot;
	InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK ViewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	ViewState* state = StateOf(window);
	switch (message)
	{
	case WM_PAINT:
		if (state != nullptr)
		{
			Paint(window, *state);
			return 0;
		}
		break;
	case WM_SIZE:
		if (state != nullptr)
		{
			HideRowTip(window, *state);
			ClampLabelWidth(window, *state);
			UpdateScrollBar(window, *state);
			ScrollTo(window, *state, state->scrollTop);
			InvalidateRect(window, nullptr, FALSE);
		}
		return 0;
	case WM_SETCURSOR:
		if (state != nullptr && LOWORD(lParam) == HTCLIENT)
		{
			POINT point = {};
			GetCursorPos(&point);
			ScreenToClient(window, &point);
			SetCursor(LoadCursorW(nullptr, state->dragging || OnDivider(*state, point.x) ? IDC_SIZEWE : IDC_ARROW));
			return TRUE;
		}
		break;
	case WM_MOUSEMOVE:
		if (state != nullptr)
		{
			const int x = GET_X_LPARAM(lParam);
			if (state->dragging)
			{
				DragLabelWidth(window, *state, x);
				return 0;
			}
			SetDividerHot(window, *state, OnDivider(*state, x));
			// The divider and a drag have nothing to explain; a row does.
			const int hotRow = OnDivider(*state, x) ? -1 : RowAt(*state, GET_Y_LPARAM(lParam));
			if (hotRow != state->hotRow)
			{
				state->hotRow = hotRow;
				HideRowTip(window, *state);
				// A clipped value needs the tip even when the row has no explanation, so the
				// timer is started for anything the tip could carry rather than for a tip alone.
				if (!RowTipText(window, *state, hotRow).empty())
				{
					SetTimer(window, kTipTimerId, kTipDelayMs, nullptr);
				}
				InvalidateRect(window, nullptr, FALSE);
			}
			// The highlight ends when the pointer leaves, which the window has to ask for.
			TRACKMOUSEEVENT track = {};
			track.cbSize = sizeof(track);
			track.dwFlags = TME_LEAVE;
			track.hwndTrack = window;
			TrackMouseEvent(&track);
		}
		return 0;
	case WM_MOUSELEAVE:
		if (state != nullptr)
		{
			SetDividerHot(window, *state, false);
			state->hotRow = -1;
			HideRowTip(window, *state);
			InvalidateRect(window, nullptr, FALSE);
		}
		return 0;
	case WM_TIMER:
		if (state != nullptr && wParam == kTipTimerId)
		{
			KillTimer(window, kTipTimerId);
			ShowRowTip(window, *state, state->hotRow);
			return 0;
		}
		break;
	case WM_LBUTTONDOWN:
		if (state != nullptr && OnDivider(*state, GET_X_LPARAM(lParam)))
		{
			state->dragging = true;
			SetCapture(window);
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		}
		if (state != nullptr)
		{
			HideRowTip(window, *state);
			// A click selects the row under it; Ctrl adds it to what is already selected. The
			// view takes the focus so the copy shortcut reaches it.
			const int index = RowAt(*state, GET_Y_LPARAM(lParam));
			if ((wParam & MK_CONTROL) != 0 && SelectableRow(*state, index))
			{
				state->selected[index] = !state->selected[index];
			}
			else
			{
				std::fill(state->selected.begin(), state->selected.end(), false);
				if (SelectableRow(*state, index))
				{
					state->selected[static_cast<size_t>(index)] = true;
				}
			}
			SetFocus(window);
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		}
		break;
	case WM_LBUTTONUP:
		if (state != nullptr && state->dragging)
		{
			state->dragging = false;
			ReleaseCapture();
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		}
		break;
	case WM_CAPTURECHANGED:
		if (state != nullptr && state->dragging)
		{
			state->dragging = false;
			InvalidateRect(window, nullptr, FALSE);
		}
		return 0;
	case WM_KEYDOWN:
		if (state != nullptr && (GetKeyState(VK_CONTROL) & 0x8000) != 0)
		{
			switch (wParam)
			{
			case 'A':
				ResizeSelection(*state);
				for (size_t i = 0; i < state->rows.size(); ++i)
				{
					state->selected[i] = !state->rows[i].section;
				}
				InvalidateRect(window, nullptr, FALSE);
				return 0;
			case 'C':
				CopyToClipboard(window, CopyText(*state));
				return 0;
			default:
				break;
			}
		}
		break;
	case WM_MOUSEWHEEL:
		if (state != nullptr)
		{
			HideRowTip(window, *state);
			const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			const int rows = delta * kWheelRows / WHEEL_DELTA;
			ScrollTo(window, *state, state->scrollTop - rows * state->rowHeight);
		}
		return 0;
	case WM_VSCROLL:
		if (state != nullptr)
		{
			HideRowTip(window, *state);
			SCROLLINFO info = {};
			info.cbSize = sizeof(info);
			info.fMask = SIF_ALL;
			GetScrollInfo(window, SB_VERT, &info);
			int position = info.nPos;
			switch (LOWORD(wParam))
			{
			case SB_LINEUP: position -= state->rowHeight; break;
			case SB_LINEDOWN: position += state->rowHeight; break;
			case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
			case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
			case SB_THUMBTRACK:
			case SB_THUMBPOSITION: position = info.nTrackPos; break;
			default: break;
			}
			ScrollTo(window, *state, position);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1; // the paint fills every pixel
	default:
		break;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

HFONT CreateBoldFont(HFONT base)
{
	LOGFONTW logFont = {};
	if (base == nullptr || GetObjectW(base, sizeof(logFont), &logFont) == 0)
	{
		return nullptr;
	}
	logFont.lfWeight = FW_SEMIBOLD;
	return CreateFontIndirectW(&logFont);
}

bool RegisterViewClass()
{
	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = ViewProc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.lpszClassName = kClassName;
	if (RegisterClassExW(&windowClass) != 0)
	{
		return true;
	}
	// A second window (or a second open of the tool) finds the class already registered.
	return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

bool CreateStatusView(HWND parent, int id, int x, int y, int width, int height, HFONT font)
{
	if (!RegisterViewClass())
	{
		return false;
	}
	const HWND window = CreateWindowExW(
			0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
			x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
			GetModuleHandleW(nullptr), nullptr);
	if (window == nullptr)
	{
		return false;
	}
	// A window scrollbar is drawn from the light theme whatever the window answers for
	// WM_CTLCOLOR; the dark scrollbar is the one Explorer's own scrolling panes are given.
	if (DarkTheme())
	{
		SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
	}
	auto* state = new ViewState();
	state->normal = font;
	state->bold = CreateBoldFont(font);
	// Rows follow the font, so a larger system font does not squeeze the text.
	const HDC dc = GetDC(window);
	if (dc != nullptr)
	{
		const HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
		TEXTMETRICW metrics = {};
		if (GetTextMetricsW(dc, &metrics) != FALSE && metrics.tmHeight > 0)
		{
			state->rowHeight = metrics.tmHeight + 8;
		}
		if (previous != nullptr)
		{
			SelectObject(dc, previous);
		}
		ReleaseDC(window, dc);
	}
	SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
	EnsureRowTooltip(window, *state);
	UpdateScrollBar(window, *state);
	return true;
}

void DestroyStatusView(HWND parent, int id)
{
	const HWND window = GetDlgItem(parent, id);
	ViewState* state = window != nullptr ? StateOf(window) : nullptr;
	if (state == nullptr)
	{
		return;
	}
	if (state->tooltip != nullptr)
	{
		DestroyWindow(state->tooltip);
		state->tooltip = nullptr;
	}
	if (state->bold != nullptr)
	{
		DeleteObject(state->bold);
	}
	delete state;
	SetWindowLongPtrW(window, GWLP_USERDATA, 0);
}

void UpdateStatusView(HWND parent, int id, const std::vector<StatusRow>& rows, const std::wstring& emptyText)
{
	const HWND window = GetDlgItem(parent, id);
	ViewState* state = window != nullptr ? StateOf(window) : nullptr;
	if (state == nullptr)
	{
		return;
	}
	if (rows.empty() && state->rows.empty() && state->emptyText == emptyText)
	{
		return;
	}
	if (!rows.empty() && state->rows.size() == rows.size())
	{
		// Same shape: only values changed, so the scroll position stays where it is.
		bool sameShape = true;
		for (size_t i = 0; i < rows.size() && sameShape; ++i)
		{
			sameShape = state->rows[i].section == rows[i].section && state->rows[i].label == rows[i].label;
		}
		if (sameShape)
		{
			// The one-second refresh runs whether or not anything moved; repainting an
			// unchanged report is what made the panel flicker while the sink was idle.
			bool sameContent = true;
			for (size_t i = 0; i < rows.size() && sameContent; ++i)
			{
				sameContent = state->rows[i].value == rows[i].value && state->rows[i].tone == rows[i].tone &&
							  state->rows[i].tip == rows[i].tip;
			}
			state->rows = rows;
			if (!sameContent)
			{
				// Keep a shown tip current: its first lines are the value the row cannot fit,
				// and a counter in it changes on every refresh.
				if (state->tipShown)
				{
					UpdateRowTip(window, *state, state->hotRow);
				}
				InvalidateRect(window, nullptr, FALSE);
			}
			return;
		}
	}
	HideRowTip(window, *state);
	state->rows = rows;
	state->emptyText = rows.empty() ? emptyText : std::wstring();
	if (rows.empty())
	{
		StatusRow row;
		row.label = emptyText;
		state->rows.push_back(row);
	}
	ResizeSelection(*state);
	UpdateColumns(window, *state);
	UpdateScrollBar(window, *state);
	ScrollTo(window, *state, state->scrollTop);
	InvalidateRect(window, nullptr, FALSE);
}

int StatusViewRowHeight(HWND parent, int id)
{
	const HWND window = GetDlgItem(parent, id);
	const ViewState* state = window != nullptr ? StateOf(window) : nullptr;
	return state != nullptr ? state->rowHeight : kRowHeight;
}

} // namespace OpenA2dp::ConfigUi
