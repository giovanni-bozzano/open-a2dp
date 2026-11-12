#pragma once

// The settings file: one text file per sink under %ProgramData%\OpenA2dp, in the layout the
// driver reads and writes. It lives here so the tests check the same code the driver runs, and
// it is kernel-safe like every other header in this directory: no STL, no exceptions, no
// allocation - the text is built into the caller's buffer and parsed in place.
//
//   version = 1              above the sections; another number is not this build's layout
//   [general]                the codec choice and the pacing lead
//   [sbc] [aac] [aptx] [aptxhd] [ldac]
//
// A `key = value` line holds one setting and `#` starts a comment. Only the values that differ
// from the driver default are written, so a key the file leaves out keeps its default; a file
// whose settings are all defaults has nothing to say and the driver removes it instead.

#include <OpenA2dp/Ioctl.h>

namespace OpenA2dp::ConfigFile {

constexpr uint32_t kVersion = 1;
// Room the writer needs for every key it can write, with space to spare.
constexpr size_t kMaxBytes = 1024;
constexpr size_t kMaxNameChars = 16;

struct Settings
{
	Ioctl::Preference preference;
	Ioctl::EndpointFormat endpointFormat;
};

// The ABI fields are not settings, so they do not count as a difference.
constexpr bool SamePreference(const Ioctl::Preference& first, const Ioctl::Preference& second) noexcept
{
	return first.flags == second.flags && first.codec == second.codec &&
		   first.aacBitrate == second.aacBitrate && first.sbcBitpool == second.sbcBitpool &&
		   first.sbcBlocks == second.sbcBlocks && first.sbcSubbands == second.sbcSubbands &&
		   first.sbcAllocation == second.sbcAllocation && first.prerollMs == second.prerollMs &&
		   first.sbcFramesPerPacket == second.sbcFramesPerPacket &&
		   first.ldacQuality == second.ldacQuality;
}

constexpr bool SameEndpointFormat(const Ioctl::EndpointFormat& first,
								  const Ioctl::EndpointFormat& second) noexcept
{
	for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
	{
		if (first.sampleRate[codec] != second.sampleRate[codec] ||
			first.channels[codec] != second.channels[codec])
		{
			return false;
		}
	}
	return true;
}

// True when nothing differs from the driver default: the automatic codec and every parameter
// zero. Such a sink has nothing to store, so its file is removed rather than written.
constexpr bool SameDefaults(const Settings& settings) noexcept
{
	const Ioctl::Preference preference = {};
	const Ioctl::EndpointFormat endpointFormat = {};
	return SamePreference(settings.preference, preference) &&
		   SameEndpointFormat(settings.endpointFormat, endpointFormat);
}

namespace Detail {

inline bool KeyIs(const char* key, size_t length, const char* name) noexcept
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

inline bool IsSpace(char character) noexcept
{
	return character == ' ' || character == '\t';
}

// An unsigned decimal number that fills the value exactly; everything this file stores is a
// count, a rate or a bitrate, so a sign, a fraction or a stray character is not this build's.
inline bool ReadNumber(const char* value, size_t length, uint32_t& number) noexcept
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
inline bool ReadChoice(const char* value, size_t length, uint32_t& number) noexcept
{
	if (KeyIs(value, length, "auto"))
	{
		number = 0;
		return true;
	}
	return ReadNumber(value, length, number);
}

// The codec a section (or the `codec` value) names: -1 for "auto", -2 for a name this build does
// not know. These are the same five names `OpenA2dpCtl format` takes.
inline int CodecFromName(const char* name, size_t length) noexcept
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

inline const char* CodecName(uint32_t codec) noexcept
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

// Bounded appends; the first line that would not fit marks the whole write failed.
struct Writer
{
	char* text;
	size_t capacity;
	size_t length = 0;
	bool ok = true;

	void String(const char* value) noexcept
	{
		if (!ok)
		{
			return;
		}
		for (size_t index = 0; value[index] != '\0'; ++index)
		{
			if (length >= capacity)
			{
				ok = false;
				return;
			}
			text[length++] = value[index];
		}
	}

	void Number(uint32_t value) noexcept
	{
		if (!ok)
		{
			return;
		}
		char digits[10] = {};
		size_t count = 0;
		do
		{
			digits[count++] = static_cast<char>('0' + value % 10);
			value /= 10;
		} while (value != 0);
		while (count > 0)
		{
			if (length >= capacity)
			{
				ok = false;
				return;
			}
			text[length++] = digits[--count];
		}
	}

	void Key(const char* name, uint32_t value) noexcept
	{
		String(name);
		Number(value);
		String("\r\n");
	}

	// A rate or a channel count writes "auto" for zero.
	void Choice(const char* name, uint32_t value) noexcept
	{
		String(name);
		if (value == 0)
		{
			String("auto\r\n");
			return;
		}
		Number(value);
		String("\r\n");
	}
};

// Ends a section, or drops it when nothing was written under its header line, so the file holds
// only the sections this sink has something to say about.
inline void CloseSection(Writer& writer, size_t start) noexcept
{
	if (!writer.ok)
	{
		return;
	}
	size_t header = 0;
	while (start + header < writer.length && writer.text[start + header] != '\n')
	{
		++header;
	}
	++header; // the newline that ends the header line
	if (writer.length == start + header)
	{
		writer.length = start;
		return;
	}
	writer.String("\r\n");
}

// One key of one section. A key or a section this build does not write is skipped, so a newer
// file still loads; a key that is known but holds a value this build cannot use refuses the
// file, because silently dropping it would apply a setting the file does not ask for.
inline bool ApplyValue(const char* section, size_t sectionLength, const char* key, size_t keyLength,
					   const char* value, size_t valueLength, Settings& settings) noexcept
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

} // namespace Detail

namespace Detail {
// The rate and channels a codec plays at, written only when pinned, so an automatic entry
// leaves nothing behind.
inline void WriteFormat(Writer& writer, uint32_t rate, uint32_t channels) noexcept
{
	if (rate != 0)
	{
		writer.Choice("rate = ", rate);
	}
	if (channels != 0)
	{
		writer.Choice("channels = ", channels);
	}
}
} // namespace Detail

// The whole settings set as text. Returns its length, or 0 when the buffer is too small.
inline size_t Write(const Settings& settings, char* text, size_t capacity) noexcept
{
	if (text == nullptr || capacity == 0)
	{
		return 0;
	}
	const Ioctl::Preference& preference = settings.preference;
	const Ioctl::EndpointFormat& format = settings.endpointFormat;
	Detail::Writer writer{text, capacity};
	writer.String("# OpenA2dp settings for this sink. Delete the file to return the driver defaults.\r\n"
				  "# Only what differs from the driver default is written; a key that is missing keeps its\r\n"
				  "# default. One section per codec, plus [general] for what is not a codec's own setting.\r\n"
				  "version = ");
	writer.Number(kVersion);
	writer.String("\r\n\r\n");

	// [general] holds what belongs to the sink rather than to one codec.
	size_t start = writer.length;
	writer.String("[general]\r\n");
	if ((preference.flags & Ioctl::kPreferenceForceCodec) != 0)
	{
		writer.String("# auto, sbc, aac, aptx, aptxhd or ldac\r\ncodec = ");
		writer.String(Detail::CodecName(preference.codec));
		writer.String("\r\n");
	}
	if (preference.prerollMs != 0)
	{
		writer.String("# How far ahead of the media clock the stream is sent, in milliseconds.\r\nprerollMs = ");
		writer.Number(preference.prerollMs);
		writer.String("\r\n");
	}
	Detail::CloseSection(writer, start);

	// One section per codec, in the order the file documents them. Only SBC, AAC and LDAC carry
	// a parameter beyond rate and channels; aptX and aptX HD are fixed-shape codecs.
	static const struct
	{
		const char* name;
		uint32_t codec;
	} kSections[] = {
		{"[sbc]\r\n", Ioctl::kCodecSbc},
		{"[aac]\r\n", Ioctl::kCodecAac},
		{"[aptx]\r\n", Ioctl::kCodecAptx},
		{"[aptxhd]\r\n", Ioctl::kCodecAptxHd},
		{"[ldac]\r\n", Ioctl::kCodecLdac},
	};
	for (const auto& section : kSections)
	{
		start = writer.length;
		writer.String(section.name);
		Detail::WriteFormat(writer, format.sampleRate[section.codec], format.channels[section.codec]);
		switch (section.codec)
		{
		case Ioctl::kCodecSbc:
			if (preference.sbcBitpool != 0)
			{
				writer.Key("bitpool = ", preference.sbcBitpool);
			}
			if (preference.sbcBlocks != 0)
			{
				writer.Key("blocks = ", preference.sbcBlocks);
			}
			if (preference.sbcSubbands != 0)
			{
				writer.Key("subbands = ", preference.sbcSubbands);
			}
			if (preference.sbcAllocation != 0)
			{
				writer.Key("allocation = ", preference.sbcAllocation);
			}
			if (preference.sbcFramesPerPacket != 0)
			{
				writer.Key("framesPerPacket = ", preference.sbcFramesPerPacket);
			}
			break;
		case Ioctl::kCodecAac:
			if (preference.aacBitrate != 0)
			{
				writer.Key("bitrate = ", preference.aacBitrate);
			}
			break;
		case Ioctl::kCodecLdac:
			if (preference.ldacQuality != 0)
			{
				writer.String("# 0 = high (990 kbps), 1 = standard (660), 2 = mobile (330).\r\nquality = ");
				writer.Number(preference.ldacQuality);
				writer.String("\r\n");
			}
			break;
		default:
			break;
		}
		Detail::CloseSection(writer, start);
	}
	return writer.ok ? writer.length : 0;
}

namespace Detail {

// One trimmed line: what remains after the surrounding space and any `#` comment. `cursor` is
// moved past the line's terminator, so the caller only loops while the text remains.
struct Line
{
	const char* begin;
	const char* end;
};

inline Line NextLine(const char* textEnd, const char*& cursor) noexcept
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
	return {begin, end};
}

// `[name]`, with nothing but space allowed after the closing bracket. The name has to fit the
// buffer the caller keeps the open section in.
inline bool ReadSectionName(const Line& line, char (&section)[kMaxNameChars + 1],
							size_t& sectionLength) noexcept
{
	const char* close = line.begin + 1;
	while (close < line.end && *close != ']')
	{
		++close;
	}
	if (close >= line.end)
	{
		return false;
	}
	for (const char* scan = close + 1; scan < line.end; ++scan)
	{
		if (!IsSpace(*scan))
		{
			return false;
		}
	}
	const size_t nameLength = static_cast<size_t>(close - (line.begin + 1));
	if (nameLength == 0 || nameLength > kMaxNameChars)
	{
		return false;
	}
	for (size_t index = 0; index < nameLength; ++index)
	{
		section[index] = line.begin[1 + index];
	}
	section[nameLength] = '\0';
	sectionLength = nameLength;
	return true;
}

// `key = value`, with the space on either side of the `=` removed.
struct KeyValue
{
	const char* key;
	size_t keyLength;
	const char* value;
	size_t valueLength;
};

inline bool SplitKeyValue(const Line& line, KeyValue& pair) noexcept
{
	const char* equals = nullptr;
	for (const char* scan = line.begin; scan < line.end; ++scan)
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
	while (keyEnd > line.begin && IsSpace(keyEnd[-1]))
	{
		--keyEnd;
	}
	const char* value = equals + 1;
	while (value < line.end && IsSpace(*value))
	{
		++value;
	}
	pair.key = line.begin;
	pair.keyLength = static_cast<size_t>(keyEnd - line.begin);
	pair.value = value;
	pair.valueLength = static_cast<size_t>(line.end - value);
	return true;
}

} // namespace Detail

// The text as settings: the version has to be this build's, `[section]` opens a section and
// `key = value` fills it in. False when the text is not a file this build wrote, or a known key
// holds a value it cannot use.
inline bool Parse(const char* text, size_t length, Settings& settings) noexcept
{
	settings = {};
	settings.preference.abiVersion = Ioctl::kAbiVersion;
	settings.endpointFormat.abiVersion = Ioctl::kAbiVersion;
	if (text == nullptr)
	{
		return false;
	}
	const char* cursor = text;
	const char* const textEnd = text + length;
	char section[kMaxNameChars + 1] = {};
	size_t sectionLength = 0;
	bool versionSeen = false;
	uint32_t version = 0;
	while (cursor < textEnd)
	{
		const Detail::Line line = Detail::NextLine(textEnd, cursor);
		if (line.begin == line.end)
		{
			continue;
		}
		if (*line.begin == '[')
		{
			if (!Detail::ReadSectionName(line, section, sectionLength))
			{
				return false;
			}
			continue;
		}

		Detail::KeyValue pair = {};
		if (!Detail::SplitKeyValue(line, pair))
		{
			return false;
		}
		// The version is metadata about the file, not a setting, so it sits above the sections.
		if (sectionLength == 0)
		{
			if (!Detail::KeyIs(pair.key, pair.keyLength, "version") ||
				!Detail::ReadNumber(pair.value, pair.valueLength, version))
			{
				return false;
			}
			versionSeen = true;
			continue;
		}
		if (!Detail::ApplyValue(section, sectionLength, pair.key, pair.keyLength, pair.value,
								pair.valueLength, settings))
		{
			return false;
		}
	}
	return versionSeen && version == kVersion;
}

} // namespace OpenA2dp::ConfigFile
