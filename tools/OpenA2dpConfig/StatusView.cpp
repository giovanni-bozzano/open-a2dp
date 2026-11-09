#include "StatusView.h"

namespace OpenA2dp::ConfigUi {
namespace {

constexpr wchar_t kClassName[] = L"OpenA2dpStatusView";
constexpr int kRowHeight = 24;
constexpr int kLabelColumnWidth = 196;
constexpr int kTextInset = 10;
// Rows per wheel notch, like a list that scrolls by items.
constexpr int kWheelRows = 3;

struct ViewState
{
	std::vector<StatusRow> rows;
	std::wstring emptyText;
	int scrollTop = 0; // pixels; always a whole row
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
	case RowTone::Good: return RGB(0x0E, 0x6B, 0x2E);
	case RowTone::Warning: return RGB(0x9A, 0x5B, 0x00);
	case RowTone::Error: return RGB(0xB0, 0x1B, 0x1B);
	default: return GetSysColor(COLOR_WINDOWTEXT);
	}
}

// One row: section headers span the whole width, a value row has two columns.
void PaintRow(HDC dc, const ViewState& state, int index, int y, int width)
{
	const StatusRow& row = state.rows[static_cast<size_t>(index)];
	RECT strip = {0, y, width, y + kRowHeight};
	if (row.section)
	{
		FillRect(dc, &strip, GetSysColorBrush(COLOR_BTNFACE));
		SelectObject(dc, state.bold);
	}
	else
	{
		SetDCBrushColor(dc, (index % 2) == 0 ? GetSysColor(COLOR_WINDOW) : RGB(0xF7, 0xF8, 0xFA));
		FillRect(dc, &strip, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		SelectObject(dc, state.normal);
	}

	RECT label = {kTextInset, y, kTextInset + kLabelColumnWidth, y + kRowHeight};
	RECT value = {kTextInset + kLabelColumnWidth, y, width - kTextInset, y + kRowHeight};
	const UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
	DrawTextW(dc, row.label.c_str(), -1, &label, format);
	if (!row.section && !row.value.empty())
	{
		SetTextColor(dc, ToneColor(row.tone));
		DrawTextW(dc, row.value.c_str(), -1, &value, format);
	}
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
		FillRect(memory, &client, GetSysColorBrush(COLOR_WINDOW));
		// The scroll position is always a whole row, so the first visible row starts at 0.
		const int first = state.scrollTop / kRowHeight;
		int y = 0;
		for (int index = first; index < static_cast<int>(state.rows.size()) && y < height; ++index, y += kRowHeight)
		{
			PaintRow(memory, state, index, y, width);
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
	const int content = static_cast<int>(state.rows.size()) * kRowHeight;
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
	const int content = static_cast<int>(state.rows.size()) * kRowHeight;
	const int maximum = content > client.bottom ? content - client.bottom : 0;
	if (scrollTop < 0)
	{
		scrollTop = 0;
	}
	if (scrollTop > maximum)
	{
		scrollTop = maximum;
	}
	// Whole rows only, so a row is never cut in half by the wheel.
	scrollTop -= scrollTop % kRowHeight;
	if (scrollTop == state.scrollTop)
	{
		return;
	}
	state.scrollTop = scrollTop;
	UpdateScrollBar(window, state);
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
			UpdateScrollBar(window, *state);
			ScrollTo(window, *state, state->scrollTop);
			InvalidateRect(window, nullptr, FALSE);
		}
		return 0;
	case WM_MOUSEWHEEL:
		if (state != nullptr)
		{
			const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			const int rows = delta * kWheelRows / WHEEL_DELTA;
			ScrollTo(window, *state, state->scrollTop - rows * kRowHeight);
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
			case SB_LINEUP: position -= kRowHeight; break;
			case SB_LINEDOWN: position += kRowHeight; break;
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
	UpdateScrollBar(window, *state);
	ScrollTo(window, *state, state->scrollTop);
	InvalidateRect(window, nullptr, FALSE);
}

} // namespace OpenA2dp::ConfigUi
