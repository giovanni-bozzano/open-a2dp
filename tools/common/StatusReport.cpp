// The report both tools show. It is deliberately plain text with one field per line: it is
// read in a console, pasted into a bug report from the window, and compared line by line
// between two runs, so aligned columns matter more than prose.

#include "common/StatusReport.h"

#include "Text.h"

#include <cstdarg>
#include <cstdio>

namespace OpenA2dp::Tools {
namespace {

// One "  name   value" line; the label column is the widest label the report uses.
void AppendLine(std::string& text, const char* label, const std::string& value)
{
	char prefix[32] = {};
	std::snprintf(prefix, sizeof(prefix), "  %-16s", label);
	text += prefix;
	text += value;
	text += '\n';
}

// One raw field: decimal and hex, so masks and NTSTATUS values read without conversion.
void AppendField(std::string& text, const char* name, uint32_t value)
{
	char line[128] = {};
	std::snprintf(line, sizeof(line), "  %-28s %u (0x%X)\n", name, value, value);
	text += line;
}

std::string Format(const char* format, ...)
{
	char buffer[256] = {};
	va_list args;
	va_start(args, format);
	(void)std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

// What the media channel can carry, which is what a codec has to fit into one packet.
void AppendMediaMtu(std::string& text, const Ioctl::Status& status)
{
	if (status.mediaMtu == 0)
	{
		AppendLine(text, "media MTU", "not reported yet");
		return;
	}
	const uint32_t payload = status.mediaMtu > Ioctl::kRtpHeaderBytes ? status.mediaMtu - Ioctl::kRtpHeaderBytes : 0;
	AppendLine(text, "media MTU",
			   Format("%u B, %u B payload; AAC up to %u bps", status.mediaMtu, payload, status.bitrateLimit));
}

void AppendCodec(std::string& text, const Ioctl::Status& status)
{
	if (status.mediaState == Ioctl::MediaState::Streaming)
	{
		// SBC is 0, so a streaming session with codec 0 is SBC and not "none".
		const char* const name = status.codec == 0 ? "SBC" : CodecName(status.codec);
		AppendLine(text, "codec",
				   Format("%s, %u Hz, %u ch, %u bps", name, status.sampleRate, status.channels, status.bitrate));
	}
	else if (status.codec != 0)
	{
		AppendLine(text, "codec",
				   Format("%s, %u Hz, %u ch, %u bps", CodecName(status.codec), status.sampleRate, status.channels,
						  status.bitrate));
	}
	else if (status.negotiatedCodec != 0)
	{
		AppendLine(text, "codec", Format("%s (negotiated, not streaming)", CodecName(status.negotiatedCodec)));
	}
	else
	{
		AppendLine(text, "codec", "none");
	}
}

// The sink's own name next to the formats it offers: the list names the sink the same way.
void AppendSinkName(std::string& text, const Ioctl::Status& status)
{
	const std::string name = Utf8(status.sinkName);
	AppendLine(text, "sink name", name.empty() ? "not reported" : name);
}

void AppendSinkFormats(std::string& text, const Ioctl::Status& status)
{
	if (status.sinkRates == 0 && status.sinkChannels == 0 && status.sinkCodecs == 0)
	{
		AppendLine(text, "sink formats", "not discovered yet");
		return;
	}
	std::string line = FormatListText(status.sinkRates, status.sinkChannels);
	const std::string codecs = CodecListText(status.sinkCodecs);
	if (!codecs.empty())
	{
		line = line.empty() ? codecs : line + " (" + codecs + ")";
	}
	AppendLine(text, "sink formats", line.empty() ? std::string("unknown") : line);
}

void AppendEndpointFormat(std::string& text, const Ioctl::Status& status)
{
	// Windows plays the first listed format and ignores the Sound-settings pick, so this line is
	// the answer to "what will the shared-mode stream run at" whenever it is pinned.
	if (status.endpointRate == 0)
	{
		AppendLine(text, "endpoint format", "automatic (every sink format)");
		return;
	}
	AppendLine(text, "endpoint format",
			   Format("%u Hz, %s (pinned; offered alone)", status.endpointRate,
					  status.endpointChannels == 1 ? "mono" : "stereo"));
}

void AppendPreference(std::string& text, const Ioctl::Preference& preference)
{
	if ((preference.flags & Ioctl::kPreferenceForceCodec) != 0)
	{
		AppendLine(text, "preference", Format("force %s", CodecName(preference.codec)));
	}
	else
	{
		AppendLine(text, "preference", "auto (AAC > SBC)");
	}
	if (preference.sbcBitpool != 0)
	{
		AppendLine(text, "SBC bitpool", Format("%u", preference.sbcBitpool));
	}
	if (preference.aacBitrate != 0)
	{
		AppendLine(text, "AAC bitrate", Format("%u bps", preference.aacBitrate));
	}
	if (preference.sbcBlocks != 0 || preference.sbcSubbands != 0 || preference.sbcAllocation != 0)
	{
		AppendLine(text, "SBC frame",
				   Format("%u blocks, %u subbands, %s", preference.sbcBlocks != 0 ? preference.sbcBlocks : 16u,
						  preference.sbcSubbands != 0 ? preference.sbcSubbands : 8u,
						  preference.sbcAllocation == Ioctl::kSbcAllocationSnr ? "SNR" : "loudness"));
	}
}

void AppendDriver(std::string& text, const Ioctl::VersionInfo& version)
{
	if (version.abiVersion != Ioctl::kAbiVersion)
	{
		AppendLine(text, "driver",
				   Format("ABI %u, but this tool speaks ABI %u; update both from one build", version.abiVersion,
						  Ioctl::kAbiVersion));
		return;
	}
	AppendLine(text, "driver",
			   Format("ABI %u, version %s, max frame %u B, rings PCM %u B, frames %u B", version.abiVersion,
					  DriverVersionText(version.driverVersion).c_str(), version.maxFramePayload,
					  version.maxPcmRingBytes, version.maxFrameRingBytes));
}

// Every field, so a value the formatted lines above do not show (a mask, a counter that was
// not worth a line, a reserved field a stale caller left unset) is still visible.
void AppendRawFields(std::string& text, const Ioctl::Status& status, const Ioctl::Preference& preference,
					 const Ioctl::VersionInfo* version)
{
	text += "  -- raw fields --\n";
	AppendField(text, "status.abiVersion", status.abiVersion);
	AppendField(text, "status.avdtpState", status.avdtpState);
	AppendField(text, "status.mediaState", static_cast<uint32_t>(status.mediaState));
	AppendField(text, "status.codec", status.codec);
	AppendField(text, "status.sampleRate", status.sampleRate);
	AppendField(text, "status.channels", status.channels);
	AppendField(text, "status.bitrate", status.bitrate);
	AppendField(text, "status.objectType", status.objectType);
	AppendField(text, "status.encoderRegistered", status.encoderRegistered);
	AppendField(text, "status.pcmBufferedBytes", status.pcmBufferedBytes);
	AppendField(text, "status.pcmCapacity", status.pcmCapacity);
	AppendField(text, "status.frameBufferedBytes", status.frameBufferedBytes);
	AppendField(text, "status.frameCapacity", status.frameCapacity);
	AppendField(text, "status.packetsSent", status.packetsSent);
	AppendField(text, "status.framesDropped", status.framesDropped);
	AppendField(text, "status.underruns", status.underruns);
	AppendField(text, "status.sendFailures", status.sendFailures);
	AppendField(text, "status.lastError", status.lastError);
	AppendField(text, "status.negotiatedCodec", status.negotiatedCodec);
	AppendField(text, "status.preferenceFlags", status.preferenceFlags);
	AppendField(text, "status.preferenceCodec", status.preferenceCodec);
	AppendField(text, "status.mediaMtu", status.mediaMtu);
	AppendField(text, "status.bitrateLimit", status.bitrateLimit);
	AppendField(text, "status.sinkRates", status.sinkRates);
	AppendField(text, "status.sinkChannels", status.sinkChannels);
	AppendField(text, "status.sinkCodecs", status.sinkCodecs);
	AppendField(text, "status.endpointRate", status.endpointRate);
	AppendField(text, "status.endpointChannels", status.endpointChannels);
	AppendField(text, "preference.abiVersion", preference.abiVersion);
	AppendField(text, "preference.flags", preference.flags);
	AppendField(text, "preference.codec", preference.codec);
	AppendField(text, "preference.aacBitrate", preference.aacBitrate);
	AppendField(text, "preference.sbcBitpool", preference.sbcBitpool);
	AppendField(text, "preference.sbcBlocks", preference.sbcBlocks);
	AppendField(text, "preference.sbcSubbands", preference.sbcSubbands);
	AppendField(text, "preference.sbcAllocation", preference.sbcAllocation);
	AppendField(text, "preference.reserved", preference.reserved);
	if (version != nullptr)
	{
		AppendField(text, "version.abiVersion", version->abiVersion);
		AppendField(text, "version.driverVersion", version->driverVersion);
		AppendField(text, "version.maxFramePayload", version->maxFramePayload);
		AppendField(text, "version.maxPcmRingBytes", version->maxPcmRingBytes);
		AppendField(text, "version.maxFrameRingBytes", version->maxFrameRingBytes);
		for (const uint32_t reserved : version->reserved)
		{
			AppendField(text, "version.reserved", reserved);
		}
	}
}

} // namespace

const char* CodecName(uint32_t codec)
{
	if (codec == Ioctl::kCodecSbc)
	{
		return "SBC";
	}
	if (codec == Ioctl::kCodecAac)
	{
		return "AAC";
	}
	return "none";
}

const char* AvdtpStateName(uint32_t state)
{
	static const char* const names[] = {
		"Idle", "Discovering", "GettingCapabilities", "Configuring", "Configured", "Opening",
		"Open", "Starting", "Streaming", "Suspending", "Closing", "Aborting", "Failed",
	};
	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "Unknown";
}

const char* MediaStateName(uint32_t state)
{
	switch (static_cast<Ioctl::MediaState>(state))
	{
	case Ioctl::MediaState::Idle: return "Idle";
	case Ioctl::MediaState::WaitingForEncoder: return "WaitingForEncoder";
	case Ioctl::MediaState::Streaming: return "Streaming";
	case Ioctl::MediaState::Suspended: return "Suspended";
	case Ioctl::MediaState::Failed: return "Failed";
	default: return "Unknown";
	}
}

std::string FormatListText(uint32_t rates, uint32_t channels)
{
	std::string text;
	const auto append = [&text](const char* part)
	{
		if (!text.empty())
		{
			text += ", ";
		}
		text += part;
	};
	if ((rates & 1) != 0)
	{
		append("44100 Hz");
	}
	if ((rates & 2) != 0)
	{
		append("48000 Hz");
	}
	if ((channels & 1) != 0)
	{
		append("mono");
	}
	if ((channels & 2) != 0)
	{
		append("stereo");
	}
	return text;
}

std::string CodecListText(uint32_t codecs)
{
	std::string text;
	if ((codecs & Ioctl::kSinkCodecSbc) != 0)
	{
		text = "SBC";
	}
	if ((codecs & Ioctl::kSinkCodecAac) != 0)
	{
		text += text.empty() ? "AAC" : ", AAC";
	}
	return text;
}

std::string DriverVersionText(uint32_t driverVersion)
{
	return Format("%u.%u.%u", (driverVersion >> 24) & 0xFF, (driverVersion >> 16) & 0xFF, driverVersion & 0xFFFF);
}

bool SplitReportLine(const std::string& line, size_t labelWidth, std::string& label, std::string& value)
{
	const size_t begin = line.find_first_not_of(' ');
	if (begin == std::string::npos || labelWidth == 0)
	{
		return false;
	}
	label = line.substr(begin, labelWidth);
	while (!label.empty() && label.back() == ' ')
	{
		label.pop_back();
	}
	value.clear();
	if (line.size() > begin + labelWidth)
	{
		const size_t valueBegin = line.find_first_not_of(' ', begin + labelWidth);
		if (valueBegin != std::string::npos)
		{
			value = line.substr(valueBegin);
			while (!value.empty() && value.back() == ' ')
			{
				value.pop_back();
			}
		}
	}
	return !label.empty();
}

std::string BuildStatusReport(const StatusReportInput& input)
{
	if (input.status == nullptr || input.preference == nullptr)
	{
		return {};
	}
	const Ioctl::Status& status = *input.status;
	const Ioctl::Preference& preference = *input.preference;

	std::string text;
	if (!input.title.empty())
	{
		text += input.title;
		text += '\n';
	}
	AppendLine(text, "AVDTP", AvdtpStateName(status.avdtpState));
	AppendLine(text, "media", MediaStateName(static_cast<uint32_t>(status.mediaState)));
	AppendCodec(text, status);
	AppendMediaMtu(text, status);
	AppendSinkName(text, status);
	AppendSinkFormats(text, status);
	AppendEndpointFormat(text, status);
	AppendPreference(text, preference);
	AppendLine(text, "encoder", status.encoderRegistered != 0 ? "registered" : "not registered");
	AppendLine(text, "rings",
			   Format("PCM %u/%u B, frames %u/%u B", status.pcmBufferedBytes, status.pcmCapacity,
					  status.frameBufferedBytes, status.frameCapacity));
	AppendLine(text, "counters",
			   Format("packets %u, dropped %u, underruns %u, send failures %u", status.packetsSent,
					  status.framesDropped, status.underruns, status.sendFailures));
	AppendLine(text, "last error", status.lastError != 0 ? Format("0x%08X", status.lastError) : "none");
	if (input.version != nullptr)
	{
		AppendDriver(text, *input.version);
	}
	if (input.verbose)
	{
		AppendRawFields(text, status, preference, input.version);
	}
	return text;
}

} // namespace OpenA2dp::Tools
