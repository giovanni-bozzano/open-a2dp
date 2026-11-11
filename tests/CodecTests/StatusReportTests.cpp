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
	CHECK(Contains(report, "endpoint format automatic (every sink format)"));
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
	// Counters and masks that the formatted lines hide, so a value the report does not call
	// out is still visible to whoever reads the dump.
	for (const char* const field : {"status.avdtpState", "status.objectType", "status.pcmBufferedBytes",
									"status.frameCapacity", "status.underruns", "status.negotiatedCodec",
									"status.preferenceFlags", "status.sinkCodecs", "preference.sbcBlocks",
									"preference.prerollMs", "status.pacingLeadMs",
									"version.maxFramePayload"})
	{
		CHECK(Contains(report, field));
	}

	CHECK(Contains(report, "0xC000009A"));
	CHECK(Contains(report, "last error      0xC000009A"));
}

TEST(StatusReport_ShowsTheDriverSideLatency)
{
	Ioctl::Status status = StreamingStatus();
	status.pacingLeadMs = 30;
	status.pcmBufferedMs = 12;
	status.frameBufferedMs = 8;
	const std::string report = Report(false, status, StreamingPreference());

	// The lead and both ring fills, which is the audio the driver holds ahead of live; the
	// sink's own buffering is not ours to report.
	CHECK(Contains(report,
			"latency         50 ms (driver side: lead 30 + buffered 20; the sink adds its own)"));
	// A spike that ended before the poll is only visible through the peak.
	status.latencyPeakMs = 180;
	const std::string spiked = Report(false, status, StreamingPreference());
	CHECK(Contains(spiked, "latency peak    180 ms (worst since this stream started)"));
}

TEST(StatusReport_ShowsWhatTheLatencyCost)
{
	// The counters that explain the latency number sit next to it, in the same session section
	// of the window: silence where the ring ran dry, and old audio dropped to stay near live.
	Ioctl::Status status = StreamingStatus();
	status.underruns = 3;
	status.pcmTrimmedBytes = 2048;
	const std::string report = Report(false, status, StreamingPreference());
	CHECK(Contains(report, "underruns       3 packets short of PCM"));
	CHECK(Contains(report, "trimmed         2048 B of old audio dropped"));

	// A clean stream says so, so the absence of the warning tone means something.
	status.underruns = 0;
	status.pcmTrimmedBytes = 0;
	const std::string clean = Report(false, status, StreamingPreference());
	CHECK(Contains(clean, "underruns       0 packets short of PCM"));
	CHECK(Contains(clean, "trimmed         0 B of old audio dropped"));
}

TEST(StatusReport_ShowsAPinnedEndpointFormat)
{
	// The one format the endpoint offers is the answer to "what will the shared-mode stream
	// run at", so it is a line of its own, not a preference field.
	Ioctl::Status status = StreamingStatus();
	status.endpointRate = 48000;
	status.endpointChannels = 1;
	const std::string report = Report(false, status, StreamingPreference());
	CHECK(Contains(report, "endpoint format 48000 Hz, mono (pinned; offered alone)"));
	CHECK(!Contains(report, "automatic (every sink format)"));
}

TEST(StatusReport_NeedsAStatus)
{
	StatusReportInput input;
	CHECK(BuildStatusReport(input).empty());
	const std::string report = Report(false, StreamingStatus(), StreamingPreference());
	CHECK(report.empty() == false);
}

TEST(StatusReport_SplitsIntoColumns)
{
	// The settings window renders the report as rows, so the column split has to work for a
	// label that fills most of its column (one space left) as well as for a short one.
	std::string label;
	std::string value;
	CHECK(SplitReportLine("  AVDTP           Streaming", 16, label, value));
	CHECK(label == "AVDTP");
	CHECK(value == "Streaming");
	CHECK(SplitReportLine("  endpoint format automatic (every sink format)", 16, label, value));
	CHECK(label == "endpoint format");
	CHECK(value == "automatic (every sink format)");
	// Raw fields are padded to 28 and separated by one more space.
	CHECK(SplitReportLine("  status.abiVersion" + std::string(11, ' ') + " 1 (0x1)", 28, label, value));
	CHECK(label == "status.abiVersion");
	CHECK(value == "1 (0x1)");
	CHECK(SplitReportLine("  driver", 16, label, value));
	CHECK(label == "driver");
	CHECK(value.empty());
	CHECK(SplitReportLine("   ", 16, label, value) == false);
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
