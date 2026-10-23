#pragma once

// A raw client of the compositor's control socket (docs/intents/platform/
// ipc.md), for what `wayland-canvas msg` cannot send: malformed lines, many
// requests at once, a half-closed connection. It runs in a forked child,
// because the compositor shares the scenario's thread and nothing here may
// block it; what the compositor sends back is written to a file in the
// scratch directory, which the scenario reads as it grows.

#include <string>
#include <string_view>
#include <sys/types.h>

namespace harness {

struct ControlClient {
	// Half-closes the connection after sending: the compositor sees EOF.
	bool half_close = false;
	// The client goes on listening through this many quiet 400 ms polls; it
	// stops at once on EOF or after 10 s.
	int quiet_polls = 2;
};

// Sends `to_send` (raw lines) on a new connection and captures the answer in
// <scratch>/<out>, with a closing "EOF" line when the compositor hung up.
// Returns the child's pid; -1 when the fork failed.
pid_t fork_control_client(std::string_view to_send, std::string_view out,
	ControlClient options = {});

// What the client writing <scratch>/<out> has captured so far.
std::string control_answer(std::string_view out);

// Reaps a client that has exited; true once it is gone. A pid of 0 or less
// is gone already.
bool control_client_done(pid_t pid);

// Kills the clients still running: called by run().
void stop_control_clients();

} // namespace harness
