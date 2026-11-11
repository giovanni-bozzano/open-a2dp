#pragma once

// Which A2DP source driver a sink is bound to, and switching it.
//
// Windows gives each remote sink its own BTHENUM node for the AudioSink service (UUID 0x110B),
// and that node's function driver is the A2DP source: the inbox BthA2dp, or OpenA2dp. Both
// INFs match the same node, so a sink moves between them by installing the other package on
// that one node - the other sinks keep the driver they are on. This is what "Update driver"
// does in Device Manager, without the UI.

#include <string>
#include <vector>

namespace OpenA2dp::Tools {

enum class SinkDriver {
	Unknown,  // no A2DP node for the sink, or one this tool does not recognise
	Windows,  // the inbox A2DP source driver
	OpenA2dp, // this driver
};

// "BthA2dp", "OpenA2dp" or "unknown", for the report and the window.
const char* SinkDriverName(SinkDriver driver);

// The other driver, for a toggle: Unknown maps to Unknown.
SinkDriver OtherDriver(SinkDriver driver);

struct SinkDriverState
{
	SinkDriver current = SinkDriver::Unknown;
	// The other package is in the driver store, so the sink can be moved to it.
	bool switchable = false;
	// `current` is installed on the node but not loaded yet: its restart was refused.
	bool restartPending = false;
	// The A2DP devnode carrying this sink's address.
	std::string instanceId;
	std::string windowsInf; // full path to the inbox A2DP INF; empty when it is not a candidate
	std::string openA2dpInf; // full path to the OpenA2dp INF in the driver store
	// Why a switch is not possible right now; empty when it is. Written for a reader, not for
	// a log: it is shown next to the sink.
	std::string problem;
};

// One sink as PnP sees it, whether or not this driver owns a control device for it: a sink on
// the inbox driver has an A2DP node and no control device, which is exactly the sink the tools
// have to be able to find in order to take it over.
struct SinkNode
{
	std::string reference; // 12 hex digits, the form the control device is named after
	SinkDriver driver = SinkDriver::Unknown;
	std::string instanceId;
};

// Every A2DP source node and which driver it is bound to, sorted by address. `error` is set
// only when PnP cannot be enumerated at all.
bool EnumerateSinkNodes(std::vector<SinkNode>& nodes, std::string& error);

// Finds the A2DP devnode for a sink address and the drivers it could bind. Read-only; nothing
// is installed and no node is touched.
bool ReadSinkDriver(const std::string& reference, SinkDriverState& state, std::string& error);

enum class SwitchOutcome {
	Switched,   // the node now runs `target`
	PlaysAudio, // nothing was changed: a program holds the node and could not be paused
	Failed,     // nothing was changed, or the node kept the driver it had; `error` says why
};

struct SwitchReport
{
	// The program paused to free the node, as "spotify.exe"; empty when nothing had to be paused.
	std::string pausedPlayer;
};

// Installs `target`'s package on that one devnode and restarts it, so the sink's endpoints and
// any open stream go with it. Needs an elevated prompt; `error` names what failed. The sink
// keeps its pairing and its other nodes. A program rendering to the sink keeps the node open,
// and Windows refuses to restart a node with an outstanding open, so that program is paused
// first (see SinkPlayback.h): the audio engine, not the driver, owns the handle. A program that
// cannot be paused makes the switch refuse before anything is touched, and `error` names it.
SwitchOutcome SwitchSinkDriver(const std::string& reference, SinkDriver target, SwitchReport& report,
							   std::string& error);

} // namespace OpenA2dp::Tools
