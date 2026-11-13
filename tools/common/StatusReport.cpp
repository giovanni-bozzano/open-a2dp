// The report both tools show. It is deliberately plain text with one field per line: it is
// read in a console, pasted into a bug report from the window, and compared line by line
// between two runs, so aligned columns matter more than prose.

#include "common/StatusReport.h"

#include "Text.h"

#include <OpenA2dp/AvdtpSource.h>
#include <OpenA2dp/Rtp.h>

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

// The counters are 64-bit: a stream can send more packets than 32 bits hold in a few hours.
void AppendField(std::string& text, const char* name, uint64_t value)
{
	char line[128] = {};
	std::snprintf(line, sizeof(line), "  %-28s %llu (0x%llX)\n", name, value, value);
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
	const uint32_t payload =
			status.mediaMtu > Rtp::kHeaderSize ? status.mediaMtu - static_cast<uint32_t>(Rtp::kHeaderSize) : 0;
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
	// A pin the sink cannot serve, or a list left empty, is why nothing plays, in either mode.
	if (status.listedFormats == 0)
	{
		AppendLine(text, "listed formats", "none (Windows cannot open a stream)");
	}
	if (status.refusedStreams != 0)
	{
		AppendLine(text, "refused streams",
				   Format("%u, last %u Hz %u ch %u-bit", status.refusedStreams, status.refusedRate,
						  status.refusedChannels, status.refusedBits));
	}
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
		AppendLine(text, "preference", "auto (LDAC > aptX HD > aptX > AAC > SBC)");
	}
	if (preference.ldacQuality != 0)
	{
		AppendLine(text, "LDAC quality",
			preference.ldacQuality == 1 ? "standard" : "mobile");
	}
	if (preference.sbcBitpool != 0)
	{
		AppendLine(text, "SBC bitpool", Format("%u", preference.sbcBitpool));
	}
	if (preference.aacBitrate != 0)
	{
		AppendLine(text, "AAC bitrate", Format("%u bps", preference.aacBitrate));
	}
	if (preference.prerollMs != 0)
	{
		AppendLine(text, "pacing lead", Format("%u ms (stored)", preference.prerollMs));
	}
	if (preference.sbcFramesPerPacket != 0)
	{
		AppendLine(text, "SBC frames", Format("%u per packet (stored)", preference.sbcFramesPerPacket));
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
// not worth a line, a reserved field the caller left unset) stays visible.
void AppendRawFields(std::string& text, const Ioctl::Status& status, const Ioctl::Preference& preference,
					 const Ioctl::VersionInfo* version)
{
	text += "  -- raw fields --\n";
	AppendField(text, "status.abiVersion", status.abiVersion);
	AppendField(text, "status.avdtpState", status.avdtpState);
	AppendField(text, "status.mediaState", static_cast<uint32_t>(status.mediaState));
	AppendField(text, "status.endpointRunning", status.endpointRunning);
	AppendField(text, "status.listedFormats", status.listedFormats);
	AppendField(text, "status.refusedStreams", status.refusedStreams);
	AppendField(text, "status.refusedRate", status.refusedRate);
	AppendField(text, "status.refusedChannels", status.refusedChannels);
	AppendField(text, "status.refusedBits", status.refusedBits);
	AppendField(text, "status.jackFlaps", status.jackFlaps);
	AppendField(text, "status.jackUnplugged", status.jackUnplugged);
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
	AppendField(text, "status.pcmBufferedMs", status.pcmBufferedMs);
	AppendField(text, "status.frameBufferedMs", status.frameBufferedMs);
	AppendField(text, "status.pcmTrimmedBytes", status.pcmTrimmedBytes);
	AppendField(text, "status.framesTrimmed", status.framesTrimmed);
	AppendField(text, "status.pacingLeadMs", status.pacingLeadMs);
	AppendField(text, "status.latencyPeakMs", status.latencyPeakMs);
	AppendField(text, "status.worstRenderGapMs", status.worstRenderGapMs);
	AppendField(text, "status.worstServiceGapMs", status.worstServiceGapMs);
	AppendField(text, "status.worstSendMs", status.worstSendMs);
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
	AppendField(text, "preference.prerollMs", preference.prerollMs);
	AppendField(text, "preference.sbcFramesPerPacket", preference.sbcFramesPerPacket);
	AppendField(text, "preference.sbcBlocks", preference.sbcBlocks);
	AppendField(text, "preference.sbcSubbands", preference.sbcSubbands);
	AppendField(text, "preference.sbcAllocation", preference.sbcAllocation);
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

// The names themselves are Avdtp's, next to the enums the driver also uses; a value from a
// corrupt or older IOCTL reply gets the tool's own word instead of indexing off a table.
const char* CodecName(uint32_t codec)
{
	// The aptX codecs are vendor codecs, so their preference/status values are not AVDTP codec
	// types.
	if (codec == Ioctl::kCodecLdac)
	{
		return "LDAC";
	}
	if (codec == Ioctl::kCodecAptxHd)
	{
		return "aptX HD";
	}
	if (codec == Ioctl::kCodecAptx)
	{
		return "aptX";
	}
	const char* const name = codec > 0xFF ? nullptr : Avdtp::CodecTypeName(static_cast<Avdtp::CodecType>(codec));
	return name != nullptr ? name : "none";
}

const char* AvdtpStateName(uint32_t state)
{
	const char* const name = state > 0xFF ? nullptr : Avdtp::SessionStateName(static_cast<Avdtp::SessionState>(state));
	return name != nullptr ? name : "Unknown";
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
	if ((rates & 4) != 0)
	{
		append("32000 Hz");
	}
	if ((rates & 8) != 0)
	{
		append("16000 Hz");
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
	if ((codecs & Ioctl::kSinkCodecAptx) != 0)
	{
		text += text.empty() ? "aptX" : ", aptX";
	}
	if ((codecs & Ioctl::kSinkCodecAptxHd) != 0)
	{
		text += text.empty() ? "aptX HD" : ", aptX HD";
	}
	if ((codecs & Ioctl::kSinkCodecLdac) != 0)
	{
		text += text.empty() ? "LDAC" : ", LDAC";
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

// The latency block: what the driver holds ahead of live audio and what that cost. The sink's
// own buffering is not reported by any A2DP profile, so this is the part the driver controls,
// not the whole mouth-to-ear delay.
void AppendLatency(std::string& text, const Ioctl::Status& status)
{
	const uint32_t bufferedMs = status.pcmBufferedMs + status.frameBufferedMs;
	AppendLine(text, "latency",
			   Format("%u ms (driver side: lead %u + buffered %u; the sink adds its own)",
					  status.pacingLeadMs + bufferedMs, status.pacingLeadMs, bufferedMs));
	// The instantaneous value hides a spike that ended before the poll, so the worst case of
	// this stream is reported next to it; it resets when the next stream starts.
	AppendLine(text, "latency peak",
			   Format("%u ms (worst since this stream started)", status.latencyPeakMs));
	// What the lead had to cover: the longest the render path, the encoder service or a send
	// held audio off. A lead at or above the largest of these has margin; one below it is what
	// the underrun and trim counts below are paying for.
	AppendLine(text, "worst gaps",
			   Format("render %u ms, frames %u ms, send %u ms (worst since this stream started)",
					  status.worstRenderGapMs, status.worstServiceGapMs, status.worstSendMs));
	// Audio the driver could not deliver in time: silence where the stream ran dry is an
	// underrun, and audio discarded to stay near live after a stall is a trim. Both are
	// cumulative and never reset, so a new stream cannot hide an earlier fault.
	AppendLine(text, "underruns", Format("%llu packets short of PCM", status.underruns));
	AppendLine(text, "trimmed",
			   Format("%llu B of PCM and %llu records of old audio dropped", status.pcmTrimmedBytes,
					  status.framesTrimmed));
}

// What the stream holds and how far ahead of the clock it runs. Milliseconds matter here: the
// bytes alone do not say how far behind live a given format or bitrate runs.
void AppendBuffers(std::string& text, const Ioctl::Status& status, const Ioctl::Preference& preference)
{
	// The driver holds the lead at one access unit of the running codec, so a stored value below
	// that is raised rather than served: a lead shorter than a packet can only be padded. The
	// report says so instead of showing a number the service does not use.
	const uint32_t storedLead = Ioctl::EffectivePrerollMs(preference.prerollMs);
	AppendLine(text, "pacing lead",
			   status.pacingLeadMs > storedLead
				   ? Format("%u ms ahead of the media clock (raised from the stored %u ms: one packet "
							"does not fit in it)", status.pacingLeadMs, storedLead)
				   : Format("%u ms ahead of the media clock", status.pacingLeadMs));
	AppendLine(text, "buffered",
			   Format("PCM %u ms of %u B, frames %u ms of %u B", status.pcmBufferedMs,
					  status.pcmBufferedBytes, status.frameBufferedMs, status.frameBufferedBytes));
	AppendLine(text, "capacity",
			   Format("PCM %u B, frames %u B", status.pcmCapacity, status.frameCapacity));
	AppendLine(text, "counters",
			   Format("packets %llu, dropped %llu, underruns %llu, send failures %llu, trimmed %llu B",
					  status.packetsSent, status.framesDropped, status.underruns, status.sendFailures,
					  status.pcmTrimmedBytes));
	AppendLine(text, "last error", status.lastError != 0 ? Format("0x%08X", status.lastError) : "none");
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
	// Windows runs a stream of its own around an endpoint rebuild, and only that run can start
	// the A2DP stream, so whether the endpoint is running is half of the session state.
	AppendLine(text, "endpoint", status.endpointRunning != 0 ? "Running" : "Paused");
	// Windows opens no stream at all while it shows the endpoint unplugged, so a flap that did
	// not end is silence with nothing in the session to show for it.
	if (status.jackUnplugged != 0)
	{
		AppendLine(text, "sink jack", "unplugged (endpoint rebuild in progress; nothing can play)");
	}
	AppendLatency(text, status);
	AppendCodec(text, status);
	AppendMediaMtu(text, status);
	AppendSinkName(text, status);
	AppendSinkFormats(text, status);
	AppendEndpointFormat(text, status);
	AppendPreference(text, preference);
	AppendLine(text, "encoder", status.encoderRegistered != 0 ? "registered" : "not registered");
	AppendBuffers(text, status, preference);
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
