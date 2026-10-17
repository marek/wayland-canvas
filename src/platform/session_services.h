#pragma once

#include <string>
#include "platform/session_bus.h"

// The services a graphical login brings with it: `graphical-session.target`
// and XDG autostart, started through this session's own systemd target
// (docs/intents/workspace/session-services.md). Pure strings and rules, so
// the decisions are testable without a systemd manager.
namespace session_services {

// The unit the package installs; a login starts it and stops it.
inline constexpr const char *target = "wayland-canvas-session.target";

// Whether this run owns the user's session services. Only a login on the
// user's own bus does: a nested or headless run shares that bus with the
// session that already owns them, and a run on a bus of its own has no
// user manager to start anything in.
bool manages(session_bus::Owner owner);

// What a login runs once it has handed its environment over. A previous
// session that crashed left the target running, so it is stopped first:
// the services of a session that is gone never outlive the next login.
std::string start_command();

// What it runs when the session ends.
std::string stop_command();

}
