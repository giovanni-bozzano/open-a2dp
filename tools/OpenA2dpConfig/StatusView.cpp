#include "StatusView.h"

#include "Theme.h"

#include <windowsx.h>

namespace OpenA2dp::ConfigUi {
namespace {

constexpr wchar_t kClassName[] = L"OpenA2dpStatusView";
constexpr int kRowHeight = 24;
constexpr int kLabelColumnWidth = 120;
constexpr int kMinLabelColumnWidth = 96;
// What the value column keeps, so dragging the divider cannot squeeze it away.
constexpr int kMinValueColumnWidth = 140;
constexpr int kTextInset = 10;
constexpr int kRowIndent = 10;   // a value row sits under its section header
constexpr int kColumnGap = 8;    // between the divider and the text on either side
constexpr int kSplitterGrab = 3; // pixels either side of the divider that start a drag
// Rows per wheel notch, like a list that scrolls by items.
constexpr int kWheelRows = 3;
// The column width the user chose lives in their own settings, so it survives a restart.
constexpr wchar_t kSettingsKey[] = L"Software\\OpenA2dp\\ConfigUi";
constexpr wchar_t kLabelWidthValue[] = L"LabelWidth";
constexpr int kMaxLabelColumnWidth = 2000; // anything larger is not a width this panel had

struct ViewState
{
	std::vector<StatusRow> rows;
	std::wstring emptyText;
	int scrollTop = 0; // pixels; the view scrolls smoothly, so the last row is fully reachable
	int rowHeight = kRowHeight;
	int labelWidth = kLabelColumnWidth;
	// The user dragged the divider, so the column stops following the widest label.
	bool labelWidthChosen = false;
	bool dragging = false;
	bool dividerHot = false;
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

// A missing or nonsensical value leaves the automatic width in charge.
int LoadLabelWidth()
{
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
	{
		return 0;
	}
	DWORD width = 0;
	DWORD bytes = sizeof(width);
	DWORD type = 0;
	const bool read = RegQueryValueExW(key, kLabelWidthValue, nullptr, &type,
							 reinterpret_cast<LPBYTE>(&width), &bytes) == ERROR_SUCCESS;
	RegCloseKey(key);
	if (!read || type != REG_DWORD || width < static_cast<DWORD>(kMinLabelColumnWidth) ||
		width > static_cast<DWORD>(kMaxLabelColumnWidth))
	{
		return 0;
	}
	return static_cast<int>(width);
}

// The width is written when a drag ends, not while it runs: every pixel of the drag would
// otherwise be a registry write.
void SaveLabelWidth(int width)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
						nullptr) != ERROR_SUCCESS)
	{
		return;
	}
	const DWORD value = static_cast<DWORD>(width);
	(void)RegSetValueExW(key, kLabelWidthValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
					 sizeof(value));
	RegCloseKey(key);
}

bool OnDivider(const ViewState& state, int x)
{
	const int divider = DividerX(state);
	return x >= divider - kSplitterGrab && x <= divider + kSplitterGrab;
}

// One row: section headers span the whole width as a band, a value row has two columns.
// `shaded` is the alternation inside the current section; the section header starts it over,
// so each group reads as its own block instead of one continuous stripe.
void PaintRow(HDC dc, const ViewState& state, int index, int y, int width, bool shaded)
{
	const StatusRow& row = state.rows[static_cast<size_t>(index)];
	const RECT strip = {0, y, width, y + state.rowHeight};
	if (row.section)
	{
		Fill(dc, strip, ThemeColors().section);
		// The band closes with a hairline, which is what separates one group from the next.
		RECT line = {0, y + state.rowHeight - 1, width, y + state.rowHeight};
		Fill(dc, line, ThemeColors().border);
	}
	else
	{
		Fill(dc, strip, shaded ? ThemeColors().alternateRow : ThemeColors().background);
	}
	SelectObject(dc, row.section ? state.bold : state.normal);

	const int divider = DividerX(state);
	const int left = kTextInset + (row.section ? 0 : kRowIndent);
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

// The column edge itself: a line the whole height of the panel, so the two columns stay
// legible as columns and the divider is findable without a header row.
void PaintDivider(HDC dc, const ViewState& state, int width, int height)
{
	const int divider = DividerX(state);
	if (divider >= width)
	{
		return;
	}
	const COLORREF color = state.dragging || state.dividerHot ? ThemeColors().muted : ThemeColors().border;
	RECT line = {divider, 0, divider + 1, height};
	Fill(dc, line, color);
}

// The alternation a row at `index` gets, from the start of its section.
bool ShadedAt(const ViewState& state, int index)
{
	bool shaded = false;
	for (int i = 0; i < index; ++i)
	{
		shaded = state.rows[static_cast<size_t>(i)].section ? false : !shaded;
	}
	return shaded;
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
		// The offset is continuous, so the last row can be scrolled fully into view.
		const int first = state.scrollTop / state.rowHeight;
		int y = first * state.rowHeight - state.scrollTop;
		bool shaded = ShadedAt(state, first);
		for (int index = first; index < static_cast<int>(state.rows.size()) && y < height;
			 ++index, y += state.rowHeight)
		{
			PaintRow(memory, state, index, y, width, shaded);
			shaded = state.rows[static_cast<size_t>(index)].section ? false : !shaded;
		}
		PaintDivider(memory, state, width, height);
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
		int widest = kLabelColumnWidth;
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
		state.labelWidth = widest + kRowIndent + kColumnGap;
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

// Ends a drag wherever it ends - the button, or another window taking the capture - and keeps
// the width that was chosen.
void EndDrag(ViewState& state)
{
	state.dragging = false;
	SaveLabelWidth(state.labelWidth);
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
		}
		return 0;
	case WM_LBUTTONDOWN:
		if (state != nullptr && OnDivider(*state, GET_X_LPARAM(lParam)))
		{
			state->dragging = true;
			SetCapture(window);
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		}
		break;
	case WM_LBUTTONUP:
		if (state != nullptr && state->dragging)
		{
			EndDrag(*state);
			ReleaseCapture();
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		}
		break;
	case WM_CAPTURECHANGED:
		if (state != nullptr && state->dragging)
		{
			EndDrag(*state);
			InvalidateRect(window, nullptr, FALSE);
		}
		return 0;
	case WM_MOUSEWHEEL:
		if (state != nullptr)
		{
			const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			const int rows = delta * kWheelRows / WHEEL_DELTA;
			ScrollTo(window, *state, state->scrollTop - rows * state->rowHeight);
		}
		return 0;
	case WM_VSCROLL:
		if (state != nullptr)
		{
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
			0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
			x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
			GetModuleHandleW(nullptr), nullptr);
	if (window == nullptr)
	{
		return false;
	}
	auto* state = new ViewState();
	state->normal = font;
	state->bold = CreateBoldFont(font);
	const int savedWidth = LoadLabelWidth();
	if (savedWidth > 0)
	{
		state->labelWidth = savedWidth;
		state->labelWidthChosen = true;
	}
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
			state->rows = rows;
			InvalidateRect(window, nullptr, FALSE);
			return;
		}
	}
	state->rows = rows;
	state->emptyText = rows.empty() ? emptyText : std::wstring();
	if (rows.empty())
	{
		StatusRow row;
		row.label = emptyText;
		state->rows.push_back(row);
	}
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
