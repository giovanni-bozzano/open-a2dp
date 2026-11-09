#include "StatusView.h"

#include <commctrl.h>
#include <uxtheme.h>

namespace OpenA2dp::ConfigUi {
namespace {

// A report row is one line high; a taller row reads as a settings list rather than a table.
constexpr int kRowHeight = 22;
constexpr int kLabelColumnWidth = 168;

struct ViewState
{
	std::vector<StatusRow> rows;
	std::wstring emptyText; // the row shown while there is nothing to report
	HFONT bold = nullptr;
	HIMAGELIST images = nullptr; // a one-pixel image list is how a report row gets a height
};

ViewState* StateOf(HWND list)
{
	return reinterpret_cast<ViewState*>(GetWindowLongPtrW(list, GWLP_USERDATA));
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

HFONT CreateBoldFont(HFONT base)
{
	LOGFONTW logFont = {};
	if (base == nullptr || GetObjectW(base, sizeof(logFont), &logFont) == 0)
	{
		NONCLIENTMETRICSW metrics = {};
		metrics.cbSize = sizeof(metrics);
		if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) == 0)
		{
			return nullptr;
		}
		logFont = metrics.lfMessageFont;
	}
	logFont.lfWeight = FW_SEMIBOLD;
	return CreateFontIndirectW(&logFont);
}

void FillRow(HWND list, size_t index, const StatusRow& row)
{
	LVITEMW item = {};
	item.mask = LVIF_TEXT;
	item.iItem = static_cast<int>(index);
	item.iSubItem = 0;
	item.pszText = const_cast<wchar_t*>(row.label.c_str());
	ListView_InsertItem(list, &item);
	if (!row.section && !row.value.empty())
	{
		ListView_SetItemText(list, static_cast<int>(index), 1, const_cast<wchar_t*>(row.value.c_str()));
	}
}

void Rebuild(HWND list, ViewState& state, const std::vector<StatusRow>& rows, const std::wstring& emptyText)
{
	SendMessageW(list, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(list);
	state.rows = rows;
	state.emptyText = rows.empty() ? emptyText : std::wstring();
	if (rows.empty())
	{
		LVITEMW item = {};
		item.mask = LVIF_TEXT;
		item.iItem = 0;
		item.pszText = const_cast<wchar_t*>(emptyText.c_str());
		ListView_InsertItem(list, &item);
	}
	else
	{
		for (size_t i = 0; i < rows.size(); ++i)
		{
			FillRow(list, i, rows[i]);
		}
	}
	SendMessageW(list, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(list, nullptr, TRUE);
}

// True when both lists have the same headers and labels, so only values need updating. The
// counters change every second, and a rebuild would throw the scroll position away.
bool SameShape(const std::vector<StatusRow>& left, const std::vector<StatusRow>& right)
{
	if (left.size() != right.size())
	{
		return false;
	}
	for (size_t i = 0; i < left.size(); ++i)
	{
		if (left[i].section != right[i].section || left[i].label != right[i].label)
		{
			return false;
		}
	}
	return true;
}

} // namespace

bool CreateStatusView(HWND parent, int id, int x, int y, int width, int height, HFONT font)
{
	const HWND list = CreateWindowExW(
			0, WC_LISTVIEWW, L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
					LVS_NOSORTHEADER,
			x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
			GetModuleHandleW(nullptr), nullptr);
	if (list == nullptr)
	{
		return false;
	}
	ListView_SetExtendedListViewStyle(
			list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
	SetWindowTheme(list, L"Explorer", nullptr);
	if (font != nullptr)
	{
		SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	LVCOLUMNW column = {};
	column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
	column.fmt = LVCFMT_LEFT;
	column.cx = kLabelColumnWidth;
	column.pszText = const_cast<wchar_t*>(L"Setting");
	ListView_InsertColumn(list, 0, &column);
	column.cx = width - kLabelColumnWidth - 26;
	column.pszText = const_cast<wchar_t*>(L"Value");
	ListView_InsertColumn(list, 1, &column);

	auto* state = new ViewState();
	state->images = ImageList_Create(1, kRowHeight, ILC_COLOR32, 1, 1);
	if (state->images != nullptr)
	{
		ListView_SetImageList(list, state->images, LVSIL_SMALL);
	}
	state->bold = CreateBoldFont(font);
	SetWindowLongPtrW(list, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
	return true;
}

void DestroyStatusView(HWND parent, int id)
{
	const HWND list = GetDlgItem(parent, id);
	ViewState* state = list != nullptr ? StateOf(list) : nullptr;
	if (state == nullptr)
	{
		return;
	}
	if (state->images != nullptr)
	{
		ImageList_Destroy(state->images);
	}
	if (state->bold != nullptr)
	{
		DeleteObject(state->bold);
	}
	delete state;
	SetWindowLongPtrW(list, GWLP_USERDATA, 0);
}

void UpdateStatusView(HWND parent, int id, const std::vector<StatusRow>& rows, const std::wstring& emptyText)
{
	const HWND list = GetDlgItem(parent, id);
	ViewState* state = list != nullptr ? StateOf(list) : nullptr;
	if (state == nullptr)
	{
		return;
	}
	if (rows.empty())
	{
		if (state->rows.empty() && state->emptyText == emptyText)
		{
			return;
		}
		Rebuild(list, *state, rows, emptyText);
		return;
	}
	if (!SameShape(state->rows, rows))
	{
		Rebuild(list, *state, rows, emptyText);
		return;
	}

	bool changed = false;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		if (state->rows[i].value == rows[i].value && state->rows[i].tone == rows[i].tone)
		{
			continue;
		}
		state->rows[i] = rows[i];
		if (!rows[i].section)
		{
			ListView_SetItemText(list, static_cast<int>(i), 1, const_cast<wchar_t*>(rows[i].value.c_str()));
		}
		changed = true;
	}
	if (changed)
	{
		InvalidateRect(list, nullptr, TRUE);
	}
}

bool OnStatusViewNotify(HWND parent, int id, LPARAM lParam, LRESULT& result)
{
	const HWND list = GetDlgItem(parent, id);
	const auto* header = reinterpret_cast<const NMHDR*>(lParam);
	if (list == nullptr || header == nullptr || header->hwndFrom != list || header->code != NM_CUSTOMDRAW)
	{
		return false;
	}
	auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(header));
	switch (draw->nmcd.dwDrawStage)
	{
	case CDDS_PREPAINT:
		result = CDRF_NOTIFYITEMDRAW;
		return true;
	case CDDS_ITEMPREPAINT:
		// The value column is coloured on its own.
		result = CDRF_NOTIFYSUBITEMDRAW;
		return true;
	case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
		break;
	default:
		return false;
	}

	const ViewState* state = StateOf(list);
	const size_t index = static_cast<size_t>(draw->nmcd.dwItemSpec);
	if (state == nullptr || index >= state->rows.size())
	{
		return false;
	}
	const StatusRow& row = state->rows[index];
	if (row.section)
	{
		draw->clrTextBk = GetSysColor(COLOR_BTNFACE);
		draw->clrText = GetSysColor(COLOR_WINDOWTEXT);
		if (state->bold != nullptr)
		{
			SelectObject(draw->nmcd.hdc, state->bold);
			result = CDRF_NEWFONT;
			return true;
		}
		result = CDRF_DODEFAULT;
		return true;
	}
	draw->clrTextBk = (index % 2) == 0 ? GetSysColor(COLOR_WINDOW) : RGB(0xF7, 0xF8, 0xFA);
	draw->clrText = draw->iSubItem == 1 ? ToneColor(row.tone) : GetSysColor(COLOR_WINDOWTEXT);
	result = CDRF_DODEFAULT;
	return true;
}

} // namespace OpenA2dp::ConfigUi
