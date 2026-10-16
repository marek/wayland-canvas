#include "platform/session_bus.h"

#include <string>

namespace session_bus {

namespace {

// The address systemd gives the user's bus: unix:path=$XDG_RUNTIME_DIR/bus,
// possibly followed by more key=value pairs after a comma.
bool is_user_bus(std::string_view address, std::string_view runtime_dir) {
	if (runtime_dir.empty()) {
		return false;
	}
	std::string expected = "unix:path=" + std::string(runtime_dir) + "/bus";
	if (!address.starts_with(expected)) {
		return false;
	}
	std::string_view rest = address.substr(expected.size());
	return rest.empty() || rest.front() == ',';
}

} // namespace

Owner owner(std::string_view address, std::string_view runtime_dir, bool login) {
	if (address.empty()) {
		return Owner::none;
	}
	if (!is_user_bus(address, runtime_dir)) {
		// Any other bus was started for this run (dbus-run-session, or a
		// login without systemd's user bus).
		return Owner::own;
	}
	return login ? Owner::login : Owner::host;
}

}
