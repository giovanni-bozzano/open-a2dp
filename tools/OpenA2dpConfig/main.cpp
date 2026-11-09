// OpenA2dpConfig.exe - desktop settings window for the OpenA2dp A2DP source driver.
//
// One window for every sink the driver owns: it lists the control devices, shows the
// session and what the last negotiation produced, and stores the codec
// preference through SET_PREFERENCE. Status is re-read on a timer; a stored change is
// applied by the driver itself (an active stream renegotiates in place), so the window
// never has to wait or guess.

#include "ControlClient.h"
#include "StatusView.h"

#include "common/StatusReport.h"

#include <OpenA2dp/Ioctl.h>

#include <windows.h>

#include <commctrl.h>

// The standard controls need the version 6 common controls, or they draw with the classic
// theme. The status panel paints itself and does not depend on it.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <cfgmgr32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace OpenA2dp::ConfigUi {
namespace {

// Nothing here is shared with the driver; the control IDs are the only contract between
// the layout and the command handlers.
enum ControlId : int {
	kSinkCombo = 100,
	kRefreshButton,
	kCopyButton,
	kDetailsCheck,
	kStatusList,
	kCodecCombo,
	kFormatCombo,
	kBitrateCombo,
	kBitpoolCombo,
	kBlocksCombo,
	kSubbandsCombo,
	kAllocationCombo,
	kApplyButton,
	kMessageLabel,
};

// The captions are constants because the layout measures them: the window is sized to the
// strings it shows instead of to fixed coordinates, so a larger system font or a longer
// caption cannot cut a control off.
constexpr wchar_t kSinkText[] = L"Sink";
constexpr wchar_t kRefreshText[] = L"Refresh";
constexpr wchar_t kCopyText[] = L"Copy";
constexpr wchar_t kDetailsText[] = L"Details";
constexpr wchar_t kApplyText[] = L"Apply";
constexpr wchar_t kSessionGroupText[] = L"Session";
constexpr wchar_t kCodecGroupText[] = L"Codec";
constexpr wchar_t kFormatGroupText[] = L"Endpoint format";
constexpr wchar_t kCodecHintText[] = L"The frame shape and bitpool apply to SBC; the bitrate to AAC.";
constexpr wchar_t kFormatHintText[] = L"Windows plays the first format the endpoint lists and ignores the "
									 L"Sound-settings pick, so a pinned format is offered alone.";
// A combo box is created with the height of its drop-down list, which is never resized.
constexpr int kComboListHeight = 320;
// Rows the session panel shows when the screen has room for them; it scrolls itself for the
// rows that do not fit, so the window never has to scroll as a whole.
constexpr int kMinPanelRows = 6;
constexpr int kMaxPanelRows = 16;

// A label and the control it describes, in one of the two columns of a group.
struct Field
{
	HWND label;
	HWND control;
	int column; // 0 = left, 1 = right
	int row;    // row inside its column
};
std::vector<Field> Fields;

constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

int MaxOf(int first, int second)
{
	return first > second ? first : second;
}
constexpr int kStatusTimerId = 1;
constexpr int kStatusRefreshMs = 1000;
// Posted from the interface notification thread; the list is re-enumerated on the UI thread.
constexpr UINT kDeviceChangeMessage = WM_APP + 1;

// The endpoint format combo carries the whole format in its item data: 0 is automatic,
// otherwise rate * 10 + channels (480001 = 48000 Hz mono). One selection, one setting.
constexpr uint32_t FormatComboValue(uint32_t sampleRate, uint32_t channels)
{
	return sampleRate == 0 ? 0u : sampleRate * 10 + channels;
}

// Codec combo indices; stored in the item data so the order in the list can change.
constexpr int kCodecAuto = 0;
constexpr int kCodecSbcChoice = 1;
constexpr int kCodecAacChoice = 2;

struct SinkState
{
	std::vector<Device> devices;
	size_t selected = 0;
	HANDLE control = INVALID_HANDLE_VALUE;
	bool open = false;
	std::string reference;
	Ioctl::Status status = {};
	Ioctl::Preference preference = {};
	Ioctl::EndpointFormat endpointFormat = {};
	Ioctl::VersionInfo version = {};
	std::wstring message; // the outcome of the last action, shown under the controls
	HFONT font = nullptr;  // the window's font, owned by the window
	bool details = false;  // show every field, including the ones the formatted lines hide
	HCMNOTIFICATION notify = nullptr; // control-device arrival/removal
	HWND sinkLabel = nullptr;
	HWND sessionGroup = nullptr;
	HWND sessionPanel = nullptr;
	HWND codecGroup = nullptr;
	HWND codecHint = nullptr;
	HWND formatGroup = nullptr;
	HWND formatHint = nullptr;
};

// What a sink is doing as far as the window can tell. The driver keeps a sink's control
// device only while the link is up, so every listed sink is connected; a running stream is
// what makes it playing.
enum class DeviceState {
	Connected,
	Playing,
};

// Relabels one sink in the list; defined with the rest of the sink list code.
void UpdateDeviceItemLabel(HWND hwnd, size_t device, DeviceState state);

std::wstring Widen(const std::string& text)
{
	if (text.empty())
	{
		return {};
	}
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	if (length <= 1)
	{
		return {};
	}
	std::vector<wchar_t> buffer(static_cast<size_t>(length));
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer.data(), length);
	return std::wstring(buffer.data());
}

std::string Narrow(const std::wstring& text)
{
	if (text.empty())
	{
		return {};
	}
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (length <= 1)
	{
		return {};
	}
	std::vector<char> buffer(static_cast<size_t>(length));
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, buffer.data(), length, nullptr, nullptr);
	return std::string(buffer.data());
}

std::wstring Format(const wchar_t* format, ...)
{
	wchar_t text[512];
	va_list args;
	va_start(args, format);
	(void)_vsnwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, format, args);
	va_end(args);
	return text;
}

// The same report OpenA2dpCtl prints, so the two surfaces can never disagree; the window
// only adds the CRLF line endings a multiline edit control needs.
std::wstring ReportText(const SinkState& state)
{
	if (!state.open)
	{
		return state.message;
	}
	Tools::StatusReportInput input;
	input.title = Narrow(Format(L"Sink %s", Widen(FormatAddress(state.reference)).c_str()));
	input.status = &state.status;
	input.preference = &state.preference;
	input.version = &state.version;
	input.verbose = state.details;
	const std::wstring report = Widen(Tools::BuildStatusReport(input));
	std::wstring text;
	text.reserve(report.size() + 32);
	for (const wchar_t character : report)
	{
		if (character == L'\n')
		{
			text += L"\r\n";
		}
		else
		{
			text += character;
		}
	}
	return text;
}

SinkState* StateOf(HWND hwnd)
{
	return reinterpret_cast<SinkState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void SetMessage(HWND hwnd, const std::wstring& message)
{
	SinkState* state = StateOf(hwnd);
	if (state != nullptr)
	{
		state->message = message;
	}
	SetWindowTextW(GetDlgItem(hwnd, kMessageLabel), message.c_str());
}

int TextWidth(HDC dc, const wchar_t* text)
{
	SIZE size = {};
	if (dc == nullptr || text == nullptr || *text == L'\0' ||
		GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size) == FALSE)
	{
		return 0;
	}
	return size.cx;
}

// Height a sentence needs at the given width, so a hint wraps onto more lines instead of
// being cut; the string is what decides, not a fixed control height.
int TextHeight(HDC dc, const wchar_t* text, int width)
{
	if (dc == nullptr || text == nullptr || *text == L'\0' || width <= 0)
	{
		return 0;
	}
	RECT rect = {0, 0, width, 0};
	DrawTextW(dc, text, -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
	return rect.bottom - rect.top;
}

// The widest item in a combo, which is what its width has to fit.
int ComboContentWidth(HDC dc, HWND combo)
{
	int widest = 0;
	const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
	for (LRESULT index = 0; index < count; ++index)
	{
		const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(index), 0);
		if (length == CB_ERR || length <= 0)
		{
			continue;
		}
		std::wstring text(static_cast<size_t>(length) + 1, L'\0');
		if (SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(index),
						 reinterpret_cast<LPARAM>(text.data())) == CB_ERR)
		{
			continue;
		}
		widest = MaxOf(widest, TextWidth(dc, text.c_str()));
	}
	return widest;
}

int WorkAreaHeight(HWND hwnd)
{
	MONITORINFO monitor = {};
	monitor.cbSize = sizeof(monitor);
	if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor))
	{
		return monitor.rcWork.bottom - monitor.rcWork.top;
	}
	return 720;
}

// Places one control (and resizes it when `resize` is set). A combo keeps the height it was
// created with, because that height is its drop-down list.
void Place(HWND control, int x, int y, int width, int height, bool resize)
{
	SetWindowPos(control, nullptr, x, y, width, height,
				 SWP_NOZORDER | SWP_NOACTIVATE | (resize ? 0u : SWP_NOSIZE));
}

// Which section of the list a report line belongs to, or null for the raw field dump, whose
// section the report marks itself.
const wchar_t* SectionForLabel(const std::string& label)
{
	if (label == "AVDTP" || label == "media" || label == "codec" || label == "media MTU")
	{
		return L"Session";
	}
	if (label == "sink formats")
	{
		return L"Sink";
	}
	if (label == "endpoint format" || label == "preference" || label == "SBC bitpool" ||
		label == "AAC bitrate" || label == "SBC frame")
	{
		return L"Endpoint";
	}
	if (label == "encoder" || label == "rings" || label == "counters" || label == "last error")
	{
		return L"Service";
	}
	return label == "driver" ? L"Driver" : nullptr;
}

// A value that reports a failure is coloured, so a glance is enough; the label keeps the
// report's casing for the acronyms and gets a leading capital for the rest.
RowTone ToneForLabel(const std::string& label, const std::string& value)
{
	if (label == "last error")
	{
		return value == "none" ? RowTone::Normal : RowTone::Error;
	}
	if (label == "driver" && value.find("does not match") != std::string::npos)
	{
		return RowTone::Error;
	}
	if (label == "media" || label == "AVDTP")
	{
		if (value == "Failed")
		{
			return RowTone::Error;
		}
		if (value == "Streaming")
		{
			return RowTone::Good;
		}
		if (value == "Suspended")
		{
			return RowTone::Warning;
		}
	}
	return label == "encoder" && value == "not registered" ? RowTone::Warning : RowTone::Normal;
}

std::wstring RowLabel(const std::string& label)
{
	if (label.empty() || label[0] < 'a' || label[0] > 'z')
	{
		return Widen(label);
	}
	std::string capital = label;
	capital[0] = static_cast<char>(capital[0] - 'a' + 'A');
	return Widen(capital);
}

std::vector<StatusRow> BuildStatusRows(const SinkState& state)
{
	std::vector<StatusRow> rows;
	if (!state.open)
	{
		return rows;
	}
	Tools::StatusReportInput input;
	input.status = &state.status;
	input.preference = &state.preference;
	input.version = &state.version;
	input.verbose = state.details;
	const std::string report = Tools::BuildStatusReport(input);

	std::wstring section;
	bool rawFields = false;
	size_t begin = 0;
	while (begin < report.size())
	{
		const size_t end = report.find('\n', begin);
		const std::string line = report.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		begin = end == std::string::npos ? report.size() : end + 1;
		if (line.find("-- raw fields --") != std::string::npos)
		{
			rawFields = true;
			rows.push_back({true, L"Raw fields", std::wstring(), RowTone::Normal});
			continue;
		}
		std::string label;
		std::string value;
		if (!Tools::SplitReportLine(line, rawFields ? 28 : 16, label, value))
		{
			continue;
		}
		if (!rawFields)
		{
			const wchar_t* const next = SectionForLabel(label);
			if (next != nullptr && section != next)
			{
				section = next;
				rows.push_back({true, next, std::wstring(), RowTone::Normal});
			}
		}
		rows.push_back({false, RowLabel(label), Widen(value), ToneForLabel(label, value)});
	}
	return rows;
}

void RefreshStatusView(HWND hwnd)
{
	const SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	const std::wstring empty = state->message.empty() ? L"No OpenA2dp device is available." : state->message;
	UpdateStatusView(hwnd, kStatusList, BuildStatusRows(*state), empty);
}

HWND CreateChild(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, int x, int y,
				 int width, int height, int id, HFONT font)
{
	const HWND child = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
									   parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
									   GetModuleHandleW(nullptr), nullptr);
	if (child != nullptr && font != nullptr)
	{
		SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}
	return child;
}

// A label and its field, which the layout sizes from the label text and the combo contents.
void CreateField(HWND hwnd, const wchar_t* text, int id, int column, int row, HFONT font)
{
	const HWND label = CreateChild(hwnd, L"STATIC", text, SS_LEFT, 0, 0, 10, 10, 0, font);
	const HWND control = CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0,
									 10, kComboListHeight, id, font);
	Fields.push_back({label, control, column, row});
}

void AddComboItem(HWND hwnd, int id, const wchar_t* text, uint32_t value)
{
	const HWND combo = GetDlgItem(hwnd, id);
	if (combo == nullptr)
	{
		return;
	}
	const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
	if (index == CB_ERR)
	{
		return;
	}
	SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(value));
}

uint32_t ComboValue(HWND hwnd, int id)
{
	const HWND combo = GetDlgItem(hwnd, id);
	if (combo == nullptr)
	{
		return 0;
	}
	const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
	if (index == CB_ERR)
	{
		return 0;
	}
	const LRESULT value = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
	return value == CB_ERR ? 0u : static_cast<uint32_t>(value);
}

void SelectComboValue(HWND hwnd, int id, uint32_t value)
{
	const HWND combo = GetDlgItem(hwnd, id);
	if (combo == nullptr)
	{
		return;
	}
	const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
	for (LRESULT i = 0; i < count; ++i)
	{
		if (SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0) == static_cast<LRESULT>(value))
		{
			SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
			return;
		}
	}
	SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

// Selects a stored value that the list may not offer (a bitpool this tool does not list), so
// reading a setting back never silently replaces it with the first item.
void SelectComboValueOrAdd(HWND hwnd, int id, uint32_t value)
{
	const HWND combo = GetDlgItem(hwnd, id);
	if (combo == nullptr)
	{
		return;
	}
	const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
	for (LRESULT i = 0; i < count; ++i)
	{
		if (SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0) == static_cast<LRESULT>(value))
		{
			SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
			return;
		}
	}
	AddComboItem(hwnd, id, Format(L"%u", value).c_str(), value);
	SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(count), 0);
}

// Sizes and places every control from the strings it holds. The window has no scroll bar of
// its own: it is made as large as its contents need, and the session panel - the one part
// whose length the driver decides - scrolls itself for the rows that do not fit.
void ApplyLayout(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	const HFONT font = state->font != nullptr ? state->font
											 : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	const HDC dc = CreateCompatibleDC(nullptr);
	const HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
	TEXTMETRICW metrics = {};
	GetTextMetricsW(dc, &metrics);
	const int textHeight = metrics.tmHeight > 0 ? metrics.tmHeight : 16;
	const int gap = textHeight / 2 + 2;
	const int margin = gap;
	const int inset = gap;
	const int rowHeight = textHeight + 10;
	const int arrow = GetSystemMetrics(SM_CXVSCROLL) + 6;
	const int captionHeight = textHeight + gap;

	// Each column is as wide as its widest label plus its widest combo content.
	int labelWidth[2] = {0, 0};
	int fieldWidth[2] = {0, 0};
	for (const Field& field : Fields)
	{
		wchar_t text[256] = {};
		GetWindowTextW(field.label, text, ARRAYSIZE(text));
		labelWidth[field.column] = MaxOf(labelWidth[field.column], TextWidth(dc, text));
		fieldWidth[field.column] = MaxOf(fieldWidth[field.column], ComboContentWidth(dc, field.control) + arrow);
	}
	const int columnGap = gap * 3;
	const int columnWidth[2] = {labelWidth[0] + gap + fieldWidth[0], labelWidth[1] + gap + fieldWidth[1]};
	const int contentWidth = columnWidth[0] + columnGap + columnWidth[1];

	// The header row: the sink picker and the three buttons.
	const int sinkLabelWidth = TextWidth(dc, kSinkText);
	const int sinkComboWidth = ComboContentWidth(dc, GetDlgItem(hwnd, kSinkCombo)) + arrow;
	const int refreshWidth = TextWidth(dc, kRefreshText) + gap * 2;
	const int copyWidth = TextWidth(dc, kCopyText) + gap * 2;
	const int detailsWidth = TextWidth(dc, kDetailsText) + gap * 5;
	const int headerWidth = margin + sinkLabelWidth + gap + sinkComboWidth + gap + refreshWidth + gap +
							copyWidth + gap + detailsWidth + margin;

	int width = contentWidth + (margin + inset) * 2;
	if (headerWidth > width)
	{
		width = headerWidth;
	}
	const int groupLeft = margin;
	const int groupWidth = width - margin * 2;
	const int innerLeft = groupLeft + inset;
	const int innerWidth = groupWidth - inset * 2;

	const int applyWidth = TextWidth(dc, kApplyText) + gap * 4;
	const int applyHeight = rowHeight + 4;
	const int codecHintHeight = MaxOf(TextHeight(dc, kCodecHintText, innerWidth), textHeight);
	const int formatHintHeight = MaxOf(TextHeight(dc, kFormatHintText, innerWidth), textHeight);
	const int codecHeight = captionHeight + 3 * rowHeight + codecHintHeight + gap;
	const int formatHeight = captionHeight + rowHeight + formatHintHeight + gap;
	const int bottomHeight = MaxOf(applyHeight, textHeight);

	// The session panel takes as many rows as the screen can show and scrolls for the rest.
	const int statusRowHeight = MaxOf(StatusViewRowHeight(hwnd, kStatusList), 1);
	const int fixedHeight = margin + rowHeight + gap * 2 + captionHeight + gap + codecHeight + gap +
							formatHeight + gap + bottomHeight + margin;
	// The border and the title bar count too, or the window as a whole would hang off the
	// screen even though its client area fits.
	RECT frame = {0, 0, 100, 100};
	AdjustWindowRectEx(&frame, kWindowStyle, FALSE, 0);
	const int frameHeight = (frame.bottom - frame.top) - 100;
	const int available = WorkAreaHeight(hwnd) - frameHeight - fixedHeight - 24;
	int panelRows = available / statusRowHeight;
	if (panelRows > kMaxPanelRows)
	{
		panelRows = kMaxPanelRows;
	}
	if (panelRows < kMinPanelRows)
	{
		panelRows = kMinPanelRows;
	}
	const int panelHeight = panelRows * statusRowHeight;

	const int headerTop = margin;
	const int sessionTop = headerTop + rowHeight + gap * 2;
	const int sessionHeight = captionHeight + panelHeight + gap;
	const int codecTop = sessionTop + sessionHeight + gap;
	const int formatTop = codecTop + codecHeight + gap;
	const int bottomTop = formatTop + formatHeight + gap;
	const int height = bottomTop + bottomHeight + margin;

	// Header row, with the sink picker filling the space the buttons leave.
	Place(state->sinkLabel, margin, headerTop + (rowHeight - textHeight) / 2, sinkLabelWidth, textHeight, true);
	const int detailsLeft = width - margin - detailsWidth;
	const int copyLeft = detailsLeft - gap - copyWidth;
	const int refreshLeft = copyLeft - gap - refreshWidth;
	Place(GetDlgItem(hwnd, kDetailsCheck), detailsLeft, headerTop + (rowHeight - textHeight) / 2, detailsWidth,
		  textHeight + 2, true);
	Place(GetDlgItem(hwnd, kCopyButton), copyLeft, headerTop, copyWidth, rowHeight, true);
	Place(GetDlgItem(hwnd, kRefreshButton), refreshLeft, headerTop, refreshWidth, rowHeight, true);
	const int sinkLeft = margin + sinkLabelWidth + gap;
	Place(GetDlgItem(hwnd, kSinkCombo), sinkLeft, headerTop, refreshLeft - gap - sinkLeft, 0, false);

	// The three groups.
	Place(state->sessionGroup, groupLeft, sessionTop, groupWidth, sessionHeight, true);
	Place(state->codecGroup, groupLeft, codecTop, groupWidth, codecHeight, true);
	Place(state->formatGroup, groupLeft, formatTop, groupWidth, formatHeight, true);
	Place(state->sessionPanel, innerLeft, sessionTop + captionHeight, innerWidth, panelHeight, true);

	// One row per field: label first, then the combo it describes.
	for (const Field& field : Fields)
	{
		const bool format = field.control == GetDlgItem(hwnd, kFormatCombo);
		const int groupTop = format ? formatTop : codecTop;
		const int top = groupTop + captionHeight + field.row * rowHeight;
		const int left = innerLeft + (field.column == 0 ? 0 : columnWidth[0] + columnGap);
		Place(field.label, left, top + (rowHeight - textHeight) / 2, labelWidth[field.column], textHeight, true);
		Place(field.control, left + labelWidth[field.column] + gap, top, fieldWidth[field.column], 0, false);
	}
	Place(state->codecHint, innerLeft, codecTop + captionHeight + 3 * rowHeight, innerWidth, codecHintHeight, true);
	Place(state->formatHint, innerLeft, formatTop + captionHeight + rowHeight, innerWidth, formatHintHeight, true);

	// Apply sits under the right edge, the outcome of the last action fills the rest.
	const int applyLeft = width - margin - applyWidth;
	Place(GetDlgItem(hwnd, kApplyButton), applyLeft, bottomTop + (bottomHeight - applyHeight) / 2, applyWidth,
		  applyHeight, true);
	Place(GetDlgItem(hwnd, kMessageLabel), margin, bottomTop + (bottomHeight - textHeight) / 2,
		  applyLeft - gap - margin, textHeight, true);

	if (dc != nullptr)
	{
		if (previous != nullptr)
		{
			SelectObject(dc, previous);
		}
		DeleteDC(dc);
	}

	RECT rect = {0, 0, width, height};
	AdjustWindowRectEx(&rect, kWindowStyle, FALSE, 0);
	const int windowWidth = rect.right - rect.left;
	const int windowHeight = rect.bottom - rect.top;
	SetWindowPos(hwnd, nullptr, 0, 0, windowWidth, windowHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	// The size is known only now, so the window is centered here rather than left where the
	// default placement put it - which can hang off the bottom of a short screen.
	MONITORINFO monitor = {};
	monitor.cbSize = sizeof(monitor);
	if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE)
	{
		const int left = monitor.rcWork.left +
						 MaxOf(0, (monitor.rcWork.right - monitor.rcWork.left - windowWidth) / 2);
		const int top = monitor.rcWork.top +
						MaxOf(0, (monitor.rcWork.bottom - monitor.rcWork.top - windowHeight) / 2);
		SetWindowPos(hwnd, nullptr, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

// Creates every control with a placeholder rect; ApplyLayout gives them their real ones, so
// the geometry comes from the measured strings and not from coordinates typed by hand.
void CreateControls(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	const HFONT font = state != nullptr && state->font != nullptr
					   ? state->font
					   : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	Fields.clear();

	if (state != nullptr)
	{
		state->sinkLabel = CreateChild(hwnd, L"STATIC", kSinkText, SS_LEFT, 0, 0, 10, 10, 0, font);
	}
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 10,
				kComboListHeight, kSinkCombo, font);
	CreateChild(hwnd, L"BUTTON", kRefreshText, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, kRefreshButton,
				font);
	// The clipboard text is the report itself, so it can be pasted next to a CLI paste.
	CreateChild(hwnd, L"BUTTON", kCopyText, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, kCopyButton, font);
	CreateChild(hwnd, L"BUTTON", kDetailsText, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 10, 10, kDetailsCheck,
				font);

	if (state != nullptr)
	{
		state->sessionGroup = CreateChild(hwnd, L"BUTTON", kSessionGroupText, BS_GROUPBOX, 0, 0, 10, 10, 0,
										  font);
	}
	CreateStatusView(hwnd, kStatusList, 0, 0, 10, 10, font);
	if (state != nullptr)
	{
		state->sessionPanel = GetDlgItem(hwnd, kStatusList);
		state->codecGroup = CreateChild(hwnd, L"BUTTON", kCodecGroupText, BS_GROUPBOX, 0, 0, 10, 10, 0, font);
	}
	CreateField(hwnd, L"Codec", kCodecCombo, 0, 0, font);
	CreateField(hwnd, L"AAC bitrate", kBitrateCombo, 1, 0, font);
	CreateField(hwnd, L"SBC bitpool", kBitpoolCombo, 0, 1, font);
	CreateField(hwnd, L"SBC blocks", kBlocksCombo, 1, 1, font);
	CreateField(hwnd, L"Subbands", kSubbandsCombo, 0, 2, font);
	CreateField(hwnd, L"Allocation", kAllocationCombo, 1, 2, font);
	if (state != nullptr)
	{
		state->codecHint = CreateChild(hwnd, L"STATIC", kCodecHintText, SS_LEFT, 0, 0, 10, 10, 0, font);
		state->formatGroup = CreateChild(hwnd, L"BUTTON", kFormatGroupText, BS_GROUPBOX, 0, 0, 10, 10, 0,
										font);
	}
	CreateField(hwnd, L"PCM format", kFormatCombo, 0, 0, font);
	if (state != nullptr)
	{
		state->formatHint = CreateChild(hwnd, L"STATIC", kFormatHintText, SS_LEFT, 0, 0, 10, 10, 0, font);
	}

	CreateChild(hwnd, L"BUTTON", kApplyText, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, kApplyButton, font);
	CreateChild(hwnd, L"STATIC", L"", SS_ENDELLIPSIS, 0, 0, 10, 10, kMessageLabel, font);

	// Worst to best, so the better format is always the lower entry: rate first, then channels.
	AddComboItem(hwnd, kFormatCombo, L"Automatic (every sink format)", 0);
	AddComboItem(hwnd, kFormatCombo, L"44100 Hz, mono", FormatComboValue(44100, 1));
	AddComboItem(hwnd, kFormatCombo, L"44100 Hz, stereo", FormatComboValue(44100, 2));
	AddComboItem(hwnd, kFormatCombo, L"48000 Hz, mono", FormatComboValue(48000, 1));
	AddComboItem(hwnd, kFormatCombo, L"48000 Hz, stereo", FormatComboValue(48000, 2));

	AddComboItem(hwnd, kCodecCombo, L"Automatic (AAC, then SBC)", kCodecAuto);
	AddComboItem(hwnd, kCodecCombo, L"SBC only", kCodecSbcChoice);
	AddComboItem(hwnd, kCodecCombo, L"AAC only", kCodecAacChoice);

	AddComboItem(hwnd, kBitrateCombo, L"Highest the sink accepts", 0);
	AddComboItem(hwnd, kBitrateCombo, L"96 kbps", 96000);
	AddComboItem(hwnd, kBitrateCombo, L"128 kbps", 128000);
	AddComboItem(hwnd, kBitrateCombo, L"160 kbps", 160000);
	AddComboItem(hwnd, kBitrateCombo, L"192 kbps", 192000);
	AddComboItem(hwnd, kBitrateCombo, L"256 kbps", 256000);
	AddComboItem(hwnd, kBitrateCombo, L"320 kbps", 320000);

	// A ladder of the values a sink is normally driven at; the sink clamps it to its own
	// range, and a stored value outside the ladder is added when the preference is loaded.
	AddComboItem(hwnd, kBitpoolCombo, L"Automatic (sink's choice)", 0);
	for (const uint32_t bitpool : {2u, 8u, 16u, 24u, 32u, 35u, 40u, 45u, 50u, 53u, 60u, 70u, 80u, 100u, 120u,
						  150u, 200u, 250u})
	{
		AddComboItem(hwnd, kBitpoolCombo, Format(L"%u", bitpool).c_str(), bitpool);
	}

	AddComboItem(hwnd, kBlocksCombo, L"Default (16)", 0);
	AddComboItem(hwnd, kBlocksCombo, L"4 blocks", 4);
	AddComboItem(hwnd, kBlocksCombo, L"8 blocks", 8);
	AddComboItem(hwnd, kBlocksCombo, L"12 blocks", 12);
	AddComboItem(hwnd, kBlocksCombo, L"16 blocks", 16);

	AddComboItem(hwnd, kSubbandsCombo, L"Default (8)", 0);
	AddComboItem(hwnd, kSubbandsCombo, L"4 subbands", 4);
	AddComboItem(hwnd, kSubbandsCombo, L"8 subbands", 8);

	AddComboItem(hwnd, kAllocationCombo, L"Default (loudness)", 0);
	AddComboItem(hwnd, kAllocationCombo, L"Loudness", Ioctl::kSbcAllocationLoudness);
	AddComboItem(hwnd, kAllocationCombo, L"SNR", Ioctl::kSbcAllocationSnr);

	for (const int id : {kCodecCombo, kFormatCombo, kBitrateCombo, kBitpoolCombo, kBlocksCombo, kSubbandsCombo,
						 kAllocationCombo})
	{
		SendMessageW(GetDlgItem(hwnd, id), CB_SETCURSEL, 0, 0);
	}
	ApplyLayout(hwnd);
}

// The controls show the stored preference, so a value the driver refused or clamped is
// visible as soon as the window reads it back.
void LoadPreferenceIntoControls(HWND hwnd)
{
	const SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	const Ioctl::Preference& preference = state->preference;
	const int codec = (preference.flags & Ioctl::kPreferenceForceCodec) == 0
						  ? kCodecAuto
						  : (preference.codec == Ioctl::kCodecAac ? kCodecAacChoice : kCodecSbcChoice);
	SelectComboValue(hwnd, kCodecCombo, static_cast<uint32_t>(codec));
	SelectComboValue(hwnd, kBitrateCombo, preference.aacBitrate);
	SelectComboValue(hwnd, kBlocksCombo, preference.sbcBlocks);
	SelectComboValue(hwnd, kSubbandsCombo, preference.sbcSubbands);
	SelectComboValue(hwnd, kAllocationCombo, preference.sbcAllocation);
	SelectComboValueOrAdd(hwnd, kBitpoolCombo, preference.sbcBitpool);
	SelectComboValue(hwnd, kFormatCombo,
					 FormatComboValue(state->endpointFormat.sampleRate, state->endpointFormat.channels));
}

void CloseControl(SinkState& state)
{
	if (state.control != INVALID_HANDLE_VALUE)
	{
		CloseHandle(state.control);
		state.control = INVALID_HANDLE_VALUE;
	}
	state.open = false;
}

// Opens the selected device and reads everything the window shows.
void OpenSelected(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	CloseControl(*state);
	EnableWindow(GetDlgItem(hwnd, kApplyButton), FALSE);
	if (state->selected >= state->devices.size())
	{
		state->message = L"No sink found: connect a paired A2DP sink, then press Refresh.";
		SetMessage(hwnd, state->message);
		RefreshStatusView(hwnd);
		return;
	}

	const Device& device = state->devices[state->selected];
	state->reference = device.reference;
	std::string error;
	const HANDLE control = OpenControl(device, error);
	if (control == INVALID_HANDLE_VALUE)
	{
		state->message = Widen(error);
		SetMessage(hwnd, state->message);
		RefreshStatusView(hwnd);
		return;
	}
	state->control = control;

	if (!ReadVersion(control, state->version, error) || !ReadStatus(control, state->status, error) ||
		!ReadPreference(control, state->preference, error) ||
		!ReadEndpointFormat(control, state->endpointFormat, error))
	{
		CloseControl(*state);
		state->message = Widen(error);
		SetMessage(hwnd, state->message);
		RefreshStatusView(hwnd);
		return;
	}

	state->open = true;
	EnableWindow(GetDlgItem(hwnd, kApplyButton), TRUE);
	LoadPreferenceIntoControls(hwnd);
	SetMessage(hwnd, L"Loaded " + Widen(FormatAddress(device.reference)) + L".");
	RefreshStatusView(hwnd);
	UpdateDeviceItemLabel(hwnd, state->selected, state->status.mediaState == Ioctl::MediaState::Streaming
														   ? DeviceState::Playing
														   : DeviceState::Connected);
}

// Puts the report on the clipboard: the same text the box shows, which is the text the CLI
// prints, so a window paste and a console paste can be compared line by line.
void CopyReport(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	const std::wstring text = ReportText(*state);
	if (text.empty())
	{
		SetMessage(hwnd, L"There is no report to copy yet.");
		return;
	}
	if (!OpenClipboard(hwnd))
	{
		SetMessage(hwnd, L"Another program holds the clipboard; try Copy again.");
		return;
	}
	EmptyClipboard();
	const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	bool copied = false;
	if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes); memory != nullptr)
	{
		if (void* buffer = GlobalLock(memory); buffer != nullptr)
		{
			std::memcpy(buffer, text.c_str(), bytes);
			GlobalUnlock(memory);
			copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
		}
		if (!copied)
		{
			GlobalFree(memory);
		}
	}
	CloseClipboard();
	SetMessage(hwnd, copied ? L"Report copied to the clipboard." : L"Copying the report failed.");
}

void RefreshStatus(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr || !state->open)
	{
		return;
	}
	std::string error;
	Ioctl::Status status = {};
	if (!ReadStatus(state->control, status, error))
	{
		CloseControl(*state);
		EnableWindow(GetDlgItem(hwnd, kApplyButton), FALSE);
		state->message = Widen(error);
		SetMessage(hwnd, state->message);
		RefreshStatusView(hwnd);
		return;
	}
	const bool wasPlaying = state->status.mediaState == Ioctl::MediaState::Streaming;
	state->status = status;
	const bool playing = status.mediaState == Ioctl::MediaState::Streaming;
	if (wasPlaying != playing)
	{
		// The sink that starts or stops streaming is the one the list should show as playing.
		UpdateDeviceItemLabel(hwnd, state->selected, playing ? DeviceState::Playing : DeviceState::Connected);
	}
	RefreshStatusView(hwnd);
}

DeviceState ReadDeviceState(const Device& device)
{
	std::string error;
	const HANDLE control = OpenControl(device, error);
	if (control == INVALID_HANDLE_VALUE)
	{
		return DeviceState::Connected;
	}
	Ioctl::Status status = {};
	const bool read = ReadStatus(control, status, error);
	CloseHandle(control);
	return read && status.mediaState == Ioctl::MediaState::Streaming ? DeviceState::Playing
																  : DeviceState::Connected;
}

std::wstring DeviceLabel(const std::string& reference, DeviceState state)
{
	const wchar_t* const text = state == DeviceState::Playing ? L" (playing)" : L" (connected)";
	return Widen(FormatAddress(reference)) + text;
}

// Relabels one sink in place, so the list shows the state of the one just opened or one whose
// stream started, without rebuilding the whole list and losing the user's selection.
void UpdateDeviceItemLabel(HWND hwnd, size_t device, DeviceState state)
{
	const SinkState* sink = StateOf(hwnd);
	const HWND combo = GetDlgItem(hwnd, kSinkCombo);
	if (sink == nullptr || combo == nullptr || device >= sink->devices.size())
	{
		return;
	}
	const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
	for (LRESULT index = 0; index < count; ++index)
	{
		if (SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0) != static_cast<LRESULT>(device))
		{
			continue;
		}
		const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
		const std::wstring label = DeviceLabel(sink->devices[device].reference, state);
		SendMessageW(combo, CB_DELETESTRING, static_cast<WPARAM>(index), 0);
		const LRESULT added = SendMessageW(combo, CB_INSERTSTRING, static_cast<WPARAM>(index),
										 reinterpret_cast<LPARAM>(label.c_str()));
		if (added != CB_ERR)
		{
			SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(added), static_cast<LPARAM>(device));
		}
		if (selected != CB_ERR)
		{
			SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
		}
		return;
	}
}

// Points the sink list at a device index, which the items carry as their item data.
void SelectDeviceItem(HWND hwnd, size_t device)
{
	const HWND combo = GetDlgItem(hwnd, kSinkCombo);
	const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
	for (LRESULT i = 0; i < count; ++i)
	{
		if (SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0) == static_cast<LRESULT>(device))
		{
			SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
			return;
		}
	}
}

// Lists the control devices the driver currently has, keeping the selection when that sink
// is still there.
void ReloadDevices(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	const std::string previous = state->open ? state->reference : std::string();
	std::string error;
	std::vector<Device> devices = EnumerateDevices(error);
	if (!error.empty())
	{
		SetMessage(hwnd, Widen(error));
		return;
	}

	const HWND combo = GetDlgItem(hwnd, kSinkCombo);
	SendMessageW(combo, CB_RESETCONTENT, 0, 0);
	state->devices = std::move(devices);
	// The sink the user already had stays selected; with none, the one that is playing is
	// the one the window opens, and only then the first of the list.
	size_t selected = state->devices.size();
	size_t playing = state->devices.size();
	for (size_t i = 0; i < state->devices.size(); ++i)
	{
		const DeviceState deviceState = ReadDeviceState(state->devices[i]);
		const std::wstring label = DeviceLabel(state->devices[i].reference, deviceState);
		const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
		if (index != CB_ERR)
		{
			SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
		}
		if (!previous.empty() && state->devices[i].reference == previous)
		{
			selected = i;
		}
		if (playing == state->devices.size() && deviceState == DeviceState::Playing)
		{
			playing = i;
		}
	}
	if (selected == state->devices.size())
	{
		selected = playing != state->devices.size() || state->devices.empty() ? playing : 0;
	}
	state->selected = selected;
	SelectDeviceItem(hwnd, state->selected);
	// Another sink arriving or leaving does not change the open one: keep its control device
	// and the settings the user may have changed but not applied yet.
	if (!previous.empty() && state->selected < state->devices.size() &&
		state->devices[state->selected].reference == previous)
	{
		return;
	}
	OpenSelected(hwnd);
}

// Runs on the interface notification thread, so it only wakes the window.
DWORD CALLBACK OnInterfaceChange(HCMNOTIFICATION notify, PVOID context, CM_NOTIFY_ACTION action,
								 PCM_NOTIFY_EVENT_DATA eventData, DWORD eventDataSize)
{
	UNREFERENCED_PARAMETER(notify);
	UNREFERENCED_PARAMETER(eventData);
	UNREFERENCED_PARAMETER(eventDataSize);
	if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
	{
		PostMessageW(static_cast<HWND>(context), kDeviceChangeMessage, 0, 0);
	}
	return ERROR_SUCCESS;
}

// The driver enables its control-device interface while the control device exists, so its
// arrival and removal are what a sink connecting or disconnecting looks like.
HCMNOTIFICATION RegisterInterfaceNotifications(HWND hwnd)
{
	CM_NOTIFY_FILTER filter = {};
	filter.cbSize = sizeof(filter);
	filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
	filter.u.DeviceInterface.ClassGuid = Ioctl::kControlArrivalInterface;
	HCMNOTIFICATION notify = nullptr;
	if (CM_Register_Notification(&filter, hwnd, OnInterfaceChange, &notify) != CR_SUCCESS)
	{
		return nullptr;
	}
	return notify;
}

// Stores the settings the controls show. The driver applies them itself: a running stream
// renegotiates in place, and a codec change reconfigures the session.
void ApplySettings(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr || !state->open)
	{
		SetMessage(hwnd, L"No device is selected.");
		return;
	}

	// Start from the stored preference so a field the window does not expose is kept.
	Ioctl::Preference preference = state->preference;
	const uint32_t codec = ComboValue(hwnd, kCodecCombo);
	preference.flags = codec == kCodecAuto ? 0 : Ioctl::kPreferenceForceCodec;
	preference.codec = codec == kCodecAacChoice ? Ioctl::kCodecAac : (codec == kCodecSbcChoice ? Ioctl::kCodecSbc : 0);
	preference.aacBitrate = ComboValue(hwnd, kBitrateCombo);
	preference.sbcBitpool = ComboValue(hwnd, kBitpoolCombo);
	preference.sbcBlocks = ComboValue(hwnd, kBlocksCombo);
	preference.sbcSubbands = ComboValue(hwnd, kSubbandsCombo);
	preference.sbcAllocation = ComboValue(hwnd, kAllocationCombo);

	// The endpoint format is its own setting; the combo carries the whole format.
	const uint32_t formatValue = ComboValue(hwnd, kFormatCombo);
	Ioctl::EndpointFormat endpointFormat = {};
	endpointFormat.sampleRate = formatValue / 10;
	endpointFormat.channels = formatValue % 10;
	if (!Ioctl::ValidEndpointFormat(endpointFormat))
	{
		SetMessage(hwnd, L"Choose a format from the list.");
		return;
	}

	std::string failure;
	if (!WriteEndpointFormat(state->control, endpointFormat, failure) ||
		!WritePreference(state->control, preference, failure))
	{
		SetMessage(hwnd, Widen(failure));
		return;
	}

	Ioctl::Preference stored = {};
	if (ReadPreference(state->control, stored, failure))
	{
		state->preference = stored;
	}
	Ioctl::EndpointFormat storedFormat = {};
	if (ReadEndpointFormat(state->control, storedFormat, failure))
	{
		state->endpointFormat = storedFormat;
	}
	LoadPreferenceIntoControls(hwnd);
	RefreshStatus(hwnd);
	const bool streaming = state->status.mediaState == Ioctl::MediaState::Streaming;
	SetMessage(hwnd, streaming ? L"Stored. The active stream renegotiates to apply it."
							   : L"Stored. The setting applies at the next negotiation.");
}

LRESULT OnColorStatic(WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
	return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CREATE:
	{
		SinkState* state = new SinkState();
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		state->font = create != nullptr && create->lpCreateParams != nullptr
						  ? *static_cast<const HFONT*>(create->lpCreateParams)
						  : nullptr;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
		CreateControls(hwnd);
		state->notify = RegisterInterfaceNotifications(hwnd);
		SetTimer(hwnd, kStatusTimerId, kStatusRefreshMs, nullptr);
		ReloadDevices(hwnd);
		return 0;
	}
	case WM_COMMAND:
	{
		const int id = LOWORD(wParam);
		const int notification = HIWORD(wParam);
		if (id == kRefreshButton && notification == BN_CLICKED)
		{
			ReloadDevices(hwnd);
			return 0;
		}
		if (id == kSinkCombo && notification == CBN_SELCHANGE)
		{
			SinkState* state = StateOf(hwnd);
			const LRESULT index = SendMessageW(GetDlgItem(hwnd, kSinkCombo), CB_GETCURSEL, 0, 0);
			if (state != nullptr && index != CB_ERR)
			{
				const LRESULT value = SendMessageW(GetDlgItem(hwnd, kSinkCombo), CB_GETITEMDATA,
												   static_cast<WPARAM>(index), 0);
				if (value != CB_ERR)
				{
					state->selected = static_cast<size_t>(value);
					OpenSelected(hwnd);
				}
			}
			return 0;
		}
		if (id == kApplyButton && notification == BN_CLICKED)
		{
			ApplySettings(hwnd);
			return 0;
		}
		if (id == kCopyButton && notification == BN_CLICKED)
		{
			CopyReport(hwnd);
			return 0;
		}
		if (id == kDetailsCheck && notification == BN_CLICKED)
		{
			SinkState* state = StateOf(hwnd);
			if (state != nullptr)
			{
				const LRESULT checked = SendMessageW(GetDlgItem(hwnd, kDetailsCheck), BM_GETCHECK, 0, 0);
				state->details = checked == BST_CHECKED;
				RefreshStatusView(hwnd);
			}
			return 0;
		}
		break;
	}
	case WM_TIMER:
		if (wParam == kStatusTimerId)
		{
			RefreshStatus(hwnd);
			return 0;
		}
		break;
	case kDeviceChangeMessage:
		ReloadDevices(hwnd);
		return 0;
	case WM_CTLCOLORSTATIC:
		return OnColorStatic(wParam, lParam);
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
	{
		KillTimer(hwnd, kStatusTimerId);
		SinkState* state = StateOf(hwnd);
		if (state != nullptr)
		{
			if (state->notify != nullptr)
			{
				CM_Unregister_Notification(state->notify);
				state->notify = nullptr;
			}
			CloseControl(*state);
			DestroyStatusView(hwnd, kStatusList);
			if (state->font != nullptr)
			{
				DeleteObject(state->font);
				state->font = nullptr;
			}
			delete state;
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		}
		PostQuitMessage(0);
		return 0;
	}
	default:
		break;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int Run(HINSTANCE instance, int showCommand)
{
	const wchar_t* const className = L"OpenA2dpConfigWindow";
	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
	windowClass.lpszClassName = className;
	if (RegisterClassExW(&windowClass) == 0)
	{
		return 1;
	}

	// The window is resized to its contents in WM_CREATE; this is only the size it is created
	// with, before the strings have been measured.
	RECT rect = {0, 0, 480, 600};
	AdjustWindowRectEx(&rect, kWindowStyle, FALSE, 0);
	// The message font is Segoe UI, which is what makes the window look current; the theme
	// drawing of the list and the group boxes follows the same font size.
	NONCLIENTMETRICSW metrics = {};
	metrics.cbSize = sizeof(metrics);
	HFONT font = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != 0
					 ? CreateFontIndirectW(&metrics.lfMessageFont)
					 : nullptr;
	const HWND hwnd = CreateWindowExW(0, className, L"OpenA2dp - A2DP sink settings", kWindowStyle,
									  CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
									  nullptr, nullptr, instance, &font);
	if (hwnd == nullptr)
	{
		if (font != nullptr)
		{
			DeleteObject(font);
		}
		return 1;
	}
	ShowWindow(hwnd, showCommand);
	UpdateWindow(hwnd);

	MSG message = {};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		// TAB and the arrow keys move between the controls, as they would in a dialog.
		if (!IsDialogMessageW(hwnd, &message))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	return 0;
}

} // namespace OpenA2dp::ConfigUi

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE previous, _In_ PWSTR commandLine, _In_ int show)
{
	UNREFERENCED_PARAMETER(previous);
	UNREFERENCED_PARAMETER(commandLine);
	INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&controls);
	return OpenA2dp::ConfigUi::Run(instance, show);
}
