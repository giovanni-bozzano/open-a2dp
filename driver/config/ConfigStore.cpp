#include "../Driver.h"

#include <ntstrsafe.h>

using namespace OpenA2dp;

// Stored settings, one INI text file per sink under %ProgramData%\OpenA2dp:
//   default.ini            the machine default
//   devices\<address>.ini  that sink's settings, which win over the default
// Text rather than a binary record so the file can be read, backed up and edited without this
// build: one section per codec plus a general one, a `key = value` line under each, and `#` for a
// comment. A key or a section this build does not know is ignored, so a newer file still loads; a
// value the driver cannot serve refuses the whole file instead of half applying it. Loading runs
// while the device is added and saving from the IOCTLs the tools send; both are at PASSIVE_LEVEL
// and both are file I/O, so there is no registry state to keep in step. An absent file means "no
// settings".
namespace
{
const wchar_t kDirectory[] = L"\\??\\C:\\ProgramData\\OpenA2dp";
const wchar_t kDevicesDirectory[] = L"\\??\\C:\\ProgramData\\OpenA2dp\\devices";
const wchar_t kDefaultPath[] = L"\\??\\C:\\ProgramData\\OpenA2dp\\default.ini";
// A different number is a file this build does not understand; it is ignored, not guessed at.
// 2 is the sectioned layout: the flat keys of the first format are not read.
constexpr uint32_t kFormatVersion = 2;
// Room for the file below with space to spare. A longer file is refused rather than read in
// part, so a hand edit cannot be half applied.
constexpr ULONG kMaxFileBytes = 1024;
constexpr size_t kMaxChoiceChars = 12;
constexpr size_t kMaxNameChars = 16;

struct Settings
{
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

bool KeyIs(const char* key, size_t length, const char* name)
{
	size_t index = 0;
	for (; index < length; ++index)
	{
		if (name[index] == '\0' || name[index] != key[index])
		{
			return false;
		}
	}
	return name[index] == '\0';
}

bool IsSpace(char character)
{
	return character == ' ' || character == '\t';
}

// An unsigned decimal number that fills the value exactly; everything this file stores is a
// count, a rate or a bitrate, so a sign, a fraction or a stray character is a file this driver
// did not write.
bool ReadNumber(const char* value, size_t length, uint32_t& number)
{
	if (length == 0)
	{
		return false;
	}
	uint64_t parsed = 0;
	for (size_t index = 0; index < length; ++index)
	{
		if (value[index] < '0' || value[index] > '9')
		{
			return false;
		}
		parsed = parsed * 10 + static_cast<uint64_t>(value[index] - '0');
		if (parsed > 0xFFFFFFFFull)
		{
			return false;
		}
	}
	number = static_cast<uint32_t>(parsed);
	return true;
}

// "auto" for a value the driver chooses, a number otherwise.
bool ReadChoice(const char* value, size_t length, uint32_t& number)
{
	if (KeyIs(value, length, "auto"))
	{
		number = 0;
		return true;
	}
	return ReadNumber(value, length, number);
}

// The codec a section name stands for, -1 for "auto" and -2 for a name this build does not know
// (the same five names `OpenA2dpCtl format` takes).
int CodecFromName(const char* name, size_t length)
{
	static const struct
	{
		const char* name;
		uint32_t codec;
	} kNames[] = {
		{"sbc", Ioctl::kCodecSbc},
		{"aptx", Ioctl::kCodecAptx},
		{"aac", Ioctl::kCodecAac},
		{"aptxhd", Ioctl::kCodecAptxHd},
		{"ldac", Ioctl::kCodecLdac},
	};
	if (KeyIs(name, length, "auto"))
	{
		return -1;
	}
	for (const auto& entry : kNames)
	{
		if (KeyIs(name, length, entry.name))
		{
			return static_cast<int>(entry.codec);
		}
	}
	return -2;
}

const char* CodecFileName(uint32_t codec)
{
	switch (codec)
	{
	case Ioctl::kCodecSbc: return "sbc";
	case Ioctl::kCodecAptx: return "aptx";
	case Ioctl::kCodecAac: return "aac";
	case Ioctl::kCodecAptxHd: return "aptxhd";
	case Ioctl::kCodecLdac: return "ldac";
	default: return "auto";
	}
}

// One key of one section. A key or a section this build does not write is skipped, so a newer
// file still loads; a key that is known but holds a value this build cannot use refuses the
// file, because silently dropping it would apply a setting the file does not ask for.
bool ApplyValue(const char* section, size_t sectionLength, const char* key, size_t keyLength,
				const char* value, size_t valueLength, Settings& settings)
{
	Ioctl::Preference& preference = settings.preference;
	Ioctl::EndpointFormat& format = settings.endpointFormat;

	if (KeyIs(section, sectionLength, "general"))
	{
		if (KeyIs(key, keyLength, "codec"))
		{
			const int codec = CodecFromName(value, valueLength);
			if (codec == -2)
			{
				return false;
			}
			preference.flags = codec < 0 ? 0 : Ioctl::kPreferenceForceCodec;
			preference.codec = codec < 0 ? Ioctl::kCodecSbc : static_cast<uint32_t>(codec);
			return true;
		}
		if (KeyIs(key, keyLength, "prerollMs"))
		{
			return ReadNumber(value, valueLength, preference.prerollMs);
		}
		return true;
	}

	// A codec section names its codec once: rate and channels are that codec's own entry, and
	// the parameters below exist only where the codec has such a setting.
	const int codec = CodecFromName(section, sectionLength);
	if (codec < 0)
	{
		return true;
	}
	if (KeyIs(key, keyLength, "rate"))
	{
		return ReadChoice(value, valueLength, format.sampleRate[codec]);
	}
	if (KeyIs(key, keyLength, "channels"))
	{
		return ReadChoice(value, valueLength, format.channels[codec]);
	}
	if (codec == static_cast<int>(Ioctl::kCodecSbc))
	{
		if (KeyIs(key, keyLength, "bitpool"))
		{
			return ReadNumber(value, valueLength, preference.sbcBitpool);
		}
		if (KeyIs(key, keyLength, "blocks"))
		{
			return ReadNumber(value, valueLength, preference.sbcBlocks);
		}
		if (KeyIs(key, keyLength, "subbands"))
		{
			return ReadNumber(value, valueLength, preference.sbcSubbands);
		}
		if (KeyIs(key, keyLength, "allocation"))
		{
			return ReadNumber(value, valueLength, preference.sbcAllocation);
		}
		if (KeyIs(key, keyLength, "framesPerPacket"))
		{
			return ReadNumber(value, valueLength, preference.sbcFramesPerPacket);
		}
	}
	if (codec == static_cast<int>(Ioctl::kCodecAac) && KeyIs(key, keyLength, "bitrate"))
	{
		return ReadNumber(value, valueLength, preference.aacBitrate);
	}
	if (codec == static_cast<int>(Ioctl::kCodecLdac) && KeyIs(key, keyLength, "quality"))
	{
		return ReadNumber(value, valueLength, preference.ldacQuality);
	}
	return true;
}

// Every line of the file: blank lines and comments are skipped, `[section]` opens a section,
// `key = value` is applied to it, and a line that is neither refuses the file. The version has
// to be this build's and is only read before the first section.
bool ParseSettings(const char* text, size_t length, Settings& settings)
{
	settings = {};
	settings.preference.abiVersion = Ioctl::kAbiVersion;
	settings.endpointFormat.abiVersion = Ioctl::kAbiVersion;
	const char* cursor = text;
	const char* const textEnd = text + length;
	char section[kMaxNameChars + 1] = {};
	size_t sectionLength = 0;
	bool versionSeen = false;
	uint32_t version = 0;
	while (cursor < textEnd)
	{
		const char* lineEnd = cursor;
		while (lineEnd < textEnd && *lineEnd != '\r' && *lineEnd != '\n')
		{
			++lineEnd;
		}
		const char* next = lineEnd;
		while (next < textEnd && (*next == '\r' || *next == '\n'))
		{
			++next;
		}

		const char* begin = cursor;
		const char* end = lineEnd;
		while (begin < end && IsSpace(*begin))
		{
			++begin;
		}
		for (const char* scan = begin; scan < end; ++scan)
		{
			if (*scan == '#')
			{
				end = scan;
				break;
			}
		}
		while (end > begin && IsSpace(end[-1]))
		{
			--end;
		}
		cursor = next;
		if (end == begin)
		{
			continue;
		}

		if (*begin == '[')
		{
			const char* close = begin + 1;
			while (close < end && *close != ']')
			{
				++close;
			}
			if (close >= end)
			{
				return false;
			}
			for (const char* scan = close + 1; scan < end; ++scan)
			{
				if (!IsSpace(*scan))
				{
					return false;
				}
			}
			const size_t nameLength = static_cast<size_t>(close - (begin + 1));
			if (nameLength == 0 || nameLength > kMaxNameChars)
			{
				return false;
			}
			for (size_t index = 0; index < nameLength; ++index)
			{
				section[index] = begin[1 + index];
			}
			section[nameLength] = '\0';
			sectionLength = nameLength;
			continue;
		}

		const char* equals = nullptr;
		for (const char* scan = begin; scan < end; ++scan)
		{
			if (*scan == '=')
			{
				equals = scan;
				break;
			}
		}
		if (equals == nullptr)
		{
			return false;
		}
		const char* keyEnd = equals;
		while (keyEnd > begin && IsSpace(keyEnd[-1]))
		{
			--keyEnd;
		}
		const char* value = equals + 1;
		while (value < end && IsSpace(*value))
		{
			++value;
		}
		const size_t keyLength = static_cast<size_t>(keyEnd - begin);
		const size_t valueLength = static_cast<size_t>(end - value);

		// The version is metadata about the file, not a setting, so it sits above the sections.
		if (sectionLength == 0)
		{
			if (!KeyIs(begin, keyLength, "version") || !ReadNumber(value, valueLength, version))
			{
				return false;
			}
			versionSeen = true;
			continue;
		}
		if (!ApplyValue(section, sectionLength, begin, keyLength, value, valueLength, settings))
		{
			return false;
		}
	}
	return versionSeen && version == kFormatVersion;
}

// "auto" or the number, for the choices the driver may leave to itself.
const char* ChoiceText(uint32_t value, char (&text)[kMaxChoiceChars])
{
	if (value == 0)
	{
		return "auto";
	}
	(void)RtlStringCchPrintfA(text, ARRAYSIZE(text), "%u", value);
	return text;
}

void Append(char (&text)[kMaxFileBytes], size_t& length, const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const NTSTATUS status = RtlStringCchVPrintfA(text + length, ARRAYSIZE(text) - length, format, arguments);
	va_end(arguments);
	if (!NT_SUCCESS(status))
	{
		return;
	}
	size_t written = 0;
	if (NT_SUCCESS(RtlStringCchLengthA(text + length, ARRAYSIZE(text) - length, &written)))
	{
		length += written;
	}
}

void WriteSettings(DEVICE_CONTEXT* ctx, const wchar_t* path, const Settings& settings)
{
	const Ioctl::Preference& preference = settings.preference;
	const Ioctl::EndpointFormat& format = settings.endpointFormat;
	const bool forced = (preference.flags & Ioctl::kPreferenceForceCodec) != 0;
	char text[kMaxFileBytes] = {};
	size_t length = 0;
	// Two, because one section can print the rate and the channel count in the same line and each
	// ChoiceText fills the buffer it is given.
	char rate[kMaxChoiceChars] = {};
	char channels[kMaxChoiceChars] = {};

	Append(text, length,
		   "# OpenA2dp settings for this sink. Delete the file to return the driver defaults.\r\n"
		   "# One section per codec, plus [general] for what is not a codec's own setting.\r\n"
		   "version = %u\r\n"
		   "\r\n"
		   "[general]\r\n"
		   "# auto, sbc, aac, aptx, aptxhd or ldac\r\n"
		   "codec = %s\r\n"
		   "# How far ahead of the media clock the stream is sent, 0 leaves the default.\r\n"
		   "prerollMs = %u\r\n"
		   "\r\n",
		   kFormatVersion, CodecFileName(forced ? preference.codec : Ioctl::kCodecCount), preference.prerollMs);
	Append(text, length,
		   "[sbc]\r\n"
		   "# Each entry is a PCM rate and channel count, or auto for every format the codec plays.\r\n"
		   "rate = %s\r\n"
		   "channels = %s\r\n"
		   "bitpool = %u\r\n"
		   "blocks = %u\r\n"
		   "subbands = %u\r\n"
		   "allocation = %u\r\n"
		   "framesPerPacket = %u\r\n"
		   "\r\n",
		   ChoiceText(format.sampleRate[Ioctl::kCodecSbc], rate),
		   ChoiceText(format.channels[Ioctl::kCodecSbc], channels), preference.sbcBitpool,
		   preference.sbcBlocks, preference.sbcSubbands, preference.sbcAllocation,
		   preference.sbcFramesPerPacket);
	Append(text, length,
		   "[aac]\r\n"
		   "rate = %s\r\n"
		   "channels = %s\r\n"
		   "bitrate = %u\r\n"
		   "\r\n",
		   ChoiceText(format.sampleRate[Ioctl::kCodecAac], rate),
		   ChoiceText(format.channels[Ioctl::kCodecAac], channels), preference.aacBitrate);
	Append(text, length,
		   "[aptx]\r\n"
		   "rate = %s\r\n"
		   "channels = %s\r\n"
		   "\r\n",
		   ChoiceText(format.sampleRate[Ioctl::kCodecAptx], rate),
		   ChoiceText(format.channels[Ioctl::kCodecAptx], channels));
	Append(text, length,
		   "[aptxhd]\r\n"
		   "rate = %s\r\n"
		   "channels = %s\r\n"
		   "\r\n",
		   ChoiceText(format.sampleRate[Ioctl::kCodecAptxHd], rate),
		   ChoiceText(format.channels[Ioctl::kCodecAptxHd], channels));
	Append(text, length,
		   "[ldac]\r\n"
		   "rate = %s\r\n"
		   "channels = %s\r\n"
		   "# 0 = high (990 kbps), 1 = standard (660), 2 = mobile (330).\r\n"
		   "quality = %u\r\n",
		   ChoiceText(format.sampleRate[Ioctl::kCodecLdac], rate),
		   ChoiceText(format.channels[Ioctl::kCodecLdac], channels), preference.ldacQuality);
	if (length == 0)
	{
		OA2DP_WARN(ctx, "Config", "cannot format the settings for %ws", path);
		return;
	}

	EnsureDirectory(kDirectory);
	EnsureDirectory(kDevicesDirectory);
	HANDLE handle = nullptr;
	const NTSTATUS status = OpenFile(path, FILE_WRITE_DATA, FILE_OVERWRITE_IF, handle);
	if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Config", "cannot open %ws 0x%08X; the settings are kept in memory only", path, status);
		return;
	}
	IO_STATUS_BLOCK statusBlock = {};
	const NTSTATUS written =
			ZwWriteFile(handle, nullptr, nullptr, nullptr, &statusBlock, text, static_cast<ULONG>(length), nullptr, nullptr);
	ZwClose(handle);
	if (!NT_SUCCESS(written) || statusBlock.Information != length)
	{
		OA2DP_WARN(ctx, "Config", "cannot write %ws 0x%08X", path, written);
	}
}

// The text of a settings file, or false when there is none: no file is "no settings", not an
// error, so nothing is logged for it.
bool ReadSettingsFile(DEVICE_CONTEXT* ctx, const wchar_t* path, char (&text)[kMaxFileBytes], size_t& length)
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
	if (!NT_SUCCESS(status) || bytes <= 0 || bytes >= static_cast<LONGLONG>(kMaxFileBytes))
	{
		ZwClose(handle);
		OA2DP_WARN(ctx, "Config", "ignoring %ws: 0x%08X reading its length", path, status);
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

// True when nothing differs from the driver default: every codec automatic and every parameter
// zero. Such a sink has nothing to store, so it gets no file and deleting one is the same as
// restoring the defaults.
bool SameDefaults(const Settings& settings)
{
	const Ioctl::Preference& preference = settings.preference;
	if ((preference.flags & Ioctl::kPreferenceForceCodec) != 0 || preference.aacBitrate != 0 ||
		preference.sbcBitpool != 0 || preference.sbcBlocks != 0 || preference.sbcSubbands != 0 ||
		preference.sbcAllocation != 0 || preference.prerollMs != 0 ||
		preference.sbcFramesPerPacket != 0 || preference.ldacQuality != 0)
	{
		return false;
	}
	for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
	{
		if (settings.endpointFormat.sampleRate[codec] != 0 || settings.endpointFormat.channels[codec] != 0)
		{
			return false;
		}
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
} // namespace

void ConfigLoadSettings(_In_ DEVICE_CONTEXT* ctx)
{
	Settings settings = {};
	settings.preference.abiVersion = Ioctl::kAbiVersion;
	settings.endpointFormat.abiVersion = Ioctl::kAbiVersion;

	// The sink's own file wins over the machine default; a field the user never set is zero in
	// both, which is the driver default.
	char text[kMaxFileBytes] = {};
	size_t length = 0;
	wchar_t sinkPath[96] = {};
	const bool sinkFile = SinkPath(ctx, sinkPath) && ReadSettingsFile(ctx, sinkPath, text, length) &&
						  ParseSettings(text, length, settings);
	bool stored = sinkFile;
	if (!sinkFile)
	{
		settings = {};
		stored = ReadSettingsFile(ctx, kDefaultPath, text, length) && ParseSettings(text, length, settings);
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

	Settings settings = {};
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
	if (SameDefaults(settings))
	{
		// A sink nobody changed keeps no file: the defaults are the absence of settings, so the
		// file is removed instead of being filled with them.
		DeleteSettingsFile(ctx, sinkPath);
		return;
	}
	WriteSettings(ctx, sinkPath, settings);
	OA2DP_INFO(ctx, "Config",
			   "settings stored in %ws: codec preference %s (SBC bitpool %u, blocks %u, subbands %u, "
			   "allocation 0x%02X, AAC bitrate %u, preroll %u ms, SBC frames per packet %u, LDAC quality %u)",
			   sinkPath, CodecValueName(settings.preference.codec), settings.preference.sbcBitpool,
			   settings.preference.sbcBlocks, settings.preference.sbcSubbands, settings.preference.sbcAllocation,
			   settings.preference.aacBitrate, settings.preference.prerollMs,
			   settings.preference.sbcFramesPerPacket, settings.preference.ldacQuality);
}
