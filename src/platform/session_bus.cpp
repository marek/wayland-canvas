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

// The value of `name` in `systemctl --user show-environment` output.
std::optional<std::string> value_of(std::string_view env, std::string_view name) {
	while (!env.empty()) {
		std::size_t end = env.find('\n');
		std::string_view line = env.substr(0, end);
		if (line.size() > name.size() && line.starts_with(name) && line[name.size()] == '=') {
			return std::string(line.substr(name.size() + 1));
		}
		if (end == std::string_view::npos) {
			break;
		}
		env.remove_prefix(end + 1);
	}
	return std::nullopt;
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

Handed handed_in(std::string_view show_environment) {
	return {
		.wayland_display = value_of(show_environment, "WAYLAND_DISPLAY"),
		.current_desktop = value_of(show_environment, "XDG_CURRENT_DESKTOP"),
	};
}

std::optional<Handed> hand_back(const HandOver &hand_over, const Handed &now) {
	if (now != hand_over.ours) {
		return std::nullopt;
	}
	return hand_over.before;
}

}
