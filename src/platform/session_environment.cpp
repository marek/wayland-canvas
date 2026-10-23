#include "platform/session_environment.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>

#include "core/process.h"
#include "core/text.h"
#include "platform/session_services.h"

namespace {

// The desktop name every client sees; "wlroots" lets the portal pick its
// wlroots backend where our portals config is not installed.
constexpr const char *desktop_name = "wayland-canvas:wlroots";
// The portals a hand-over and a hand-back restart, so they serve the display
// the systemd environment now names.
constexpr const char *portal_units =
	"xdg-desktop-portal.service xdg-desktop-portal-wlr.service xdg-desktop-portal-gtk.service";

// The handed-over values in the user's systemd environment, read now.
session_bus::Handed systemd_handed() {
	FILE *pipe = popen("systemctl --user show-environment", "r");
	if (pipe == nullptr) {
		return {};
	}
	std::string out;
	char chunk[4096];
	std::size_t n = 0;
	while ((n = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) {
		out.append(chunk, n);
	}
	pclose(pipe);
	return session_bus::handed_in(out);
}

session_bus::Owner bus_owner(bool login) {
	const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	return session_bus::owner(address ? address : "", runtime ? runtime : "", login);
}

// Puts this compositor's WAYLAND_DISPLAY and desktop name into the
// activation environment of the services the session bus starts — the
// screen-sharing portal and its wlroots backend first among them — when
// the bus is this run's to take. A login returns the values it replaced,
// for the hand-back on exit.
std::optional<session_bus::Handed> hand_over_session_bus(session_bus::Owner owner) {
	switch (owner) {
	case session_bus::Owner::none:
	case session_bus::Owner::host:
		return std::nullopt;
	case session_bus::Owner::own:
		spawn("dbus-update-activation-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP");
		return std::nullopt;
	case session_bus::Owner::login: {
		session_bus::Handed before = systemd_handed();
		// Portals an earlier session left running still serve that
		// session's display: restart the ones that run.
		spawn(std::format("dbus-update-activation-environment --systemd WAYLAND_DISPLAY"
			" XDG_CURRENT_DESKTOP && systemctl --user try-restart {}", portal_units).c_str());
		return before;
	}
	}
	return std::nullopt;
}

// Gives the portal back to the session a login took it from: the values it
// replaced restored (unset ones unset) and the portals restarted, unless
// another session has taken it since.
void hand_back_session_bus(const session_bus::HandOver &hand_over) {
	std::optional<session_bus::Handed> restore =
		session_bus::hand_back(hand_over, systemd_handed());
	if (!restore.has_value()) {
		return;
	}
	std::string set, unset;
	auto add = [&set, &unset](const char *name, const std::optional<std::string> &value) {
		if (value.has_value()) {
			set += " " + text::shell_quote(std::format("{}={}", name, *value));
			return;
		}
		unset += std::format(" {}", name);
	};
	add("WAYLAND_DISPLAY", restore->wayland_display);
	add("XDG_CURRENT_DESKTOP", restore->current_desktop);
	std::string cmd;
	if (!set.empty()) {
		cmd += "dbus-update-activation-environment --systemd" + set + " && ";
	}
	if (!unset.empty()) {
		cmd += "systemctl --user unset-environment" + unset + " && ";
	}
	spawn((cmd + "systemctl --user try-restart " + portal_units).c_str());
}

} // namespace

SessionEnvironment::SessionEnvironment(std::string_view socket, bool login) {
	// Every client is in a wayland-canvas session.
	setenv("XDG_CURRENT_DESKTOP", desktop_name, true);
	session_bus::Owner owner = bus_owner(login);
	if (std::optional<session_bus::Handed> before = hand_over_session_bus(owner)) {
		hand_over_ = session_bus::HandOver{
			.ours = {.wayland_display = std::string(socket), .current_desktop = desktop_name},
			.before = *before,
		};
	}
	// The services come up once the environment is where the user manager
	// can see it.
	if (session_services::manages(owner)) {
		manages_services_ = true;
		spawn(session_services::start_command().c_str());
	}
}

SessionEnvironment::~SessionEnvironment() {
	if (manages_services_) {
		// The session ends: its services go with it. A crash runs nothing
		// here, and the next login stops what it left behind.
		spawn(session_services::stop_command().c_str());
	}
	if (hand_over_.has_value()) {
		hand_back_session_bus(*hand_over_);
	}
}
