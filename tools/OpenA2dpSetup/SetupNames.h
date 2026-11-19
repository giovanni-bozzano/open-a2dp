#pragma once

// Names the installer, the service and the uninstaller must agree on. One place, so a rename
// cannot leave the installer looking for a file the build named differently.

namespace OpenA2dp::Setup {

inline constexpr wchar_t kProductName[] = L"OpenA2DP";
inline constexpr wchar_t kSetupExeName[] = L"OpenA2dpSetup.exe";
inline constexpr wchar_t kServiceExeName[] = L"OpenA2dpSvc.exe";
inline constexpr wchar_t kControlExeName[] = L"OpenA2dpCtl.exe";
inline constexpr wchar_t kConfigExeName[] = L"OpenA2dpConfig.exe";
inline constexpr wchar_t kInfName[] = L"OpenA2dp.inf";
inline constexpr wchar_t kSysName[] = L"OpenA2dp.sys";
inline constexpr wchar_t kCatName[] = L"OpenA2dp.cat";

inline constexpr wchar_t kEncoderServiceName[] = L"OpenA2dpSvc";
inline constexpr wchar_t kEncoderServiceDisplayName[] = L"OpenA2DP Encoder Service";
inline constexpr wchar_t kDriverServiceName[] = L"OpenA2dp";

inline constexpr wchar_t kAppKey[] = L"SOFTWARE\\OpenA2DP";
inline constexpr wchar_t kUninstallKey[] =
	L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OpenA2DP";
inline constexpr wchar_t kStartMenuFolder[] = L"OpenA2DP";
inline constexpr wchar_t kShortcutName[] = L"OpenA2DP Settings.lnk";

} // namespace OpenA2dp::Setup
