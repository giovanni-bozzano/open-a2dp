// The CLI and the settings window show one report, and the tools cannot be run in the test
// environment (they need an elevated session and a sink), so the text they print is checked
// here: a field that stops appearing is a diagnostic that silently disappeared.

#include "TestFramework.h"

#include <OpenA2dp/AvdtpSource.h>
#include <OpenA2dp/Ioctl.h>

#include "common/StatusReport.h"

#include <string>

using namespace OpenA2dp;
using namespace OpenA2dp::Tools;

namespace {

bool Contains(const std::string& text, const std::string& part)
{
	return text.find(part) != std::string::npos;
}

// A streaming AAC session, which is what a bug report normally starts from.
Ioctl::Status StreamingStatus()
{
	Ioctl::Status status = {};
	status.abiVersion = Ioctl::kAbiVersion;
	status.avdtpState = static_cast<uint32_t>(Avdtp::SessionState::Streaming);
	status.mediaState = Ioctl::MediaState::Streaming;
	status.codec = Ioctl::kCodecAac;
	status.sampleRate = 48000;
	status.channels = 2;
	status.bitrate = 256000;
	status.encoderRegistered = 1;
	status.mediaMtu = 672;
	status.bitrateLimit = 352000;
	status.sinkRates = 3;
	status.sinkChannels = 2;
	status.sinkCodecs = Ioctl::kSinkCodecSbc | Ioctl::kSinkCodecAac;
	status.negotiatedCodec = Ioctl::kCodecAac;
	status.packetsSent = 12;
	status.pcmCapacity = 65536;
	status.frameCapacity = 32768;
	return status;
}

Ioctl::Preference StreamingPreference()
{
	Ioctl::Preference preference = {};
	preference.abiVersion = Ioctl::kAbiVersion;
	preference.flags = Ioctl::kPreferenceForceCodec;
	preference.codec = Ioctl::kCodecAac;
	preference.aacBitrate = 256000;
	return preference;
}

Ioctl::VersionInfo Version()
{
	Ioctl::VersionInfo version = {};
	version.abiVersion = Ioctl::kAbiVersion;
	version.driverVersion = 0x01020003; // 1.2.3: two bytes major/minor, then the 16-bit build
	version.maxFramePayload = Ioctl::kMaxFramePayload;
	version.maxPcmRingBytes = Ioctl::kPcmRingBytes;
	version.maxFrameRingBytes = Ioctl::kFrameRingBytes;
	return version;
}

std::string Report(bool verbose, Ioctl::Status status, Ioctl::Preference preference)
{
	const Ioctl::VersionInfo version = Version();
	StatusReportInput input;
	input.title = "1. AABBCCDDEEFF";
	input.status = &status;
	input.preference = &preference;
	input.version = &version;
	input.verbose = verbose;
	return BuildStatusReport(input);
}

} // namespace

TEST(StatusReport_FormatsTheSession)
{
	const std::string report = Report(false, StreamingStatus(), StreamingPreference());
	CHECK(Contains(report, "1. AABBCCDDEEFF\n"));
	CHECK(Contains(report, "Streaming"));
	// AAC is reported by number, SBC is 0, so the streaming codec line is the one that says
	// which codec actually runs.
	CHECK(Contains(report, "AAC, 48000 Hz, 2 ch, 256000 bps"));
	// MTU minus the RTP header, which is the payload a packet can carry.
	CHECK(Contains(report, "672 B, 660 B payload; AAC up to 352000 bps"));
	CHECK(Contains(report, "44100 Hz, 48000 Hz, stereo (SBC, AAC)"));
	CHECK(Contains(report, "force AAC"));
	CHECK(Contains(report, "endpoint        Windows-selected; all supported PCM formats"));
	CHECK(Contains(report, "registered"));
	CHECK(Contains(report, "packets 12"));
	CHECK(Contains(report, "ABI " + std::to_string(Ioctl::kAbiVersion) + ", version 1.2.3"));
	CHECK(Contains(report, "not reported yet") == false);
	CHECK(Contains(report, "fallback") == false);
	CHECK(Contains(report, "-- raw fields --") == false);
}

TEST(StatusReport_ExplainsTheUnknowns)
{
	// Before the first discovery.
	Ioctl::Status status = StreamingStatus();
	status.mediaState = Ioctl::MediaState::Idle;
	status.codec = 0;
	status.negotiatedCodec = 0;
	status.mediaMtu = 0;
	status.sinkRates = 0;
	status.sinkChannels = 0;
	status.sinkCodecs = 0;

	const std::string report = Report(false, status, StreamingPreference());
	CHECK(Contains(report, "codec           none"));
	CHECK(Contains(report, "media MTU       not reported yet"));
	CHECK(Contains(report, "sink formats    not discovered yet"));
	CHECK(!Contains(report, "fallback"));
	CHECK(Contains(report, "last error      none"));
	CHECK(Contains(report, "preference      force AAC"));
}

TEST(StatusReport_VerboseListsEveryField)
{
	Ioctl::Status status = StreamingStatus();
	status.lastError = 0xC000009A;
	const std::string report = Report(true, status, StreamingPreference());
	CHECK(Contains(report, "-- raw fields --"));
	// Counters and masks that the formatted lines hide, plus the reserved field that shows
	// whether a caller wrote the whole structure.
	for (const char* const field : {"status.avdtpState", "status.objectType", "status.pcmBufferedBytes",
									"status.frameCapacity", "status.underruns", "status.negotiatedCodec",
									"status.preferenceFlags", "status.sinkCodecs", "preference.sbcBlocks",
									"preference.reserved",
									"version.maxFramePayload"})
	{
		CHECK(Contains(report, field));
	}

	CHECK(Contains(report, "0xC000009A"));
	CHECK(Contains(report, "last error      0xC000009A"));
}

TEST(StatusReport_NeedsAStatus)
{
	StatusReportInput input;
	CHECK(BuildStatusReport(input).empty());
	const std::string report = Report(false, StreamingStatus(), StreamingPreference());
	CHECK(report.empty() == false);
}

TEST(StatusReport_Names)
{
	CHECK(std::string(CodecName(Ioctl::kCodecSbc)) == "SBC");
	CHECK(std::string(CodecName(Ioctl::kCodecAac)) == "AAC");
	CHECK(std::string(AvdtpStateName(0)) == "Idle");
	CHECK(std::string(MediaStateName(static_cast<uint32_t>(Ioctl::MediaState::Suspended))) == "Suspended");
	CHECK(std::string(DriverVersionText(0x01020003)) == "1.2.3");
	CHECK(std::string(DriverVersionText(0x02010403)) == "2.1.1027");
	const std::string noFormats = FormatListText(0, 0);
	const std::string noCodecs = CodecListText(0);
	CHECK(noFormats.empty());
	CHECK(noCodecs.empty());
	const std::string formats = FormatListText(1, 2);
	CHECK(formats == "44100 Hz, stereo");
}
