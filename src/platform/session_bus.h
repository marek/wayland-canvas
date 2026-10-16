#pragma once

#include <string_view>

namespace session_bus {

// Whose D-Bus session bus the compositor runs on, which decides whether it
// may hand its WAYLAND_DISPLAY to the services the bus activates — the
// screen-sharing portal first among them.
enum class Owner {
	none, // no session bus at all: nothing to hand over
	host, // the user's systemd bus in a run that is not a login (nested,
	      // headless): it belongs to the host session and is left alone
	login, // a login session on the user's systemd bus: hand over through
	       // systemd, and restart the portals an earlier session left
	       // running
	own, // a bus of the run's own (dbus-run-session): hand over on it
};

// `address` is $DBUS_SESSION_BUS_ADDRESS, `runtime_dir` $XDG_RUNTIME_DIR
// (either may be empty), `login` whether the compositor opened a seat
// session from a TTY or a display manager.
Owner owner(std::string_view address, std::string_view runtime_dir, bool login);

}
