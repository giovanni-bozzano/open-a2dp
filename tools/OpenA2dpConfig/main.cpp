// OpenA2dpConfig.exe - desktop settings window for the OpenA2dp A2DP source driver.
//
// One window for every sink the driver owns: it lists the control devices, shows the
// session and what the last negotiation produced, and stores the codec
// preference through SET_PREFERENCE. Status is re-read on a timer; a stored change is
// applied by the driver itself (an active stream renegotiates in place), so the window
// never has to wait or guess.

#include "StatusView.h"
#include "Theme.h"

#include "common/ControlDevice.h"
#include "common/SinkDriver.h"
#include "common/StatusReport.h"
#include "common/Text.h"

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/Ldac.h>
#include <OpenA2dp/SbcConfig.h>

#include <windows.h>

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>

// The standard controls need the version 6 common controls, or they draw with the classic
// theme. The status panel paints itself and does not depend on it.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <cfgmgr32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace OpenA2dp::ConfigUi {
namespace {

// The control devices and the IOCTLs are the ones the CLI uses too; the window keeps the names
// its own code reads naturally with.
using Tools::Device;
using Tools::EnumerateDevices;
using Tools::FormatAddress;
using Tools::OpenControl;
using Tools::ReadEndpointFormat;
using Tools::ReadPreference;
using Tools::ReadStatus;
using Tools::ReadVersion;
using Tools::WriteEndpointFormat;
using Tools::WritePreference;
using Tools::EnumerateSinkNodes;
using Tools::ReadSinkDriver;
using Tools::SinkDriver;
using Tools::SinkDriverState;
using Tools::SinkNode;
using Tools::SwitchSinkDriver;

// Nothing here is shared with the driver; the control IDs are the only contract between
// the layout and the command handlers.
enum ControlId : int {
	kSinkCombo = 100,
	kRefreshButton,
	kCopyButton,
	kDetailsCheck,
	kStatusList,
	kCodecCombo,
	kCodecTabs,
	kFormatCombo,
	kBitrateCombo,
	kBitpoolCombo,
	kBlocksCombo,
	kSubbandsCombo,
	kAllocationCombo,
	kPacingLeadCombo,
	kSbcFramesCombo,
	kLdacQualityCombo,
	kApplyButton,
	kDriverButton,
	kRestoreButton,
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
constexpr wchar_t kRestoreText[] = L"Restore defaults";
// The switch names the driver the sink is not on, so the caption reads as the action.
constexpr wchar_t kUseWindowsText[] = L"Use Windows driver";
constexpr wchar_t kUseOpenA2dpText[] = L"Use OpenA2dp driver";
constexpr wchar_t kFinishSwitchText[] = L"Finish driver switch";
constexpr wchar_t kCodecText[] = L"Codec";
constexpr wchar_t kSbcTabText[] = L"SBC";
constexpr wchar_t kAacTabText[] = L"AAC";
constexpr wchar_t kAptxTabText[] = L"aptX";
constexpr wchar_t kAptxHdTabText[] = L"aptX HD";
constexpr wchar_t kLdacTabText[] = L"LDAC";
constexpr wchar_t kAutomaticText[] = L"Automatic (the best both ends support)";
// The widest sample-rate entry any tab can hold, so the columns do not move when the list is
// rebuilt for another codec.
constexpr wchar_t kWidestFormatText[] = L"96000 Hz, stereo";
// The sink picker holds whatever name a sink reports, which the driver may change while a
// stream runs. The window is measured from this sample instead of the live list, so a rename
// cannot resize it; the picker itself takes the space the header's buttons leave.
constexpr wchar_t kSinkPickerText[] = L"Headphones (00:00:00:00:00:00)";
// One hint per codec, under its table: what that codec's settings do and what limits them, so
// the body explains itself instead of repeating every tooltip.
constexpr wchar_t kSbcHintText[] = L"The frame shape, bitpool and frames per packet, clamped to the range "
								   L"this sink advertises.";
constexpr wchar_t kAacHintText[] = L"The bitrate AAC aims for, capped by what one media packet carries.";
constexpr wchar_t kAptxHintText[] = L"16-bit stereo at a fixed bitpool: the sample rate is the only setting.";
constexpr wchar_t kAptxHdHintText[] = L"24-bit stereo at a fixed bitpool: the sample rate is the only setting.";
constexpr wchar_t kLdacHintText[] = L"The quality LDAC encodes at. It is local: the sink is never told "
									L"which mode is in use.";
// The widest entry each field's list can hold, keyed by control. The sample rate list is rebuilt
// for the open codec and the bitrate list is cut by the link, so a live measurement would move
// the columns and resize the window on every tab switch; this is what the lists can grow to, so
// the window is the same size whatever is open. Every number a stored value can add is shorter
// than the "Default (...)" entries here.
struct WidestField
{
	int id;
	const wchar_t* text;
};
constexpr WidestField kWidestFields[] = {
	{kFormatCombo, kWidestFormatText},
	{kBitrateCombo, L"320 kbps"},
	{kBitpoolCombo, L"Automatic (sink's choice)"},
	{kBlocksCombo, L"Default (16)"},
	{kSubbandsCombo, L"Default (8)"},
	{kAllocationCombo, L"Default (loudness)"},
	{kPacingLeadCombo, L"Default (60 ms)"},
	{kSbcFramesCombo, L"Default (fill the packet)"},
	{kLdacQualityCombo, L"High (990 kbps)"},
};

// A combo box is created with the height of its drop-down list, which is never resized.
constexpr int kComboListHeight = 320;
// Rows the session panel shows when the screen has room for them; it scrolls itself for the
// rows that do not fit, so the window never has to scroll as a whole.
constexpr int kMinPanelRows = 6;
constexpr int kMaxPanelRows = 16;

// One table per codec, one tab each and all of them always there, so any codec's settings can
// be read without choosing that codec for the sink: the tabs are the view and the codec choice
// is the drop-down above them.
enum class CodecTab {
	Sbc,
	Aac,
	Aptx,
	AptxHd,
	Ldac,
	Count,
};
constexpr int kTabCount = static_cast<int>(CodecTab::Count);

// A row every tab shows: the sample rate and the pacing lead shape every codec.
constexpr int kAllTabs = -1;

struct TabInfo
{
	const wchar_t* caption;
	const wchar_t* hint;
};

constexpr TabInfo kTabInfo[] = {
	{kSbcTabText, kSbcHintText},
	{kAacTabText, kAacHintText},
	{kAptxTabText, kAptxHintText},
	{kAptxHdTabText, kAptxHdHintText},
	{kLdacTabText, kLdacHintText},
};
static_assert(ARRAYSIZE(kTabInfo) == static_cast<size_t>(kTabCount),
			  "a caption and a hint for every codec tab");
// The combo's entry for a stored choice: 0 is automatic, otherwise the codec's tab plus one,
// because SBC's own value would be 0 too.
uint32_t CodecEntryValue(bool automatic, int tab)
{
	return automatic ? 0u : static_cast<uint32_t>(tab) + 1u;
}

// The codec the driver pins the endpoint format for, which is the one whose settings Apply
// writes: the last negotiation's, else the one the stream runs. This mirrors the driver's own
// rule, so the table never shows settings that a rate change would store for another codec -
// which is what made switching the rate move nothing.
uint32_t EndpointCodec(const Ioctl::Status& status)
{
	if (status.negotiatedCodec > 0 && status.negotiatedCodec < Ioctl::kCodecCount)
	{
		return status.negotiatedCodec;
	}
	return status.codec < Ioctl::kCodecCount ? status.codec : 0;
}

// A label and the control it describes, in one of the two columns of the table body.
struct Field
{
	HWND label;
	HWND control;
	int tab;    // CodecTab, or kAllTabs for a row every tab shows
	int column; // 0 = left, 1 = right
	int row;    // row inside the body
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

// What a sink is doing as far as the window can tell. The driver keeps a sink's control
// device only while the link is up, so every listed sink is connected; a running stream is
// what makes it playing.
enum class DeviceState {
	Connected,
	Playing,
	Suspended,     // a stream the driver suspended because Windows paused the endpoint
	WindowsDriver, // an A2DP node the inbox driver owns: listed, but with no control device
};

// What the sink list says about one report. A configured stream Windows paused is its own state,
// not "connected": pressing play on this PC resumes it where it left off.
DeviceState DeviceStateOf(const Ioctl::Status& status)
{
	switch (status.mediaState)
	{
	case Ioctl::MediaState::Streaming:
		return DeviceState::Playing;
	case Ioctl::MediaState::Suspended:
		return DeviceState::Suspended;
	default:
		return DeviceState::Connected;
	}
}

// One group of controls: the frame the window draws and the caption on its top line. A
// system group box cannot be used, because it draws itself from the system colours, which
// stay light in a process that has only opted into the dark theme.
struct GroupFrame
{
	RECT rect = {};
	std::wstring caption; // empty: a frame with no caption line
};

struct SinkState
{
	std::vector<Device> devices;
	// One entry per device, so the list is only relabelled when the state really changed.
	std::vector<DeviceState> deviceStates;
	// The name each sink reported, read with the state above. It cannot change while the
	// device exists, so it is cached with the list.
	std::vector<std::wstring> deviceNames;
	size_t selected = 0;
	HANDLE control = INVALID_HANDLE_VALUE;
	bool open = false;
	std::string reference;
	// Which A2DP source driver owns the selected sink: it decides what the switch offers, and
	// a sink on the inbox driver has no control device to open at all.
	SinkDriver driver = SinkDriver::Unknown;
	bool driverSwitchable = false;
	// The driver is installed on the node but its restart was refused; the button finishes it.
	bool driverRestartPending = false;
	std::wstring driverProblem;
	Ioctl::Status status = {};
	Ioctl::Preference preference = {};
	Ioctl::EndpointFormat endpointFormat = {};
	Ioctl::VersionInfo version = {};
	std::wstring message; // the outcome of the last action, shown under the controls
	HFONT font = nullptr;  // the window's font, owned by the window
	bool details = false;  // show every field, including the ones the formatted lines hide
	HCMNOTIFICATION notify = nullptr; // control-device arrival/removal
	HWND sinkLabel = nullptr;
	HWND sessionPanel = nullptr;
	// The codec whose table the pane shows, and whether the stored choice lets the driver pick
	// rather than naming a codec. The two are independent: the tabs are the view.
	int tab = 0;
	bool automatic = true;
	HWND codecLabel = nullptr;
	std::vector<HWND> hints; // one hint line per codec, created with the other controls
	// Owns the one tooltip window the fields share; a child of the frame, so it goes with it.
	HWND tooltip = nullptr;
	// The client size the layout settled on. Both only grow, so a measurement that changes with
	// the shown codec or with a sink the driver renames cannot resize the window under the
	// user's hand.
	int laidOutWidth = 0;
	int laidOutHeight = 0;
	std::vector<GroupFrame> groups; // filled by ApplyLayout, drawn by WM_PAINT
};

// Relabels a sink in the list and re-reads every sink's state; defined with the sink list
// code below, which the status refresh calls into.
void UpdateDeviceItemLabel(HWND hwnd, size_t device, DeviceState state);
void RefreshDeviceLabels(HWND hwnd);
// The window's layout; the tab code below rebuilds lists and then asks for it.
void ApplyLayout(HWND hwnd);
// Puts the window in front of whatever the user was looking at when it opens.
void BringToFront(HWND hwnd);
// "name (address)", which the list, the message line and the report share.
std::wstring SinkTitle(const std::string& reference, const std::wstring& name);

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
	input.title = Tools::Utf8(Format(L"Sink %s", Tools::Widen(FormatAddress(state.reference)).c_str()));
	input.status = &state.status;
	input.preference = &state.preference;
	input.version = &state.version;
	input.verbose = state.details;
	const std::wstring report = Tools::Widen(Tools::BuildStatusReport(input));
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

int TextWidth(HDC dc, const wchar_t* text);

// Whether the message line shows its whole sentence. It is one line, cut with an ellipsis when
// the sentence is wider than it, so only a cut sentence needs a box under the pointer.
bool MessageFits(HWND hwnd, const std::wstring& message)
{
	const HWND label = GetDlgItem(hwnd, kMessageLabel);
	RECT rect = {};
	if (label == nullptr || message.empty() || GetClientRect(label, &rect) == FALSE)
	{
		return true;
	}
	const HDC dc = GetDC(label);
	if (dc == nullptr)
	{
		return true;
	}
	// The label is measured with its own font, which is the one it draws with.
	const HFONT font = reinterpret_cast<HFONT>(SendMessageW(label, WM_GETFONT, 0, 0));
	const HGDIOBJ previousFont = font != nullptr ? SelectObject(dc, font) : nullptr;
	const int width = TextWidth(dc, message.c_str());
	if (previousFont != nullptr)
	{
		SelectObject(dc, previousFont);
	}
	ReleaseDC(label, dc);
	return width <= static_cast<int>(rect.right - rect.left);
}

// The message line's tool is registered once, with a text callback that reads the window's own
// message: adding and removing a tool per message is what made the box miss a message that was
// long enough to be cut. Whether it is worth showing is decided when it is about to appear
// (WM_NOTIFY), because only the layout knows how much room the line ended up with.
void AddMessageTooltip(HWND hwnd, HWND label)
{
	const SinkState* state = StateOf(hwnd);
	if (state == nullptr || state->tooltip == nullptr || label == nullptr)
	{
		return;
	}
	TTTOOLINFOW tool = {};
	tool.cbSize = sizeof(tool);
	tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
	tool.hwnd = hwnd;
	tool.uId = reinterpret_cast<UINT_PTR>(label);
	tool.lpszText = LPSTR_TEXTCALLBACKW;
	SendMessageW(state->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
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
// The widest entry a list can hold, or null for a control this window does not size that way.
const wchar_t* WidestFieldText(int id)
{
	for (const WidestField& field : kWidestFields)
	{
		if (field.id == id)
		{
			return field.text;
		}
	}
	return nullptr;
}

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

// Places one control. A combo is given the height it was created with, because that height is
// its drop-down list and not the row it occupies.
void Place(HWND control, int x, int y, int width, int height)
{
	SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Which section of the list a report line belongs to, or null for the raw field dump, whose
// section the report marks itself.
const wchar_t* SectionForLabel(const std::string& label)
{
	if (label == "AVDTP" || label == "media" || label == "codec" || label == "media MTU" ||
		label == "latency" || label == "latency peak" || label == "underruns" || label == "trimmed")
	{
		return L"Session";
	}
	if (label == "sink name" || label == "sink formats")
	{
		return L"Sink";
	}
	if (label == "endpoint format" || label == "preference" || label == "SBC bitpool" ||
		label == "AAC bitrate" || label == "SBC frame")
	{
		return L"Endpoint";
	}
	if (label == "encoder" || label == "rings" || label == "counters" || label == "last error" ||
		label == "pacing lead" || label == "buffered" || label == "capacity")
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
	// A stream that lost audio to a short lead is a warning, not a failure: it plays, but the
	// listener heard the gap. The counts cover everything since the sink appeared and are never
	// reset, so the tone says this sink has lost audio, and the count itself says whether it
	// still is: a number that stopped growing is a fault that stopped.
	if ((label == "underruns" || label == "trimmed") && !value.empty() && value[0] != '0')
	{
		return RowTone::Warning;
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
		return Tools::Widen(label);
	}
	std::string capital = label;
	capital[0] = static_cast<char>(capital[0] - 'a' + 'A');
	return Tools::Widen(capital);
}

// What a session row means, shown when the pointer rests on it. Every explanation has the same
// two halves: what the value is, then what it does to what the listener hears. The labels are the
// ones BuildStatusReport prints, so the text lives beside the row it explains instead of in a
// help file that would drift. A row with no entry is left unexplained - the raw fields, which
// exist to be copied into a bug report rather than read.
struct RowTip
{
	const char* label;
	const wchar_t* text;
};

constexpr RowTip kRowTips[] = {
	{"AVDTP",
	 L"State of the A2DP signalling link - discovery, configuration, open, streaming.\n\n"
	 L"A sink that never reaches Streaming plays nothing, and Refused or Closed means "
	 L"the link is the problem, not the audio format - no codec setting will help."},
	{"media",
	 L"Where the media path stands - idle, waiting for the encoder service, streaming, "
	 L"suspended or failed.\n\n"
	 L"Suspended while a player runs is silence you can see here but would not find "
	 L"explained anywhere else."},
	{"endpoint",
	 L"Whether Windows' render endpoint is running or paused.\n\n"
	 L"Windows pauses the endpoint whenever nothing is playing, so paused with music "
	 L"running means the player stopped, not the driver."},
	{"sink jack",
	 L"The render jack is reported unplugged while an endpoint rebuild is in progress.\n\n"
	 L"Windows opens no stream while it shows unplugged, so the song is silent for that "
	 L"moment and comes back by itself."},
	{"latency",
	 L"Audio the driver holds ahead of live - the pacing lead plus both ring fills. The "
	 L"sink's own buffer adds on top and no A2DP profile reports it.\n\n"
	 L"Most of the delay between a click and hearing it. Shorter feels snappier; too "
	 L"short and the sound starts stuttering."},
	{"latency peak",
	 L"The worst lead-plus-rings value sampled since this stream started, reset by the "
	 L"next stream.\n\n"
	 L"A number well above the lead means something stalled the audio for a moment, "
	 L"even if it recovered before you noticed."},
	{"worst gaps",
	 L"The longest each stage held audio off for since the stream started - render copies, "
	 L"encoder service records, one media send.\n\n"
	 L"How much buffer the machine actually needed. A gap larger than the lead explains "
	 L"the underruns or trims below; nothing larger means the setting is fine."},
	{"underruns",
	 L"Packets built with silence because the ring held no audio the media clock had "
	 L"already asked for. Counted for the endpoint's whole life and never reset.\n\n"
	 L"Each one is a tiny silent patch - a click or a small gap. A number that keeps "
	 L"growing is a problem now; one that never changes is history."},
	{"trimmed",
	 L"Old audio discarded when a ring held more than the lead plus its slack for two "
	 L"observations. Counted for the endpoint's whole life and never reset.\n\n"
	 L"Each one is a small skip - the stream jumps forward to live sound instead of "
	 L"staying behind. Growing means Bluetooth or the PC fell behind for a moment."},
	{"codec",
	 L"The codec, sample rate, channel count and bitrate the running stream uses.\n\n"
	 L"Why one headset sounds duller than another - SBC instead of aptX, or a lower "
	 L"bitrate, is what you hear as flat or mushy."},
	{"media MTU",
	 L"The largest media packet the link agreed on, and the AAC bitrate it can carry.\n\n"
	 L"A small packet carries less audio per send and caps the bitrate, so the link - "
	 L"not the setting - decides how good AAC can sound."},
	{"sink name",
	 L"The Bluetooth name the sink reported.\n\n"
	 L"Only an identifier, so two similar headsets can be told apart."},
	{"sink formats",
	 L"The sample rates, channel counts and codecs the sink advertised in the last "
	 L"discovery.\n\n"
	 L"What the headset can accept. A codec missing here is one the driver will never "
	 L"use, however it is forced."},
	{"listed formats",
	 L"The formats the endpoint is offering Windows right now.\n\n"
	 L"Empty means Windows cannot open a stream at all, so nothing plays until a format "
	 L"is listed again."},
	{"refused streams",
	 L"Streams Windows asked for that the endpoint does not offer, with the last format "
	 L"refused.\n\n"
	 L"A player asking for the wrong format stays silent while this count grows."},
	{"endpoint format",
	 L"The format the endpoint offers Windows - automatic, or pinned and therefore "
	 L"offered alone.\n\n"
	 L"Windows plays the first format listed and ignores the Sound-settings pick, so "
	 L"pinning is what makes your chosen format the one you actually hear."},
	{"preference",
	 L"The stored codec choice: one forced codec, or automatic (LDAC > aptX HD > aptX > "
	 L"AAC > SBC).\n\n"
	 L"Automatic gives the best both ends support; forcing a codec the sink does not "
	 L"advertise is refused rather than silently substituted."},
	{"SBC bitpool",
	 L"The stored SBC bitpool - bits per frame, clamped to the sink's own range.\n\n"
	 L"Higher is better SBC sound in larger packets. The default takes the sink's own "
	 L"choice, usually the best it can decode."},
	{"AAC bitrate",
	 L"The stored AAC bitrate target, capped by what the media MTU can carry.\n\n"
	 L"Higher is better AAC sound and uses more of the link; past the cap it is reduced "
	 L"instead of failing."},
	{"pacing lead",
	 L"How far ahead of the media clock the stream is sent - the stored value, or the "
	 L"running one raised to one packet when the stored one is too short. The kernel pacer and the "
	 L"encoder service use the same number.\n\n"
	 L"The main latency dial. Larger is more forgiving and a little more delayed; "
	 L"smaller is snappier and risks the stutters the counters below report."},
	{"SBC frames",
	 L"Stored cap on SBC frames per packet; 0 fills the MTU.\n\n"
	 L"Fewer frames is a shorter packet and less delay, at the cost of more packets a "
	 L"second for the link to carry."},
	{"SBC frame",
	 L"The stored SBC frame shape - blocks, subbands and allocation.\n\n"
	 L"It changes how SBC spends its bits: longer frames, finer frequency resolution, or "
	 L"bits spread by loudness instead of evenly."},
	{"driver",
	 L"Driver ABI and version, maximum packet and ring sizes, and whether this window "
	 L"speaks the same ABI.\n\n"
	 L"A mismatch means the window and the driver came from different builds - update "
	 L"both, or neither the numbers nor the settings can be trusted."},
	{"encoder",
	 L"Whether the encoder service registered its codecs for this device.\n\n"
	 L"Not registered means AAC and aptX cannot be used at all, so the stream falls "
	 L"back to SBC and sounds worse."},
	{"buffered",
	 L"What the two rings are holding right now, in milliseconds and bytes.\n\n"
	 L"Small and steady is healthy. A number that climbs means audio is piling up and "
	 L"will be trimmed - you hear a skip instead of growing delay."},
	{"capacity",
	 L"The bytes each ring can hold.\n\n"
	 L"A limit, not a target - buffered stays well under it unless something stalled."},
	{"counters",
	 L"Packets sent, records the driver dropped, underruns, failed sends and PCM bytes "
	 L"trimmed.\n\n"
	 L"A one-line summary to copy into a bug report; the rows above say the same "
	 L"things in readable terms."},
	{"last error",
	 L"NTSTATUS of the last media failure, 0 while healthy.\n\n"
	 L"A code here means a send or a ring failed - the stream either recovered by "
	 L"itself or fell back to another codec."},
};

const wchar_t* RowTipFor(const std::string& label)
{
	for (const RowTip& tip : kRowTips)
	{
		if (label == tip.label)
		{
			return tip.text;
		}
	}
	return nullptr;
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
		StatusRow row;
		row.label = RowLabel(label);
		row.value = Tools::Widen(value);
		row.tone = ToneForLabel(label, value);
		if (const wchar_t* const tip = RowTipFor(label))
		{
			row.tip = tip;
		}
		rows.push_back(std::move(row));
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

// What a field means and what changing it costs, shown when the pointer rests on its box. The
// descriptions live here, next to the fields they describe, rather than in a help file that
// would drift from the code; each line is short enough to read without moving the pointer.
struct FieldTip
{
	int id;
	const wchar_t* text;
};

constexpr FieldTip kFieldTips[] = {
	{kSinkCombo,
	 L"The sink this window controls.\n\n"
	 L"Every field below is stored for this sink alone, so two headsets keep their own "
	 L"codec and their own latency - a setting made here never silently reshapes the other."},
	{kCodecCombo,
	 L"Which codec the sink may use.\n\n"
	 L"Automatic lets the driver take the best both ends support - LDAC, then aptX HD, "
	 L"then aptX, then AAC, then SBC - so that is what to leave it on unless a headset "
	 L"misbehaves. Picking one forces it, and one the sink does not advertise is refused. "
	 L"The tabs below are only what you are looking at: a codec can be tuned there without "
	 L"being the one the sink runs."},
	{kBitrateCombo,
	 L"The bitrate the AAC encoder aims for, capped by what the media packet can carry.\n\n"
	 L"Higher is better sound and uses more of the link; the default is the highest the "
	 L"sink accepts, which is what you want unless the link is congested."},
	{kBitpoolCombo,
	 L"SBC's main quality control - bits per frame, clamped to the sink's range.\n\n"
	 L"A higher bitpool is better sound in larger packets; the default takes the sink's "
	 L"own choice, capped at its maximum."},
	{kBlocksCombo,
	 L"Samples per SBC frame, in blocks of subband samples.\n\n"
	 L"More blocks means longer frames - fewer packets a second, but each packet takes "
	 L"longer to encode, which can add a little delay."},
	{kSubbandsCombo,
	 L"Frequency resolution of an SBC frame.\n\n"
	 L"8 Subbands split the band finer than 4 at the same bitpool, for a little more "
	 L"work per frame and usually cleaner treble."},
	{kAllocationCombo,
	 L"How SBC hands out bits across the band.\n\n"
	 L"Loudness follows the ear's sensitivity and usually sounds fuller; "
	 L"signal-to-noise spends the bits evenly."},
	{kLdacQualityCombo,
	 L"The quality LDAC encodes at, which is a local setting - the sink is never told "
	 L"which one is in use.\n\n"
	 L"High is 990 kbps and the reason to use LDAC at all; standard and mobile fit a "
	 L"crowded link at 660 and 330 kbps, and only matter if high stutters with the sink at a "
	 L"distance."},
	{kPacingLeadCombo,
	 L"How far ahead of the media clock the stream is sent - the audio queued in the "
	 L"sink. It has a floor of one packet. The session panel's latency line is this lead plus what "
	 L"the rings hold.\n\n"
	 L"The main latency dial. Shorter is snappier and has less margin, so the sound can "
	 L"start stuttering: 60 ms suits most sinks, 20 ms needs one that buffers little - and is "
	 L"served at the floor instead."},
	{kSbcFramesCombo,
	 L"The most SBC frames one packet carries.\n\n"
	 L"Fewer frames is a shorter packet and less delay, but more packets a second for "
	 L"the link to carry - past what the link can take, that is where stutters come from. The "
	 L"default fills the packet the MTU allows."},
	{kFormatCombo,
	 L"The sample rate and channel count the endpoint offers Windows, limited to the rates "
	 L"the selected codec can play.\n\n"
	 L"Automatic offers every format this codec can play; pinning one makes Windows use "
	 L"exactly it, because Windows plays the first format listed. A rate this codec cannot "
	 L"play is not offered, and is shown as Automatic instead of being pinned."},
};

const wchar_t* TipFor(int id)
{
	for (const FieldTip& tip : kFieldTips)
	{
		if (tip.id == id)
		{
			return tip.text;
		}
	}
	return nullptr;
}

// Adds one control to the shared tooltip. The tooltip control keeps its own copy of the text,
// and TTF_SUBCLASS is what makes it watch the control's mouse without a message hook.
void AddTooltip(HWND tooltip, HWND parent, HWND control, const wchar_t* text)
{
	if (tooltip == nullptr || control == nullptr || text == nullptr)
	{
		return;
	}
	TTTOOLINFOW tool = {};
	tool.cbSize = sizeof(tool);
	tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
	tool.hwnd = parent;
	tool.uId = reinterpret_cast<UINT_PTR>(control);
	tool.lpszText = const_cast<LPWSTR>(text);
	SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

// The tooltip every field box shares: one window, one delay, so hovering any of them behaves
// the same. Created once, with the first set of controls.
HWND EnsureTooltip(HWND hwnd, SinkState* state)
{
	if (state == nullptr)
	{
		return nullptr;
	}
	if (state->tooltip == nullptr)
	{
		state->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
										 WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
										 CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr,
										 GetModuleHandleW(nullptr), nullptr);
		if (state->tooltip != nullptr)
		{
			SetWindowPos(state->tooltip, HWND_TOPMOST, 0, 0, 0, 0,
						 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			// Long enough that moving across the window does not flash tips, short enough that
			// resting on a box answers without a wait.
			SendMessageW(state->tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
			// Wrap long explanations instead of running them across the screen; the fields keep
			// their own line breaks for the sentence boundary.
			SendMessageW(state->tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
		}
	}
	return state->tooltip;
}

// A label and its field, which the layout sizes from the label text and the combo contents.
void CreateField(HWND hwnd, const wchar_t* text, int id, int tab, int column, int row, HFONT font)
{
	const HWND label = CreateChild(hwnd, L"STATIC", text, SS_LEFT, 0, 0, 10, 10, 0, font);
	const HWND control = CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0,
									 10, kComboListHeight, id, font);
	Fields.push_back({label, control, tab, column, row});

	SinkState* state = StateOf(hwnd);
	if (state != nullptr)
	{
		AddTooltip(state->tooltip, hwnd, control, TipFor(id));
	}
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

// The tick box of an owner-drawn check box and the gap after it: the layout sizes the
// control with them and the window paints it with them, so the caption is never cut off.
int CheckSide(int textHeight)
{
	return textHeight - 4;
}
int CheckTextGap(int textHeight)
{
	return textHeight / 2;
}

// The tab of a codec, and the codec of a tab; -1 when the value is not one of them.
int TabOfCodec(uint32_t codec)
{
	switch (codec)
	{
	case Ioctl::kCodecSbc:
		return static_cast<int>(CodecTab::Sbc);
	case Ioctl::kCodecAac:
		return static_cast<int>(CodecTab::Aac);
	case Ioctl::kCodecAptx:
		return static_cast<int>(CodecTab::Aptx);
	case Ioctl::kCodecAptxHd:
		return static_cast<int>(CodecTab::AptxHd);
	case Ioctl::kCodecLdac:
		return static_cast<int>(CodecTab::Ldac);
	default:
		return -1;
	}
}

uint32_t CodecOfTab(int tab)
{
	switch (static_cast<CodecTab>(tab))
	{
	case CodecTab::Sbc:
		return Ioctl::kCodecSbc;
	case CodecTab::Aac:
		return Ioctl::kCodecAac;
	case CodecTab::Aptx:
		return Ioctl::kCodecAptx;
	case CodecTab::AptxHd:
		return Ioctl::kCodecAptxHd;
	default:
		return Ioctl::kCodecLdac;
	}
}

// The codec the table opens on: the stored choice when one is forced, otherwise the codec the
// stream runs or the last negotiation chose, so the window starts on what is actually in use.
int TabForState(const SinkState& state)
{
	if ((state.preference.flags & Ioctl::kPreferenceForceCodec) != 0)
	{
		const int forced = TabOfCodec(state.preference.codec);
		if (forced >= 0)
		{
			return forced;
		}
	}
	const int running = TabOfCodec(EndpointCodec(state.status));
	return running >= 0 ? running : static_cast<int>(CodecTab::Sbc);
}

// The sample rates one codec can play, in the order the endpoint prefers them, and the channel
// counts it can carry as a mask (1 = mono, 2 = stereo). Every rate the endpoint offers is
// measured against the codec's own limit; SBC's is the one advertisement the status reports per
// codec, so that tab is narrowed to what the sink itself listed.
int TabFormats(int tab, const Ioctl::Status& status, uint32_t* rates, uint32_t& channelMask)
{
	int count = 0;
	channelMask = 3;
	for (uint32_t i = 0; i < Ioctl::kEndpointRateCount; ++i)
	{
		const uint32_t rate = Ioctl::kEndpointRates[i];
		bool plays = false;
		if (static_cast<CodecTab>(tab) == CodecTab::Sbc)
		{
			plays = status.sinkRates == 0 ||
					(status.sinkRates & SbcConfig::AdvertisedRateBit(rate)) != 0;
		}
		else if (static_cast<CodecTab>(tab) == CodecTab::Aac)
		{
			plays = AacConfig::EncoderSupportsRate(rate);
		}
		else if (static_cast<CodecTab>(tab) == CodecTab::Ldac)
		{
			// LDAC runs stereo only, so its 88.2 and 96 kHz fit where the lower rates do.
			plays = Ldac::RateBit(rate) != 0;
			channelMask = 2;
		}
		else
		{
			// aptX and aptX HD are 44.1 and 48 kHz stereo and have no other rate.
			plays = rate == 44100 || rate == 48000;
			channelMask = 2;
		}
		if (plays)
		{
			rates[count++] = rate;
		}
	}
	if (static_cast<CodecTab>(tab) == CodecTab::Sbc && status.sinkChannels != 0)
	{
		channelMask = status.sinkChannels;
	}
	// Worst to best, so the list reads as a ladder rather than following the endpoint's own
	// preference order, which leads with 44.1 kHz because Windows plays the first entry.
	std::sort(rates, rates + count);
	return count;
}

// Builds the lists whose entries depend on the codec and on the link: the sample rates this
// codec can play, and the AAC bitrates one media packet can carry. A value the codec cannot use
// is never offered, so the table only shows what Apply can actually store.
void RebuildTabLists(HWND hwnd)
{
	const SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	uint32_t rates[8] = {};
	uint32_t channelMask = 3;
	const int count = TabFormats(state->tab, state->status, rates, channelMask);
	SendMessageW(GetDlgItem(hwnd, kFormatCombo), CB_RESETCONTENT, 0, 0);
	AddComboItem(hwnd, kFormatCombo, L"Automatic (every format this codec plays)", 0);
	for (int i = 0; i < count; ++i)
	{
		const uint32_t rate = rates[i];
		if ((channelMask & 1) != 0)
		{
			AddComboItem(hwnd, kFormatCombo, Format(L"%u Hz, mono", rate).c_str(),
						 FormatComboValue(rate, 1));
		}
		if ((channelMask & 2) != 0)
		{
			AddComboItem(hwnd, kFormatCombo, Format(L"%u Hz, stereo", rate).c_str(),
						 FormatComboValue(rate, 2));
		}
	}

	SendMessageW(GetDlgItem(hwnd, kBitrateCombo), CB_RESETCONTENT, 0, 0);
	AddComboItem(hwnd, kBitrateCombo, L"Highest the sink accepts", 0);
	for (size_t i = 0; i < AacConfig::kSupportedBitrateCount; ++i)
	{
		const uint32_t bitrate = AacConfig::kSupportedBitrates[i];
		if (state->status.bitrateLimit != 0 && bitrate > state->status.bitrateLimit)
		{
			continue;
		}
		AddComboItem(hwnd, kBitrateCombo, Format(L"%u kbps", bitrate / 1000).c_str(), bitrate);
	}
}

// Shows the stored values in the controls. The sample rate falls back to the automatic entry
// when the pinned format is not one the open codec can play, which is the only line of the
// table whose stored value a codec can rule out.
void SelectPreferenceValues(HWND hwnd)
{
	const SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	const Ioctl::Preference& preference = state->preference;
	SelectComboValue(hwnd, kBitrateCombo, preference.aacBitrate);
	SelectComboValue(hwnd, kBlocksCombo, preference.sbcBlocks);
	SelectComboValue(hwnd, kSubbandsCombo, preference.sbcSubbands);
	SelectComboValue(hwnd, kAllocationCombo, preference.sbcAllocation);
	SelectComboValueOrAdd(hwnd, kBitpoolCombo, preference.sbcBitpool);
	SelectComboValueOrAdd(hwnd, kPacingLeadCombo, preference.prerollMs);
	SelectComboValueOrAdd(hwnd, kSbcFramesCombo, preference.sbcFramesPerPacket);
	SelectComboValue(hwnd, kLdacQualityCombo, preference.ldacQuality);
	// The sample rate row is the open codec's own entry: every codec keeps its own format.
	const int tab = state->tab >= 0 && state->tab < kTabCount ? state->tab : 0;
	SelectComboValue(hwnd, kFormatCombo,
					 FormatComboValue(state->endpointFormat.sampleRate[tab],
									  state->endpointFormat.channels[tab]));
}

// Shows one codec's table. The tabs only say what the pane shows: the codec the sink runs is
// the drop-down's business, so looking at AAC's settings never forces AAC.
void ShowCodecTable(HWND hwnd, int tab)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr || tab < 0 || tab >= kTabCount)
	{
		return;
	}
	state->tab = tab;
	const HWND tabs = GetDlgItem(hwnd, kCodecTabs);
	if (tabs != nullptr)
	{
		SendMessageW(tabs, TCM_SETCURSEL, static_cast<WPARAM>(tab), 0);
	}
	RebuildTabLists(hwnd);
	SelectPreferenceValues(hwnd);
	ApplyLayout(hwnd);
	// The body's rows are replaced, so the whole client area is repainted.
	InvalidateRect(hwnd, nullptr, TRUE);
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
	// The table body fills its pane: no inset, so a row's label starts where the pane begins.
	const int rowHeight = textHeight + 10;
	const int arrow = GetSystemMetrics(SM_CXVSCROLL) + 6;

	// Each column is as wide as its widest label plus its widest combo content. The other
	// codecs' rows count too, so the columns do not move when another tab is opened.
	int labelWidth[2] = {0, 0};
	int fieldWidth[2] = {0, 0};
	for (const Field& field : Fields)
	{
		wchar_t text[256] = {};
		GetWindowTextW(field.label, text, ARRAYSIZE(text));
		labelWidth[field.column] = MaxOf(labelWidth[field.column], TextWidth(dc, text));
		// The widest entry the list can hold, not the entries it holds now: measuring the live
		// list would size the column to the open codec's own rates.
		const wchar_t* widest = WidestFieldText(GetDlgCtrlID(field.control));
		const int content =
				widest != nullptr ? TextWidth(dc, widest) : ComboContentWidth(dc, field.control);
		fieldWidth[field.column] = MaxOf(fieldWidth[field.column], content + arrow);
	}
	const int columnGap = gap * 3;
	const int columnWidth[2] = {labelWidth[0] + gap + fieldWidth[0], labelWidth[1] + gap + fieldWidth[1]};
	const int contentWidth = columnWidth[0] + columnGap + columnWidth[1];

	// The header row: the sink picker and the three buttons.
	const int sinkLabelWidth = TextWidth(dc, kSinkText);
	const int sinkComboWidth = TextWidth(dc, kSinkPickerText) + arrow;
	const int refreshWidth = TextWidth(dc, kRefreshText) + gap * 2;
	const int copyWidth = TextWidth(dc, kCopyText) + gap * 2;
	const int detailsWidth =
			CheckSide(textHeight) + CheckTextGap(textHeight) + TextWidth(dc, kDetailsText) + 4;
	const int headerWidth = margin + sinkLabelWidth + gap + sinkComboWidth + gap + refreshWidth + gap +
							copyWidth + gap + detailsWidth + margin;

	// The bottom row holds the driver switch and Apply, and the message beside them keeps a
	// button's width, so the switch cannot squeeze the outcome of the last action out of view.
	const int applyWidth = TextWidth(dc, kApplyText) + gap * 4;
	const int applyHeight = rowHeight + 4;
	const int driverWidth = MaxOf(MaxOf(TextWidth(dc, kUseWindowsText), TextWidth(dc, kUseOpenA2dpText)),
								  TextWidth(dc, kFinishSwitchText)) + gap * 4;
	const int restoreWidth = TextWidth(dc, kRestoreText) + gap * 4;
	const int bottomWidth = margin + applyWidth + gap + driverWidth + gap + applyWidth + gap + restoreWidth + margin;

	// The codec choice is a drop-down above the pane, as wide as its widest entry, and the pane's
	// own tabs have to fit side by side.
	const int codecLabelWidth = TextWidth(dc, kCodecText);
	int codecWidth = TextWidth(dc, kAutomaticText);
	int stripWidth = 0;
	for (int tab = 0; tab < kTabCount; ++tab)
	{
		codecWidth = MaxOf(codecWidth, TextWidth(dc, kTabInfo[tab].caption));
		stripWidth += TextWidth(dc, kTabInfo[tab].caption) + gap * 3;
	}
	const int codecComboWidth = codecWidth + arrow;
	const int selectorWidth = margin + codecLabelWidth + gap + codecComboWidth + margin;

	int width = contentWidth + margin * 2 + 2;
	if (headerWidth > width)
	{
		width = headerWidth;
	}
	if (bottomWidth > width)
	{
		width = bottomWidth;
	}
	if (selectorWidth > width)
	{
		width = selectorWidth;
	}
	if (stripWidth + margin * 2 > width)
	{
		width = stripWidth + margin * 2;
	}
	// The width only grows: a transient measurement must not move the window the user placed.
	if (width < state->laidOutWidth)
	{
		width = state->laidOutWidth;
	}
	else
	{
		state->laidOutWidth = width;
	}
	const int groupLeft = margin;
	const int groupWidth = width - margin * 2;
	const int innerLeft = groupLeft + 1;
	const int innerWidth = groupWidth - 2;

	// The tab control's own height comes from its first item, so a larger system font or a
	// longer caption cannot cut the tabs off; the body frame starts under it.
	const HWND tabs = GetDlgItem(hwnd, kCodecTabs);
	RECT tabItem = {0, 0, 0, 0};
	int tabHeight = textHeight + gap * 2;
	if (tabs != nullptr && SendMessageW(tabs, TCM_GETITEMCOUNT, 0, 0) > 0 &&
		SendMessageW(tabs, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&tabItem)) != FALSE)
	{
		// The item plus the row the shared line is drawn in: the strip's bottom row is the table
		// body's top edge, which is what makes the open tab part of its pane.
		tabHeight = (tabItem.bottom - tabItem.top) + 3;
	}

	// The body is as tall as the longest codec's table and its hint, so choosing another codec
	// never moves the window the user placed.
	int rows = 1;
	int hintHeight = 0;
	for (int tab = 0; tab < kTabCount; ++tab)
	{
		int tabRows = 1;
		for (const Field& field : Fields)
		{
			if ((field.tab == tab || field.tab == kAllTabs) && field.row + 1 > tabRows)
			{
				tabRows = field.row + 1;
			}
		}
		rows = MaxOf(rows, tabRows);
		hintHeight = MaxOf(hintHeight, TextHeight(dc, kTabInfo[tab].hint, innerWidth));
	}
	hintHeight = MaxOf(hintHeight, textHeight);
	// The outcome of the last action is a line of its own above the buttons: beside them it was
	// cut to a few words, which is no use when it is the reason a setting did not apply.
	const int bottomHeight = textHeight + gap + applyHeight;

	// The session panel takes as many rows as the screen can show and scrolls for the rest.
	const int statusRowHeight = MaxOf(StatusViewRowHeight(hwnd, kStatusList), 1);
	const int bodyHeight = rows * rowHeight + hintHeight + 2;
	int fixedHeight = margin + rowHeight + gap * 2 + bottomHeight + margin;
	fixedHeight += rowHeight + gap + tabHeight + bodyHeight + gap;
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
	// The panel reaches the frame's bottom line, so the last row ends where the pane does and
	// no empty band is left under it; the one extra pixel is the frame's own border.
	const int sessionHeight = panelHeight + 2;
	const int codecTop = sessionTop + sessionHeight + gap;
	const int stripTop = codecTop + rowHeight + gap;
	const int bodyTop = stripTop + tabHeight;
	const int bottomTop = bodyTop + bodyHeight + gap;
	// The height only grows for the same reason as the width.
	const int height = MaxOf(bottomTop + bottomHeight + margin, state->laidOutHeight);
	state->laidOutHeight = height;

	// Header row, with the sink picker filling the space the buttons leave.
	Place(state->sinkLabel, margin, headerTop + (rowHeight - textHeight) / 2, sinkLabelWidth, textHeight);
	const int detailsLeft = width - margin - detailsWidth;
	const int copyLeft = detailsLeft - gap - copyWidth;
	const int refreshLeft = copyLeft - gap - refreshWidth;
	const int sinkLeft = margin + sinkLabelWidth + gap;
	const HWND sinkCombo = GetDlgItem(hwnd, kSinkCombo);
	Place(sinkCombo, sinkLeft, headerTop, refreshLeft - gap - sinkLeft, kComboListHeight);
	// A drop-down combo is only as tall as its closed field, whatever height it is given, so
	// the controls beside it are given the height it settled at instead of a guessed one.
	RECT comboRect = {};
	const int controlHeight = GetWindowRect(sinkCombo, &comboRect) != FALSE && comboRect.bottom > comboRect.top
								  ? static_cast<int>(comboRect.bottom - comboRect.top)
								  : rowHeight;
	Place(GetDlgItem(hwnd, kDetailsCheck), detailsLeft, headerTop, detailsWidth, controlHeight);
	Place(GetDlgItem(hwnd, kCopyButton), copyLeft, headerTop, copyWidth, controlHeight);
	Place(GetDlgItem(hwnd, kRefreshButton), refreshLeft, headerTop, refreshWidth, controlHeight);

	// The session frame and the settings frame. The window draws them, so only the rectangles
	// are kept here; neither has a caption - the session is the pane above the choice, and the
	// settings pane is headed by its tab strip.
	state->groups.clear();
	state->groups.push_back(
			{groupLeft, sessionTop, groupLeft + groupWidth, sessionTop + sessionHeight, std::wstring()});
	// The settings pane has no caption of its own: the tab strip on its top edge is its head, so
	// the open tab reads as the head of the table under it.
	state->groups.push_back(
			{groupLeft, bodyTop - 1, groupLeft + groupWidth, bodyTop + bodyHeight, std::wstring()});
	// The panel reaches the frame's own edge, so its section bands and rows run the full width of
	// the pane they belong to instead of stopping at the table body's border.
	Place(state->sessionPanel, groupLeft + 1, sessionTop + 1, groupWidth - 2, panelHeight);

	// The codec choice, above the pane whose tabs are only what is being looked at; the pane
	// below it is the window's, which keeps its own frame and the palette's colours, and the tab
	// strip sits on that frame's top edge.
	Place(state->codecLabel, innerLeft, codecTop + (rowHeight - textHeight) / 2, codecLabelWidth,
		  textHeight);
	Place(GetDlgItem(hwnd, kCodecCombo), innerLeft + codecLabelWidth + gap, codecTop, codecComboWidth,
		  kComboListHeight);
	if (tabs != nullptr)
	{
		Place(tabs, groupLeft, stripTop, groupWidth, tabHeight);
	}

	// The top of the body is the same on every codec; the rows below it belong to the shown
	// codec, so the other codecs' controls are hidden rather than destroyed - their stored
	// values keep.
	const int contentTop = bodyTop + 1;
	for (const Field& field : Fields)
	{
		const bool visible = field.tab == kAllTabs || field.tab == state->tab;
		ShowWindow(field.label, visible ? SW_SHOW : SW_HIDE);
		ShowWindow(field.control, visible ? SW_SHOW : SW_HIDE);
		if (!visible)
		{
			continue;
		}
		const int top = contentTop + field.row * rowHeight;
		const int left = innerLeft + (field.column == 0 ? 0 : columnWidth[0] + columnGap);
		Place(field.label, left, top + (rowHeight - textHeight) / 2, labelWidth[field.column], textHeight);
		Place(field.control, left + labelWidth[field.column] + gap, top, fieldWidth[field.column],
			  kComboListHeight);
	}
	for (int tab = 0; tab < kTabCount && tab < static_cast<int>(state->hints.size()); ++tab)
	{
		Place(state->hints[tab], innerLeft, contentTop + rows * rowHeight, innerWidth, hintHeight);
		ShowWindow(state->hints[tab], tab == state->tab ? SW_SHOW : SW_HIDE);
	}

	// The message keeps the whole width above the buttons, so a refusal reads in full; the buttons
	// sit under its right end.
	Place(GetDlgItem(hwnd, kMessageLabel), margin, bottomTop, width - margin * 2, textHeight);
	const int buttonsTop = bottomTop + textHeight + gap;
	const int applyLeft = width - margin - applyWidth;
	Place(GetDlgItem(hwnd, kApplyButton), applyLeft, buttonsTop, applyWidth, applyHeight);
	const int driverLeft = applyLeft - gap - driverWidth;
	Place(GetDlgItem(hwnd, kDriverButton), driverLeft, buttonsTop, driverWidth, applyHeight);
	const int restoreLeft = driverLeft - gap - restoreWidth;
	Place(GetDlgItem(hwnd, kRestoreButton), restoreLeft, buttonsTop, restoreWidth, applyHeight);

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
	// A second pass over the same strings - the list refilling after a sink connects, say -
	// leaves the window where the user has it instead of re-centering it.
	RECT placed = {};
	GetWindowRect(hwnd, &placed);
	if (placed.right - placed.left == windowWidth && placed.bottom - placed.top == windowHeight)
	{
		return;
	}
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

constexpr UINT_PTR kTabSubclassId = 1;

// The tab strip heads the table, and the tab control has no dark part on this Windows: themed
// or not, it paints its items and the strip around them light. So the strip is painted from the
// palette here - the open tab in the window's own background, so it reads as part of the table
// below it, the others on the section colour - while the control keeps the items, the selection
// and the keyboard, which is what makes it a tab control and not a picture of one.
void PaintCodecTabs(HWND tab)
{
	PAINTSTRUCT paint = {};
	const HDC dc = BeginPaint(tab, &paint);
	RECT client = {};
	GetClientRect(tab, &client);
	FillRect(dc, &client, BackgroundBrush());
	if (dc != nullptr)
	{
		const Colors& colors = ThemeColors();
		const HFONT font = reinterpret_cast<HFONT>(SendMessageW(tab, WM_GETFONT, 0, 0));
		const HGDIOBJ previousFont = font != nullptr ? SelectObject(dc, font) : nullptr;
		const HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
		const HGDIOBJ previousPen = SelectObject(dc, pen);
		const LRESULT selected = SendMessageW(tab, TCM_GETCURSEL, 0, 0);
		const int count = static_cast<int>(SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0));
		RECT openTab = {0, 0, 0, 0};
		for (int index = 0; index < count; ++index)
		{
			RECT item = {};
			if (SendMessageW(tab, TCM_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item)) == FALSE)
			{
				continue;
			}
			const bool open = index == selected;
			// The open tab carries the window's own background and runs down to the shared line, so
			// it reads as the head of the table under it; a closed tab is a box of its own, lifted
			// off that line on the section colour.
			RECT face = item;
			if (open)
			{
				face.bottom = client.bottom;
				openTab = face;
			}
			SetDCBrushColor(dc, open ? colors.background : colors.section);
			FillRect(dc, &face, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

			const int bottom = open ? client.bottom - 1 : item.bottom - 1;
			MoveToEx(dc, item.left, bottom, nullptr);
			LineTo(dc, item.left, item.top);
			LineTo(dc, item.right - 1, item.top);
			LineTo(dc, item.right - 1, bottom);
			if (!open)
			{
				LineTo(dc, item.left, bottom);
			}

			wchar_t text[64] = {};
			TCITEMW info = {};
			info.mask = TCIF_TEXT;
			info.pszText = text;
			info.cchTextMax = ARRAYSIZE(text);
			SendMessageW(tab, TCM_GETITEMW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&info));
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, open ? colors.text : colors.muted);
			RECT label = item;
			label.left += 6;
			label.right -= 6;
			DrawTextW(dc, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}

		// The line the table body's top edge continues, drawn here because only the strip knows
		// where the open tab is: it stops at the tab, which is what joins the two.
		const int line = client.bottom - 1;
		RECT left = {0, line, openTab.right > openTab.left ? openTab.left : client.right, line + 1};
		RECT right = {openTab.right, line, client.right, line + 1};
		SetDCBrushColor(dc, colors.border);
		FillRect(dc, &left, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		if (openTab.right > openTab.left && openTab.right < client.right)
		{
			FillRect(dc, &right, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		}

		SelectObject(dc, previousPen);
		DeleteObject(pen);
		if (previousFont != nullptr)
		{
			SelectObject(dc, previousFont);
		}
	}
	EndPaint(tab, &paint);
}

// The strip's painting is this window's; everything else - clicks, the keyboard, the selection
// a TCM_SETCURSEL moves - stays the control's.
LRESULT CALLBACK TabSubclass(HWND tab, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
							 DWORD_PTR reference)
{
	UNREFERENCED_PARAMETER(wParam);
	UNREFERENCED_PARAMETER(lParam);
	UNREFERENCED_PARAMETER(id);
	UNREFERENCED_PARAMETER(reference);
	switch (message)
	{
	case WM_PAINT:
		PaintCodecTabs(tab);
		return 0;
	case WM_ERASEBKGND:
		return 1; // the paint fills every pixel
	case WM_NCDESTROY:
		RemoveWindowSubclass(tab, TabSubclass, kTabSubclassId);
		break;
	default:
		break;
	}
	return DefSubclassProc(tab, message, wParam, lParam);
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
	// The fields' tooltips come from one window, which has to exist before the fields do.
	EnsureTooltip(hwnd, state);

	if (state != nullptr)
	{
		state->sinkLabel = CreateChild(hwnd, L"STATIC", kSinkText, SS_LEFT, 0, 0, 10, 10, 0, font);
	}
	const HWND sinkCombo = CreateChild(hwnd, L"COMBOBOX", L"",
									  CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, kComboListHeight,
									  kSinkCombo, font);
	if (state != nullptr)
	{
		AddTooltip(state->tooltip, hwnd, sinkCombo, TipFor(kSinkCombo));
	}
	CreateChild(hwnd, L"BUTTON", kRefreshText, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, kRefreshButton,
				font);
	// The clipboard text is the report itself, so it can be pasted next to a CLI paste.
	CreateChild(hwnd, L"BUTTON", kCopyText, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, kCopyButton, font);
	CreateChild(hwnd, L"BUTTON", kDetailsText, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 10, 10, kDetailsCheck,
				font);

	// The codec choice: automatic, or one of the five. The tab strip on the settings pane is a
	// separate thing - it only says which settings are being looked at.
	if (state != nullptr)
	{
		state->codecLabel = CreateChild(hwnd, L"STATIC", kCodecText, SS_LEFT, 0, 0, 10, 10, 0, font);
	}
	const HWND codecCombo = CreateChild(hwnd, L"COMBOBOX", L"",
									  CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, kComboListHeight,
									  kCodecCombo, font);
	AddComboItem(hwnd, kCodecCombo, kAutomaticText, 0);
	for (int tab = 0; tab < kTabCount; ++tab)
	{
		AddComboItem(hwnd, kCodecCombo, kTabInfo[tab].caption, CodecEntryValue(false, tab));
	}
	SendMessageW(codecCombo, CB_SETCURSEL, 0, 0);
	if (state != nullptr)
	{
		AddTooltip(state->tooltip, hwnd, codecCombo, TipFor(kCodecCombo));
	}

	// The settings pane's tab strip: one tab per codec, all of them always there, so any
	// codec's settings can be read without choosing that codec for the sink. The control owns
	// the items, the selection and the keyboard; only the strip's colours are the window's.
	const HWND tabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
									  WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_HOTTRACK, 0, 0, 10, 10, hwnd,
									  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCodecTabs)),
									  GetModuleHandleW(nullptr), nullptr);
	if (tabs != nullptr)
	{
		if (font != nullptr)
		{
			SendMessageW(tabs, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}
		for (int tab = 0; tab < kTabCount; ++tab)
		{
			TCITEMW item = {};
			item.mask = TCIF_TEXT;
			item.pszText = const_cast<LPWSTR>(kTabInfo[tab].caption);
			SendMessageW(tabs, TCM_INSERTITEMW, static_cast<WPARAM>(tab), reinterpret_cast<LPARAM>(&item));
		}
		SendMessageW(tabs, TCM_SETCURSEL, 0, 0);
		SetWindowSubclass(tabs, TabSubclass, kTabSubclassId, 0);
	}
	CreateStatusView(hwnd, kStatusList, 0, 0, 10, 10, font);
	if (state != nullptr)
	{
		state->sessionPanel = GetDlgItem(hwnd, kStatusList);
	}

	// The top of every table: the two settings every codec shares. The rows below them belong
	// to the shown codec alone, so a box is only offered where it changes something.
	CreateField(hwnd, L"Sample rate", kFormatCombo, kAllTabs, 0, 0, font);
	CreateField(hwnd, L"Pacing lead", kPacingLeadCombo, kAllTabs, 1, 0, font);
	CreateField(hwnd, L"Bitpool", kBitpoolCombo, static_cast<int>(CodecTab::Sbc), 0, 1, font);
	CreateField(hwnd, L"Blocks", kBlocksCombo, static_cast<int>(CodecTab::Sbc), 1, 1, font);
	CreateField(hwnd, L"Subbands", kSubbandsCombo, static_cast<int>(CodecTab::Sbc), 0, 2, font);
	CreateField(hwnd, L"Allocation", kAllocationCombo, static_cast<int>(CodecTab::Sbc), 1, 2, font);
	CreateField(hwnd, L"Frames per packet", kSbcFramesCombo, static_cast<int>(CodecTab::Sbc), 0, 3, font);
	CreateField(hwnd, L"Bitrate", kBitrateCombo, static_cast<int>(CodecTab::Aac), 0, 1, font);
	CreateField(hwnd, L"Quality", kLdacQualityCombo, static_cast<int>(CodecTab::Ldac), 0, 1, font);
	if (state != nullptr)
	{
		// One hint line per codec, under its table; the shown codec's is the only one shown.
		state->hints.assign(kTabCount, nullptr);
		for (int tab = 0; tab < kTabCount; ++tab)
		{
			state->hints[tab] =
					CreateChild(hwnd, L"STATIC", kTabInfo[tab].hint, SS_LEFT, 0, 0, 10, 10, 0, font);
		}
	}

	CreateChild(hwnd, L"BUTTON", kApplyText, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, kApplyButton, font);
	CreateChild(hwnd, L"BUTTON", kUseOpenA2dpText, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, kDriverButton,
				font);
	// The way back: the same two IOCTLs `OpenA2dpCtl reset` sends, so a setting that turned out
	// worse than expected is undone from the window it was made in.
	CreateChild(hwnd, L"BUTTON", kRestoreText, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, kRestoreButton,
				font);
	const HWND messageLabel = CreateChild(hwnd, L"STATIC", L"", SS_ENDELLIPSIS, 0, 0, 10, 10, kMessageLabel, font);
	AddMessageTooltip(hwnd, messageLabel);
	EnableWindow(GetDlgItem(hwnd, kDriverButton), FALSE);

	// The sample rate and AAC bitrate lists depend on the codec and on the link, so
	// RebuildTabLists builds them; the rest are fixed ladders.

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

	// The lead the stream is sent ahead of the clock; the ladder skips 60 because that is the
	// default entry above it.
	AddComboItem(hwnd, kPacingLeadCombo, L"Default (60 ms)", 0);
	for (const uint32_t lead : {20u, 30u, 40u, 50u, 80u, 100u, 150u, 200u})
	{
		AddComboItem(hwnd, kPacingLeadCombo, Format(L"%u ms", lead).c_str(), lead);
	}

	// SBC frames per packet: fewer frames is a shorter packet with less audio in it.
	AddComboItem(hwnd, kSbcFramesCombo, L"Default (fill the packet)", 0);
	for (const uint32_t frames : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u})
	{
		AddComboItem(hwnd, kSbcFramesCombo, Format(L"%u per packet", frames).c_str(), frames);
	}

	// LDAC's quality mode is local: it is not negotiated, so it is simply what this sink's
	// streams encode at.
	AddComboItem(hwnd, kLdacQualityCombo, L"High (990 kbps)", 0);
	AddComboItem(hwnd, kLdacQualityCombo, L"Standard (660 kbps)", 1);
	AddComboItem(hwnd, kLdacQualityCombo, L"Mobile (330 kbps)", 2);

	for (const int id : {kBitpoolCombo, kBlocksCombo, kSubbandsCombo, kAllocationCombo, kPacingLeadCombo,
						 kSbcFramesCombo, kLdacQualityCombo})
	{
		SendMessageW(GetDlgItem(hwnd, id), CB_SETCURSEL, 0, 0);
	}
	// The first open rebuilds the codec-dependent lists from the sink's own advertisement.
	RebuildTabLists(hwnd);
	ApplyLayout(hwnd);
}

// The controls show the stored preference, so a value the driver refused or clamped is
// visible as soon as the window reads it back. The drop-down shows the choice and the pane's
// tabs open on the codec in use, which is what makes the window start on the settings the sink
// actually runs.
void LoadPreferenceIntoControls(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	state->automatic = (state->preference.flags & Ioctl::kPreferenceForceCodec) == 0;
	const HWND tabs = GetDlgItem(hwnd, kCodecTabs);
	if (tabs != nullptr)
	{
		SendMessageW(tabs, TCM_SETCURSEL, static_cast<WPARAM>(state->tab), 0);
	}
	// The drop-down shows the stored choice, not the open tab. A stored codec this window has
	// no tab for reads as the automatic choice, which is what the driver does with it as well.
	const int forced = TabOfCodec(state->preference.codec);
	SelectComboValue(hwnd, kCodecCombo, CodecEntryValue(state->automatic || forced < 0, forced));
	RebuildTabLists(hwnd);
	SelectPreferenceValues(hwnd);
	ApplyLayout(hwnd);
	InvalidateRect(hwnd, nullptr, TRUE);
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

// The switch offers the driver the sink is not on, and only while the other package is there to
// move it to: a button that stays enabled would promise a switch that cannot happen.
void UpdateDriverButton(HWND hwnd)
{
	const SinkState* state = StateOf(hwnd);
	const HWND button = GetDlgItem(hwnd, kDriverButton);
	if (state == nullptr || button == nullptr)
	{
		return;
	}
	SetWindowTextW(button, state->driverRestartPending		   ? kFinishSwitchText
						   : state->driver == SinkDriver::OpenA2dp ? kUseWindowsText
																   : kUseOpenA2dpText);
	EnableWindow(button, state->driverSwitchable || state->driverRestartPending);
}

// Reads which driver owns the sink, and whether the other one is there to switch to.
void ReadSelectedDriver(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	SinkDriverState driver;
	std::string error;
	if (ReadSinkDriver(state->reference, driver, error))
	{
		state->driver = driver.current;
		state->driverSwitchable = driver.switchable;
		state->driverRestartPending = driver.restartPending;
		state->driverProblem = Tools::Widen(driver.problem);
	}
	else
	{
		state->driver = SinkDriver::Unknown;
		state->driverSwitchable = false;
		state->driverRestartPending = false;
		state->driverProblem = Tools::Widen(error);
	}
	UpdateDriverButton(hwnd);
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
		state->driver = SinkDriver::Unknown;
		state->driverSwitchable = false;
		state->driverRestartPending = false;
		UpdateDriverButton(hwnd);
		state->message = L"No sink found: connect a paired A2DP sink, then press Refresh.";
		SetMessage(hwnd, state->message);
		RefreshStatusView(hwnd);
		return;
	}

	const Device& device = state->devices[state->selected];
	state->reference = device.reference;
	ReadSelectedDriver(hwnd);
	if (device.path.empty())
	{
		// Listed from its A2DP node: the inbox driver owns the sink, so there is no control
		// device to read and the switch is the only thing this window can do with it.
		state->message = SinkTitle(device.reference, std::wstring()) +
						 L" runs the Windows A2DP driver; " +
						 (state->driverSwitchable ? L"switch it to use these settings."
												  : state->driverProblem + L".");
		SetMessage(hwnd, state->message);
		RefreshStatusView(hwnd);
		return;
	}
	std::string error;
	const HANDLE control = OpenControl(device, error);
	if (control == INVALID_HANDLE_VALUE)
	{
		state->message = Tools::Widen(error);
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
		state->message = Tools::Widen(error);
		SetMessage(hwnd, state->message);
		RefreshStatusView(hwnd);
		return;
	}

	state->open = true;
	EnableWindow(GetDlgItem(hwnd, kApplyButton), TRUE);
	// A newly opened sink starts on the codec it runs; a later read-back leaves the open tab
	// where it is, because the user may be tuning a codec the sink does not use.
	state->tab = TabForState(*state);
	LoadPreferenceIntoControls(hwnd);
	const std::wstring name =
		state->status.sinkName[0] != L'\0' ? std::wstring(state->status.sinkName) : std::wstring();
	SetMessage(hwnd, L"Loaded " + SinkTitle(device.reference, name) + L".");
	RefreshStatusView(hwnd);
	UpdateDeviceItemLabel(hwnd, state->selected, DeviceStateOf(state->status));
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
		state->message = Tools::Widen(error);
		SetMessage(hwnd, state->message);
		RefreshStatusView(hwnd);
		return;
	}
	state->status = status;
	RefreshStatusView(hwnd);
}

// Reads one sink's state, and its name when the sink reported one. False when the control
// device cannot be read right now, which is not the same as "connected": the caller keeps
// what it had instead of guessing.
bool TryReadDeviceState(const Device& device, DeviceState& deviceState, std::wstring* name = nullptr)
{
	std::string error;
	const HANDLE control = OpenControl(device, error);
	if (control == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	Ioctl::Status status = {};
	const bool read = ReadStatus(control, status, error);
	CloseHandle(control);
	if (!read)
	{
		return false;
	}
	if (name != nullptr)
	{
		*name = status.sinkName[0] != L'\0' ? std::wstring(status.sinkName) : std::wstring();
	}
	deviceState = DeviceStateOf(status);
	return true;
}

// Refreshes what the sink list says about every sink, so a stream that starts on a sink other
// than the selected one appears without pressing Refresh. The selected sink is not opened
// again: its status comes from the read this refresh already did.
void RefreshDeviceLabels(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr || state->deviceStates.size() != state->devices.size())
	{
		return;
	}
	for (size_t device = 0; device < state->devices.size(); ++device)
	{
		DeviceState current = state->deviceStates[device];
		const bool selected = state->open && device == state->selected;
		if (selected)
		{
			current = DeviceStateOf(state->status);
		}
		else if (!TryReadDeviceState(state->devices[device], current))
		{
			continue;
		}
		if (current != state->deviceStates[device])
		{
			state->deviceStates[device] = current;
			UpdateDeviceItemLabel(hwnd, device, current);
		}
	}
}

// "name (address)", or just the address when the sink reported no name.
std::wstring SinkTitle(const std::string& reference, const std::wstring& name)
{
	const std::wstring address = Tools::Widen(FormatAddress(reference));
	return name.empty() ? address : name + L" (" + address + L")";
}

// "<name> (<address>) - <state>".
std::wstring DeviceLabel(const std::string& reference, DeviceState state, const std::wstring& name)
{
	const wchar_t* const text = state == DeviceState::Playing   ? L" - playing"
								 : state == DeviceState::Suspended ? L" - suspended"
								 : state == DeviceState::WindowsDriver ? L" - Windows driver"
																		  : L" - connected";
	return SinkTitle(reference, name) + text;
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
		const std::wstring name = device < sink->deviceNames.size() ? sink->deviceNames[device]
																			  : std::wstring();
		const std::wstring label = DeviceLabel(sink->devices[device].reference, state, name);
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
	// The sink the window was on stays selected across a reload, including the one it is on
	// after a driver switch, where the control device is gone but the sink is still there.
	const std::string previous = state->reference;
	std::string error;
	std::vector<Device> devices = EnumerateDevices(error);
	if (!error.empty())
	{
		SetMessage(hwnd, Tools::Widen(error));
		return;
	}

	// A sink on the inbox A2DP driver is an A2DP node with no control device, and it is exactly
	// the sink the switch is for: list it too, so the window can take it over.
	std::vector<SinkNode> nodes;
	std::string nodeError;
	if (!EnumerateSinkNodes(nodes, nodeError))
	{
		SetMessage(hwnd, Tools::Widen(nodeError));
	}
	for (const SinkNode& node : nodes)
	{
		if (node.driver != SinkDriver::Windows)
		{
			continue;
		}
		const bool listed = std::any_of(devices.begin(), devices.end(), [&node](const Device& device) {
			return device.reference == node.reference;
		});
		if (!listed)
		{
			devices.push_back(Device{std::wstring(), node.reference});
		}
	}
	// One order whatever driver a sink is on, so the list does not reshuffle when it moves.
	std::sort(devices.begin(), devices.end(), [](const Device& left, const Device& right) {
		return left.reference < right.reference;
	});

	const HWND combo = GetDlgItem(hwnd, kSinkCombo);
	SendMessageW(combo, CB_RESETCONTENT, 0, 0);
	state->devices = std::move(devices);
	// The sink the user already had stays selected; with none, the one that is playing is
	// the one the window opens, and only then the first of the list.
	size_t selected = state->devices.size();
	size_t playing = state->devices.size();
	state->deviceStates.assign(state->devices.size(), DeviceState::Connected);
	state->deviceNames.assign(state->devices.size(), std::wstring());
	for (size_t i = 0; i < state->devices.size(); ++i)
	{
		DeviceState deviceState =
				state->devices[i].path.empty() ? DeviceState::WindowsDriver : DeviceState::Connected;
		if (!state->devices[i].path.empty())
		{
			(void)TryReadDeviceState(state->devices[i], deviceState, &state->deviceNames[i]);
		}
		state->deviceStates[i] = deviceState;
		const std::wstring label =
			DeviceLabel(state->devices[i].reference, deviceState, state->deviceNames[i]);
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
	// The labels carry the sink names, which are only known now, so the window is measured
	// again: the picker is as wide as the longest "name (address)" the list holds.
	ApplyLayout(hwnd);
	// Another sink arriving or leaving does not change the open one: keep its control device
	// and the settings the user may have changed but not applied yet.
	if (state->open && !previous.empty() && state->selected < state->devices.size() &&
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

// What storing one set of settings did, so the window can say it: the driver renegotiates only
// what moved, and a set that matches what it already reports moves nothing.
enum class StoreOutcome {
	Failed,
	Unchanged,
	Applied,
};

// The ABI fields are not settings, so they do not count as a difference.
bool SamePreference(const Ioctl::Preference& first, const Ioctl::Preference& second)
{
	return first.flags == second.flags && first.codec == second.codec &&
		   first.aacBitrate == second.aacBitrate && first.sbcBitpool == second.sbcBitpool &&
		   first.sbcBlocks == second.sbcBlocks && first.sbcSubbands == second.sbcSubbands &&
		   first.sbcAllocation == second.sbcAllocation && first.prerollMs == second.prerollMs &&
		   first.sbcFramesPerPacket == second.sbcFramesPerPacket &&
		   first.ldacQuality == second.ldacQuality;
}

bool SameEndpointFormat(const Ioctl::EndpointFormat& first, const Ioctl::EndpointFormat& second)
{
	for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
	{
		if (first.sampleRate[codec] != second.sampleRate[codec] ||
			first.channels[codec] != second.channels[codec])
		{
			return false;
		}
	}
	return true;
}

// Stores one whole set of settings and applies it. The preference goes first: the endpoint
// format write is the one that rebuilds the listed formats and makes a running stream
// renegotiate, so a single rebuild carries both instead of one per write. The driver clamps and
// refuses, so the result is read back and the window shows what it will actually use. A set that
// matches what the driver reports is not written at all: the driver would ignore it and restart
// nothing, which is not what a message about a renegotiation would say.
StoreOutcome StoreSettings(HWND hwnd, const Ioctl::Preference& preference,
						   const Ioctl::EndpointFormat& endpointFormat)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr || !state->open)
	{
		SetMessage(hwnd, L"No device is selected.");
		return StoreOutcome::Failed;
	}
	if (SamePreference(preference, state->preference) &&
		SameEndpointFormat(endpointFormat, state->endpointFormat))
	{
		return StoreOutcome::Unchanged;
	}
	std::string failure;
	if (!WritePreference(state->control, preference, failure) ||
		!WriteEndpointFormat(state->control, endpointFormat, failure))
	{
		SetMessage(hwnd, Tools::Widen(failure));
		return StoreOutcome::Failed;
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
	return StoreOutcome::Applied;
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
	// The drop-down decides whether a codec is forced at all, and which one; the tab the pane
	// shows has nothing to say about it.
	const uint32_t choice = ComboValue(hwnd, kCodecCombo);
	preference.flags = choice == 0 ? 0 : Ioctl::kPreferenceForceCodec;
	preference.codec = choice == 0 ? 0 : CodecOfTab(static_cast<int>(choice) - 1);
	preference.aacBitrate = ComboValue(hwnd, kBitrateCombo);
	preference.sbcBitpool = ComboValue(hwnd, kBitpoolCombo);
	preference.sbcBlocks = ComboValue(hwnd, kBlocksCombo);
	preference.sbcSubbands = ComboValue(hwnd, kSubbandsCombo);
	preference.sbcAllocation = ComboValue(hwnd, kAllocationCombo);
	preference.prerollMs = ComboValue(hwnd, kPacingLeadCombo);
	preference.sbcFramesPerPacket = ComboValue(hwnd, kSbcFramesCombo);
	preference.ldacQuality = ComboValue(hwnd, kLdacQualityCombo);

	// The combo carries the format of the open codec; the other codecs keep their own entry.
	const uint32_t formatValue = ComboValue(hwnd, kFormatCombo);
	Ioctl::EndpointFormat endpointFormat = state->endpointFormat;
	const int codecIndex = state->tab >= 0 && state->tab < kTabCount ? state->tab : 0;
	endpointFormat.sampleRate[codecIndex] = formatValue / 10;
	endpointFormat.channels[codecIndex] = formatValue % 10;
	endpointFormat.abiVersion = Ioctl::kAbiVersion;
	if (!Ioctl::ValidEndpointFormat(endpointFormat))
	{
		SetMessage(hwnd, L"Choose a format from the list.");
		return;
	}

	switch (StoreSettings(hwnd, preference, endpointFormat))
	{
	case StoreOutcome::Failed:
		return;
	case StoreOutcome::Unchanged:
		SetMessage(hwnd, L"Nothing to apply: the sink already uses these settings.");
		return;
	case StoreOutcome::Applied:
		break;
	}
	// A setting that belongs to a codec the sink does not run is stored and nothing more: the
	// codec choice did not move, so the stream keeps playing and the message must not promise a
	// renegotiation it will not get. Only a codec the choice names is worth taking the stream down
	// for.
	const bool streaming = state->status.mediaState == Ioctl::MediaState::Streaming;
	const bool forcedDiffers =
			(choice != 0) != !state->automatic ||
			(choice != 0 && CodecOfTab(static_cast<int>(choice) - 1) != EndpointCodec(state->status));
	const bool streamCodecEdited = CodecOfTab(state->tab) == EndpointCodec(state->status);
	if (forcedDiffers)
	{
		SetMessage(hwnd, streaming ? L"Stored. The playing stream is reconfigured for the new codec."
								   : L"Stored. They apply when a stream next starts.");
		return;
	}
	if (streaming && !streamCodecEdited)
	{
		SetMessage(hwnd, L"Stored for " + std::wstring(kTabInfo[state->tab].caption) +
							L". The playing stream is left alone.");
		return;
	}
	SetMessage(hwnd, streaming ? L"Stored. The playing stream is renegotiated to apply them."
							   : L"Stored. They apply when a stream next starts.");
}

// Puts every setting back to the driver default: the automatic codec, every codec automatic and
// every parameter zero. The driver rewrites the sink's settings file from what it receives, so
// the file is what a fresh install would have written.
void RestoreDefaults(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr || !state->open)
	{
		SetMessage(hwnd, L"No device is selected.");
		return;
	}
	Ioctl::Preference preference = {};
	preference.abiVersion = Ioctl::kAbiVersion;
	Ioctl::EndpointFormat endpointFormat = {};
	endpointFormat.abiVersion = Ioctl::kAbiVersion;
	// Nothing is restored on a sink that already uses the defaults, so there is nothing to
	// confirm and no stream to touch.
	if (SamePreference(preference, state->preference) &&
		SameEndpointFormat(endpointFormat, state->endpointFormat))
	{
		SetMessage(hwnd, L"Already using the driver defaults: nothing was stored and no stream was touched.");
		return;
	}
	if (MessageBoxW(hwnd,
					L"Restore every setting of this sink to the driver default?\n\nA playing stream is "
					L"renegotiated to them; the sink itself stays connected.",
					L"Restore defaults", MB_ICONQUESTION | MB_YESNO) != IDYES)
	{
		return;
	}
	switch (StoreSettings(hwnd, preference, endpointFormat))
	{
	case StoreOutcome::Failed:
		return;
	case StoreOutcome::Unchanged:
		SetMessage(hwnd, L"Already using the driver defaults: nothing was stored and no stream was touched.");
		return;
	case StoreOutcome::Applied:
		break;
	}
	// The write applies both halves the same way Apply does, so the message says what happens
	// rather than sending the user looking for an Apply.
	const bool streaming = state->status.mediaState == Ioctl::MediaState::Streaming;
	SetMessage(hwnd, streaming ? L"Defaults restored. The playing stream is renegotiated to them."
							   : L"Defaults restored. They apply when a stream next starts.");
}

// A themed static or button fills its own text background with the light theme's colour and
// draws the text in its own colour, whatever the window answers for WM_CTLCOLOR; taking them
// off the theme is what lets the palette reach the captions and the field labels.
BOOL CALLBACK UnthemeTextControl(HWND child, LPARAM context)
{
	UNREFERENCED_PARAMETER(context);
	wchar_t className[64] = {};
	GetClassNameW(child, className, ARRAYSIZE(className));
	if (lstrcmpiW(className, L"Static") == 0 || lstrcmpiW(className, L"Button") == 0)
	{
		SetWindowTheme(child, L"", L"");
	}
	return TRUE;
}

// The dark parts of the styled controls. The buttons and the check box are taken off the theme
// entirely and painted from the palette, the combos keep it and are given its dark parts, which
// is also what their drop-down lists ask the window to fill.
void ApplyDarkControls(HWND hwnd)
{
	if (!DarkTheme())
	{
		return;
	}
	EnumChildWindows(hwnd, UnthemeTextControl, 0);
	for (const int id : {kSinkCombo, kCodecCombo, kFormatCombo, kBitrateCombo, kBitpoolCombo, kBlocksCombo,
						 kSubbandsCombo, kAllocationCombo, kPacingLeadCombo, kSbcFramesCombo,
						 kLdacQualityCombo})
	{
		SetWindowTheme(GetDlgItem(hwnd, id), L"DarkMode_CFD", nullptr);
	}
	for (const int id : {kRefreshButton, kCopyButton, kApplyButton, kDriverButton, kRestoreButton,
						 kDetailsCheck})
	{
		// Owner-drawn, because a themed or classic button paints its own face from the light
		// system colours; the window then keeps the check box's state itself.
		SendMessageW(GetDlgItem(hwnd, id), BM_SETSTYLE, BS_OWNERDRAW, TRUE);
	}
	InvalidateRect(hwnd, nullptr, TRUE);
}

// A button or the check box, drawn from the palette: its own drawing would use the light
// system colours whatever the window answers for WM_CTLCOLOR, so in the dark theme they are
// owner-drawn.
void PaintButton(const DRAWITEMSTRUCT& item)
{
	const HDC dc = item.hDC;
	const Colors& colors = ThemeColors();
	const bool checkbox = item.CtlID == kDetailsCheck;
	const bool pressed = (item.itemState & ODS_SELECTED) != 0;
	const bool disabled = (item.itemState & ODS_DISABLED) != 0;

	// The layout measured the control with the window's font, and a WM_DRAWITEM DC carries the
	// system font, so the button's own font is selected for both measuring and drawing.
	const HFONT font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
	const HGDIOBJ previousFont = font != nullptr ? SelectObject(dc, font) : nullptr;
	TEXTMETRICW metrics = {};
	GetTextMetricsW(dc, &metrics);
	const int textHeight = metrics.tmHeight > 0 ? metrics.tmHeight : 16;

	wchar_t text[128] = {};
	GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
	RECT box = item.rcItem;
	const HBRUSH face = CreateSolidBrush(pressed ? colors.section : colors.background);
	FillRect(dc, &box, face);
	DeleteObject(face);

	const HBRUSH border = CreateSolidBrush(colors.border);
	RECT textRect = box;
	if (checkbox)
	{
		// The tick box and the caption are the sizes the layout reserved; the state is the
		// window's, because an owner-drawn button keeps none of its own.
		bool checked = false;
		if (const SinkState* state = StateOf(GetParent(item.hwndItem)); state != nullptr)
		{
			checked = state->details;
		}
		const int side = CheckSide(textHeight);
		RECT glyph = {box.left, box.top + (box.bottom - box.top - side) / 2, box.left + side, 0};
		glyph.bottom = glyph.top + side;
		FrameRect(dc, &glyph, border);
		if (checked)
		{
			// A tick drawn with a pen, so it does not depend on a glyph the font may not have.
			const HPEN pen = CreatePen(PS_SOLID, 2, colors.text);
			const HGDIOBJ previousPen = SelectObject(dc, pen);
			MoveToEx(dc, glyph.left + 3, glyph.top + side / 2, nullptr);
			LineTo(dc, glyph.left + side / 2 - 1, glyph.bottom - 4);
			LineTo(dc, glyph.right - 3, glyph.top + 3);
			SelectObject(dc, previousPen);
			DeleteObject(pen);
		}
		textRect.left = glyph.right + CheckTextGap(textHeight);
	}
	else
	{
		FrameRect(dc, &box, border);
	}
	DeleteObject(border);

	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, disabled ? colors.muted : colors.text);
	const UINT format = (checkbox ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
	DrawTextW(dc, text, -1, &textRect, format);
	if (!checkbox && (item.itemState & ODS_FOCUS) != 0)
	{
		// A button's frame is the caption it is drawn around. The check box draws none: a frame
		// around the whole control reads as a second border next to the tick box.
		RECT focus = textRect;
		InflateRect(&focus, -1, -1);
		DrawFocusRect(dc, &focus);
	}
	if (previousFont != nullptr)
	{
		SelectObject(dc, previousFont);
	}
}

LRESULT OnEraseBackground(HWND hwnd, HDC dc)
{
	RECT client = {};
	GetClientRect(hwnd, &client);
	FillRect(dc, &client, BackgroundBrush());
	return 1;
}

LRESULT OnColorStatic(WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	const HDC dc = reinterpret_cast<HDC>(wParam);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, ThemeColors().text);
	return reinterpret_cast<LRESULT>(BackgroundBrush());
}

// A combo's drop-down list is a list box of its own and asks the window for its colours; an
// opaque background is what stops the items from being drawn over the light list fill.
LRESULT OnColorListBox(WPARAM wParam)
{
	const HDC dc = reinterpret_cast<HDC>(wParam);
	SetBkMode(dc, OPAQUE);
	SetBkColor(dc, ThemeColors().background);
	SetTextColor(dc, ThemeColors().text);
	return reinterpret_cast<LRESULT>(BackgroundBrush());
}

// The group frames and their captions, in the palette's colours. The caption sits on the top
// line with the window's background behind it, which is what makes the line pass behind it.
void PaintGroups(HWND hwnd, HDC dc)
{
	const SinkState* state = StateOf(hwnd);
	if (state == nullptr || state->groups.empty())
	{
		return;
	}
	const Colors& colors = ThemeColors();
	if (state->font != nullptr)
	{
		SelectObject(dc, state->font);
	}
	TEXTMETRICW metrics = {};
	GetTextMetricsW(dc, &metrics);
	const int textHeight = metrics.tmHeight;

	const HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
	const HGDIOBJ previousPen = SelectObject(dc, pen);
	const HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, colors.text);
	for (const GroupFrame& group : state->groups)
	{
		if (group.caption.empty())
		{
			Rectangle(dc, group.rect.left, group.rect.top, group.rect.right, group.rect.bottom);
			continue;
		}
		Rectangle(dc, group.rect.left, group.rect.top + textHeight / 2, group.rect.right, group.rect.bottom);
		SIZE size = {};
		GetTextExtentPoint32W(dc, group.caption.c_str(), static_cast<int>(group.caption.size()), &size);
		RECT caption = {group.rect.left + textHeight / 2, group.rect.top,
						group.rect.left + textHeight / 2 + size.cx, group.rect.top + textHeight + 2};
		FillRect(dc, &caption, BackgroundBrush());
		DrawTextW(dc, group.caption.c_str(), -1, &caption, DT_LEFT | DT_SINGLELINE | DT_TOP | DT_NOPREFIX);
	}
	SelectObject(dc, previousBrush);
	SelectObject(dc, previousPen);
	DeleteObject(pen);
}

// How the window names a driver in a sentence.
std::wstring DriverTitle(SinkDriver driver)
{
	return driver == SinkDriver::Windows	? L"the Windows A2DP driver"
		   : driver == SinkDriver::OpenA2dp ? L"the OpenA2dp driver"
											: L"no A2DP driver";
}

// Moves the selected sink to the other A2DP source driver. The install restarts that sink's
// A2DP node, so its control device, endpoints and stream go with it: the list is read again
// afterwards and reopens the sink under whichever driver now owns it.
void SwitchSelectedDriver(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr || state->selected >= state->devices.size())
	{
		return;
	}
	// A switch whose restart was refused is finished on the driver already installed.
	const SinkDriver target =
		state->driverRestartPending ? state->driver : Tools::OtherDriver(state->driver);
	if (target == SinkDriver::Unknown)
	{
		SetMessage(hwnd, L"The A2DP node this sink is on was not found: reconnect it, then Refresh.");
		return;
	}

	const std::string reference = state->devices[state->selected].reference;
	const std::wstring title = SinkTitle(reference, state->deviceNames[state->selected]);
	const std::wstring prompt = L"Switch " + title + L" to " + DriverTitle(target) +
			L"?\n\nThe sink's A2DP device is restarted, so the audio stops for a moment; a program "
			L"playing to it is paused first. The switch needs administrator rights.";
	if (MessageBoxW(hwnd, prompt.c_str(), L"Switch driver", MB_ICONQUESTION | MB_YESNO) != IDYES)
	{
		return;
	}

	// The window's own handle on the control device would keep the old OpenA2dp image loaded
	// after its node is gone, and a later switch back then fails until a reboot (problem 38).
	// It is closed before the node restarts, not after.
	CloseControl(*state);
	std::string error;
	Tools::SwitchReport report;
	const Tools::SwitchOutcome outcome = SwitchSinkDriver(reference, target, report, error);
	ReloadDevices(hwnd);
	if (outcome != Tools::SwitchOutcome::Switched)
	{
		SetMessage(hwnd, Tools::Widen(error));
		return;
	}
	SetMessage(hwnd, L"Switched " + title + L" to " + DriverTitle(target) + L"." +
					   (report.pausedPlayer.empty()
								? std::wstring()
								: L" " + Tools::Widen(report.pausedPlayer) + L" was paused."));
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
		if (id == kCodecCombo && notification == CBN_SELCHANGE)
		{
			const uint32_t value = ComboValue(hwnd, kCodecCombo);
			SinkState* state = StateOf(hwnd);
			if (state != nullptr)
			{
				// The choice is what Apply stores: automatic, or the codec it names. A codec picked
				// here is also the one worth looking at, so the pane follows the choice; the
				// automatic entry leaves the pane where it is, because every tab is available.
				state->automatic = value == 0;
				if (value != 0)
				{
					ShowCodecTable(hwnd, static_cast<int>(value) - 1);
				}
			}
			return 0;
		}
		if (id == kApplyButton && notification == BN_CLICKED)
		{
			ApplySettings(hwnd);
			return 0;
		}
		if (id == kRestoreButton && notification == BN_CLICKED)
		{
			RestoreDefaults(hwnd);
			return 0;
		}
		if (id == kDriverButton && notification == BN_CLICKED)
		{
			SwitchSelectedDriver(hwnd);
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
				// The dark theme's check box is owner-drawn, and an owner-drawn button keeps no
				// check state of its own: the window's flag is the state both themes draw from.
				state->details = !state->details;
				InvalidateRect(GetDlgItem(hwnd, kDetailsCheck), nullptr, TRUE);
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
			// Another sink may have started or stopped streaming, which no device notification
			// reports, so the list is checked on the same tick as the status.
			RefreshDeviceLabels(hwnd);
			return 0;
		}
		break;
	case WM_NOTIFY:
	{
		const auto* header = reinterpret_cast<const NMHDR*>(lParam);
		if (header == nullptr)
		{
			break;
		}
		SinkState* state = StateOf(hwnd);
		// The message line caps its sentence with an ellipsis, and only a cut sentence needs a box
		// under the pointer: a tool that asks not to be shown is the only way to stay in step with
		// what the line actually drew. The one tooltip is shared, so the tool behind the request
		// has to be the message line before its answer means anything.
		if (state != nullptr && state->tooltip != nullptr && header->hwndFrom == state->tooltip &&
			header->code == TTN_GETDISPINFOW)
		{
			// The callback reads the window's own message, which outlives the request: a pointer to a
			// temporary string would leave the tooltip reading freed memory when it shows.
			reinterpret_cast<LPNMTTDISPINFOW>(lParam)->lpszText = state->message.data();
			return 0;
		}
		if (state != nullptr && state->tooltip != nullptr && header->hwndFrom == state->tooltip &&
			header->code == TTN_SHOW)
		{
			TTTOOLINFOW current = {};
			current.cbSize = sizeof(current);
			const HWND label = GetDlgItem(hwnd, kMessageLabel);
			if (SendMessageW(state->tooltip, TTM_GETCURRENTTOOLW, 0, reinterpret_cast<LPARAM>(&current)) != FALSE &&
				current.hwnd == hwnd && label != nullptr && current.uId == reinterpret_cast<UINT_PTR>(label) &&
				MessageFits(hwnd, state->message))
			{
				return TRUE;
			}
		}
		// The pane's tabs only say which settings are being looked at, so a click moves the
		// table and nothing else.
		if (header->idFrom == kCodecTabs && header->code == TCN_SELCHANGE)
		{
			ShowCodecTable(hwnd, static_cast<int>(SendMessageW(header->hwndFrom, TCM_GETCURSEL, 0, 0)));
			return 0;
		}
		break;
	}
	case kDeviceChangeMessage:
		ReloadDevices(hwnd);
		return 0;
	case WM_DRAWITEM:
		PaintButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
		return TRUE;
	case WM_PAINT:
	{
		PAINTSTRUCT paint = {};
		const HDC dc = BeginPaint(hwnd, &paint);
		PaintGroups(hwnd, dc);
		EndPaint(hwnd, &paint);
		return 0;
	}
	case WM_ERASEBKGND:
		return OnEraseBackground(hwnd, reinterpret_cast<HDC>(wParam));
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		return OnColorStatic(wParam, lParam);
	case WM_CTLCOLORLISTBOX:
		return OnColorListBox(wParam);
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

// Puts the window in front of whatever the user was looking at. A process started by another
// one - a terminal, a script, or the elevation path, which starts it from the AppInfo service -
// is not the foreground process, so Windows shows its window without activating it and it
// appears behind the windows already open. The foreground thread's input is joined for the one
// call that moves the window, which is the documented way for a program that has just been
// started to take the user's attention.
void BringToFront(HWND hwnd)
{
	if (GetForegroundWindow() == hwnd)
	{
		return;
	}
	const DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
	const DWORD current = GetCurrentThreadId();
	const bool joined = foreground != 0 && foreground != current &&
						AttachThreadInput(current, foreground, TRUE) != FALSE;
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);
	SetActiveWindow(hwnd);
	if (joined)
	{
		AttachThreadInput(current, foreground, FALSE);
	}
}

} // namespace

int Run(HINSTANCE instance, int showCommand)
{
	// The theme is read once, before the class brush and the first measurement exist.
	SelectTheme();
	const wchar_t* const className = L"OpenA2dpConfigWindow";
	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hbrBackground = BackgroundBrush();
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
	ApplyTheme(hwnd);
	ApplyDarkControls(hwnd);
	ShowWindow(hwnd, showCommand);
	UpdateWindow(hwnd);
	BringToFront(hwnd);

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
	// The tooltip window is a Win95 common control, not one of the standard classes a button
	// or an edit comes from.
	INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
	InitCommonControlsEx(&controls);
	return OpenA2dp::ConfigUi::Run(instance, show);
}
