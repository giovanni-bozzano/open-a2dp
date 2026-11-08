#pragma once

// The session report shared by OpenA2dpCtl and OpenA2dpConfig: one formatter, so the window
// shows exactly what `OpenA2dpCtl status` prints and a report copied out of either one can be
// compared with, or pasted into, the other.

#include <OpenA2dp/Ioctl.h>

#include <string>

namespace OpenA2dp::Tools {

// "SBC", "AAC" or "none" for a kCodec* value.
const char* CodecName(uint32_t codec);

// Avdtp::SessionState, as the driver reports it in Status.avdtpState.
const char* AvdtpStateName(uint32_t state);

// Ioctl::MediaState.
const char* MediaStateName(uint32_t state);

// "44100 Hz, 48000 Hz, stereo": what a Status rate/channel mask pair describes, empty
// before the first discovery.
std::string FormatListText(uint32_t rates, uint32_t channels);

// "SBC, AAC": what a Status.sinkCodecs mask describes.
std::string CodecListText(uint32_t codecs);

// "1.2.3" from VersionInfo::driverVersion.
std::string DriverVersionText(uint32_t driverVersion);

struct StatusReportInput
{
	const Ioctl::Status* status = nullptr;
	const Ioctl::Preference* preference = nullptr;
	const Ioctl::VersionInfo* version = nullptr; // null: no driver line
	std::string title;                           // first line: the device, as the caller names it
	bool verbose = false; // append every field of both structures, for a bug report
};

// Lines are '\n' separated; the caller decides what its surface needs. The report is empty
// when status or preference is null.
std::string BuildStatusReport(const StatusReportInput& input);

} // namespace OpenA2dp::Tools
