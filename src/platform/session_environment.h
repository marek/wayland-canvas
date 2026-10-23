#pragma once

#include <optional>
#include <string_view>
#include "platform/session_bus.h"

// What this run does to the user's session besides serving clients: names
// itself to them, hands its display to the services the session bus
// activates, and — in a login — starts the session's own services
// (docs/intents/platform/protocols.md, docs/intents/workspace/session-services.md).
// Ending the run undoes what a login took: the destructor stops the services
// this run started and gives the portal back.
class SessionEnvironment {
public:
	// `socket` is the Wayland display's name; `login` whether the compositor
	// opened a seat session from a TTY or a display manager.
	SessionEnvironment(std::string_view socket, bool login);
	~SessionEnvironment();
	SessionEnvironment(const SessionEnvironment &) = delete;
	SessionEnvironment &operator=(const SessionEnvironment &) = delete;

private:
	// A login's hand-over of the portal, handed back on exit.
	std::optional<session_bus::HandOver> hand_over_;
	// Whether this run started the session's services and so stops them.
	bool manages_services_ = false;
};
