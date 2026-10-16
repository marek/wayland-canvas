// Unit tests for src/platform/session_bus.cpp. Self-contained: no framework,
// links only session_bus.cpp. Run via `meson test` (test 'session-bus').

#include <cstdio>
#include <source_location>
#include "platform/session_bus.h"

namespace {

int failures = 0;

void check(bool cond, std::source_location loc = std::source_location::current()) {
	if (!cond) {
		failures++;
		std::printf("FAIL %s:%u: %s\n", loc.file_name(), loc.line(), loc.function_name());
	}
}

using session_bus::Owner;
using session_bus::owner;

constexpr const char *run = "/run/user/1000";

// A run nested in, or beside, another session on the user's systemd bus
// never takes that session's activation environment.
void test_host_bus_left_alone() {
	check(owner("unix:path=/run/user/1000/bus", run, false) == Owner::host);
	check(owner("unix:path=/run/user/1000/bus,guid=0123abcd", run, false) == Owner::host);
}

// A login on the same bus hands over through systemd.
void test_login_on_user_bus() {
	check(owner("unix:path=/run/user/1000/bus", run, true) == Owner::login);
	check(owner("unix:path=/run/user/1000/bus,guid=0123abcd", run, true) == Owner::login);
}

// dbus-run-session's bus, or any bus not systemd's, is the run's own,
// login or not.
void test_own_bus() {
	const char *private_bus = "unix:path=/tmp/dbus-X7fbcpGG5G,guid=1da382f4";
	check(owner(private_bus, run, false) == Owner::own);
	check(owner(private_bus, run, true) == Owner::own);
	check(owner("unix:abstract=/tmp/dbus-Q,guid=9", run, false) == Owner::own);
}

// Only the exact user bus counts: a longer path or another user's
// runtime dir is a different bus.
void test_near_misses_are_other_buses() {
	check(owner("unix:path=/run/user/1000/bus2", run, false) == Owner::own);
	check(owner("unix:path=/run/user/1001/bus", run, false) == Owner::own);
	check(owner("unix:path=/run/user/1000/bus", "", false) == Owner::own);
}

void test_no_bus() {
	check(owner("", run, false) == Owner::none);
	check(owner("", run, true) == Owner::none);
}

} // namespace

int main() {
	test_host_bus_left_alone();
	test_login_on_user_bus();
	test_own_bus();
	test_near_misses_are_other_buses();
	test_no_bus();
	if (failures != 0) {
		std::printf("%d session-bus test failure(s)\n", failures);
		return 1;
	}
	std::printf("all session-bus tests passed\n");
	return 0;
}
