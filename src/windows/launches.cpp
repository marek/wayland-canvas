#include <algorithm>
#include <cstdio>
#include <cstring>

#include "windows/launches.h"
#include "core/config.h"
#include "core/process.h"
#include "platform/output.h"

namespace {

// Levels walked up the process tree before giving up: a launch reaches its
// app through /bin/sh and at most a few wrappers (flatpak, a launcher script).
constexpr int max_ancestry = 32;

// The parent of `pid`, from /proc/<pid>/stat ("pid (comm) state ppid ..."),
// or 0 when it is gone. The command name may hold spaces and parentheses, so
// the fields after it start past the last ')'.
pid_t parent_of(pid_t pid) {
	char path[32];
	std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
	FILE *file = std::fopen(path, "r");
	if (file == nullptr) {
		return 0;
	}
	// The name is at most 16 bytes: the parent's pid comes well within this.
	char stat[128];
	std::size_t n = std::fread(stat, 1, sizeof stat - 1, file);
	std::fclose(file);
	stat[n] = '\0';
	const char *end = std::strrchr(stat, ')');
	char state = 0;
	int ppid = 0;
	if (end == nullptr || std::sscanf(end + 1, " %c %d", &state, &ppid) != 2) {
		return 0;
	}
	return ppid;
}

} // namespace

void Launches::add(pid_t pid, const Output &output) {
	if (pid <= 0) {
		return;
	}
	launches_.push_back({
		.pid = pid,
		.spot = {
			.target = output.overview.target(),
			.zoom = output.overview.target_camera().zoom,
			.output = output.wlr->name,
		},
		.expires_ms = now_ms() + config::launch_trace_ms,
	});
}

std::optional<LaunchSpot> Launches::spot_for(pid_t pid) {
	double now = now_ms();
	std::erase_if(launches_, [now](const Launch &launch) { return launch.expires_ms <= now; });
	// The launched shell may exec the app or start it as a child, and the app
	// may connect from a process of its own: walk up from the client.
	for (int depth = 0; !launches_.empty() && pid > 1 && depth < max_ancestry; depth++) {
		auto found = std::ranges::find(launches_, pid, &Launch::pid);
		if (found != launches_.end()) {
			return found->spot;
		}
		pid = parent_of(pid);
	}
	return std::nullopt;
}
