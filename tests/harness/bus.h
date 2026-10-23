#pragma once

// The private D-Bus world: a system bus and a session bus of the run's own,
// so nothing the scenario starts can reach the machine's notification
// server, polkit agent, logind session or login stack. Everything is
// allowed on them and they exist for this run only. A killed scenario takes
// its daemons with it.

#include <string>

namespace harness {

// Starts a dbus-daemon of each kind with sockets under `dir`, waits until
// both serve, and points DBUS_SYSTEM_BUS_ADDRESS and DBUS_SESSION_BUS_ADDRESS
// at them. A unix socket path is capped near 108 bytes, which a scenario
// folder under a worktree can spend on its own: `dir` is the shortest path
// there is under the scratch root (for example <tmp_root>/sb). False,
// explained on stderr, when either did not start; nothing is left running.
bool start_private_buses(const std::string &dir);

// The bus addresses start_private_buses exported: the plain listen form,
// which dbus-python can connect to (the printed one carries a guid it
// never finishes connecting to).
const std::string &system_bus_address();
const std::string &session_bus_address();

// Stops both daemons, SIGTERM first.
void stop_private_buses();

} // namespace harness
