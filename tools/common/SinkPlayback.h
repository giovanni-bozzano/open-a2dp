#pragma once

// Whether a program is playing to a sink's A2DP endpoint.
//
// A driver switch restarts the sink's BTHENUM node, and PnP asks every holder of that node for
// its consent first: the audio engine refuses while it plays to the endpoint. Windows then only
// stages the driver, and the restart is vetoed after several seconds. Asking the audio engine
// first answers the same question in milliseconds, for either A2DP source driver.

#include <string>

namespace OpenA2dp::Tools {

// True when the endpoint Windows created for this A2DP node has a session in the active state,
// which is audio actually being rendered. `plays` is left false when the endpoint or its
// sessions cannot be read, so an unknown is never reported as playback.
bool SinkPlaysAudio(const std::string& nodeInstanceId, bool& plays);

} // namespace OpenA2dp::Tools
