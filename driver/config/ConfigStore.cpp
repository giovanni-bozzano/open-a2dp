#include "../Driver.h"

#include <ntstrsafe.h>

using namespace OpenA2dp;

// Stored settings, one small record per file under %ProgramData%\OpenA2dp:
//   default.cfg             the machine default
//   devices\<address>.cfg   that sink's settings, which win over the default
// The record carries the codec preference and the per-codec endpoint formats together, so a
// sink's settings are one file the user can copy, back up or delete. Loading runs while the
// device is added and saving from the IOCTLs the tools send; both are at PASSIVE_LEVEL and
// both are file I/O, so there is no registry state to keep in step. An absent file means "no
// settings"; a file another build wrote is ignored rather than misread.
namespace
{
const wchar_t kDirectory[] = L"\\??\\C:\\ProgramData\\OpenA2dp";
const wchar_t kDevicesDirectory[] = L"\\??\\C:\\ProgramData\\OpenA2dp\\devices";
const wchar_t kDefaultPath[] = L"\\??\\C:\\ProgramData\\OpenA2dp\\default.cfg";
constexpr uint32_t kRecordMagic = 0x4432414F; // 'OA2D'
// Bumped with the record layout; a file with another version is ignored, not reinterpreted.
constexpr uint32_t kRecordVersion = 1;

struct Record
{
	uint32_t magic;
	uint32_t version;
	uint32_t size; // sizeof(Record), so a truncated write is detected
	uint32_t reserved;
	Ioctl::Preference preference;
	Ioctl::EndpointFormat endpointFormat;
};

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

bool ReadRecord(DEVICE_CONTEXT* ctx, const wchar_t* path, Record& record)
{
	HANDLE handle = nullptr;
	NTSTATUS status = OpenFile(path, FILE_READ_DATA, FILE_OPEN, handle);
	if (!NT_SUCCESS(status))
	{
		// No settings yet for this key: the defaults apply and nothing is logged.
		return false;
	}
	IO_STATUS_BLOCK statusBlock = {};
	status = ZwReadFile(handle, nullptr, nullptr, nullptr, &statusBlock, &record, sizeof(record), nullptr,
						nullptr);
	ZwClose(handle);
	if (!NT_SUCCESS(status) || statusBlock.Information != sizeof(record))
	{
		OA2DP_WARN(ctx, "Config", "ignoring %ws: %u of %u bytes read (0x%08X)", path,
				   static_cast<unsigned>(statusBlock.Information), static_cast<unsigned>(sizeof(record)), status);
		return false;
	}
	if (record.magic != kRecordMagic || record.version != kRecordVersion || record.size != sizeof(record))
	{
		OA2DP_WARN(ctx, "Config", "ignoring %ws: not a settings file this build wrote", path);
		return false;
	}
	return true;
}

void WriteRecord(DEVICE_CONTEXT* ctx, const wchar_t* path, const Record& record)
{
	EnsureDirectory(kDirectory);
	EnsureDirectory(kDevicesDirectory);
	HANDLE handle = nullptr;
	NTSTATUS status = OpenFile(path, FILE_WRITE_DATA, FILE_OVERWRITE_IF, handle);
	if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Config", "cannot open %ws 0x%08X; the settings are kept in memory only", path, status);
		return;
	}
	IO_STATUS_BLOCK statusBlock = {};
	status = ZwWriteFile(handle, nullptr, nullptr, nullptr, &statusBlock, const_cast<Record*>(&record),
						 sizeof(record), nullptr, nullptr);
	ZwClose(handle);
	if (!NT_SUCCESS(status) || statusBlock.Information != sizeof(record))
	{
		OA2DP_WARN(ctx, "Config", "cannot write %ws 0x%08X", path, status);
	}
}

// devices\<12 hex address>.cfg, the file the sink's settings live in.
bool SinkPath(DEVICE_CONTEXT* ctx, wchar_t (&path)[96])
{
	const NTSTATUS status = RtlStringCchPrintfW(path, ARRAYSIZE(path), L"%ws\\%012I64X.cfg",
												kDevicesDirectory, ctx->RemoteAddress);
	return NT_SUCCESS(status);
}
} // namespace

void ConfigLoadSettings(_In_ DEVICE_CONTEXT* ctx)
{
	Ioctl::Preference preference = {};
	preference.abiVersion = Ioctl::kAbiVersion;
	Ioctl::EndpointFormat endpointFormat = {};
	endpointFormat.abiVersion = Ioctl::kAbiVersion;

	// The sink's own file wins over the machine default; a field the user never set is zero in
	// both, which is the driver default.
	Record record = {};
	wchar_t sinkPath[96] = {};
	const bool sinkFile = SinkPath(ctx, sinkPath) && ReadRecord(ctx, sinkPath, record);
	const bool stored = sinkFile || ReadRecord(ctx, kDefaultPath, record);
	if (stored)
	{
		preference = record.preference;
		endpointFormat = record.endpointFormat;
	}

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
		OA2DP_INFO(ctx, "Config",
				   "settings restored from %ws: codec preference %s (SBC bitpool %u, blocks %u, subbands %u, "
				   "allocation 0x%02X, AAC bitrate %u, preroll %u ms, SBC frames per packet %u, LDAC "
				   "quality %u)",
				   sinkFile ? sinkPath : kDefaultPath, CodecValueName(preference.codec), preference.sbcBitpool,
				   preference.sbcBlocks, preference.sbcSubbands, preference.sbcAllocation, preference.aacBitrate,
				   preference.prerollMs, preference.sbcFramesPerPacket, preference.ldacQuality);
	}
}

void ConfigSaveSettings(_In_ DEVICE_CONTEXT* ctx)
{
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
	{
		// File I/O needs PASSIVE_LEVEL; a change made higher is only kept in memory.
		return;
	}

	Record record = {};
	record.magic = kRecordMagic;
	record.version = kRecordVersion;
	record.size = sizeof(record);
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		record.preference = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
		{
			record.endpointFormat.sampleRate[codec] = ctx->CodecSampleRate[codec];
			record.endpointFormat.channels[codec] = ctx->CodecChannels[codec];
		}
		KeReleaseSpinLock(&ctx->AudioLock, irql);
	}
	record.preference.abiVersion = Ioctl::kAbiVersion;
	record.endpointFormat.abiVersion = Ioctl::kAbiVersion;

	wchar_t sinkPath[96] = {};
	if (!SinkPath(ctx, sinkPath))
	{
		OA2DP_WARN(ctx, "Config", "cannot name this sink's settings file; nothing is stored");
		return;
	}
	WriteRecord(ctx, sinkPath, record);
	OA2DP_INFO(ctx, "Config",
			   "settings stored in %ws: codec preference %s (SBC bitpool %u, blocks %u, subbands %u, "
			   "allocation 0x%02X, AAC bitrate %u, preroll %u ms, SBC frames per packet %u, LDAC quality %u)",
			   sinkPath, CodecValueName(record.preference.codec), record.preference.sbcBitpool,
			   record.preference.sbcBlocks, record.preference.sbcSubbands, record.preference.sbcAllocation,
			   record.preference.aacBitrate, record.preference.prerollMs, record.preference.sbcFramesPerPacket,
			   record.preference.ldacQuality);
}
