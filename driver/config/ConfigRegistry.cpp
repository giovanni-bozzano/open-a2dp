#include "../Driver.h"

#include <ntstrsafe.h>

using namespace OpenA2dp;

// Stored codec configuration, under the driver's Parameters key:
//   Parameters                          global default
//   Parameters\Devices\<12 hex address> per-sink override
// The sink-specific values win over the global ones, so one sink can be pinned to SBC
// without changing the others. Both functions run at PASSIVE_LEVEL: the driver loads
// when the sink address is known and stores from the SET_PREFERENCE IOCTL.
namespace
{
const wchar_t kDevicesKeyName[] = L"Devices";
const wchar_t kFlagsValueName[] = L"CodecPreferenceFlags";
const wchar_t kCodecValueName[] = L"CodecPreferenceCodec";
const wchar_t kAacBitrateValueName[] = L"CodecPreferenceAacBitrate";
const wchar_t kAacSampleRateValueName[] = L"CodecPreferenceAacSampleRate";
const wchar_t kSbcBitpoolValueName[] = L"CodecPreferenceSbcBitpool";
const wchar_t kSbcBlocksValueName[] = L"CodecPreferenceSbcBlocks";
const wchar_t kSbcSubbandsValueName[] = L"CodecPreferenceSbcSubbands";
const wchar_t kSbcAllocationValueName[] = L"CodecPreferenceSbcAllocation";

const char* PreferenceName(const Ioctl::Preference& preference)
{
	if ((preference.flags & Ioctl::kPreferenceForceCodec) == 0)
	{
		return "auto";
	}
	return preference.codec == Ioctl::kCodecSbc ? "SBC" : "AAC";
}

bool OpenParametersKey(ACCESS_MASK access, WDFKEY* key)
{
	return NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), access, WDF_NO_OBJECT_ATTRIBUTES, key));
}

bool OpenSubkey(WDFKEY parent, const wchar_t* name, ACCESS_MASK access, WDFKEY* key)
{
	UNICODE_STRING keyName;
	RtlInitUnicodeString(&keyName, name);
	return NT_SUCCESS(WdfRegistryOpenKey(parent, &keyName, access, WDF_NO_OBJECT_ATTRIBUTES, key));
}

// Opens (creating if needed) a configuration subkey; a sink that never had one is not
// an error, so only a real failure is logged.
bool CreateSubkey(DEVICE_CONTEXT* ctx, WDFKEY parent, const wchar_t* name, WDFKEY* key)
{
	UNICODE_STRING keyName;
	RtlInitUnicodeString(&keyName, name);
	const NTSTATUS status = WdfRegistryCreateKey(parent, &keyName, KEY_READ | KEY_WRITE | KEY_CREATE_SUB_KEY,
		REG_OPTION_NON_VOLATILE, nullptr, WDF_NO_OBJECT_ATTRIBUTES, key);
	if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Config", "cannot open the %ws configuration key 0x%08X", name, status);
		return false;
	}
	return true;
}

bool ReadDword(WDFKEY key, const wchar_t* name, uint32_t& value)
{
	UNICODE_STRING valueName;
	RtlInitUnicodeString(&valueName, name);
	ULONG data = 0;
	ULONG length = sizeof(data);
	ULONG type = 0;
	if (!NT_SUCCESS(WdfRegistryQueryValue(key, &valueName, sizeof(data), &data, &length, &type)))
	{
		return false;
	}
	if (type != REG_DWORD || length != sizeof(data))
	{
		return false;
	}
	value = data;
	return true;
}

void WriteDword(DEVICE_CONTEXT* ctx, WDFKEY key, const wchar_t* name, uint32_t value)
{
	UNICODE_STRING valueName;
	RtlInitUnicodeString(&valueName, name);
	const NTSTATUS status = WdfRegistryAssignValue(key, &valueName, REG_DWORD, sizeof(value), &value);
	if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Config", "cannot store %ws 0x%08X", name, status);
	}
}

// Applies whichever values the key holds; returns true when it held any.
bool ReadPreference(WDFKEY key, Ioctl::Preference& preference)
{
	bool found = false;
	found |= ReadDword(key, kFlagsValueName, preference.flags);
	found |= ReadDword(key, kCodecValueName, preference.codec);
	found |= ReadDword(key, kAacBitrateValueName, preference.aacBitrate);
	found |= ReadDword(key, kAacSampleRateValueName, preference.aacSampleRate);
	found |= ReadDword(key, kSbcBitpoolValueName, preference.sbcBitpool);
	found |= ReadDword(key, kSbcBlocksValueName, preference.sbcBlocks);
	found |= ReadDword(key, kSbcSubbandsValueName, preference.sbcSubbands);
	found |= ReadDword(key, kSbcAllocationValueName, preference.sbcAllocation);
	return found;
}

// A hand-edited or stale store must not force a codec the driver cannot select.
bool ValidPreference(const Ioctl::Preference& preference)
{
	if ((preference.flags & ~Ioctl::kPreferenceForceCodec) != 0)
	{
		return false;
	}
	if (preference.sbcBitpool != 0 &&
		(preference.sbcBitpool < Ioctl::kSbcMinBitpool || preference.sbcBitpool > Ioctl::kSbcMaxBitpool))
	{
		return false;
	}
	if (!Ioctl::ValidSbcFrameParameters(preference.sbcBlocks, preference.sbcSubbands,
										preference.sbcAllocation))
	{
		return false;
	}
	if ((preference.flags & Ioctl::kPreferenceForceCodec) == 0)
	{
		return true;
	}
	return preference.codec == Ioctl::kCodecSbc || preference.codec == Ioctl::kCodecAac;
}

bool FormatSinkKeyName(DEVICE_CONTEXT* ctx, wchar_t (&name)[Ioctl::kControlAddressChars + 1])
{
	const NTSTATUS status =
		RtlStringCchPrintfW(name, ARRAYSIZE(name), L"%012I64X", ctx->RemoteAddress);
	return NT_SUCCESS(status);
}
} // namespace

void ConfigLoadPreference(_In_ DEVICE_CONTEXT* ctx)
{
	Ioctl::Preference preference = {};
	preference.abiVersion = Ioctl::kAbiVersion;
	bool stored = false;

	WDFKEY parameters = nullptr;
	if (OpenParametersKey(KEY_READ, &parameters))
	{
		stored = ReadPreference(parameters, preference);
		WDFKEY devices = nullptr;
		if (OpenSubkey(parameters, kDevicesKeyName, KEY_READ, &devices))
		{
			wchar_t sinkName[Ioctl::kControlAddressChars + 1] = {};
			WDFKEY sink = nullptr;
			if (FormatSinkKeyName(ctx, sinkName) && OpenSubkey(devices, sinkName, KEY_READ, &sink))
			{
				stored |= ReadPreference(sink, preference);
				WdfRegistryClose(sink);
			}
			WdfRegistryClose(devices);
		}
		WdfRegistryClose(parameters);
	}

	if (!ValidPreference(preference))
	{
		OA2DP_WARN(ctx, "Config", "ignoring the stored codec preference (flags 0x%08X, codec %u)",
			preference.flags, static_cast<unsigned>(preference.codec));
		preference = {};
		preference.abiVersion = Ioctl::kAbiVersion;
		stored = false;
	}

	KIRQL irql;
	KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
	ctx->Preference = preference;
	KeReleaseSpinLock(&ctx->PreferenceLock, irql);

	if (stored)
	{
		OA2DP_INFO(ctx, "Config",
			"codec preference %s restored from the registry (SBC bitpool %u, blocks %u, subbands %u, "
			"allocation 0x%02X, AAC bitrate %u)",
			PreferenceName(preference), preference.sbcBitpool, preference.sbcBlocks,
			preference.sbcSubbands, preference.sbcAllocation, preference.aacBitrate);
	}
}

void ConfigSavePreference(_In_ DEVICE_CONTEXT* ctx)
{
	Ioctl::Preference preference;
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		preference = ctx->Preference;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	}

	WDFKEY parameters = nullptr;
	if (!OpenParametersKey(KEY_READ | KEY_WRITE | KEY_CREATE_SUB_KEY, &parameters))
	{
		OA2DP_WARN(ctx, "Config", "cannot open the Parameters key; the preference is not stored");
		return;
	}

	WDFKEY devices = nullptr;
	if (CreateSubkey(ctx, parameters, kDevicesKeyName, &devices))
	{
		wchar_t sinkName[Ioctl::kControlAddressChars + 1] = {};
		WDFKEY sink = nullptr;
		if (FormatSinkKeyName(ctx, sinkName) && CreateSubkey(ctx, devices, sinkName, &sink))
		{
			WriteDword(ctx, sink, kFlagsValueName, preference.flags);
			WriteDword(ctx, sink, kCodecValueName, preference.codec);
			WriteDword(ctx, sink, kAacBitrateValueName, preference.aacBitrate);
			WriteDword(ctx, sink, kAacSampleRateValueName, preference.aacSampleRate);
			WriteDword(ctx, sink, kSbcBitpoolValueName, preference.sbcBitpool);
			WriteDword(ctx, sink, kSbcBlocksValueName, preference.sbcBlocks);
			WriteDword(ctx, sink, kSbcSubbandsValueName, preference.sbcSubbands);
			WriteDword(ctx, sink, kSbcAllocationValueName, preference.sbcAllocation);
			WdfRegistryClose(sink);
		}
		WdfRegistryClose(devices);
	}
	WdfRegistryClose(parameters);

	OA2DP_INFO(ctx, "Config",
		"codec preference %s stored for this sink (SBC bitpool %u, blocks %u, subbands %u, "
		"allocation 0x%02X, AAC bitrate %u)",
		PreferenceName(preference), preference.sbcBitpool, preference.sbcBlocks,
		preference.sbcSubbands, preference.sbcAllocation, preference.aacBitrate);
}
