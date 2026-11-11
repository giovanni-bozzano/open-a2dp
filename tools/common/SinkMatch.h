#pragma once

// Deciding which node, driver and program a switch is about.
//
// These are the decisions with a consequence: matching the wrong BTHENUM node moves the wrong
// sink's driver, and pausing the wrong program interrupts audio that had nothing to do with the
// switch. They are pure string work - no PnP, no COM - so they live here instead of in
// SinkDriver.cpp and SinkPause.cpp, where they could not be tested without a paired sink.

#include "SinkDriver.h"

#include <string>
#include <vector>

namespace OpenA2dp::Tools {

// The AudioSink service UUID: the only BTHENUM node a sink's A2DP source driver can be
// replaced on. The radio and the other profile nodes are never candidates.
inline constexpr wchar_t kA2dpNodeMarker[] = L"{0000110B-";

// Case-insensitive substring test for the wide strings the PnP and Bluetooth APIs hand out.
bool ContainsNoCase(const std::wstring& text, const wchar_t* needle);

// Which driver a node's service name names; Unknown when it is neither.
SinkDriver DriverOfServiceName(const std::wstring& service);

// True when a BTHENUM instance id carries `reference` (12 hex digits). Some builds write the
// address as it reads, others byte-reversed, so both orders identify the sink.
bool MatchesSinkAddress(const std::wstring& instanceId, const std::string& reference);

// The address a BTHENUM instance id carries: its last run of 12 hex digits, upper case, in the
// order the stack wrote it. Empty when there is none.
std::string AddressInInstanceId(const std::wstring& instanceId);

// True for the A2DP source service itself as a driver entry's matching id: the UUID and nothing
// after its closing brace. `..._GENERIC` and `..._LOCALMFG&nnnn` say something else.
bool IsA2dpServiceId(const std::wstring& id);

// The program name a media session's app id carries, for a message: the part after the last
// separator of the app id. Empty when there is none.
std::string PlayerTitleFromAppId(const std::string& appId);

// True when a media session app id belongs to one of the programs rendering to the sink, matched
// on the image name without its extension.
bool AppIdMatchesPlayer(const std::string& appId, const std::vector<std::string>& players);

} // namespace OpenA2dp::Tools
