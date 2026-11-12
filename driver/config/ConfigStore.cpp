#include "../Driver.h"

#include <ntstrsafe.h>
#include <OpenA2dp/ConfigFile.h>

using namespace OpenA2dp;

// Stored settings, one text file per sink under %ProgramData%\OpenA2dp:
//   default.ini            the machine default, used only when the sink has no file of its own
//   devices\<address>.ini  that sink's settings, which win over the default
// The layout, the sections and what a value has to look like live in OpenA2dp/ConfigFile.h, so
// the tests check the code the driver runs; this file only moves that text to and from disk.
// Loading runs while the device is added and saving from the IOCTLs the tools send; both are at
// PASSIVE_LEVEL and both are file I/O, so there is no registry state to keep in step. An absent
// file means "no settings".
namespace
{
const wchar_t kDirectory[] = L"\\??\\C:\\ProgramData\\OpenA2dp";
const wchar_t kDevicesDirectory[] = L"\\??\\C:\\ProgramData\\OpenA2dp\\devices";
const wchar_t kDefaultPath[] = L"\\??\\C:\\ProgramData\\OpenA2dp\\default.ini";

NTSTATUS OpenFile(const wchar_t* path, ACCESS_MASK access, ULONG disposition, HANDLE& handle)
{
	UNICODE_STRING name;
	RtlInitUnicodeString(&name, path);
	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
	IO_STATUS_BLOCK statusBlock = {};
	return ZwCreateFile(&handle, access | SYNCHRONIZE, &attributes, &statusBlock, nullptr,
						FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, disposition,
						FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, nullptr, 0);
}

// The installer creates both directories; this covers a machine where it did not, so a first
// save does not fail. A directory that already exists is the normal case, not an error.
void EnsureDirectory(const wchar_t* path)
{
	UNICODE_STRING name;
	RtlInitUnicodeString(&name, path);
	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
	IO_STATUS_BLOCK statusBlock = {};
	HANDLE handle = nullptr;
	const NTSTATUS status = ZwCreateFile(&handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &statusBlock,
										nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
										FILE_CREATE, FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE, nullptr,
										0);
	if (NT_SUCCESS(status))
	{
		ZwClose(handle);
	}
}

// The text of a settings file, or false when there is none: no file is "no settings", not an
// error, so nothing is logged for it.
bool ReadSettingsFile(DEVICE_CONTEXT* ctx, const wchar_t* path, char (&text)[ConfigFile::kMaxBytes],
					  size_t& length)
{
	HANDLE handle = nullptr;
	if (!NT_SUCCESS(OpenFile(path, FILE_READ_DATA, FILE_OPEN, handle)))
	{
		return false;
	}
	FILE_STANDARD_INFORMATION information = {};
	IO_STATUS_BLOCK statusBlock = {};
	NTSTATUS status = ZwQueryInformationFile(handle, &statusBlock, &information, sizeof(information),
											 FileStandardInformation);
	const LONGLONG bytes = information.EndOfFile.QuadPart;
	if (!NT_SUCCESS(status))
	{
		ZwClose(handle);
		OA2DP_WARN(ctx, "Config", "ignoring %ws: 0x%08X reading its length", path, status);
		return false;
	}
	// An empty file is "no settings", the same as a missing one, so it is not worth a warning.
	if (bytes <= 0)
	{
		ZwClose(handle);
		return false;
	}
	if (bytes >= static_cast<LONGLONG>(ConfigFile::kMaxBytes))
	{
		ZwClose(handle);
		OA2DP_WARN(ctx, "Config", "ignoring %ws: %I64d bytes is more than this build reads", path, bytes);
		return false;
	}
	status = ZwReadFile(handle, nullptr, nullptr, nullptr, &statusBlock, text, static_cast<ULONG>(bytes), nullptr,
						nullptr);
	ZwClose(handle);
	if (!NT_SUCCESS(status) || statusBlock.Information != static_cast<ULONG>(bytes))
	{
		OA2DP_WARN(ctx, "Config", "ignoring %ws: 0x%08X reading it", path, status);
		return false;
	}
	length = static_cast<size_t>(bytes);
	text[length] = '\0';
	return true;
}

// Writes the sink's file. False when the text could not be produced or could not reach the disk,
// so the caller reports what happened instead of claiming the settings were stored.
bool WriteSettings(DEVICE_CONTEXT* ctx, const wchar_t* path, const ConfigFile::Settings& settings)
{
	char text[ConfigFile::kMaxBytes] = {};
	const size_t length = ConfigFile::Write(settings, text, sizeof(text));
	if (length == 0)
	{
		OA2DP_WARN(ctx, "Config", "cannot format the settings for %ws", path);
		return false;
	}
	EnsureDirectory(kDirectory);
	EnsureDirectory(kDevicesDirectory);
	HANDLE handle = nullptr;
	const NTSTATUS status = OpenFile(path, FILE_WRITE_DATA, FILE_OVERWRITE_IF, handle);
	if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Config", "cannot open %ws 0x%08X; the settings are kept in memory only", path, status);
		return false;
	}
	IO_STATUS_BLOCK statusBlock = {};
	const NTSTATUS written = ZwWriteFile(handle, nullptr, nullptr, nullptr, &statusBlock, text,
										 static_cast<ULONG>(length), nullptr, nullptr);
	ZwClose(handle);
	if (!NT_SUCCESS(written) || statusBlock.Information != length)
	{
		OA2DP_WARN(ctx, "Config", "cannot write %ws 0x%08X", path, written);
		return false;
	}
	return true;
}

// Removes the sink's file. There is nothing to remove when the sink was never changed, which is
// the normal case and not worth a warning.
void DeleteSettingsFile(DEVICE_CONTEXT* ctx, const wchar_t* path)
{
	HANDLE handle = nullptr;
	const NTSTATUS status = OpenFile(path, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, handle);
	if (!NT_SUCCESS(status))
	{
		if (status != STATUS_OBJECT_NAME_NOT_FOUND && status != STATUS_OBJECT_PATH_NOT_FOUND)
		{
			OA2DP_WARN(ctx, "Config", "cannot open %ws to remove it 0x%08X", path, status);
		}
		return;
	}
	FILE_DISPOSITION_INFORMATION disposition = {};
	disposition.DeleteFile = TRUE;
	IO_STATUS_BLOCK statusBlock = {};
	const NTSTATUS removed = ZwSetInformationFile(handle, &statusBlock, &disposition, sizeof(disposition),
												  FileDispositionInformation);
	ZwClose(handle);
	if (!NT_SUCCESS(removed))
	{
		OA2DP_WARN(ctx, "Config", "cannot remove %ws 0x%08X", path, removed);
	}
}

// devices\<12 hex address>.ini, the file the sink's settings live in.
bool SinkPath(DEVICE_CONTEXT* ctx, wchar_t (&path)[96])
{
	const NTSTATUS status = RtlStringCchPrintfW(path, ARRAYSIZE(path), L"%ws\\%012I64X.ini",
												kDevicesDirectory, ctx->RemoteAddress);
	return NT_SUCCESS(status);
}

// One line with every stored value, so a log reader knows what a sink runs with without opening
// the file. Both the restore and the save path report through here, so a new field is added once.
void LogSettings(_In_ DEVICE_CONTEXT* ctx, const char* action, const wchar_t* path,
				 const Ioctl::Preference& preference)
{
	OA2DP_INFO(ctx, "Config",
			   "settings %s %ws: codec preference %s (SBC bitpool %u, blocks %u, subbands %u, "
			   "allocation 0x%02X, AAC bitrate %u, preroll %u ms, SBC frames per packet %u, "
			   "LDAC quality %u)",
			   action, path, CodecValueName(preference.codec), preference.sbcBitpool,
			   preference.sbcBlocks, preference.sbcSubbands, preference.sbcAllocation,
			   preference.aacBitrate, preference.prerollMs, preference.sbcFramesPerPacket,
			   preference.ldacQuality);
}
} // namespace

void ConfigLoadSettings(_In_ DEVICE_CONTEXT* ctx)
{
	ConfigFile::Settings settings = {};
	settings.preference.abiVersion = Ioctl::kAbiVersion;
	settings.endpointFormat.abiVersion = Ioctl::kAbiVersion;

	// The sink's own file wins over the machine default; a field the user never set is zero in
	// both, which is the driver default.
	char text[ConfigFile::kMaxBytes] = {};
	size_t length = 0;
	wchar_t sinkPath[96] = {};
	const bool sinkFile = SinkPath(ctx, sinkPath) && ReadSettingsFile(ctx, sinkPath, text, length) &&
						  ConfigFile::Parse(text, length, settings);
	bool stored = sinkFile;
	if (!sinkFile)
	{
		settings = {};
		stored = ReadSettingsFile(ctx, kDefaultPath, text, length) && ConfigFile::Parse(text, length, settings);
	}

	Ioctl::Preference preference = settings.preference;
	Ioctl::EndpointFormat endpointFormat = settings.endpointFormat;
	preference.abiVersion = Ioctl::kAbiVersion;
	if (!Ioctl::ValidPreference(preference))
	{
		OA2DP_WARN(ctx, "Config", "ignoring the stored codec preference (flags 0x%08X, codec %u)",
				   preference.flags, static_cast<unsigned>(preference.codec));
		preference = {};
		preference.abiVersion = Ioctl::kAbiVersion;
	}
	endpointFormat.abiVersion = Ioctl::kAbiVersion;
	if (!Ioctl::ValidEndpointFormat(endpointFormat))
	{
		OA2DP_WARN(ctx, "Config", "ignoring the stored endpoint formats");
		endpointFormat = {};
		endpointFormat.abiVersion = Ioctl::kAbiVersion;
	}

	KIRQL irql;
	KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
	ctx->Preference = preference;
	KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	// No other thread can look at the formats yet: this runs while the device is being added,
	// before the endpoint exists.
	for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
	{
		ctx->CodecSampleRate[codec] = endpointFormat.sampleRate[codec];
		ctx->CodecChannels[codec] = static_cast<USHORT>(endpointFormat.channels[codec]);
	}

	if (stored)
	{
		LogSettings(ctx, "restored from", sinkFile ? sinkPath : kDefaultPath, preference);
	}
}

void ConfigSaveSettings(_In_ DEVICE_CONTEXT* ctx)
{
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
	{
		// File I/O needs PASSIVE_LEVEL; a change made higher is only kept in memory.
		return;
	}

	ConfigFile::Settings settings = {};
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		settings.preference = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
		{
			settings.endpointFormat.sampleRate[codec] = ctx->CodecSampleRate[codec];
			settings.endpointFormat.channels[codec] = ctx->CodecChannels[codec];
		}
		KeReleaseSpinLock(&ctx->AudioLock, irql);
	}
	settings.preference.abiVersion = Ioctl::kAbiVersion;
	settings.endpointFormat.abiVersion = Ioctl::kAbiVersion;

	wchar_t sinkPath[96] = {};
	if (!SinkPath(ctx, sinkPath))
	{
		OA2DP_WARN(ctx, "Config", "cannot name this sink's settings file; nothing is stored");
		return;
	}
	if (ConfigFile::SameDefaults(settings))
	{
		// A sink nobody changed keeps no file: the defaults are the absence of settings, so the
		// file is removed instead of being filled with them.
		DeleteSettingsFile(ctx, sinkPath);
		return;
	}
	if (WriteSettings(ctx, sinkPath, settings))
	{
		LogSettings(ctx, "stored in", sinkPath, settings.preference);
	}
}
