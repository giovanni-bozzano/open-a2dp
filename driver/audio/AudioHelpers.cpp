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

// One 16-bit PCM format per render rate and channel count; everything else is identical.
void BuildRenderFormats()
{
	for (ULONG i = 0; i < AudioRenderFormatCount; ++i)
	{
		RENDER_FORMAT& format = RenderFormats[i];
		const ULONG rate = AudioRenderRates[i / AudioRenderChannelCount];
		const USHORT channels = AudioRenderChannels[i % AudioRenderChannelCount];
		const USHORT bytesPerFrame = static_cast<USHORT>(channels * sizeof(INT16));
		RtlZeroMemory(&format, sizeof(format));
		format.DataFormat.FormatSize = sizeof(RENDER_FORMAT);
		format.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
		format.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
		format.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

		WAVEFORMATEX& wave = format.WaveFormatExt.Format;
		wave.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wave.nChannels = channels;
		wave.nSamplesPerSec = rate;
		wave.wBitsPerSample = 16;
		wave.nBlockAlign = bytesPerFrame;
		wave.nAvgBytesPerSec = rate * bytesPerFrame;
		wave.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		format.WaveFormatExt.Samples.wValidBitsPerSample = 16;
		format.WaveFormatExt.dwChannelMask = channels == 1 ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
		format.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
	}
}
} // namespace OpenA2dpDriver::Audio
