#pragma once

// Whether a program is playing to a sink's A2DP endpoint, and how to make it stop.
//
// A driver switch restarts the sink's BTHENUM node, and Windows asks every holder of that node
// for its consent first: the audio engine refuses while it renders to the endpoint, and the node
// then cannot be restarted at all. The handle that blocks it belongs to the audio engine, so no
// tool can close it; only the program that renders can end its session. This header answers who
// that is, and asks it to pause through the media transport manager, which is the supported way.

#include <string>
#include <vector>

namespace OpenA2dp::Tools {

struct SinkPlayback
{
	// The node's endpoint and its sessions were read, so `playing` is meaningful.
	bool known = false;
	// A session on the endpoint is active, which is audio actually being rendered.
	bool playing = false;
	// The programs rendering to it, as image names ("spotify.exe"): what holds the node, and
	// what a refusal has to name so the reader knows what to stop.
	std::vector<std::string> players;
};

// Reads whether the sink is playing and who renders to it. False when the endpoint cannot be
// found or read, so an unknown is never reported as playback.
bool ReadSinkPlayback(const std::string& nodeInstanceId, SinkPlayback& playback);

// Asks the programs rendering to the sink to pause, which is what lets the audio engine release
// the node; `paused` names what was paused, for the message. Best effort: a program that
// publishes no media session cannot be asked, and this never ends a stream the way taking the
// device away would.
bool PauseSinkPlayback(const std::vector<std::string>& players, std::string& paused,
					   std::string& error);

} // namespace OpenA2dp::Tools
