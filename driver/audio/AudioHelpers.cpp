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

RENDER_FORMAT RenderFormat = {
	{
		sizeof(RENDER_FORMAT),
		0,
		0,
		0,
		STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
		STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
		STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX),
	},
	{
		{
			WAVE_FORMAT_EXTENSIBLE,
			AudioChannels,
			AudioSampleRate,
			AudioSampleRate * AudioBytesPerFrame,
			AudioBytesPerFrame,
			16,
			sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX),
		},
		{16},
		KSAUDIO_SPEAKER_STEREO,
		STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
	},
};
} // namespace OpenA2dpDriver::Audio
