// End-to-end scenario for a window that goes while something holds it: a
// title-bar move, the capture picker hovering it, a window cast copying its
// frames, and the fullscreen desktop made for it. Each hold is made for real,
// the window is closed under it, and the hold must end with nothing touching
// the window afterwards. Built to run clean under the sanitizer build
// (tools/check.sh --sanitize).
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include "apps/capture_ui.h"
#include "core/process.h"
#include "core/server.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/devices.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "windows/view.h"

using namespace harness;

namespace {

Child cast; // the cast client, copying the window "cast"
bool pick_answered = false, pick_cancelled = false; // what the asked pick was answered with
std::size_t desktops_before = 0;

void open_window(const char *title) {
	spawn(std::format("foot -T {} sh -c 'exec sleep 600'", title).c_str());
}

// Whether the picker is drawing an outline round a hovered window.
bool picker_outlines_a_window() {
	return !server->capture_ui.overlay(output(), 1.0f).outlines.empty();
}

Result devices_and_first_window() {
	attach_pointer();
	attach_keyboard();
	open_window("mover");
	return after(600);
}

// A title-bar move: the window closes under the held button.
Result move_begins_then_the_window_closes() {
	if (auto wait = wait_for(titled("mover") != nullptr)) {
		return *wait;
	}
	View *view = titled("mover");
	check(view != nullptr, "the first window mapped");
	if (view == nullptr) {
		return finish();
	}
	Point grip = title_point(*view);
	move_to(grip.x, grip.y);
	button(BTN_LEFT, true);
	for (int i = 1; i <= 4; i++) {
		move_to(grip.x + 10 * i, grip.y + 5 * i);
	}
	check(server->input.grab.active(), "the title bar press began a move");
	view->close();
	return after(300);
}

Result move_ended_with_the_window() {
	if (auto wait = wait_for(titled("mover") == nullptr)) {
		return *wait;
	}
	check(titled("mover") == nullptr, "the window closed under the move");
	check(!server->input.grab.active(), "the move ended with the window");
	for (int i = 5; i <= 8; i++) {
		move_to(600 + 10 * i, 300 + 5 * i);
	}
	button(BTN_LEFT, false);
	check(!server->input.grab.active(), "the release after it left nothing held");
	open_window("picked");
	return after(600);
}

// The capture picker hovering a window: it closes while hovered.
Result picker_hovers_then_the_window_closes() {
	if (auto wait = wait_for(titled("picked") != nullptr)) {
		return *wait;
	}
	View *view = titled("picked");
	check(view != nullptr, "the second window mapped");
	if (view == nullptr) {
		return finish();
	}
	wlr_box frame = frame_on_screen(*view);
	double x = frame.x + frame.width / 2.0, y = frame.y + frame.height / 2.0;
	move_to(x, y);
	check(server->capture_ui.ask_pick(&output(), PickWhat::window,
			[](std::optional<CapturePicker::Pick> pick) {
				pick_answered = true;
				pick_cancelled = !pick.has_value();
			}),
		"the window picker opened");
	move_to(x + 4, y + 4);
	check(picker_outlines_a_window(), "the picker outlines the window under the pointer");
	view->close();
	return after(300);
}

Result picker_hovers_nothing_then_escape() {
	if (auto wait = wait_for(titled("picked") == nullptr)) {
		return *wait;
	}
	check(titled("picked") == nullptr, "the hovered window closed");
	check(server->capture_ui.picking() && !picker_outlines_a_window(),
		"the picker is still up and hovers nothing");
	tap(KEY_ESC);
	check(!server->capture_ui.picking(), "Escape closed the picker");
	check(pick_answered && pick_cancelled, "Escape answered the asked pick with nothing");
	open_window("cast");
	return after(600);
}

// A window cast: the window closes while the client copies its frames.
Result cast_starts() {
	if (auto wait = wait_for(titled("cast") != nullptr)) {
		return *wait;
	}
	check(titled("cast") != nullptr, "the third window mapped");
	check(cast.start_client("CANVAS_CAST_CLIENT", {"cast"}), "the cast client started");
	return after(800);
}

Result cast_runs_then_the_window_closes() {
	if (auto wait = wait_for(cast.count("CAST READY") > 0)) {
		return *wait;
	}
	check(cast.count("CAST READY") > 0, "the cast client received frames of the window");
	if (View *view = titled("cast")) {
		view->close();
	}
	return after(300);
}

Result cast_ended_with_the_window() {
	if (auto wait = wait_for(titled("cast") == nullptr && cast.count("CAST STOPPED") > 0)) {
		return *wait;
	}
	check(titled("cast") == nullptr, "the cast window closed");
	check(cast.count("CAST STOPPED") > 0, "the compositor ended the session with its window");
	open_window("full");
	open_window("beside");
	return after(800);
}

// A fullscreen desktop: the window closes while it holds one.
Result fullscreen_over_another_window() {
	if (auto wait = wait_for(titled("full") != nullptr && titled("beside") != nullptr)) {
		return *wait;
	}
	View *view = titled("full");
	check(view != nullptr && titled("beside") != nullptr, "two windows share the desktop");
	if (view == nullptr) {
		return finish();
	}
	desktops_before = server->canvas.desktops.size();
	view->set_fullscreen(true);
	return after(400);
}

Result fullscreen_window_closes() {
	View *view = titled("full");
	check(view != nullptr && view->mode == WindowMode::fullscreen
			&& server->canvas.fullscreen_desktops.area(*view).has_value()
			&& server->canvas.desktops.size() == desktops_before + 1,
		"fullscreen over another window made a desktop of its own");
	if (view != nullptr) {
		view->close();
	}
	return after(300);
}

Result its_desktop_goes_with_it() {
	if (auto wait = wait_for(titled("full") == nullptr
			&& server->canvas.desktops.size() == desktops_before)) {
		return *wait;
	}
	check(titled("full") == nullptr, "the fullscreen window closed");
	check(server->canvas.desktops.size() == desktops_before,
		std::format("its desktop went with it ({} desktops, {} before)",
			server->canvas.desktops.size(), desktops_before));
	check(titled("beside") != nullptr, "the window beside it is untouched");
	return finish();
}

} // namespace

int main() {
	if (!begin("viewgone", {"CANVAS_CAST_CLIENT"})) {
		return 2;
	}
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	return run(options, {
		{"devices and first window", devices_and_first_window},
		{"move begins, then the window closes", move_begins_then_the_window_closes},
		{"move ended with the window", move_ended_with_the_window},
		{"picker hovers, then the window closes", picker_hovers_then_the_window_closes},
		{"picker hovers nothing, then escape", picker_hovers_nothing_then_escape},
		{"cast starts", cast_starts},
		{"cast runs, then the window closes", cast_runs_then_the_window_closes},
		{"cast ended with the window", cast_ended_with_the_window},
		{"fullscreen over another window", fullscreen_over_another_window},
		{"fullscreen window closes", fullscreen_window_closes},
		{"its desktop goes with it", its_desktop_goes_with_it},
	});
}
