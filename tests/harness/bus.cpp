#include "harness/bus.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <unistd.h>
#include "harness/child.h"
#include "harness/files.h"

namespace harness {

namespace {

constexpr int serving_timeout_ms = 5000;

struct Bus {
	Child daemon;
	std::string address;
};

Bus system_bus, session_bus;

bool start(Bus &bus, const char *type, const char *name, const std::string &dir) {
	mkdir_p(dir);
	std::string socket = std::format("{}/{}", dir, name);
	std::string conf = socket + ".conf";
	unlink(socket.c_str()); // a stale socket from a killed run
	bool wrote = write_file(conf, std::format(
		"<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
		" \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
		"<busconfig>\n"
		"  <type>{}</type>\n"
		"  <listen>unix:path={}</listen>\n"
		"  <auth>EXTERNAL</auth>\n"
		"  <policy context=\"default\">\n"
		"    <allow own=\"*\"/>\n"
		"    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
		"    <allow eavesdrop=\"true\"/>\n"
		"  </policy>\n"
		"</busconfig>\n", type, socket));
	if (!wrote || !bus.daemon.start({"dbus-daemon", "--config-file", conf, "--print-address=1"})) {
		std::fprintf(stderr, "the private %s bus could not be started\n", type);
		return false;
	}
	// The address the daemon prints only says it is serving.
	if (!bus.daemon.prints("unix:", serving_timeout_ms)) {
		std::fprintf(stderr, "the private %s bus did not start: %s\n", type,
			bus.daemon.since(0).c_str());
		bus.daemon.stop();
		return false;
	}
	bus.address = "unix:path=" + socket;
	return true;
}

} // namespace

bool start_private_buses(const std::string &dir) {
	if (!start(system_bus, "system", "y", dir) || !start(session_bus, "session", "n", dir)) {
		stop_private_buses();
		return false;
	}
	setenv("DBUS_SYSTEM_BUS_ADDRESS", system_bus.address.c_str(), 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", session_bus.address.c_str(), 1);
	return true;
}

const std::string &system_bus_address() {
	return system_bus.address;
}

const std::string &session_bus_address() {
	return session_bus.address;
}

void stop_private_buses() {
	session_bus.daemon.stop();
	system_bus.daemon.stop();
}

} // namespace harness
