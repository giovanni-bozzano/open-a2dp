#include "TestFramework.h"

#include <OpenA2dp/ConfigFile.h>
#include <OpenA2dp/Ioctl.h>

#include <string>

using namespace OpenA2dp;

// The settings file the driver writes and reads is checked here, on the same header the driver
// compiles, so the layout the tools promise and the layout the driver loads cannot drift apart.

namespace
{
ConfigFile::Settings Defaults()
{
	ConfigFile::Settings settings = {};
	settings.preference.abiVersion = Ioctl::kAbiVersion;
	settings.endpointFormat.abiVersion = Ioctl::kAbiVersion;
	return settings;
}

std::string Text(const ConfigFile::Settings& settings)
{
	char text[ConfigFile::kMaxBytes] = {};
	const size_t length = ConfigFile::Write(settings, text, sizeof(text));
	CHECK(length > 0);
	return std::string(text, length);
}

ConfigFile::Settings Read(const std::string& text)
{
	ConfigFile::Settings settings = {};
	const bool parsed = ConfigFile::Parse(text.data(), text.size(), settings);
	CHECK(parsed);
	return settings;
}
} // namespace

// One pinned entry stores that entry and nothing else: the file records what this sink does
// differently, so the codecs left automatic get no section and no default value is restated.
TEST(ConfigFile_WritesOnlyTheChangedEntry)
{
	ConfigFile::Settings settings = Defaults();
	settings.endpointFormat.sampleRate[Ioctl::kCodecSbc] = 48000;
	settings.endpointFormat.channels[Ioctl::kCodecSbc] = 1;
	const std::string text = Text(settings);
	CHECK(text.find("version = 2") != std::string::npos);
	CHECK(text.find("[sbc]") != std::string::npos);
	CHECK(text.find("rate = 48000") != std::string::npos);
	CHECK(text.find("channels = 1") != std::string::npos);
	// A section only counts as written when it starts a line; the header comment names them too.
	CHECK(text.find("\r\n[general]") == std::string::npos);
	CHECK(text.find("\r\n[aac]") == std::string::npos);
	CHECK(text.find("bitpool") == std::string::npos);
}

// A sink nobody changed has nothing to store; the driver removes its file rather than writing
// one, so the text holds the header alone.
TEST(ConfigFile_WritesNothingButTheHeaderForDefaults)
{
	const ConfigFile::Settings settings = Defaults();
	CHECK(ConfigFile::SameDefaults(settings));
	const std::string text = Text(settings);
	CHECK(text.find("\r\n[") == std::string::npos);
	CHECK(text.find("= 0") == std::string::npos);
}

// Every key the writer can emit survives a round trip, so a stored setting is the setting the
// next device start applies.
TEST(ConfigFile_RoundTripsEverySetting)
{
	ConfigFile::Settings written = Defaults();
	written.preference.flags = Ioctl::kPreferenceForceCodec;
	written.preference.codec = Ioctl::kCodecLdac;
	written.preference.prerollMs = 30;
	written.preference.sbcBitpool = 35;
	written.preference.sbcBlocks = 8;
	written.preference.sbcSubbands = 4;
	written.preference.sbcAllocation = Ioctl::kSbcAllocationSnr;
	written.preference.sbcFramesPerPacket = 3;
	written.preference.aacBitrate = 192000;
	written.preference.ldacQuality = 2;
	for (uint32_t codec = 0; codec < Ioctl::kCodecCount; ++codec)
	{
		written.endpointFormat.sampleRate[codec] = Ioctl::kEndpointRates[codec % Ioctl::kEndpointRateCount];
		written.endpointFormat.channels[codec] = codec % 2 == 0 ? 1u : 2u;
	}
	const std::string text = Text(written);
	const ConfigFile::Settings read = Read(text);
	CHECK(ConfigFile::SamePreference(read.preference, written.preference));
	CHECK(ConfigFile::SameEndpointFormat(read.endpointFormat, written.endpointFormat));
}

// Only this layout's version is accepted: a file numbered anything else is left alone, so a
// format this build does not understand can never be half applied.
TEST(ConfigFile_RejectsAnotherLayoutVersion)
{
	const std::string text = "version = 1\n\n[sbc]\nrate = 48000\n";
	ConfigFile::Settings settings = {};
	CHECK(!ConfigFile::Parse(text.data(), text.size(), settings));
	const std::string missing = "\n[sbc]\nrate = 48000\n";
	CHECK(!ConfigFile::Parse(missing.data(), missing.size(), settings));
}

// A key or a section this build does not write is skipped, so a file written by a newer build
// still loads everything this one understands.
TEST(ConfigFile_IgnoresKeysAndSectionsItDoesNotKnow)
{
	const std::string text = "# a comment\nversion = 2\n\n[general]\ncodec = aac\nfutureSetting = 7\n\n"
							 "[ldac]\nrate = 96000\nchannels = 2\nquality = 1\n\n[zyx]\nrate = 48000\n";
	const ConfigFile::Settings read = Read(text);
	CHECK_EQ(read.preference.flags, Ioctl::kPreferenceForceCodec);
	CHECK_EQ(read.preference.codec, Ioctl::kCodecAac);
	CHECK_EQ(read.preference.ldacQuality, 1u);
	CHECK_EQ(read.endpointFormat.sampleRate[Ioctl::kCodecLdac], 96000u);
	CHECK_EQ(read.endpointFormat.channels[Ioctl::kCodecLdac], 2u);
	CHECK_EQ(read.endpointFormat.sampleRate[Ioctl::kCodecSbc], 0u);
}

// A parameter belongs to the codec's section: `bitpool` under [aac] is not the SBC bitpool, and
// `rate` under [sbc] is not the AAC rate.
TEST(ConfigFile_ScopesKeysToTheirCodec)
{
	const std::string text = "version = 2\n\n[aac]\nbitpool = 35\nrate = 44100\n\n[sbc]\nbitpool = 40\n";
	const ConfigFile::Settings read = Read(text);
	CHECK_EQ(read.preference.sbcBitpool, 40u);
	CHECK_EQ(read.preference.aacBitrate, 0u);
	CHECK_EQ(read.endpointFormat.sampleRate[Ioctl::kCodecSbc], 0u);
	CHECK_EQ(read.endpointFormat.sampleRate[Ioctl::kCodecAac], 44100u);
}

// A known key with a value this build cannot use refuses the whole file, because skipping it
// would silently apply a setting the file does not ask for.
TEST(ConfigFile_RefusesAnUnusableValue)
{
	ConfigFile::Settings settings = {};
	const std::string text = "version = 2\n\n[sbc]\nrate = fast\n";
	CHECK(!ConfigFile::Parse(text.data(), text.size(), settings));
	const std::string unknownCodec = "version = 2\n\n[general]\ncodec = opus\n";
	CHECK(!ConfigFile::Parse(unknownCodec.data(), unknownCodec.size(), settings));
}

// Hand edits are expected: indentation, trailing spaces, comment lines and CRLF all read.
TEST(ConfigFile_ToleratesCommentsAndBlankLines)
{
	const std::string text = "# heading\r\n\r\nversion = 2\r\n\r\n[sbc]   # SBC\r\n  rate = 48000  \r\n\r\n";
	const ConfigFile::Settings read = Read(text);
	CHECK_EQ(read.endpointFormat.sampleRate[Ioctl::kCodecSbc], 48000u);
}

// A writer buffer too small to hold the text reports failure instead of truncating a file the
// driver would then load in part.
TEST(ConfigFile_RefusesATooSmallBuffer)
{
	ConfigFile::Settings settings = Defaults();
	settings.endpointFormat.sampleRate[Ioctl::kCodecSbc] = 48000;
	settings.endpointFormat.channels[Ioctl::kCodecSbc] = 2;
	char text[64] = {};
	CHECK_EQ(ConfigFile::Write(settings, text, sizeof(text)), size_t{0});
}
