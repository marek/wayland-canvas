#pragma once

#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

#include "canvas/overview.h"

class Output;

// Where a window traced to a launch from the overview opens: the overview's
// target at the launch, at the zoom leaving into it rests at, on the output
// the launch was made on (whose bars inset it).
struct LaunchSpot {
	OverviewTarget target;
	double zoom = 1;
	std::string output;
};

// Launches made from the overview. Every window a launch's processes open
// within config::launch_trace_ms of it opens in the launch's spot, whatever
// the overview has done since. A window no launch can be traced to (one
// handed to an instance already running, one started over D-Bus) is placed
// by the usual rules.
class Launches {
public:
	// `pid` was started from `output`'s open overview; -1 (a failed spawn)
	// records nothing.
	void add(pid_t pid, const Output &output);
	// The spot of the launch `pid` descends from: the launched process
	// itself or one of its ancestors.
	std::optional<LaunchSpot> spot_for(pid_t pid);

private:
	struct Launch {
		pid_t pid = 0;
		LaunchSpot spot;
		double expires_ms = 0;
	};
	std::vector<Launch> launches_;
};
