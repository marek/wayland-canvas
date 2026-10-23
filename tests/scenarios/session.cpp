// End-to-end session scenario: the shell plugins a login needs — the
// notification server, the locker's PAM check and the password dialog —
// driven in a private world. Before the compositor starts, the scenario
// brings up a dbus-daemon system bus and session bus of its own and points
// every child at them (see harness/bus.h), so the machine's own
// notification server, polkit agent and logind session are never reached.
// XDG config, data and state are scratch directories, and the PAM service
// the locker checks against is a scratch one that accepts one known
// password through pam_exec — never the real login stack and never a real
// password.
#include <cstdlib>
#include <ctime>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <string_view>
#include "core/process.h"
#include "core/server.h"
#include "harness/bus.h"
#include "harness/check.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "platform/layer.h"

using namespace harness;

namespace {

constexpr const char *password = "canvas-scratch-password";

int bell_tries = 0; // the bar's slots tried so far, from the right
int action_tries = 0; // the card's left edge walked so far

// How many pixels of the shot are not black in `box`, -1 when it cannot be
// read.
long ink_in(std::string_view name, wlr_box box) {
	std::optional<Image> image = read_shot(name);
	return image.has_value() ? image->ink_in(box) : -1;
}

// How many pixels differ between two shots in `box`, -1 when either cannot
// be read.
long pixels_changed(std::string_view before, std::string_view after, wlr_box box) {
	std::optional<Image> a = read_shot(before), b = read_shot(after);
	return a.has_value() && b.has_value() ? a->differing(*b, {.within = box}) : -1;
}

bool popup_shown() {
	return layer_named("wayland-canvas-notifications") != nullptr;
}

bool polkit_dialog_shown() {
	return layer_named("wayland-canvas-polkit") != nullptr;
}

void press_enter() {
	tap(KEY_ENTER);
}

// A shell action over the control socket, the way a key or a script runs
// one.
void run_action(std::string_view name, std::string_view args) {
	spawn(std::format("'{}' msg action.run '{{\"name\":\"{}\",\"args\":{}}}'"
		" >/dev/null 2>&1", std::getenv("WAYLAND_CANVAS_BIN"), name, args).c_str());
}

// What the stand-in polkit flow recorded, from the plugin's own storage in
// the run's scratch state.
std::string flow_log() {
	return read_text(scratch() + "/xdg/state/wayland-canvas/plugins/canvas.polkit/test-flow.json");
}

// notify-send on the private session bus, where the shell's own server is
// the only one listening.
void notify(std::string_view args) {
	spawn(std::format("notify-send {} >/dev/null 2>&1", args).c_str());
}

// The fake devices; the shell is coming up with the compositor.
Result devices() {
	attach_pointer();
	attach_keyboard();
	move_to(40, 400);
	return after(1500);
}

// The shell is up: its bar is on screen.
Result shell_is_up() {
	if (auto wait = wait_for(layer_named("wayland-canvas-bar") != nullptr,
			{.budget_ms = 20'000})) {
		return *wait;
	}
	check(layer_named("wayland-canvas-bar") != nullptr, "the shell came up on the private buses");
	return after(500);
}

// A notification arrives.
Result a_notification_arrives() {
	notify("-a Tests 'Scenario summary' 'The body of it'");
	return after(1200);
}

Result the_popup_appears() {
	if (auto wait = wait_for(popup_shown())) {
		return *wait;
	}
	check(popup_shown(), "a popup appeared as the shell's own surface");
	shot("popup");
	return after(300);
}

// It is drawn top-right, and it took no keyboard.
Result the_popup_is_top_right() {
	long ink = ink_in("popup", {screen_w - 360, 26, 352, 160});
	check(ink > 500, std::format("the popup is drawn top-right ({} px of ink)", ink));
	check(server->focus.focused_layer() == nullptr, "no popup ever holds the keyboard");
	return after(0);
}

// The dismiss key takes it off the screen.
Result dismiss_key() {
	chord(KEY_N, mods::super);
	return after(800);
}

Result three_at_once() {
	if (auto wait = wait_for(!popup_shown())) {
		return *wait;
	}
	check(!popup_shown(), "Super+N took the popup off the screen");
	// Three at once, then all of them at once.
	notify("-a Tests 'One' 'first'");
	notify("-a Tests 'Two' 'second'");
	notify("-a Tests -u critical 'Three' 'critical one'");
	return after(1500);
}

Result three_popups_then_dismiss_all() {
	if (auto wait = wait_for(popup_shown())) {
		return *wait;
	}
	check(popup_shown(), "three notifications show as popups");
	chord(KEY_N, mods::super_shift);
	return after(800);
}

Result all_dismissed() {
	if (auto wait = wait_for(!popup_shown())) {
		return *wait;
	}
	check(!popup_shown(), "Super+Shift+N took every popup off the screen");
	return after(300);
}

// The history kept them: the bell's panel lists them. Which slot of the
// bar's right section the bell took depends on what this machine's own
// widgets (battery, bluetooth) offer, so the strip is walked from the right
// until the history panel is the one that opens. A panel another widget
// opens is closed again before the next try, and the theme switch's own slot
// is left alone.
Result find_the_bell() {
	if (layer_named("wayland-canvas-panel-history") != nullptr) {
		return after(300);
	}
	if (bell_tries > 28) {
		check(false, "the bell was not found along the bar");
		return after(300);
	}
	tap(KEY_ESC);
	click(screen_w - 40 - 9 * bell_tries, bar_mid_y());
	bell_tries++;
	return again(1000);
}

Result the_bell_opens_history() {
	check(layer_named("wayland-canvas-panel-history") != nullptr,
		"the bell opens the history panel");
	shot("history");
	return after(300);
}

Result history_lists_what_arrived() {
	const LayerSurface *panel = layer_named("wayland-canvas-panel-history");
	if (panel != nullptr) {
		long ink = ink_in("history", {panel->geo.x, panel->geo.y,
			panel->geo.width, panel->geo.height});
		check(ink > 1000, std::format("the history panel lists what arrived "
			"({} px of ink)", ink));
	}
	tap(KEY_ESC);
	return after(500);
}

// Silenced: a notification arrives and no popup shows.
Result silence_on() {
	run_action("canvas.notifications/silence", "{\"on\":true}");
	return after(800);
}

Result a_silenced_notification() {
	notify("-a Tests 'Silent one' 'nothing should show'");
	return after(1500);
}

Result no_popup_while_silenced() {
	check(!popup_shown(), "a silenced notification shows no popup");
	run_action("canvas.notifications/silence", "{\"on\":false}");
	return after(800);
}

// An action button on a popup reaches the sender.
Result a_popup_with_an_action() {
	spawn(std::format("notify-send -a Tests -A go=Open 'With an action' "
		"'click it' > '{}/action.txt' 2>/dev/null", scratch()).c_str());
	return after(1500);
}

Result the_action_popup_shows() {
	if (auto wait = wait_for(popup_shown())) {
		return *wait;
	}
	check(popup_shown(), "a notification with an action shows its popup");
	shot("action-popup");
	return after(300);
}

// The action button sits at the bottom left of the card, inside the column
// the plugin lays out: the card is 340 wide against the output's right edge,
// below the bar.
Result click_the_action() {
	click(screen_w - 340 - 8 + 10 + 40, 26 + 8 + 86);
	return after(1500);
}

Result the_action_reached_the_sender() {
	std::string chosen = read_text(scratch() + "/action.txt");
	if (chosen.find("go") == std::string::npos && action_tries < 20) {
		// The card's height follows its text: walk down its left edge until
		// the button is hit.
		click(screen_w - 340 - 8 + 10 + 40, 26 + 8 + 70 + 6 * action_tries);
		action_tries++;
		return again(600);
	}
	check(chosen.find("go") != std::string::npos, std::format(
		"clicking the action reached the sender ('{}')", chosen));
	return after(500);
}

// Expiry: a normal popup goes by itself, a critical one stays.
Result a_short_notification() {
	spawn("notify-send -a Tests -t 1000 'Short' 'goes by itself'");
	return after(600);
}

Result the_short_one_is_on_screen() {
	check(popup_shown(), "the short notification is on screen");
	return after(2500); // past its own timeout
}

Result the_short_one_left() {
	if (auto wait = wait_for(!popup_shown(), {.poll_ms = 300, .budget_ms = 12'000})) {
		return *wait;
	}
	check(!popup_shown(), "a notification with a timeout leaves the screen by itself");
	spawn("notify-send -a Tests -u critical 'Critical' 'waits for the user'");
	return after(1200);
}

Result a_critical_one_shows() {
	if (auto wait = wait_for(popup_shown())) {
		return *wait;
	}
	check(popup_shown(), "a critical notification shows");
	return after(4000); // longer than any normal popup's life
}

Result a_critical_one_stays() {
	check(popup_shown(), "a critical notification stays until it is dismissed");
	chord(KEY_N, mods::super_shift);
	return after(800);
}

// The bus name is the session's one notification server.
Result a_second_daemon_asks_for_the_name() {
	check(!popup_shown(), "and the dismiss-all key takes it off the screen");
	spawn(std::format(
		"busctl --address='{}' --user call org.freedesktop.DBus "
		"/org/freedesktop/DBus org.freedesktop.DBus RequestName su "
		"org.freedesktop.Notifications 4 > '{}/name.txt' 2>&1",
		session_bus_address(), scratch()).c_str());
	return after(1200);
}

Result the_shell_keeps_the_name() {
	std::string answer = read_text(scratch() + "/name.txt");
	// 1 is "you are the owner now", 3 "someone else holds it".
	check(answer.find("u 3") != std::string::npos, std::format(
		"a second notification daemon does not take the name from the "
		"shell ('{}')", answer));
	spawn("notify-send -a Tests 'Still ours' 'the shell is the server'");
	return after(1500);
}

Result the_shell_keeps_serving() {
	if (auto wait = wait_for(popup_shown())) {
		return *wait;
	}
	check(popup_shown(), "and it keeps serving the session's notifications");
	chord(KEY_N, mods::super_shift);
	return after(800);
}

// The lock plugin: the shortcut locks the session.
Result lock_shortcut() {
	chord(KEY_L, mods::super);
	return after(1500);
}

Result the_session_is_locked() {
	if (auto wait = wait_for(server->lock.locked())) {
		return *wait;
	}
	check(server->lock.locked(), "the lock shortcut locked the session");
	shot("locked");
	return after(300);
}

// The clock keeps time while locked: a shot just past the next minute.
Result wait_for_the_next_minute() {
	int second = static_cast<int>(std::time(nullptr) % 60);
	return after((60 - second) * 1000 + 2000);
}

Result shot_next_minute() {
	shot("locked-next-minute");
	return after(300);
}

// A wrong password keeps it locked.
Result locker_wrong_password() {
	long moved = pixels_changed("locked", "locked-next-minute",
		{screen_w / 2 - 150, 280, 300, 80});
	check(moved > 100, std::format("the lock clock moved on to the next minute "
		"({} px of it changed)", moved));
	long ink = ink_in("locked", {screen_w / 2 - 200, screen_h / 2 - 120, 400, 240});
	check(ink > 1000, std::format("the locker drew its card ({} px of ink)", ink));
	type_text("wrong-password");
	press_enter();
	return after(2500);
}

Result locker_right_password() {
	check(server->lock.locked(), "a wrong password leaves the session locked");
	type_text(password);
	press_enter();
	return after(2500);
}

Result the_session_is_unlocked() {
	if (auto wait = wait_for(!server->lock.locked())) {
		return *wait;
	}
	check(!server->lock.locked(), "the right password unlocked the session");
	return after(300);
}

// The password dialog, on the plugin's stand-in flow: the machine's own
// polkitd is never registered with.
Result polkit_dialog() {
	run_action("canvas.polkit/test-dialog",
		"{\"message\":\"Install a package\",\"user\":\"tester\","
		"\"password\":\"canvas-scratch-password\"}");
	return after(1200);
}

Result the_dialog_opens_and_holds_the_keyboard() {
	if (auto wait = wait_for(polkit_dialog_shown())) {
		return *wait;
	}
	check(polkit_dialog_shown(), "a privileged action opens the password dialog");
	check(server->focus.focused_layer() != nullptr
			|| server->seat->keyboard_state.focused_surface != nullptr,
		"the dialog holds the keyboard while it is open");
	shot("polkit");
	return after(300);
}

// What it says, then a wrong password.
Result dialog_wrong_password() {
	long ink = ink_in("polkit", {screen_w / 2 - 190, screen_h / 2 - 90, 380, 180});
	check(ink > 2000, std::format("the dialog shows the action and its field "
		"({} px of ink)", ink));
	type_text("wrong-password");
	press_enter();
	return after(1200);
}

Result dialog_stays_open() {
	check(polkit_dialog_shown(), "a wrong password leaves the dialog open");
	check(flow_log().find("refused") != std::string::npos,
		"polkit was told the answer was wrong");
	type_text(password);
	press_enter();
	return after(1200);
}

Result dialog_right_password() {
	if (auto wait = wait_for(!polkit_dialog_shown())) {
		return *wait;
	}
	check(!polkit_dialog_shown(), "the right password closes the dialog");
	check(flow_log().find("authenticated") != std::string::npos,
		"polkit was told the password was right");
	// And a cancel: Escape ends the request without an answer.
	run_action("canvas.polkit/test-dialog",
		"{\"message\":\"Something else\",\"password\":\"never typed\"}");
	return after(1200);
}

Result a_second_request() {
	if (auto wait = wait_for(polkit_dialog_shown())) {
		return *wait;
	}
	check(polkit_dialog_shown(), "a second request opens the dialog again");
	tap(KEY_ESC);
	return after(1200);
}

Result escape_cancels() {
	if (auto wait = wait_for(!polkit_dialog_shown())) {
		return *wait;
	}
	check(!polkit_dialog_shown(), "Escape cancels the dialog");
	check(flow_log().find("cancelled") != std::string::npos,
		"polkit was told the request was cancelled");
	return after(300);
}

} // namespace

int main() {
	if (!begin("session", {"WAYLAND_CANVAS_BIN"})) {
		return 2;
	}
	setenv("GSETTINGS_BACKEND", "keyfile", 1);
	setenv("PYTHONDONTWRITEBYTECODE", "1", 1);

	// Isolation first: without both private buses nothing of the shell may
	// start, so the machine's notification server and polkit agent are
	// never reached.
	if (!start_private_buses(tmp_root() + "/sb")) {
		return 2;
	}

	// The scratch PAM service the locker checks against: pam_exec hands the
	// typed password to a script that compares it with one known string.
	// The real login stack is never read and no real password is typed.
	std::string pam_dir = scratch_dir("pam.d");
	std::string checker = scratch() + "/check-password.sh";
	write_script(checker, std::format(
		"#!/usr/bin/env bash\n"
		"# The scenario's own password check: pam_exec hands the typed\n"
		"# password on stdin (expose_authtok).\n"
		"read -r given\n"
		"[ \"$given\" = '{}' ]\n", password));
	write_file(pam_dir + "/wayland-canvas-scenario", std::format(
		"auth required pam_exec.so expose_authtok quiet {}\n"
		"account required pam_permit.so\n", checker));

	std::string config = scratch_env("XDG_CONFIG_HOME", "xdg/config");
	mkdir_p(config + "/wayland-canvas");
	scratch_env("XDG_DATA_HOME", "xdg/data");
	scratch_env("XDG_STATE_HOME", "xdg/state");
	// The locker is pointed at the scratch service through the plugin's own
	// settings; everything else is the shipped default.
	write_file(config + "/wayland-canvas/shell.json", std::format(
		"{{\n\t\"plugins\": {{\n\t\t\"canvas.lock\": {{\n\t\t\t\"settings\": {{\n"
		"\t\t\t\t\"pamService\": \"wayland-canvas-scenario\",\n"
		"\t\t\t\t\"pamConfigDirectory\": \"{}\"\n\t\t\t}}\n\t\t}}\n\t}}\n}}\n",
		pam_dir));

	Options options; // the shell starts with the compositor: it is the point
	return run(options, {
		{"devices", devices},
		{"shell is up", shell_is_up},
		{"a notification arrives", a_notification_arrives},
		{"the popup appears", the_popup_appears},
		{"the popup is top-right", the_popup_is_top_right},
		{"dismiss key", dismiss_key},
		{"three at once", three_at_once},
		{"three popups, then dismiss all", three_popups_then_dismiss_all},
		{"all dismissed", all_dismissed},
		{"find the bell", find_the_bell},
		{"the bell opens history", the_bell_opens_history},
		{"history lists what arrived", history_lists_what_arrived},
		{"silence on", silence_on},
		{"a silenced notification", a_silenced_notification},
		{"no popup while silenced", no_popup_while_silenced},
		{"a popup with an action", a_popup_with_an_action},
		{"the action popup shows", the_action_popup_shows},
		{"click the action", click_the_action},
		{"the action reached the sender", the_action_reached_the_sender},
		{"a short notification", a_short_notification},
		{"the short one is on screen", the_short_one_is_on_screen},
		{"the short one left", the_short_one_left},
		{"a critical one shows", a_critical_one_shows},
		{"a critical one stays", a_critical_one_stays},
		{"a second daemon asks for the name", a_second_daemon_asks_for_the_name},
		{"the shell keeps the name", the_shell_keeps_the_name},
		{"the shell keeps serving", the_shell_keeps_serving},
		{"lock shortcut", lock_shortcut},
		{"the session is locked", the_session_is_locked},
		{"wait for the next minute", wait_for_the_next_minute},
		{"shot next minute", shot_next_minute},
		{"locker wrong password", locker_wrong_password},
		{"locker right password", locker_right_password},
		{"the session is unlocked", the_session_is_unlocked},
		{"polkit dialog", polkit_dialog},
		{"the dialog opens and holds the keyboard", the_dialog_opens_and_holds_the_keyboard},
		{"dialog wrong password", dialog_wrong_password},
		{"dialog stays open", dialog_stays_open},
		{"dialog right password", dialog_right_password},
		{"a second request", a_second_request},
		{"escape cancels", escape_cancels},
	});
}
