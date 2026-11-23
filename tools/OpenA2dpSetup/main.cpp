// OpenA2dpSetup.exe - interactive installer for the OpenA2DP driver, service and tools.
//
// The executable carries the driver package, the encoder service and both tools as resources, so
// it is one file to hand out. Installing stages the package in the driver store but never installs
// it on a sink: the user chooses headsets in OpenA2DP Settings. An update replaces the files and
// service and restarts only headsets that were already on OpenA2DP; a sink on the Windows driver
// is never touched. Uninstall returns every OpenA2dp sink to BthA2dp and removes the package from
// the driver store.

#include "SetupEnvironment.h"
#include "SetupNames.h"
#include "SetupOperations.h"
#include "SetupRegistry.h"
#include "SetupResources.h"
#include "SetupTypes.h"

#include "common/Text.h"

#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <prsht.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "uxtheme.lib")

namespace OpenA2dp::Setup {
namespace {

constexpr UINT kJobUpdate = WM_APP + 1;
constexpr UINT kJobDone = WM_APP + 2;
constexpr UINT kBringToFront = WM_APP + 3;

enum class Mode
{
	Install,
	Uninstall,
};

struct Job
{
	Mode mode = Mode::Install;
	bool updating = false;
	std::wstring installDir;
	std::wstring existingVersion;
	std::wstring setupExe;
	bool removeSettings = false;
	bool openSettings = true;
	bool started = false;
	HWND sheet = nullptr;
	HWND progressPage = nullptr;
	std::thread worker;

	std::mutex mutex;
	std::wstring log;
	std::wstring status;
	Result result;
	bool done = false;
};

HINSTANCE g_instance = nullptr;

Job* JobOf(HWND page)
{
	return reinterpret_cast<Job*>(GetWindowLongPtrW(page, DWLP_USER));
}

// A property sheet page receives a pointer to its PROPSHEETPAGE copy in WM_INITDIALOG; the Job
// pointer was passed in that structure's lParam.
Job* JobFromPage(LPARAM lParam)
{
	const auto* page = reinterpret_cast<const PROPSHEETPAGEW*>(lParam);
	return page != nullptr ? reinterpret_cast<Job*>(page->lParam) : nullptr;
}

INT_PTR PageResult(HWND page, INT_PTR result)
{
	SetWindowLongPtrW(page, DWLP_MSGRESULT, result);
	return TRUE;
}

// A process started by another one - a terminal, a script, or the elevation path, which starts it
// from the AppInfo service - is not the foreground process, so Windows shows its window without
// activating it and the wizard appears behind the windows already open. The foreground thread's
// input is joined for the one call that moves the window, which is the documented way for a
// program that has just been started to take the user's attention.
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

std::wstring WindowsLineEndings(const std::wstring& text)
{
	std::wstring result;
	result.reserve(text.size() + 16);
	for (size_t i = 0; i < text.size(); ++i)
	{
		if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
		{
			result += L"\r\n";
		}
		else
		{
			result += text[i];
		}
	}
	return result;
}

std::wstring Trim(const std::wstring& text)
{
	size_t begin = 0;
	while (begin < text.size() && iswspace(text[begin]) != 0)
	{
		++begin;
	}
	size_t end = text.size();
	while (end > begin && iswspace(text[end - 1]) != 0)
	{
		--end;
	}
	return text.substr(begin, end - begin);
}

std::wstring ModulePath()
{
	std::vector<wchar_t> buffer(MAX_PATH);
	for (;;)
	{
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0)
		{
			return {};
		}
		if (length < buffer.size())
		{
			return std::wstring(buffer.data(), length);
		}
		buffer.resize(buffer.size() * 2);
	}
}

std::wstring TempFolder()
{
	wchar_t temp[MAX_PATH + 1] = {};
	const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
	return length != 0 && length < ARRAYSIZE(temp) ? std::wstring(temp, length) : std::wstring();
}

void AddLog(Job* job, const std::wstring& line)
{
	{
		std::lock_guard<std::mutex> lock(job->mutex);
		job->log += line;
		job->log += L"\r\n";
	}
	if (job->progressPage != nullptr)
	{
		PostMessageW(job->progressPage, kJobUpdate, 0, 0);
	}
}

void SetStatus(Job* job, const std::wstring& status)
{
	{
		std::lock_guard<std::mutex> lock(job->mutex);
		job->status = status;
	}
	if (job->progressPage != nullptr)
	{
		PostMessageW(job->progressPage, kJobUpdate, 0, 0);
	}
}

void RunWorker(Job* job)
{
	const Log log = [job](const std::wstring& line) { AddLog(job, line); };
	Result result;
	try
	{
		if (job->mode == Mode::Install)
		{
			result = Install(job->installDir, job->setupExe, job->sheet, log);
		}
		else
		{
			result = Uninstall(job->installDir, job->removeSettings, log);
		}
	}
	catch (const std::exception& failure)
	{
		result = {};
		result.message = L"Setup failed unexpectedly: " + Tools::Widen(failure.what());
	}
	catch (...)
	{
		result = {};
		result.message = L"Setup failed unexpectedly.";
	}
	{
		std::lock_guard<std::mutex> lock(job->mutex);
		job->result = result;
		job->status = result.message;
		job->done = true;
	}
	if (job->progressPage != nullptr)
	{
		PostMessageW(job->progressPage, kJobDone, 0, 0);
	}
}

void UpdateProgressPage(Job* job, HWND page)
{
	std::wstring status;
	std::wstring log;
	bool done = false;
	Result result;
	{
		std::lock_guard<std::mutex> lock(job->mutex);
		status = job->status;
		log = job->log;
		done = job->done;
		result = job->result;
	}
	if (status.empty())
	{
		status = L"Working...";
	}
	if (done)
	{
		// The status box is two lines; a failure can be longer and its recovery command must stay
		// readable. Keep the status short and put the full result at the end of the log.
		status = result.success ? L"Setup finished." : L"Setup failed.";
		if (!result.message.empty())
		{
			log += L"\r\n";
			log += result.message;
			log += L"\r\n";
		}
	}
	SetDlgItemTextW(page, IDC_STATUS, status.c_str());
	SendDlgItemMessageW(page, IDC_LOG, EM_SETLIMITTEXT, 1024 * 1024, 0);
	const std::wstring display = WindowsLineEndings(log);
	SetDlgItemTextW(page, IDC_LOG, display.c_str());
	SendDlgItemMessageW(page, IDC_LOG, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
	SendDlgItemMessageW(page, IDC_LOG, EM_SCROLLCARET, 0, 0);
	if (!done)
	{
		return;
	}

	SendDlgItemMessageW(page, IDC_PROGRESS, PBM_SETMARQUEE, FALSE, 0);
	PropSheet_SetWizButtons(job->sheet, PSWIZB_FINISH);
	if (const HWND finish = GetDlgItem(job->sheet, IDOK))
	{
		SetWindowTextW(finish, L"Finish");
	}
	EnableWindow(GetDlgItem(job->sheet, IDCANCEL), TRUE);
	EnableMenuItem(GetSystemMenu(job->sheet, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
	if (job->mode == Mode::Install && result.success && !result.restartRequired)
	{
		ShowWindow(GetDlgItem(page, IDC_OPEN_SETTINGS), SW_SHOW);
		CheckDlgButton(page, IDC_OPEN_SETTINGS, BST_CHECKED);
	}
	if (result.restartRequired)
	{
		ShowWindow(GetDlgItem(page, IDC_OPEN_SETTINGS), SW_HIDE);
		ShowWindow(GetDlgItem(page, IDC_RESTART_NOTE), SW_SHOW);
		SetDlgItemTextW(page, IDC_RESTART_NOTE,
						job->mode == Mode::Uninstall
							? L"Restart Windows to finish removing the driver package."
							: L"Restart Windows to finish removing the previous driver package.");
	}
}

void SetNextText(HWND sheet, const wchar_t* text)
{
	if (const HWND next = GetDlgItem(sheet, IDOK))
	{
		SetWindowTextW(next, text);
	}
}

INT_PTR CALLBACK WelcomeProc(HWND page, UINT message, WPARAM, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		Job* job = JobFromPage(lParam);
		SetWindowLongPtrW(page, DWLP_USER, reinterpret_cast<LONG_PTR>(job));
		std::wstring text;
		if (job->updating)
		{
			text = L"OpenA2DP is already installed in:\n\n    " + job->installDir + L"\n";
			if (!job->existingVersion.empty())
			{
				text += L"\nVersion " + job->existingVersion + L"\n";
			}
			text += L"\nSetup will update it to this build. Your saved headset settings are kept.\n\n"
					L"Headsets already using OpenA2DP will be restarted to load the new driver. "
					L"Headsets on the Windows driver are not touched.";
		}
		else
		{
			text = L"This installs the OpenA2DP A2DP source driver, its encoder service and the two tools.\n\n"
				   L"Setup adds the driver package to the Windows driver store but does not switch any "
				   L"headset. After setup, open OpenA2DP Settings and choose \"Use OpenA2DP driver\" for "
				   L"the headsets you want.\n\n"
				   L"Requirements: Windows 11 x64. This build is test-signed, so test signing must be "
				   L"enabled on this PC.";
		}
		SetDlgItemTextW(page, IDC_WELCOME_TEXT, text.c_str());
		// The sheet is not shown yet; bring it forward once the modal loop runs.
		PostMessageW(page, kBringToFront, 0, 0);
		return TRUE;
	}
	case kBringToFront:
		BringToFront(GetParent(page));
		return TRUE;
	case WM_NOTIFY:
		if (reinterpret_cast<NMHDR*>(lParam)->code == PSN_SETACTIVE)
		{
			Job* job = JobOf(page);
			PropSheet_SetWizButtons(GetParent(page), PSWIZB_NEXT);
			SetNextText(GetParent(page), job->updating ? L"Update" : L"Install");
			return PageResult(page, 0);
		}
		break;
	default:
		break;
	}
	return FALSE;
}

int CALLBACK BrowseCallback(HWND browse, UINT message, LPARAM, LPARAM data)
{
	if (message == BFFM_INITIALIZED && data != 0)
	{
		SendMessageW(browse, BFFM_SETSELECTIONW, TRUE, data);
	}
	return 0;
}

std::wstring BrowseForFolder(HWND owner, const std::wstring& current)
{
	wchar_t display[MAX_PATH] = {};
	BROWSEINFOW info = {};
	info.hwndOwner = owner;
	info.pszDisplayName = display;
	info.lpszTitle = L"Choose the folder where OpenA2DP is installed.";
	info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
	info.lpfn = BrowseCallback;
	info.lParam = reinterpret_cast<LPARAM>(current.c_str());
	PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
	if (item == nullptr)
	{
		return {};
	}
	wchar_t path[MAX_PATH] = {};
	const bool read = SHGetPathFromIDListW(item, path) != FALSE;
	CoTaskMemFree(item);
	return read ? std::wstring(path) : std::wstring();
}

INT_PTR CALLBACK LocationProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		Job* job = JobFromPage(lParam);
		SetWindowLongPtrW(page, DWLP_USER, reinterpret_cast<LONG_PTR>(job));
		SetDlgItemTextW(page, IDC_PATH, job->installDir.c_str());
		if (job->updating)
		{
			EnableWindow(GetDlgItem(page, IDC_PATH), FALSE);
			EnableWindow(GetDlgItem(page, IDC_BROWSE), FALSE);
			SetDlgItemTextW(page, IDC_LOCATION_NOTE,
							L"OpenA2DP is already installed here. Updates use the same folder; to move "
							L"it, remove OpenA2DP first.");
		}
		else
		{
			SetDlgItemTextW(page, IDC_LOCATION_NOTE,
							L"The folder holds the tools, the encoder service and the driver package. "
							L"Setup does not switch any headset.");
		}
		return TRUE;
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_BROWSE && HIWORD(wParam) == BN_CLICKED)
		{
			wchar_t current[MAX_PATH] = {};
			GetDlgItemTextW(page, IDC_PATH, current, ARRAYSIZE(current));
			const std::wstring chosen = BrowseForFolder(page, current);
			if (!chosen.empty())
			{
				SetDlgItemTextW(page, IDC_PATH, chosen.c_str());
			}
			return TRUE;
		}
		break;
	case WM_NOTIFY:
		switch (reinterpret_cast<NMHDR*>(lParam)->code)
		{
		case PSN_SETACTIVE:
		{
			Job* job = JobOf(page);
			PropSheet_SetWizButtons(GetParent(page), PSWIZB_BACK | PSWIZB_NEXT);
			SetNextText(GetParent(page), job->updating ? L"Update" : L"Install");
			return PageResult(page, 0);
		}
		case PSN_WIZNEXT:
		{
			Job* job = JobOf(page);
			if (!job->updating)
			{
				wchar_t text[32768] = {};
				GetDlgItemTextW(page, IDC_PATH, text, ARRAYSIZE(text));
				std::wstring normalized;
				std::wstring error;
				if (!ValidateInstallDir(Trim(text), normalized, error))
				{
					MessageBoxW(page, error.c_str(), L"OpenA2DP Setup", MB_OK | MB_ICONWARNING);
					return PageResult(page, 1);
				}
				job->installDir = normalized;
			}
			std::wstring running;
			if (IsToolRunning(running))
			{
				const std::wstring text =
					running + L" is running. Close the OpenA2DP tools, then run setup again.";
				MessageBoxW(page, text.c_str(), L"OpenA2DP Setup", MB_OK | MB_ICONWARNING);
				return PageResult(page, 1);
			}
			return PageResult(page, 0);
		}
		default:
			break;
		}
		break;
	default:
		break;
	}
	return FALSE;
}

INT_PTR CALLBACK ConfirmProc(HWND page, UINT message, WPARAM, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		Job* job = JobFromPage(lParam);
		SetWindowLongPtrW(page, DWLP_USER, reinterpret_cast<LONG_PTR>(job));
		std::wstring error;
		const std::vector<std::wstring> sinks = CurrentOpenA2dpSinks(error);
		std::wstring list;
		if (!error.empty())
		{
			list = L"Could not list the headsets: " + error;
		}
		else if (sinks.empty())
		{
			list = L"No connected headset is using OpenA2DP right now.";
		}
		else
		{
			for (const std::wstring& sink : sinks)
			{
				list += L"    " + sink + L"\r\n";
			}
		}
		SetDlgItemTextW(page, IDC_SINKS, list.c_str());
		CheckDlgButton(page, IDC_REMOVE_SETTINGS, BST_UNCHECKED);
		// The sheet is not shown yet; bring it forward once the modal loop runs.
		PostMessageW(page, kBringToFront, 0, 0);
		return TRUE;
	}
	case kBringToFront:
		BringToFront(GetParent(page));
		return TRUE;
	case WM_NOTIFY:
		switch (reinterpret_cast<NMHDR*>(lParam)->code)
		{
		case PSN_SETACTIVE:
			PropSheet_SetWizButtons(GetParent(page), PSWIZB_BACK | PSWIZB_NEXT);
			SetNextText(GetParent(page), L"Remove");
			return PageResult(page, 0);
		case PSN_WIZNEXT:
		{
			Job* job = JobOf(page);
			job->removeSettings = IsDlgButtonChecked(page, IDC_REMOVE_SETTINGS) == BST_CHECKED;
			return PageResult(page, 0);
		}
		default:
			break;
		}
		break;
	default:
		break;
	}
	return FALSE;
}

LRESULT CALLBACK LogEditProc(HWND log, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
	const LRESULT result = DefSubclassProc(log, message, wParam, lParam);
	switch (message)
	{
	case WM_SETFOCUS:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_KEYDOWN:
	case WM_CHAR:
		HideCaret(log);
		break;
	default:
		break;
	}
	return result;
}

INT_PTR CALLBACK ProgressProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		Job* job = JobFromPage(lParam);
		SetWindowLongPtrW(page, DWLP_USER, reinterpret_cast<LONG_PTR>(job));
		if (const HWND log = GetDlgItem(page, IDC_LOG))
		{
			// The resource marks the log read-only; enforce it, disable the themed white
			// background, and hide the caret so the output pane cannot look like an input field.
			SendMessageW(log, EM_SETREADONLY, TRUE, 0);
			SetWindowTheme(log, L"", L"");
			SetWindowSubclass(log, LogEditProc, 1, 0);
		}
		ShowWindow(GetDlgItem(page, IDC_OPEN_SETTINGS), SW_HIDE);
		ShowWindow(GetDlgItem(page, IDC_RESTART_NOTE), SW_HIDE);
		SendDlgItemMessageW(page, IDC_PROGRESS, PBM_SETMARQUEE, FALSE, 0);
		return TRUE;
	}
	case kJobUpdate:
	case kJobDone:
		if (Job* job = JobOf(page))
		{
			UpdateProgressPage(job, page);
		}
		return TRUE;
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLOREDIT:
		if (reinterpret_cast<HWND>(lParam) == GetDlgItem(page, IDC_LOG))
		{
			const HDC dc = reinterpret_cast<HDC>(wParam);
			SetBkColor(dc, GetSysColor(COLOR_3DFACE));
			SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
			return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
		}
		break;
	case WM_NOTIFY:
		switch (reinterpret_cast<NMHDR*>(lParam)->code)
		{
		case PSN_SETACTIVE:
		{
			Job* job = JobOf(page);
			job->sheet = GetParent(page);
			job->progressPage = page;
			PropSheet_SetWizButtons(job->sheet, 0);
			EnableWindow(GetDlgItem(job->sheet, IDCANCEL), FALSE);
			EnableMenuItem(GetSystemMenu(job->sheet, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
			SendDlgItemMessageW(page, IDC_PROGRESS, PBM_SETMARQUEE, TRUE, 30);
			SetDlgItemTextW(page, IDC_STATUS, L"Working...");
			if (!job->started)
			{
				job->started = true;
				try
				{
					job->worker = std::thread([job] { RunWorker(job); });
				}
				catch (...)
				{
					std::lock_guard<std::mutex> lock(job->mutex);
					job->result.message = L"Setup could not start its worker thread.";
					job->status = job->result.message;
					job->done = true;
					PostMessageW(page, kJobDone, 0, 0);
				}
			}
			return PageResult(page, 0);
		}
		case PSN_WIZFINISH:
		{
			Job* job = JobOf(page);
			if (job->mode == Mode::Install)
			{
				job->openSettings = IsDlgButtonChecked(page, IDC_OPEN_SETTINGS) == BST_CHECKED;
			}
			return PageResult(page, 0);
		}
		case PSN_QUERYCANCEL:
		{
			Job* job = JobOf(page);
			if (job != nullptr && job->started && !job->done)
			{
				return PageResult(page, 1);
			}
			return PageResult(page, 0);
		}
		default:
			break;
		}
		break;
	default:
		break;
	}
	return FALSE;
}

void WriteLog(Job& job)
{
	std::wstring text;
	{
		std::lock_guard<std::mutex> lock(job.mutex);
		text = job.log;
		if (!job.status.empty())
		{
			text += L"\r\n";
			text += job.status;
			text += L"\r\n";
		}
	}
	text = WindowsLineEndings(text);
	const std::wstring folder = TempFolder();
	if (folder.empty())
	{
		return;
	}
	const std::wstring path = folder + L"OpenA2dpSetup.log";
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
									FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return;
	}
	const WORD bom = 0xFEFF;
	DWORD written = 0;
	(void)WriteFile(file, &bom, sizeof(bom), &written, nullptr);
	(void)WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
	CloseHandle(file);
}

void LaunchSettings(const std::wstring& installDir)
{
	const std::wstring path = installDir + L"\\" + kConfigExeName;
	const HINSTANCE launched = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, installDir.c_str(),
											 SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(launched) <= 32)
	{
		MessageBoxW(nullptr, L"OpenA2DP Settings could not be started. Run it from the install folder.",
					L"OpenA2DP Setup", MB_OK | MB_ICONWARNING);
	}
}

int RunWizard(Job& job)
{
	const HINSTANCE module = g_instance != nullptr ? g_instance : GetModuleHandleW(nullptr);
	PROPSHEETPAGEW pages[3] = {};
	UINT count = 0;
	const auto page = [&](int dialog, DLGPROC procedure, const wchar_t* title) {
		PROPSHEETPAGEW& entry = pages[count++];
		entry.dwSize = sizeof(entry);
		entry.dwFlags = PSP_DEFAULT | PSP_USETITLE;
		entry.hInstance = module;
		entry.pszTemplate = MAKEINTRESOURCEW(dialog);
		entry.pfnDlgProc = procedure;
		entry.pszTitle = title;
		entry.lParam = reinterpret_cast<LPARAM>(&job);
	};

	const std::wstring titleWelcome = job.updating ? L"Update OpenA2DP" : L"Welcome to OpenA2DP Setup";
	const std::wstring titleLocation = L"Choose the install folder";
	const std::wstring titleProgress =
		job.updating ? L"Updating OpenA2DP" : L"Installing OpenA2DP";
	const std::wstring titleConfirm = L"Remove OpenA2DP";
	const std::wstring titleRemoving = L"Removing OpenA2DP";
	if (job.mode == Mode::Install)
	{
		page(IDD_WELCOME, WelcomeProc, titleWelcome.c_str());
		page(IDD_LOCATION, LocationProc, titleLocation.c_str());
		page(IDD_PROGRESS, ProgressProc, titleProgress.c_str());
	}
	else
	{
		page(IDD_CONFIRM, ConfirmProc, titleConfirm.c_str());
		page(IDD_PROGRESS, ProgressProc, titleRemoving.c_str());
	}

	PROPSHEETHEADERW header = {};
	header.dwSize = sizeof(header);
	// ppsp points at PROPSHEETPAGE structures, not CreatePropertySheetPage handles. Without this
	// flag comctl32 interprets the first structure's dwSize/dwFlags as a page handle.
	header.dwFlags = PSH_WIZARD | PSH_NOAPPLYNOW | PSH_USEHICON | PSH_PROPSHEETPAGE;
	header.hInstance = module;
	header.pszCaption = L"OpenA2DP Setup";
	header.nPages = count;
	header.ppsp = pages;
	header.hIcon = LoadIconW(module, MAKEINTRESOURCEW(IDI_SETUP_APP));
	(void)PropertySheetW(&header);

	if (job.worker.joinable())
	{
		job.worker.join();
	}
	WriteLog(job);
	return job.result.success ? 0 : 1;
}

bool SpawnCleanup(const std::wstring& installDir)
{
	const std::wstring self = ModulePath();
	const std::wstring folder = TempFolder();
	if (self.empty() || folder.empty())
	{
		return false;
	}
	wchar_t name[96] = {};
	swprintf_s(name, L"OpenA2dpSetup.cleanup.%lu.%llu.exe", GetCurrentProcessId(),
			   static_cast<unsigned long long>(GetTickCount64()));
	const std::wstring helper = folder + name;
	if (!CopyFileW(self.c_str(), helper.c_str(), FALSE))
	{
		return false;
	}
	const std::wstring command = L"\"" + helper + L"\" /cleanup " + Quoted(installDir) + L" " +
								 std::to_wstring(GetCurrentProcessId());
	std::vector<wchar_t> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back(L'\0');
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
						nullptr, &startup, &process))
	{
		DeleteFileW(helper.c_str());
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

int CleanupMode(const std::wstring& installDir, DWORD processId)
{
	if (!installDir.empty())
	{
		const HANDLE process = processId != 0 ? OpenProcess(SYNCHRONIZE, FALSE, processId) : nullptr;
		if (process != nullptr)
		{
			(void)WaitForSingleObject(process, 60000);
			CloseHandle(process);
		}
		const Log log = [](const std::wstring&) {};
		(void)CleanupInstalledFiles(installDir, log);
	}
	const std::wstring self = ModulePath();
	if (!self.empty())
	{
		(void)MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
	}
	return 0;
}

int RunInstall(const std::wstring& setupExe)
{
	Job job;
	job.mode = Mode::Install;
	job.setupExe = setupExe;
	InstallInfo info;
	if (ReadInstallInfo(info))
	{
		job.updating = true;
		job.installDir = info.installDir;
		job.existingVersion = info.version;
	}
	else
	{
		job.installDir = DefaultInstallDir();
	}
	if (job.installDir.empty())
	{
		MessageBoxW(nullptr, L"The Program Files folder could not be located. Run setup from an "
							 L"elevated prompt and choose an install folder.",
					L"OpenA2DP Setup", MB_OK | MB_ICONERROR);
		return 1;
	}
	(void)RunWizard(job);
	if (job.result.success && job.openSettings)
	{
		LaunchSettings(job.installDir);
	}
	return job.result.success ? 0 : 1;
}

int RunUninstall(const std::wstring& setupExe)
{
	InstallInfo info;
	if (!ReadInstallInfo(info) || info.installDir.empty())
	{
		MessageBoxW(nullptr, L"OpenA2DP is not installed on this PC.", L"OpenA2DP Setup",
					MB_OK | MB_ICONINFORMATION);
		return 1;
	}
	Job job;
	job.mode = Mode::Uninstall;
	job.setupExe = setupExe;
	job.installDir = info.installDir;
	(void)RunWizard(job);
	if (job.result.success)
	{
		if (!SpawnCleanup(job.installDir))
		{
			const std::wstring installedSetup = job.installDir + L"\\" + kSetupExeName;
			if (!MoveFileExW(installedSetup.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
			{
				MessageBoxW(nullptr,
							L"OpenA2DP was removed, but its installer could not be scheduled for "
							L"deletion. You can delete the install folder by hand.",
							L"OpenA2DP Setup", MB_OK | MB_ICONWARNING);
			}
		}
	}
	return job.result.success ? 0 : 1;
}

void Usage()
{
	MessageBoxW(nullptr,
				L"OpenA2dpSetup.exe installs or updates OpenA2DP.\n\n"
				L"OpenA2dpSetup.exe /uninstall removes OpenA2DP and returns every headset to the "
				L"Windows driver.\n\n"
				L"Remove OpenA2DP from Settings > Apps to run the uninstaller.",
				L"OpenA2DP Setup", MB_OK | MB_ICONINFORMATION);
}

} // namespace
} // namespace OpenA2dp::Setup

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	using namespace OpenA2dp::Setup;
	OpenA2dp::Setup::g_instance = instance;

	int argumentCount = 0;
	LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
	if (arguments == nullptr)
	{
		return 2;
	}
	bool uninstall = false;
	for (int index = 1; index < argumentCount; ++index)
	{
		if (_wcsicmp(arguments[index], L"/cleanup") == 0)
		{
			const std::wstring directory = index + 1 < argumentCount ? arguments[index + 1] : L"";
			const DWORD processId =
				index + 2 < argumentCount ? static_cast<DWORD>(_wtoi(arguments[index + 2])) : 0;
			LocalFree(arguments);
			return CleanupMode(directory, processId);
		}
		if (_wcsicmp(arguments[index], L"/uninstall") == 0)
		{
			uninstall = true;
		}
		else if (_wcsicmp(arguments[index], L"/help") == 0 || _wcsicmp(arguments[index], L"/?") == 0)
		{
			LocalFree(arguments);
			Usage();
			return 0;
		}
		else
		{
			LocalFree(arguments);
			Usage();
			return 2;
		}
	}
	LocalFree(arguments);

	if (!IsElevated())
	{
		MessageBoxW(nullptr, L"OpenA2DP setup needs an elevated prompt. Right-click the setup file and "
							 L"choose \"Run as administrator\".",
					L"OpenA2DP Setup", MB_OK | MB_ICONERROR);
		return 1;
	}

	const HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\OpenA2dpSetup");
	if (mutex == nullptr)
	{
		return 1;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		MessageBoxW(nullptr, L"OpenA2DP Setup is already running.", L"OpenA2DP Setup",
					MB_OK | MB_ICONINFORMATION);
		CloseHandle(mutex);
		return 1;
	}

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	INITCOMMONCONTROLSEX controls = {};
	controls.dwSize = sizeof(controls);
	controls.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&controls);

	const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const std::wstring setupExe = ModulePath();
	const int code = setupExe.empty()
						 ? 1
						 : (uninstall ? OpenA2dp::Setup::RunUninstall(setupExe)
									  : OpenA2dp::Setup::RunInstall(setupExe));
	if (SUCCEEDED(com))
	{
		CoUninitialize();
	}
	CloseHandle(mutex);
	return code;
}
