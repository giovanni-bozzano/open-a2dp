// OpenA2dpConfig.exe - desktop settings window for the OpenA2dp A2DP source driver.
//
// One window for every sink the driver owns: it lists the control devices, shows the
// session and what the last negotiation produced, and stores the codec and endpoint
// preference through SET_PREFERENCE. Status is re-read on a timer; a stored change is
// applied by the driver itself (an active stream renegotiates in place), so the window
// never has to wait or guess.

#include "ControlClient.h"

#include <OpenA2dp/Ioctl.h>

#include <windows.h>

#include <cfgmgr32.h>

#include <cstdarg>
#include <cstdio>
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
	kStatusBox,
	kCodecCombo,
	kBitrateCombo,
	kBitpoolEdit,
	kBlocksCombo,
	kSubbandsCombo,
	kAllocationCombo,
	kRateCombo,
	kChannelsCombo,
	kApplyButton,
	kMessageLabel,
};

constexpr int kStatusTimerId = 1;
constexpr int kStatusRefreshMs = 1000;
// Posted from the interface notification thread; the list is re-enumerated on the UI thread.
constexpr UINT kDeviceChangeMessage = WM_APP + 1;

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
	Ioctl::VersionInfo version = {};
	std::wstring message; // the outcome of the last action, shown under the controls
	std::wstring shownText; // what the status box holds, so a refresh only redraws real changes
	HCMNOTIFICATION notify = nullptr; // control-device arrival/removal
};

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

std::wstring Format(const wchar_t* format, ...)
{
	wchar_t text[512];
	va_list args;
	va_start(args, format);
	(void)_vsnwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, format, args);
	va_end(args);
	return text;
}

void AddLine(std::wstring& text, const wchar_t* label, const std::wstring& value)
{
	text += Format(L"%-14s %s\r\n", label, value.c_str());
}

const wchar_t* AvdtpName(uint32_t state)
{
	static const wchar_t* const names[] = {
		L"Idle", L"Discovering", L"GettingCapabilities", L"Configuring", L"Configured", L"Opening",
		L"Open", L"Starting", L"Streaming", L"Suspending", L"Closing", L"Aborting", L"Failed",
	};
	return state < ARRAYSIZE(names) ? names[state] : L"Unknown";
}

const wchar_t* MediaName(uint32_t state)
{
	switch (static_cast<Ioctl::MediaState>(state))
	{
	case Ioctl::MediaState::Idle: return L"Idle";
	case Ioctl::MediaState::WaitingForEncoder: return L"Waiting for the encoder";
	case Ioctl::MediaState::Streaming: return L"Streaming";
	case Ioctl::MediaState::Suspended: return L"Suspended";
	case Ioctl::MediaState::Failed: return L"Failed";
	default: return L"Unknown";
	}
}

const wchar_t* CodecName(uint32_t codec)
{
	if (codec == Ioctl::kCodecSbc)
	{
		return L"SBC";
	}
	if (codec == Ioctl::kCodecAac)
	{
		return L"AAC";
	}
	return L"none";
}

// The formats a status rate/channel mask pair describes.
std::wstring FormatListText(uint32_t rates, uint32_t channels)
{
	std::wstring text;
	const auto append = [&text](const wchar_t* part)
	{
		if (!text.empty())
		{
			text += L", ";
		}
		text += part;
	};
	if ((rates & 1) != 0)
	{
		append(L"44100 Hz");
	}
	if ((rates & 2) != 0)
	{
		append(L"48000 Hz");
	}
	if ((channels & 1) != 0)
	{
		append(L"mono");
	}
	if ((channels & 2) != 0)
	{
		append(L"stereo");
	}
	return text;
}

std::wstring CodecListText(uint32_t codecs)
{
	std::wstring text;
	if ((codecs & Ioctl::kSinkCodecSbc) != 0)
	{
		text = L"SBC";
	}
	if ((codecs & Ioctl::kSinkCodecAac) != 0)
	{
		text += text.empty() ? L"AAC" : L", AAC";
	}
	return text;
}

std::wstring EndpointText(uint32_t rate, uint32_t channels)
{
	std::wstring text = rate != 0 ? Format(L"%u Hz", rate) : L"any rate";
	text += L", ";
	text += channels == 1 ? L"mono" : (channels == 2 ? L"stereo" : L"any channels");
	return text;
}

// Everything the window knows about the selected sink, as the status box shows it.
std::wstring StatusText(const SinkState& state)
{
	if (!state.open)
	{
		return state.message;
	}
	const Ioctl::Status& status = state.status;
	const Ioctl::Preference& preference = state.preference;

	std::wstring text;
	AddLine(text, L"Sink", Widen(FormatAddress(state.reference)));
	AddLine(text, L"AVDTP", AvdtpName(status.avdtpState));
	AddLine(text, L"Media", MediaName(static_cast<uint32_t>(status.mediaState)));
	if (status.mediaState == Ioctl::MediaState::Streaming)
	{
		AddLine(text, L"Stream",
				Format(L"%s, %u Hz, %u ch, %u bps", CodecName(status.codec), status.sampleRate,
					   status.channels, status.bitrate));
	}
	else if (status.negotiatedCodec != 0 || status.codec != 0)
	{
		AddLine(text, L"Stream", Format(L"%s (negotiated, not streaming)",
										CodecName(status.negotiatedCodec != 0 ? status.negotiatedCodec : status.codec)));
	}
	else
	{
		AddLine(text, L"Stream", L"none");
	}
	AddLine(text, L"Encoder", status.encoderRegistered != 0 ? L"registered" : L"not registered");
	if (status.sinkRates != 0 || status.sinkChannels != 0 || status.sinkCodecs != 0)
	{
		std::wstring sink = FormatListText(status.sinkRates, status.sinkChannels);
		const std::wstring codecs = CodecListText(status.sinkCodecs);
		if (!codecs.empty())
		{
			sink = sink.empty() ? codecs : sink + L" (" + codecs + L")";
		}
		AddLine(text, L"Sink formats", sink.empty() ? L"unknown" : sink);
	}
	AddLine(text, L"Media MTU",
			Format(L"%u B, %u B payload; AAC up to %u bps", status.mediaMtu,
				   status.mediaMtu - Ioctl::kRtpHeaderBytes, status.bitrateLimit));
	AddLine(text, L"Preference",
			(preference.flags & Ioctl::kPreferenceForceCodec) != 0
				? Format(L"%s forced", CodecName(preference.codec))
				: std::wstring(L"automatic (AAC, then SBC)"));
	AddLine(text, L"Endpoint pin", EndpointText(preference.endpointRate, preference.endpointChannels));
	if (status.pinNotServed != 0)
	{
		AddLine(text, L"Fallback", L"the sink does not serve the stored endpoint format");
	}
	AddLine(text, L"Counters",
			Format(L"packets %u, dropped %u, underruns %u, send failures %u", status.packetsSent,
				   status.framesDropped, status.underruns, status.sendFailures));
	if (status.lastError != 0)
	{
		AddLine(text, L"Last error", Format(L"0x%08X", status.lastError));
	}
	AddLine(text, L"Driver", Format(L"ABI %u, version %u.%u.%u", state.version.abiVersion,
									(state.version.driverVersion >> 24) & 0xFF,
									(state.version.driverVersion >> 16) & 0xFF,
									state.version.driverVersion & 0xFFFF));
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

void SetStatusText(HWND hwnd)
{
	SinkState* state = StateOf(hwnd);
	if (state == nullptr)
	{
		return;
	}
	std::wstring text = StatusText(*state);
	// Re-writing the same text every timer tick would reset the scroll position and the
	// selection while the user reads it.
	if (text == state->shownText)
	{
		return;
	}
	state->shownText = std::move(text);
	SetWindowTextW(GetDlgItem(hwnd, kStatusBox), state->shownText.c_str());
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

// Creates every control once; the layout is fixed, so the window is not resizable.
void CreateControls(HWND hwnd)
{
	const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	CreateChild(hwnd, L"STATIC", L"Sink", SS_LEFT, 12, 16, 96, 20, 0, font);
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 112, 12, 372, 220,
				kSinkCombo, font);
	CreateChild(hwnd, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 496, 11, 132, 27, kRefreshButton,
				font);

	CreateChild(hwnd, L"BUTTON", L"Status", BS_GROUPBOX, 12, 48, 616, 224, 0, font);
	CreateChild(hwnd, L"EDIT", L"",
				WS_BORDER | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 24, 68, 592,
				192, kStatusBox, font);

	CreateChild(hwnd, L"BUTTON", L"Codec", BS_GROUPBOX, 12, 280, 616, 148, 0, font);
	CreateChild(hwnd, L"STATIC", L"Codec", SS_LEFT, 24, 302, 84, 20, 0, font);
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 112, 298, 190, 160,
				kCodecCombo, font);
	CreateChild(hwnd, L"STATIC", L"AAC bitrate", SS_LEFT, 320, 302, 92, 20, 0, font);
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 420, 298, 196, 220,
				kBitrateCombo, font);
	CreateChild(hwnd, L"STATIC", L"SBC bitpool", SS_LEFT, 24, 334, 84, 20, 0, font);
	CreateChild(hwnd, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_RIGHT, 112, 332, 80, 22,
				kBitpoolEdit, font);
	CreateChild(hwnd, L"STATIC", L"SBC blocks", SS_LEFT, 320, 334, 92, 20, 0, font);
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 420, 330, 196, 160,
				kBlocksCombo, font);
	CreateChild(hwnd, L"STATIC", L"Subbands", SS_LEFT, 24, 366, 84, 20, 0, font);
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 112, 362, 190, 160,
				kSubbandsCombo, font);
	CreateChild(hwnd, L"STATIC", L"Allocation", SS_LEFT, 320, 366, 92, 20, 0, font);
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 420, 362, 196, 160,
				kAllocationCombo, font);
	CreateChild(hwnd, L"STATIC", L"The frame shape and bitpool apply to SBC; the bitrate to AAC.",
				SS_LEFT, 24, 396, 592, 20, 0, font);

	CreateChild(hwnd, L"BUTTON", L"Endpoint format", BS_GROUPBOX, 12, 436, 616, 72, 0, font);
	CreateChild(hwnd, L"STATIC", L"Rate", SS_LEFT, 24, 458, 84, 20, 0, font);
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 112, 454, 190, 120,
				kRateCombo, font);
	CreateChild(hwnd, L"STATIC", L"Channels", SS_LEFT, 320, 458, 92, 20, 0, font);
	CreateChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 420, 454, 196, 120,
				kChannelsCombo, font);

	CreateChild(hwnd, L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 496, 516, 132, 28, kApplyButton, font);
	CreateChild(hwnd, L"STATIC", L"", SS_LEFT, 12, 522, 476, 20, kMessageLabel, font);

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

	AddComboItem(hwnd, kRateCombo, L"Any (44.1 and 48 kHz)", 0);
	AddComboItem(hwnd, kRateCombo, L"44.1 kHz", 44100);
	AddComboItem(hwnd, kRateCombo, L"48 kHz", 48000);

	AddComboItem(hwnd, kChannelsCombo, L"Any (mono and stereo)", 0);
	AddComboItem(hwnd, kChannelsCombo, L"Mono", 1);
	AddComboItem(hwnd, kChannelsCombo, L"Stereo", 2);

	for (const int id : {kCodecCombo, kBitrateCombo, kBlocksCombo, kSubbandsCombo, kAllocationCombo, kRateCombo,
						 kChannelsCombo})
	{
		SendMessageW(GetDlgItem(hwnd, id), CB_SETCURSEL, 0, 0);
	}
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
	SelectComboValue(hwnd, kRateCombo, preference.endpointRate);
	SelectComboValue(hwnd, kChannelsCombo, preference.endpointChannels);
	const std::wstring bitpool = preference.sbcBitpool != 0 ? Format(L"%u", preference.sbcBitpool) : L"";
	SetWindowTextW(GetDlgItem(hwnd, kBitpoolEdit), bitpool.c_str());
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
		SetStatusText(hwnd);
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
		SetStatusText(hwnd);
		return;
	}
	state->control = control;

	if (!ReadVersion(control, state->version, error) || !ReadStatus(control, state->status, error) ||
		!ReadPreference(control, state->preference, error))
	{
		CloseControl(*state);
		state->message = Widen(error);
		SetMessage(hwnd, state->message);
		SetStatusText(hwnd);
		return;
	}

	state->open = true;
	EnableWindow(GetDlgItem(hwnd, kApplyButton), TRUE);
	LoadPreferenceIntoControls(hwnd);
	SetMessage(hwnd, L"Loaded " + Widen(FormatAddress(device.reference)) + L".");
	SetStatusText(hwnd);
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
		SetStatusText(hwnd);
		return;
	}
	state->status = status;
	SetStatusText(hwnd);
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
	state->selected = 0;
	for (size_t i = 0; i < state->devices.size(); ++i)
	{
		const std::wstring label = Widen(FormatAddress(state->devices[i].reference));
		const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
		if (index == CB_ERR)
		{
			continue;
		}
		SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
		if (!previous.empty() && state->devices[i].reference == previous)
		{
			state->selected = i;
		}
	}
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

// Reads the bitpool box; an empty box means "driver default".
bool ReadBitpool(HWND hwnd, uint32_t& value, std::wstring& error)
{
	wchar_t text[16] = {};
	GetWindowTextW(GetDlgItem(hwnd, kBitpoolEdit), text, ARRAYSIZE(text));
	std::wstring trimmed(text);
	while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t'))
	{
		trimmed.erase(0, 1);
	}
	if (trimmed.empty())
	{
		value = 0;
		return true;
	}
	wchar_t* end = nullptr;
	const unsigned long parsed = std::wcstoul(trimmed.c_str(), &end, 10);
	if (end == nullptr || *end != L'\0' || parsed < Ioctl::kSbcMinBitpool || parsed > Ioctl::kSbcMaxBitpool)
	{
		error = Format(L"The SBC bitpool is %u..%u, or empty for the driver default.", Ioctl::kSbcMinBitpool,
					   Ioctl::kSbcMaxBitpool);
		return false;
	}
	value = static_cast<uint32_t>(parsed);
	return true;
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

	uint32_t bitpool = 0;
	std::wstring error;
	if (!ReadBitpool(hwnd, bitpool, error))
	{
		SetMessage(hwnd, error);
		return;
	}

	// Start from the stored preference so a field the window does not expose is kept.
	Ioctl::Preference preference = state->preference;
	const uint32_t codec = ComboValue(hwnd, kCodecCombo);
	preference.flags = codec == kCodecAuto ? 0 : Ioctl::kPreferenceForceCodec;
	preference.codec = codec == kCodecAacChoice ? Ioctl::kCodecAac : (codec == kCodecSbcChoice ? Ioctl::kCodecSbc : 0);
	preference.aacBitrate = ComboValue(hwnd, kBitrateCombo);
	preference.sbcBitpool = bitpool;
	preference.sbcBlocks = ComboValue(hwnd, kBlocksCombo);
	preference.sbcSubbands = ComboValue(hwnd, kSubbandsCombo);
	preference.sbcAllocation = ComboValue(hwnd, kAllocationCombo);
	preference.endpointRate = ComboValue(hwnd, kRateCombo);
	preference.endpointChannels = ComboValue(hwnd, kChannelsCombo);

	std::string failure;
	if (!WritePreference(state->control, preference, failure))
	{
		SetMessage(hwnd, Widen(failure));
		return;
	}

	Ioctl::Preference stored = {};
	if (ReadPreference(state->control, stored, failure))
	{
		state->preference = stored;
		LoadPreferenceIntoControls(hwnd);
	}
	RefreshStatus(hwnd);
	const bool streaming = state->status.mediaState == Ioctl::MediaState::Streaming;
	SetMessage(hwnd, streaming ? L"Stored. The active stream renegotiates to apply it."
							   : L"Stored. The setting applies at the next negotiation.");
}

LRESULT OnColorStatic(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
	const HWND child = reinterpret_cast<HWND>(lParam);
	if (child == GetDlgItem(hwnd, kStatusBox))
	{
		SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
		return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
	}
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
		return OnColorStatic(hwnd, wParam, lParam);
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

	RECT rect = {0, 0, 640, 556};
	const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	AdjustWindowRectEx(&rect, style, FALSE, 0);
	const HWND hwnd = CreateWindowExW(0, className, L"OpenA2dp - A2DP sink settings", style,
									  CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
									  rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
	if (hwnd == nullptr)
	{
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
	return OpenA2dp::ConfigUi::Run(instance, show);
}
