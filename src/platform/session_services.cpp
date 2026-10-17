#include "platform/session_services.h"

#include <format>

namespace session_services {

bool manages(session_bus::Owner owner) {
	return owner == session_bus::Owner::login;
}

std::string start_command() {
	return std::format("systemctl --user stop {} ; systemctl --user start {}",
		target, target);
}

std::string stop_command() {
	return std::format("systemctl --user stop {}", target);
}

}
