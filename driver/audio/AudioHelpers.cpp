#include "AudioInternal.h"

namespace OpenA2dpDriver::Audio
{
// Endpoint type (shown as "<type> (<device name>)") from the sink's Class of Device.
const GUID* EndpointCategory(_In_ BTH_COD cod)
{
	if (GET_COD_MAJOR(cod) == COD_MAJOR_AUDIO)
	{
		switch (GET_COD_MINOR(cod))
		{
		case COD_AUDIO_MINOR_HEADSET:
		case COD_AUDIO_MINOR_HANDS_FREE:
		case COD_AUDIO_MINOR_HEADSET_HANDS_FREE:
		case COD_AUDIO_MINOR_HEADPHONES:
			return &KSNODETYPE_HEADPHONES;
		default:
			break;
		}
	}
	return &KSNODETYPE_SPEAKER;
}

RENDER_FORMAT RenderFormats[AudioRenderFormatCount];
} // namespace OpenA2dpDriver::Audio

// Declared in Driver.h with the rest of the driver-wide audio interface, so it lives
// outside the subsystem namespace like AudioInitialize.
using namespace OpenA2dpDriver::Audio;

// One PCM format per render rate, channel count and depth; everything else is identical.
// The 24-bit entries carry 24 valid bits in a 32-bit container, left-justified the way
// Windows describes 24-bit audio, so a 24-bit sample keeps its bytes in the ring. Called
// once from DriverEntry: every endpoint lists this table, so it must not change once a
// device has started.
void BuildRenderFormats()
{
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		RENDER_FORMAT& format = RenderFormats[i];
		const ULONG rate = AudioRenderRateAt(i);
		const USHORT channels = AudioRenderChannelsAt(i);
		const USHORT validBits = AudioRenderValidBitsAt(i);
		const USHORT containerBits = AudioRenderSampleBytes(validBits) * 8;
		const USHORT bytesPerFrame = static_cast<USHORT>(channels * AudioRenderSampleBytes(validBits));
		RtlZeroMemory(&format, sizeof(format));
		format.DataFormat.FormatSize = sizeof(RENDER_FORMAT);
		format.DataFormat.SampleSize = bytesPerFrame;
		format.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
		format.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
		format.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

		WAVEFORMATEX& wave = format.WaveFormatExt.Format;
		wave.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wave.nChannels = channels;
		wave.nSamplesPerSec = rate;
		wave.wBitsPerSample = containerBits;
		wave.nBlockAlign = bytesPerFrame;
		wave.nAvgBytesPerSec = rate * bytesPerFrame;
		wave.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		format.WaveFormatExt.Samples.wValidBitsPerSample = validBits;
		format.WaveFormatExt.dwChannelMask = channels == 1 ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
		format.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
	}
}
