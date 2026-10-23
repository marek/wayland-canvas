// End-to-end lock scenario: ext-session-lock-v1 against the scenarios' own
// locker (tests/clients/lock-client.c). Locking hides the windows on screen
// and in a capture, keys, clicks and gestures reach only the locker, a
// killed locker leaves the session locked, the next locker takes it over
// and unlocks it, and the triggers - the `lock` action and the idle
// timeout, with an idle inhibitor holding it off - ask the shell to lock.
// Writes only under $CANVAS_TMP; never touches the machine's logind
// session, and the run's canvas.conf lives in its own scratch config dir.
#include <csignal>
#include <cstdlib>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <string_view>
#include "core/config.h"
#include "core/server.h"
#include "core/process.h"
#include "core/settings.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/run.h"
#include "platform/output.h"
#include "render/capture.h"
#include "windows/view.h"

using namespace harness;

namespace {

Child locker; // the locker under test
Child events; // the events subscriber: what the compositor asks the shell to do

// The locker's own fill, as the scenarios' locker draws it (lock-client.c).
constexpr uint32_t locker_fill = 0xff3050a0;

// How many times the compositor has asked the shell to run the lock action.
int lock_asks() {
	return count_of(events.said(), config::lock_action);
}

// A shot's share of the colour `argb`, -1 when it cannot be read.
double share_of(std::string_view name, uint32_t argb) {
	std::optional<Image> image = read_shot(name);
	return image.has_value() ? image->share_of(argb) : -1;
}

Result window_and_subscriber() {
	attach_keyboard();
	attach_pointer();
	spawn("foot -T lock sh -c 'echo lock scenario; exec sleep 600'");
	check(events.start({std::getenv("WAYLAND_CANVAS_BIN"), "msg", "subscribe"}),
		"the events subscriber started");
	return after(500);
}

Result unlocked_start() {
	if (auto wait = wait_for(first_mapped_view() != nullptr && settled())) {
		return *wait;
	}
	check(first_mapped_view() != nullptr, "one foot window mapped");
	check(!server->lock.locked(), "the session starts unlocked");
	move_to(40, 40);
	return after(1500);
}

Result shot_unlocked() {
	shot("unlocked");
	return after(300);
}

Result lock_action() {
	double fill = share_of("unlocked", argb_of(config::lock_fill));
	check(fill >= 0 && fill < 0.9,
		std::format("the unlocked screen is not an opaque fill ({:.2f})", fill));
	chord(KEY_L, mods::super);
	return after(400);
}

Result action_reached_the_shell() {
	check(lock_asks() >= 1, "the lock shortcut pushed the locker's action to the shell");
	check(!server->lock.locked(), "nothing is locked until a locker takes the protocol");
	check(locker.start_client("CANVAS_LOCK_CLIENT"), "the scenarios' locker started");
	return after(600);
}

Result locker_took_the_session() {
	if (auto wait = wait_for(locker.count("LOCKED") > 0)) {
		return *wait;
	}
	check(locker.count("LOCKED") == 1, "the compositor confirmed the lock");
	check(server->lock.locked(), "the session is locked");
	check(locker.count("DRAWN") >= 1, "the locker drew the output");
	return after(600);
}

Result shot_locked() {
	shot("locked");
	return after(300);
}

Result locked_screen() {
	double fill = share_of("locked", locker_fill);
	check(fill > 0.99, std::format("the locked screen is the locker's own fill ({:.3f})", fill));
	View *view = first_mapped_view();
	bool refused = view == nullptr || capture_view(*view, 1.0f) == nullptr;
	check(refused, "a window capture is refused while locked");
	return after(0);
}

Result keys_reach_only_the_locker() {
	bool tiled_before = output().desktop_showing() != nullptr;
	chord(KEY_T, mods::super); // the tiling shortcut, if anything heard it
	tap(KEY_A);
	check(output().desktop_showing() != nullptr ? tiled_before : !tiled_before,
		"a canvas shortcut did nothing while locked");
	return after(300);
}

Result gestures_and_clicks_reach_only_the_locker() {
	check(locker.count("KEY") >= 2,
		std::format("the keys reached the locker ({} seen)", locker.count("KEY")));
	Camera before = output().camera.current();
	swipe(3, 300, 0);
	check(output().camera.current().cx == before.cx
			&& output().camera.current().cy == before.cy,
		"a three-finger swipe moved no camera while locked");
	click(400, 300);
	return after(300);
}

Result click_reached_the_locker_then_it_dies() {
	check(locker.count("BUTTON") >= 1, "the click reached the locker");
	check(server->seat->pointer_state.focused_surface == server->lock.surface_for(output()),
		"the pointer focus is the locker's surface");
	// The locker dies: the session must stay locked.
	locker.kill();
	return after(600);
}

Result killed_locker_leaves_it_locked() {
	check(server->lock.locked(), "a killed locker leaves the session locked");
	check(server->lock.abandoned(), "with nothing drawing it");
	output().damage();
	return after(400);
}

Result shot_abandoned() {
	shot("abandoned");
	return after(300);
}

Result next_locker() {
	double fill = share_of("abandoned", argb_of(config::lock_fill));
	check(fill > 0.99, std::format("a dead locker leaves an opaque fill ({:.3f})", fill));
	check(locker.start_client("CANVAS_LOCK_CLIENT"), "a second locker started");
	return after(600);
}

Result new_locker_took_over() {
	if (auto wait = wait_for(locker.count("DRAWN") > 0)) {
		return *wait;
	}
	check(locker.count("LOCKED") == 1, "the new locker took the lock over");
	locker.signal(SIGUSR1); // unlock_and_destroy
	return after(600);
}

Result locker_unlocked() {
	if (auto wait = wait_for(!server->lock.locked())) {
		return *wait;
	}
	check(!server->lock.locked(), "the locker unlocked the session");
	check(locker.count("UNLOCKED") == 1, "it said so itself");
	locker.stop();
	output().damage();
	return after(600);
}

Result shot_unlocked_again() {
	shot("unlocked-again");
	return after(300);
}

Result windows_are_back() {
	double fill = share_of("unlocked-again", locker_fill);
	check(fill < 0.01, std::format("nothing of the locker is left on screen ({:.3f})", fill));
	check(first_mapped_view() != nullptr, "the window is still there");
	return after(0);
}

// The idle timeout: armed from canvas.conf, and what it does.
Result idle_timeout() {
	check(server->conf.resolved.lock_idle_minutes == settings::lock_idle.fallback,
		"canvas.conf starts with the default idle lock");
	check(server->lock.idle_armed(), "the idle countdown is running");
	server->lock.idle_elapsed();
	check(server->lock.requested(), "the idle timeout asked for the lock");
	return after(400);
}

Result idle_asked_the_shell() {
	int asks = lock_asks();
	check(asks >= 2, std::format("the idle timeout asked the shell to lock ({} asks)", asks));
	// An idle inhibitor holds the idle lock off.
	check(locker.start_client("CANVAS_LOCK_CLIENT", {"--inhibit-idle"}),
		"a locker holding an idle inhibitor started");
	return after(800);
}

Result inhibitor_holds_idle_off() {
	if (auto wait = wait_for(locker.count("DRAWN") > 0)) {
		return *wait;
	}
	check(!server->lock.idle_armed(), "an idle inhibitor stops the idle countdown");
	locker.signal(SIGUSR1);
	return after(800);
}

Result countdown_runs_again() {
	if (auto wait = wait_for(!server->lock.locked())) {
		return *wait;
	}
	check(server->lock.idle_armed(), "the countdown runs again once the inhibitor is gone");
	locker.stop();
	return after(300);
}

// A headless run is no login: it starts no session services.
Result no_session_services() {
	std::string calls = read_text(scratch() + "/systemctl.log");
	check(calls.empty(), std::format("no systemctl ran in a headless run ({})",
		calls.empty() ? "none" : calls.substr(0, calls.find('\n'))));
	return finish();
}

} // namespace

int main() {
	if (!begin("lock", {"WAYLAND_CANVAS_BIN", "CANVAS_LOCK_CLIENT"})) {
		return 2;
	}
	// A stub systemctl first on PATH: a run that is not a login must start no
	// session services, and the machine's own user manager must never be
	// spoken to from a test.
	std::string stubs = scratch_dir("bin");
	write_script(stubs + "/systemctl", std::format(
		"#!/usr/bin/env bash\nprintf '%s\\n' \"$*\" >> '{}/systemctl.log'\n", scratch()));
	put_first_on_path(stubs);
	Options options;
	options.start_bar = false; // the suites never spawn the shell
	return run(options, {
		{"window and subscriber", window_and_subscriber},
		{"unlocked start", unlocked_start},
		{"shot unlocked", shot_unlocked},
		{"lock action", lock_action},
		{"action reached the shell", action_reached_the_shell},
		{"locker took the session", locker_took_the_session},
		{"shot locked", shot_locked},
		{"locked screen", locked_screen},
		{"keys reach only the locker", keys_reach_only_the_locker},
		{"gestures and clicks reach only the locker",
			gestures_and_clicks_reach_only_the_locker},
		{"click reached the locker, then it dies", click_reached_the_locker_then_it_dies},
		{"killed locker leaves it locked", killed_locker_leaves_it_locked},
		{"shot abandoned", shot_abandoned},
		{"next locker", next_locker},
		{"new locker took over", new_locker_took_over},
		{"locker unlocked", locker_unlocked},
		{"shot unlocked again", shot_unlocked_again},
		{"windows are back", windows_are_back},
		{"idle timeout", idle_timeout},
		{"idle asked the shell", idle_asked_the_shell},
		{"inhibitor holds idle off", inhibitor_holds_idle_off},
		{"countdown runs again", countdown_runs_again},
		{"no session services", no_session_services},
	});
}
