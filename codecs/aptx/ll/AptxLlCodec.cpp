#include <OpenA2dp/AptxLlCodec.h>

#include <OpenA2dp/Aptx.h>
#include <OpenA2dp/AptxCodec.h>

#include <cstring>

namespace OpenA2dp {

using namespace AptxLl;

size_t AptxLlCodec::BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept
{
	if (ie == nullptr || capacity < kIeLength)
	{
		return 0;
	}
	WriteIe(static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000), ie);
	return kIeLength;
}

bool AptxLlCodec::SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
									  const CodecPreference& preference,
									  A2dpCodecConfig& out) const noexcept
{
	uint8_t capabilities = 0;
	if (!ParseIe(remoteIe, remoteIeLength, capabilities) || (capabilities & kChannelStereo) == 0)
	{
		return false;
	}
	const uint8_t rates = static_cast<uint8_t>(capabilities & kSampleRateMask);
	uint8_t rate = 0;
	if (preference.preferredSampleRate == 44100 && (rates & kSampleRate44100) != 0)
	{
		rate = kSampleRate44100;
	}
	else if (preference.preferredSampleRate == 48000 && (rates & kSampleRate48000) != 0)
	{
		rate = kSampleRate48000;
	}
	else if ((rates & kSampleRate48000) != 0)
	{
		rate = kSampleRate48000;
	}
	else if ((rates & kSampleRate44100) != 0)
	{
		rate = kSampleRate44100;
	}
	else
	{
		return false;
	}
	out = {};
	out.codec = CodecType::Vendor;
	WriteIe(rate, out.ie);
	out.ieLength = static_cast<uint8_t>(kIeLength);
	return true;
}

uint8_t AptxLlCodec::ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept
{
	return ValidateIe(ie, ieLength);
}

HRESULT AptxLlCodec::CreateEncoder(const A2dpCodecConfig& config,
								   std::unique_ptr<IA2dpEncoder>& encoder) const
{
	if (config.codec != CodecType::Vendor || ValidateConfiguration(config.ie, config.ieLength) != 0)
	{
		return E_INVALIDARG;
	}
	// The wire format is classic aptX: same four samples per four-byte codeword. Only the
	// vendor identity and the buffer levels differ, so the vendored classic encoder serves
	// this codec too and the adapter is built with a classic IE it understands.
	const uint8_t rate = static_cast<uint8_t>(config.ie[6] & kSampleRateMask);
	uint8_t classicIe[Aptx::kIeLength] = {};
	Aptx::WriteIe(rate, classicIe);
	A2dpCodecConfig classic = {};
	classic.codec = CodecType::Vendor;
	std::memcpy(classic.ie, classicIe, sizeof(classicIe));
	classic.ieLength = static_cast<uint8_t>(sizeof(classicIe));
	return AptxCodec().CreateEncoder(classic, encoder);
}

} // namespace OpenA2dp
