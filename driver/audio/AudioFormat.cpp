#include "AudioInternal.h"

#include "../Service.h"

#include <ntstrsafe.h>

#include <OpenA2dp/SbcConfig.h>

namespace OpenA2dpDriver::Audio
{
NTSTATUS OnRetrieveJackPresence(_In_ ACXJACK jack, _Out_ PBOOLEAN isConnected)
{
	const DEVICE_CONTEXT* ctx = GetSinkJackContext(jack)->Device;
	// A format change unplugs the jack briefly (AudioRestartRenderStream) even though the link
	// is up: Windows only re-reads the endpoint's formats when the endpoint is rebuilt.
	*isConnected = ctx->SignalingState == ChannelOpen && !ctx->JackFlap ? TRUE : FALSE;
	return STATUS_SUCCESS;
}

// The jack is what Windows uses to show the endpoint as plugged or unplugged. The
// endpoint itself lives as long as the device, so without this it stays listed as
// available after the sink disconnects.
NTSTATUS AddSinkJack(_In_ DEVICE_CONTEXT* ctx, _In_ ACXPIN pin)
{
	ACX_JACK_CALLBACKS callbacks;
	ACX_JACK_CALLBACKS_INIT(&callbacks);
	callbacks.EvtAcxJackRetrievePresenceState = OnRetrieveJackPresence;

	ACX_JACK_CONFIG config;
	ACX_JACK_CONFIG_INIT(&config);
	config.Flags = AcxJackConfigJackDetection;
	config.Callbacks = &callbacks;
	config.Description.ChannelMapping = KSAUDIO_SPEAKER_STEREO;
	config.Description.ConnectionType = AcxConnTypeUnknown;
	config.Description.GeoLocation = AcxGeoLocNotApplicable;
	config.Description.GenLocation = AcxGenLocSeparate;
	config.Description.PortConnection = AcxPortConnJack;

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, SINK_JACK_CONTEXT);
	attrs.ParentObject = pin;
	NTSTATUS status = AcxJackCreate(pin, &attrs, &config, &ctx->SinkJack);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "AcxJackCreate failed 0x%08X", status);
		return status;
	}
	GetSinkJackContext(ctx->SinkJack)->Device = ctx;

	status = AcxPinAddJacks(pin, &ctx->SinkJack, 1);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "AcxPinAddJacks failed 0x%08X", status);
	}
	return status;
}

namespace
{
// The encoder follows the actual stream, not the Windows mix or advertised default.
void RecordEndpointFormat(_In_ DEVICE_CONTEXT* ctx, _In_ ULONG sampleRate, _In_ USHORT channels,
	_In_ USHORT validBits)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const ULONG previousRate = ctx->PinSampleRate;
	const USHORT previousChannels = ctx->PinChannels;
	const USHORT previousValidBits = ctx->PinValidBits;
	ctx->PinSampleRate = sampleRate;
	ctx->PinChannels = channels;
	ctx->PinValidBits = validBits;
	ctx->PinBytesPerFrame = static_cast<USHORT>(channels * AudioRenderSampleBytes(validBits));
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	if (sampleRate != previousRate || channels != previousChannels || validBits != previousValidBits)
	{
		SignalingOnStreamFormatChanged(ctx);
	}
}
// Index in RenderFormats of a rate, channel count and depth, or AudioRenderFormatCount when
// the triple is not one of the endpoint formats.
ULONG RenderFormatIndex(ULONG sampleRate, USHORT channels, USHORT validBits)
{
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		if (sampleRate == AudioRenderRateAt(i) && channels == AudioRenderChannelsAt(i) &&
			validBits == AudioRenderValidBitsAt(i))
		{
			return i;
		}
	}
	return AudioRenderFormatCount;
}

// Removes and re-adds the listed formats so startIndex comes first. Windows opens the
// shared-mode stream with the first listed format, so the selection has to lead both lists;
// the assigned default alone does not reach the engine. The caller holds FormatLock.
NTSTATUS RebuildListing(_In_ DEVICE_CONTEXT* ctx, ULONG desired, ULONG startIndex)
{
	ACXDATAFORMATLIST list =
		ctx->StreamingPin != nullptr ? AcxPinGetRawDataFormatList(ctx->StreamingPin) : nullptr;
	const ACXDATAFORMATLIST defaultList = ctx->DefaultModeFormats;
	if (list == nullptr || defaultList == nullptr)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	NTSTATUS status = STATUS_SUCCESS;
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		if ((ctx->FormatsListed & (1u << i)) == 0)
		{
			continue;
		}
		NTSTATUS result = AcxDataFormatListRemoveDataFormat(list, ctx->FormatObjects[i]);
		if (NT_SUCCESS(result))
		{
			result = AcxDataFormatListRemoveDataFormat(defaultList, ctx->FormatObjects[i]);
		}
		if (!NT_SUCCESS(result))
		{
			OA2DP_ERROR(ctx, "Audio", "removing render format %u failed 0x%08X", i, result);
			if (NT_SUCCESS(status))
			{
				status = result;
			}
		}
	}
	// Built aside and published whole below: readers outside FormatLock keep seeing the
	// previous set rather than the empty one this rebuild passes through.
	ULONG listed = 0;
	for (ULONG n = 0; n < AudioRenderFormatCount; ++n)
	{
		const ULONG i = (startIndex + n) % AudioRenderFormatCount;
		if ((desired & (1u << i)) == 0)
		{
			continue;
		}
		NTSTATUS result = AcxDataFormatListAddDataFormat(list, ctx->FormatObjects[i]);
		if (NT_SUCCESS(result))
		{
			result = AcxDataFormatListAddDataFormat(defaultList, ctx->FormatObjects[i]);
		}
		if (!NT_SUCCESS(result))
		{
			OA2DP_ERROR(ctx, "Audio", "adding render format %u failed 0x%08X", i, result);
			if (NT_SUCCESS(status))
			{
				status = result;
			}
			continue;
		}
		listed |= 1u << i;
	}
	ctx->FormatsListed = listed;
	return status;
}

} // namespace

USHORT PcmValidBits(const KSDATAFORMAT_WAVEFORMATEX& wave)
{
	const WAVEFORMATEX& pcm = wave.WaveFormatEx;
	if (pcm.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
		pcm.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
		wave.DataFormat.FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE))
	{
		return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&pcm)->Samples.wValidBitsPerSample;
	}
	return pcm.wBitsPerSample;
}

NTSTATUS AudioValidatePinFormat(_In_ DEVICE_CONTEXT* ctx, _In_ ACXDATAFORMAT format)
{
	if (format == nullptr)
	{
		return STATUS_INVALID_PARAMETER;
	}
	const auto* wave = static_cast<const KSDATAFORMAT_WAVEFORMATEX*>(AcxDataFormatGetKsDataFormat(format));
	if (wave == nullptr ||
		wave->DataFormat.FormatSize < FIELD_OFFSET(KSDATAFORMAT_WAVEFORMATEX, WaveFormatEx) + sizeof(WAVEFORMATEX) ||
		!IsEqualGUID(wave->DataFormat.Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
	{
		OA2DP_WARN(ctx, "Audio", "stream format missing or not WAVEFORMATEX; refusing it");
		return STATUS_NOT_SUPPORTED;
	}
	NTSTATUS status = STATUS_NOT_SUPPORTED;
	const WAVEFORMATEX& pcm = wave->WaveFormatEx;
	const bool extensible = pcm.wFormatTag == WAVE_FORMAT_EXTENSIBLE;
	const USHORT validBits = PcmValidBits(*wave);
	const USHORT sampleBytes = AudioRenderSampleBytes(validBits);
	// PCM only, in the depths the endpoint lists: 16-bit samples, and 24 bits in a 32-bit
	// container. A plain WAVEFORMATEX has no valid-bits field, so it cannot describe 24-bit.
	bool pcmFormat = IsEqualGUID(wave->DataFormat.MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
		IsEqualGUID(wave->DataFormat.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
		(validBits == 16 || (extensible && validBits == 24)) &&
		pcm.wBitsPerSample == sampleBytes * 8 && pcm.nBlockAlign == pcm.nChannels * sampleBytes &&
		pcm.nAvgBytesPerSec == pcm.nSamplesPerSec * pcm.nBlockAlign;
	if (extensible)
	{
		pcmFormat = pcmFormat && pcm.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
			wave->DataFormat.FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);
		if (pcmFormat)
		{
			const auto& extended = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&pcm);
			pcmFormat = IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
				extended.Samples.wValidBitsPerSample == validBits &&
				(extended.dwChannelMask == 0 || extended.dwChannelMask ==
					static_cast<ULONG>(pcm.nChannels == 1 ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO));
		}
	}
	else
	{
		pcmFormat = pcmFormat && pcm.wFormatTag == WAVE_FORMAT_PCM;
	}
	// Offered, not merely supported: a pinned endpoint format is offered alone, and a stream at
	// any other format would move the pin away from the setting Windows kept using. The listing
	// is read as one published set, and it already narrows the sink's formats to the setting.
	const ULONG listed = ctx->FormatsListed;
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		// The depth is part of the check, because 24-in-32 follows aptX HD rather than SBC.
		if (pcmFormat && (listed & (1u << i)) != 0 &&
			pcm.nSamplesPerSec == AudioRenderRateAt(i) && pcm.nChannels == AudioRenderChannelsAt(i) &&
			validBits == AudioRenderValidBitsAt(i))
		{
			status = STATUS_SUCCESS;
			break;
		}
	}
	if (!NT_SUCCESS(status))
	{
		// A refused stream is a silent player: Windows fails the stream it asked for and no
		// error reaches the user, so the request is kept for the status report.
		InterlockedIncrement(&ctx->RefusedStreams);
		InterlockedExchange64(&ctx->RefusedFormat,
			static_cast<LONG64>((static_cast<ULONGLONG>(pcm.nSamplesPerSec) << 32) |
				(static_cast<ULONGLONG>(pcm.nChannels) << 16) | validBits));
		OA2DP_WARN(ctx, "Audio",
			"stream format %u Hz %u ch %u-bit is not one the endpoint lists; refusing it",
			pcm.nSamplesPerSec, pcm.nChannels, validBits);
	}
	return status;
}

// Called when a stream starts running, not when it is created: Windows creates a stream per
// format it enumerates, so only a stream that actually carries audio may commit the endpoint
// format. Every committed change invalidates the negotiated configuration, and committing the
// probes tore the session down once per probe until the shared-mode stream ended up at the
// first listed format again.
void AudioSetPinFormat(_In_ DEVICE_CONTEXT* ctx, ULONG sampleRate, USHORT channels, USHORT validBits)
{
	OA2DP_INFO(ctx, "Audio", "endpoint format %u Hz %u ch %u-bit (actual stream)", sampleRate,
		static_cast<unsigned>(channels), static_cast<unsigned>(validBits));
	RecordEndpointFormat(ctx, sampleRate, channels, validBits);
}

namespace
{
// The stored preference, read without holding AudioLock: the format list depends on it.
Ioctl::Preference StoredPreference(_In_ DEVICE_CONTEXT* ctx)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
	const Ioctl::Preference preference = ctx->Preference;
	KeReleaseSpinLock(&ctx->PreferenceLock, irql);
	return preference;
}

// True while the 24-in-32 formats can be offered: an aptX HD provider is registered, this
// connection's discovery found the codec, and the user has not pinned another codec.
bool HdFormatsAvailable(_In_ DEVICE_CONTEXT* ctx)
{
	const LONG sinkCodecs = InterlockedCompareExchange(&ctx->SinkCodecs, 0, 0);
	if ((sinkCodecs & static_cast<LONG>(Ioctl::kSinkCodecAptxHd)) == 0 ||
		!ServiceCodecRegistered(ctx, Ioctl::kCodecAptxHd))
	{
		return false;
	}
	const Ioctl::Preference preference = StoredPreference(ctx);
	return (preference.flags & Ioctl::kPreferenceForceCodec) == 0 ||
		   preference.codec == Ioctl::kCodecAptxHd;
}

// Index of the first listed format, used as the endpoint default.
ULONG FirstListed(_In_ ULONG listed)
{
	ULONG index = 0;
	while (index < AudioRenderFormatCount && (listed & (1u << index)) == 0)
	{
		++index;
	}
	return index < AudioRenderFormatCount ? index : 0;
}

// Commits the format a stream would run at now to PinSampleRate/PinChannels while no stream
// is open, and reports whether it moved. A running stream sets the pin itself when it starts
// (AudioSetPinFormat), so this only fills the gap between one stream and the next - which is
// where a format change leaves the endpoint.
bool CommitPinDefault(_In_ DEVICE_CONTEXT* ctx, _In_ ULONG index)
{
	const ULONG sampleRate = AudioRenderRateAt(index);
	const USHORT channels = AudioRenderChannelsAt(index);
	const USHORT validBits = AudioRenderValidBitsAt(index);
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const bool changed = ctx->AudioStream == nullptr &&
		(ctx->PinSampleRate != sampleRate || ctx->PinChannels != channels ||
			ctx->PinValidBits != validBits);
	if (changed)
	{
		ctx->PinSampleRate = sampleRate;
		ctx->PinChannels = channels;
		ctx->PinValidBits = validBits;
		ctx->PinBytesPerFrame = static_cast<USHORT>(channels * AudioRenderSampleBytes(validBits));
	}
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return changed;
}

// 16-bit entries follow the sink's SBC formats or, where SBC cannot play them, its classic
// aptX or LDAC formats; 24-in-32 entries follow aptX HD, which reads the depth, or LDAC,
// which narrows it. The high SBC rates (16 and 32 kHz) and LDAC's 88.2/96 kHz are only listed
// while the codec that owns them is advertised, because no other codec here can play them.
// Forcing a codec removes the entries it cannot play when it can actually run - a forced codec
// the endpoint cannot serve must not leave it with no format at all, so the fallback list
// stays in that case.
ULONG DesiredFormats(_In_ DEVICE_CONTEXT* ctx)
{
	const uint8_t sinkFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0));
	const uint8_t hdFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkHdCapabilities, 0, 0));
	const uint8_t aptxFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkAptxCapabilities, 0, 0));
	// LDAC's advertisement packs the rate mask in the high byte and the channel modes in the low
	// one; only the rate mask decides formats here, because LDAC runs stereo only.
	const uint8_t ldacRates =
			static_cast<uint8_t>((InterlockedCompareExchange(&ctx->SinkLdacCapabilities, 0, 0) >> 8) & 0xFF);
	const LONG sinkCodecs = InterlockedCompareExchange(&ctx->SinkCodecs, 0, 0);
	const bool hdAvailable = HdFormatsAvailable(ctx);
	// Classic aptX needs a registered provider and this connection's advertisement; its 16-bit
	// formats follow it just like SBC's, because it is another way to play them.
	const bool aptxAvailable = ServiceCodecRegistered(ctx, Ioctl::kCodecAptx) &&
		(sinkCodecs & static_cast<LONG>(Ioctl::kSinkCodecAptx)) != 0;
	const Ioctl::Preference preference = StoredPreference(ctx);
	const bool forcedHd = (preference.flags & Ioctl::kPreferenceForceCodec) != 0 &&
		preference.codec == Ioctl::kCodecAptxHd;
	const bool forcedAptx = (preference.flags & Ioctl::kPreferenceForceCodec) != 0 &&
		preference.codec == Ioctl::kCodecAptx;
	// LDAC plays 88.2 and 96 kHz as well, so its advertisement is what lists those formats, and
	// it narrows a 24-in-32 stream the way classic aptX does.
	const bool ldacAvailable = ServiceCodecRegistered(ctx, Ioctl::kCodecLdac) &&
		(sinkCodecs & static_cast<LONG>(Ioctl::kSinkCodecLdac)) != 0;
	const bool forcedLdac = (preference.flags & Ioctl::kPreferenceForceCodec) != 0 &&
		preference.codec == Ioctl::kCodecLdac;

	ULONG supported = 0;
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		const ULONG sampleRate = AudioRenderRateAt(i);
		const USHORT channels = AudioRenderChannelsAt(i);
		const bool stereo = channels == 2;
		// The two vendor IEs carry different rate sets: aptX and aptX HD only 44.1 and 48 kHz,
		// LDAC 44.1, 48, 88.2 and 96.
		const uint8_t aptxBit = sampleRate == 44100 ? VendorAptx::kSampleRate44100
			: sampleRate == 48000 ? VendorAptx::kSampleRate48000 : 0;
		const uint8_t ldacBit = Ldac::RateBit(sampleRate);
		const bool aptxOk = stereo && aptxBit != 0 && aptxAvailable && (aptxFormats & aptxBit) != 0;
		const bool ldacOk = stereo && ldacBit != 0 && ldacAvailable && (ldacRates & ldacBit) != 0;
		bool keep = false;
		if (AudioRenderValidBitsAt(i) == AptxHd::kBitsPerSample)
		{
			// 24-in-32 is listed while an encoder that reads it - or narrows it - can run: HD
			// takes it as it is, classic aptX and LDAC narrow it per sample.
			const bool hdOk = stereo && aptxBit != 0 && hdAvailable && (hdFormats & aptxBit) != 0;
			if (forcedHd && hdAvailable)
			{
				keep = hdOk;
			}
			else if (forcedAptx && aptxAvailable)
			{
				keep = aptxOk;
			}
			else if (forcedLdac && ldacAvailable)
			{
				keep = ldacOk;
			}
			else
			{
				keep = hdOk || ldacOk;
			}
		}
		else if (forcedHd && hdAvailable)
		{
			// The stream will run HD, so only its own 24-bit formats stay listed.
			keep = false;
		}
		else if (forcedAptx && aptxAvailable)
		{
			// The stream will run classic aptX, so only the formats it can play stay listed.
			keep = aptxOk;
		}
		else if (forcedLdac && ldacAvailable)
		{
			// The stream will run LDAC, so the endpoint offers what LDAC can carry.
			keep = ldacOk;
		}
		else
		{
			// No sink capabilities yet means a provisional list, which is narrowed again once
			// discovery reports the sink's formats.
			const bool sbcOk = sinkFormats == 0 ||
				SbcConfig::SupportsFormat(sinkFormats, sampleRate, static_cast<uint8_t>(channels));
			keep = sbcOk || aptxOk || ldacOk;
		}
		if (keep)
		{
			supported |= 1u << i;
		}
	}
	return supported;
}
} // namespace

NTSTATUS CreateRenderFormats(_In_ DEVICE_CONTEXT* ctx, _In_ ACXPIN pin)
{
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		ACX_DATAFORMAT_CONFIG config;
		ACX_DATAFORMAT_CONFIG_INIT_KS(&config, &RenderFormats[i]);
		WDF_OBJECT_ATTRIBUTES attrs;
		WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
		attrs.ParentObject = pin;
		const NTSTATUS status = AcxDataFormatCreate(ctx->Device, &attrs, &config, &ctx->FormatObjects[i]);
		if (!NT_SUCCESS(status))
		{
			OA2DP_ERROR(ctx, "Audio", "AcxDataFormatCreate failed 0x%08X", status);
			return status;
		}
	}

	// Sound settings' default format is the device format of the DEFAULT mode. A pin that
	// lists only RAW gives Windows no mode to apply it to, so it stays on the list default.
	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = pin;
	ACX_DATAFORMAT_LIST_CONFIG config;
	ACX_DATAFORMAT_LIST_CONFIG_INIT(&config);
	NTSTATUS status = AcxDataFormatListCreate(ctx->Device, &attrs, &config, &ctx->DefaultModeFormats);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "creating the DEFAULT mode format list failed 0x%08X", status);
		return status;
	}
	status = AcxPinAssignModeDataFormatList(pin, &AUDIO_SIGNALPROCESSINGMODE_DEFAULT, ctx->DefaultModeFormats);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Audio", "assigning the DEFAULT mode format list failed 0x%08X", status);
	}
	return status;
}

} // namespace OpenA2dpDriver::Audio

// Declared in Driver.h with the rest of the driver-wide audio interface, so it lives
// outside the subsystem namespace like AudioInitialize.
using namespace OpenA2dpDriver::Audio;

// The list is the sink's formats, narrowed to the selected one when the user pinned it.
NTSTATUS AudioApplyEndpointFormats(_In_ DEVICE_CONTEXT* ctx)
{
	const ACXPIN pin = ctx->StreamingPin;
	ACXDATAFORMATLIST list = pin != nullptr ? AcxPinGetRawDataFormatList(pin) : nullptr;
	const ACXDATAFORMATLIST defaultList = ctx->DefaultModeFormats;
	if (pin == nullptr || list == nullptr || defaultList == nullptr || ctx->FormatLock == nullptr)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	NTSTATUS status = STATUS_SUCCESS;
	WdfWaitLockAcquire(ctx->FormatLock, nullptr);
	const uint8_t sinkFormats = static_cast<uint8_t>(InterlockedCompareExchange(&ctx->SinkSbcCapabilities, 0, 0));
	ULONG desired = DesiredFormats(ctx);
	if (desired == 0)
	{
		WdfWaitLockRelease(ctx->FormatLock);
		OA2DP_ERROR(ctx, "Audio", "sink SBC formats 0x%02X have no driver-supported PCM format", sinkFormats);
		return STATUS_NOT_SUPPORTED;
	}
	// Windows opens the shared-mode stream with the first listed format and ignores the
	// Sound-settings pick, so a pinned format is offered alone: listing it first is enough
	// for the engine to use it but leaves the other formats selectable, which is how the pick
	// ends up ignored. With no selection every sink-supported format stays listed.
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const ULONG selectedRate = ctx->SelectedSampleRate;
	const USHORT selectedChannels = ctx->SelectedChannels;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	ULONG startIndex = FirstListed(desired);
	if (selectedRate != 0)
	{
		const USHORT listedDepth = HdFormatsAvailable(ctx) ? AptxHd::kBitsPerSample : 16;
		const ULONG index = RenderFormatIndex(selectedRate, selectedChannels, listedDepth);
		if (index < AudioRenderFormatCount && (desired & (1u << index)) != 0)
		{
			desired = 1u << index;
			startIndex = index;
		}
		else
		{
			OA2DP_WARN(ctx, "Audio",
				"the selected endpoint format %u Hz %u ch is not one the sink supports; offering "
				"every supported format instead",
				selectedRate, static_cast<unsigned>(selectedChannels));
		}
	}

	const ULONG listedBefore = ctx->FormatsListed;
	const bool firstBuild = listedBefore == 0;
	if (listedBefore == desired && ctx->FirstListedIndex == startIndex)
	{
		// The list already says this; the pin still follows it while no stream runs, so the
		// stream Windows opens next takes the format this endpoint now offers.
		const bool pinChanged = CommitPinDefault(ctx, startIndex);
		WdfWaitLockRelease(ctx->FormatLock);
		if (pinChanged)
		{
			SignalingOnStreamFormatChanged(ctx);
		}
		return STATUS_SUCCESS;
	}
	status = RebuildListing(ctx, desired, startIndex);
	ctx->FirstListedIndex = startIndex;
	const ULONG listed = ctx->FormatsListed;
	if (listed == 0)
	{
		OA2DP_ERROR(ctx, "Audio", "no render format is listed; the endpoint cannot be opened");
		WdfWaitLockRelease(ctx->FormatLock);
		return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
	}

	// The leading format is the advertised default too, so ACX's own answers agree with it.
	const ULONG defaultIndex = startIndex;
	NTSTATUS defaultStatus = AcxDataFormatListAssignDefaultDataFormat(list, ctx->FormatObjects[defaultIndex]);
	if (NT_SUCCESS(defaultStatus))
	{
		defaultStatus = AcxDataFormatListAssignDefaultDataFormat(defaultList, ctx->FormatObjects[defaultIndex]);
	}
	const bool pinChanged = NT_SUCCESS(defaultStatus) && CommitPinDefault(ctx, defaultIndex);
	WdfWaitLockRelease(ctx->FormatLock);
	// The first build runs before SignalingInitialize created the work item, and there is
	// nothing to renegotiate before a client exists.
	if (pinChanged && !firstBuild)
	{
		SignalingOnStreamFormatChanged(ctx);
	}
	// Editing the lists entry by entry does not raise the pin's format-change event, and
	// without it Windows keeps a running stream at the format it was opened with. The event
	// makes the audio engine re-query the endpoint and rebuild the shared-mode stream at the
	// default this list now leads with. The engine answers the event by running a stream even
	// when nothing plays, which is why the AVDTP start follows the audio and not the run edge.
	// The first build happens while the circuit is still being created, when there is no
	// client to notify, and completing it from there would re-enter ACX.
	if (!firstBuild)
	{
		const NTSTATUS notifyStatus = AcxPinNotifyDataFormatChange(pin);
		if (!NT_SUCCESS(notifyStatus))
		{
			OA2DP_WARN(ctx, "Audio", "the endpoint format change notification failed 0x%08X", notifyStatus);
		}
	}
	if (!NT_SUCCESS(defaultStatus))
	{
		if (firstBuild)
		{
			OA2DP_INFO(ctx, "Audio", "the endpoint default format was not assigned (0x%08X)", defaultStatus);
		}
		else
		{
			OA2DP_WARN(ctx, "Audio", "assigning the endpoint default format failed 0x%08X", defaultStatus);
		}
	}

	char formats[192];
	formats[0] = '\0';
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		if ((listed & (1u << i)) == 0)
		{
			continue;
		}
		char entry[28];
		(void)RtlStringCchPrintfA(entry, ARRAYSIZE(entry), "%s%u Hz %u ch %u-bit",
			formats[0] != '\0' ? ", " : "", AudioRenderRateAt(i),
			static_cast<unsigned>(AudioRenderChannelsAt(i)),
			static_cast<unsigned>(AudioRenderValidBitsAt(i)));
		(void)RtlStringCchCatA(formats, ARRAYSIZE(formats), entry);
	}
	OA2DP_INFO(ctx, "Audio", "endpoint formats %s (PCM, RAW and DEFAULT modes, %s)", formats,
		sinkFormats == 0 && InterlockedCompareExchange(&ctx->SinkHdCapabilities, 0, 0) == 0 &&
			InterlockedCompareExchange(&ctx->SinkAptxCapabilities, 0, 0) == 0
			? "provisional; sink not discovered"
			: "sink capabilities");
	return NT_SUCCESS(status) ? defaultStatus : status;
}

bool AudioSinkSupportsEndpointFormat(_In_ DEVICE_CONTEXT* ctx, ULONG sampleRate, USHORT channels)
{
	const ULONG desired = DesiredFormats(ctx);
	const ULONG bits16 = RenderFormatIndex(sampleRate, channels, 16);
	const ULONG bits24 = RenderFormatIndex(sampleRate, channels, AptxHd::kBitsPerSample);
	return (bits16 < AudioRenderFormatCount && (desired & (1u << bits16)) != 0) ||
		   (bits24 < AudioRenderFormatCount && (desired & (1u << bits24)) != 0);
}
