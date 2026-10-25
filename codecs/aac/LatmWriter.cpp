#include "LatmWriter.h"

#include <OpenA2dp/AacCodec.h>
#include <OpenA2dp/BitWriter.h>

namespace OpenA2dp::Latm {

namespace {

constexpr uint32_t kSampleRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
									 22050, 16000, 12000, 11025, 8000, 7350};

void WriteStreamMuxConfig(BitWriter& w, const StreamConfig& c, int frequencyIndex) noexcept
{
	w.Write(0, 1);  // audioMuxVersion
	w.Write(1, 1);  // allStreamsSameTimeFraming
	w.Write(0, 6);  // numSubFrames
	w.Write(0, 4);  // numProgram
	w.Write(0, 3);  // numLayer
	// AudioSpecificConfig
	w.Write(c.audioObjectType, 5);
	w.Write(static_cast<uint32_t>(frequencyIndex), 4);
	w.Write(c.channels, 4);
	// GASpecificConfig
	w.Write(0, 1);  // frameLengthFlag (1024)
	w.Write(0, 1);  // dependsOnCoreCoder
	w.Write(0, 1);  // extensionFlag
	w.Write(0, 3);  // frameLengthType
	w.Write(0xFF, 8); // latmBufferFullness
	w.Write(0, 1);  // otherDataPresent
	w.Write(0, 1);  // crcCheckPresent
}

} // namespace

size_t MaxOverhead(size_t accessUnitLength) noexcept
{
	// 45 bits of StreamMuxConfig + useSameStreamMux, payload length bytes, final alignment.
	return 7 + accessUnitLength / 255 + 1;
}

size_t WriteAudioMuxElement(const StreamConfig& config, const uint8_t* accessUnit, size_t accessUnitLength,
							uint8_t* out, size_t capacity) noexcept
{
	const int frequencyIndex = Aac::SamplingFrequencyIndex(config.sampleRate);
	if (frequencyIndex < 0 || accessUnit == nullptr || accessUnitLength == 0) {
		return 0;
	}

	BitWriter w(out, capacity);
	w.Write(0, 1); // useSameStreamMux = 0 -> StreamMuxConfig follows
	WriteStreamMuxConfig(w, config, frequencyIndex);

	// PayloadLengthInfo
	size_t remaining = accessUnitLength;
	while (remaining >= 255) {
		w.Write(255, 8);
		remaining -= 255;
	}
	w.Write(static_cast<uint32_t>(remaining), 8);

	// PayloadMux
	for (size_t i = 0; i < accessUnitLength; ++i) {
		w.Write(accessUnit[i], 8);
	}
	w.AlignToByte();
	return w.Overflow() ? 0 : w.BytesUsed();
}

bool ParseAudioMuxElement(const uint8_t* data, size_t length, ParsedElement& out) noexcept
{
	BitReader r(data, length);
	if (r.Read(1) != 0) {
		return false; // useSameStreamMux not supported here
	}
	if (r.Read(1) != 0 || r.Read(1) != 1 || r.Read(6) != 0 || r.Read(4) != 0 || r.Read(3) != 0) {
		return false;
	}
	out.config.audioObjectType = static_cast<uint8_t>(r.Read(5));
	const uint32_t frequencyIndex = r.Read(4);
	if (frequencyIndex >= sizeof(kSampleRates) / sizeof(kSampleRates[0])) {
		return false;
	}
	out.config.sampleRate = kSampleRates[frequencyIndex];
	out.config.channels = static_cast<uint8_t>(r.Read(4));
	r.Read(3); // GASpecificConfig
	if (r.Read(3) != 0) {
		return false; // frameLengthType
	}
	r.Read(8); // latmBufferFullness
	if (r.Read(1) != 0 || r.Read(1) != 0) {
		return false; // otherDataPresent / crcCheckPresent
	}

	size_t payloadLength = 0;
	uint32_t tmp = 0;
	do {
		tmp = r.Read(8);
		payloadLength += tmp;
	} while (tmp == 255 && !r.Overflow());

	if (payloadLength > sizeof(out.accessUnit)) {
		return false;
	}
	for (size_t i = 0; i < payloadLength; ++i) {
		out.accessUnit[i] = static_cast<uint8_t>(r.Read(8));
	}
	out.accessUnitLength = payloadLength;
	return !r.Overflow();
}

} // namespace OpenA2dp::Latm
