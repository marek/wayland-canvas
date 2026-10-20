// Unit tests for src/platform/session_bus.cpp. Self-contained: no framework,
// links only session_bus.cpp. Run via `meson test` (test 'session-bus').

#include "check.h"
#include "platform/session_bus.h"

namespace {

using unit::check;

using session_bus::Handed;
using session_bus::HandOver;
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

// The two values among the rest of systemd's environment; a longer name
// that starts the same is another variable.
void test_handed_in() {
	Handed both = session_bus::handed_in(
		"HOME=/home/marek\nWAYLAND_DISPLAYX=no\nWAYLAND_DISPLAY=wayland-1\n"
		"XDG_CURRENT_DESKTOP=Hyprland\nXDG_SESSION_TYPE=wayland\n");
	check(both.wayland_display == "wayland-1" && both.current_desktop == "Hyprland");
	Handed last_line = session_bus::handed_in("PATH=/usr/bin\nXDG_CURRENT_DESKTOP=Hyprland");
	check(!last_line.wayland_display.has_value() && last_line.current_desktop == "Hyprland");
	Handed none = session_bus::handed_in("HOME=/home/marek\n");
	check(!none.wayland_display.has_value() && !none.current_desktop.has_value());
}

// On exit the replaced values come back, unset ones included, unless
// another session has taken the portal since.
void test_hand_back() {
	Handed ours{.wayland_display = "wayland-0", .current_desktop = "wayland-canvas:wlroots"};
	Handed hyprland{.wayland_display = "wayland-1", .current_desktop = "Hyprland"};
	HandOver from_hyprland{.ours = ours, .before = hyprland};
	check(session_bus::hand_back(from_hyprland, ours) == hyprland);
	HandOver from_nothing{.ours = ours, .before = {}};
	check(session_bus::hand_back(from_nothing, ours) == Handed{});
	// A session started since took it: theirs stays.
	Handed later{.wayland_display = "wayland-2", .current_desktop = "sway"};
	check(!session_bus::hand_back(from_hyprland, later).has_value());
	Handed half_taken{.wayland_display = "wayland-0", .current_desktop = "sway"};
	check(!session_bus::hand_back(from_hyprland, half_taken).has_value());
}

} // namespace

int main() {
	test_host_bus_left_alone();
	test_login_on_user_bus();
	test_own_bus();
	test_near_misses_are_other_buses();
	test_no_bus();
	test_handed_in();
	test_hand_back();
	return unit::summary();
}
