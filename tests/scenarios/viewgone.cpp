// End-to-end scenario for a window that goes while something holds it: a
// title-bar move under way, the capture picker hovering it, a window cast
// copying its frames, and the fullscreen desktop made for it. Each hold is
// made for real (fake pointer and keyboard devices, foot windows, the
// cast-client), the window is closed under it, and the run goes on: the
// hold must end and nothing may touch the window afterwards. Built to run
// clean under the sanitizer build (tools/check.sh --sanitize). Writes only
// under $CANVAS_TMP (meson passes the repo's tmp/).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <signal.h>
#include <source_location>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "apps/capture_ui.h"
#include "core/config.h"
#include "core/server.h"
#include "core/wlr.h"
#include "platform/output.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false; // the run reached its end; see main()
uint32_t t = 50000;
wlr_pointer ptr{};
wlr_keyboard kbd{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
pid_t cast_pid = -1;
std::size_t desktops_before = 0;
std::string shot_dir;

// Prints PASS/FAIL with the note; a failure counts and names its source line.
void check(bool cond, std::string_view note,
		std::source_location loc = std::source_location::current()) {
	if (!cond) {
		failures++;
	}
	std::println("{}: {} ({}:{})", cond ? "PASS" : "FAIL", note, loc.file_name(), loc.line());
}

void mkdir_p(const std::string &path) {
	for (std::size_t i = 1; i < path.size(); i++) {
		if (path[i] == '/') {
			mkdir(path.substr(0, i).c_str(), 0755);
		}
	}
	mkdir(path.c_str(), 0755);
}

std::string read_file(const std::string &path) {
	std::string out;
	if (FILE *f = std::fopen(path.c_str(), "r"); f != nullptr) {
		char buf[256];
		std::size_t n;
		while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
			out.append(buf, n);
		}
		std::fclose(f);
	}
	return out;
}

View *titled(const char *title) {
	for (View *v : server->views) {
		const char *name = v->shell_title();
		if (v->mapped && name != nullptr && std::strcmp(name, title) == 0) {
			return v;
		}
	}
	return nullptr;
}

void open_window(const char *title) {
	spawn(std::format("foot -T {} sh -c 'exec sleep 600'", title).c_str());
}

// Pointer at output-local logical (x, y) on the single 1280x720 output.
void move_to(double x, double y) {
	t += 8;
	wlr_pointer_motion_absolute_event ev = {
		.pointer = &ptr, .time_msec = t, .x = x / 1280.0, .y = y / 720.0};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

void button(uint32_t code, bool pressed) {
	t += 8;
	wlr_pointer_button_event ev = {.pointer = &ptr, .time_msec = t, .button = code,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&ptr.events.button, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

void key(uint32_t code, bool pressed) {
	t += 8;
	wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

Output *output() {
	return server->output_under_cursor();
}

// Ends the run where it stands: the caller checked what it could not go on without.
int stop() {
	wl_display_terminate(server->display);
	return -1;
}

// Whether the picker is drawing an outline round a hovered window.
bool picker_outlines_a_window() {
	Output *out = output();
	return out != nullptr && !server->capture_ui.overlay(*out, 1.0f).outlines.empty();
}

int run_step() {
	switch (step) {
	case 0:
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		open_window("mover");
		return 600;

	// A title-bar move: the window closes under the held button.
	case 1: {
		View *view = titled("mover");
		if (view == nullptr && retries++ < 100) {
			return -100;
		}
		check(view != nullptr, "the first window mapped");
		if (view == nullptr) {
			return stop();
		}
		wlr_box f = view->frame_box();
		move_to(f.x + 80, f.y + 16);
		button(BTN_LEFT, true);
		for (int i = 1; i <= 4; i++) {
			move_to(f.x + 80 + 10 * i, f.y + 16 + 5 * i);
		}
		check(server->input.grab.active(), "the title bar press began a move");
		view->close();
		return 300;
	}
	case 2: {
		if (titled("mover") != nullptr && retries++ < 100) {
			return -100;
		}
		check(titled("mover") == nullptr, "the window closed under the move");
		check(!server->input.grab.active(), "the move ended with the window");
		for (int i = 5; i <= 8; i++) {
			move_to(600 + 10 * i, 300 + 5 * i);
		}
		button(BTN_LEFT, false);
		check(!server->input.grab.active(), "the release after it left nothing held");
		open_window("picked");
		return 600;
	}

	// The capture picker hovering a window: it closes while hovered.
	case 3: {
		View *view = titled("picked");
		if (view == nullptr && retries++ < 100) {
			return -100;
		}
		check(view != nullptr, "the second window mapped");
		if (view == nullptr) {
			return stop();
		}
		wlr_box f = view->frame_box();
		move_to(f.x + f.width / 2.0, f.y + f.height / 2.0);
		check(server->capture_ui.start_pick(output(), CaptureUi::PickWhat::window,
				CaptureUi::PickUse::ipc, CaptureUi::Options{}),
			"the window picker opened");
		move_to(f.x + f.width / 2.0 + 4, f.y + f.height / 2.0 + 4);
		check(picker_outlines_a_window(), "the picker outlines the window under the pointer");
		view->close();
		return 300;
	}
	case 4: {
		if (titled("picked") != nullptr && retries++ < 100) {
			return -100;
		}
		check(titled("picked") == nullptr, "the hovered window closed");
		check(server->capture_ui.picking() && !picker_outlines_a_window(),
			"the picker is still up and hovers nothing");
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		check(!server->capture_ui.picking(), "Escape closed the picker");
		open_window("cast");
		return 600;
	}

	// A window cast: the window closes while the client copies its frames.
	case 5: {
		if (titled("cast") == nullptr && retries++ < 100) {
			return -100;
		}
		check(titled("cast") != nullptr, "the third window mapped");
		cast_pid = spawn(std::format("exec \"$CANVAS_CAST_CLIENT\" cast > '{}/cast.out' 2>&1",
			shot_dir).c_str());
		return 800;
	}
	case 6: {
		View *view = titled("cast");
		if (read_file(shot_dir + "/cast.out").find("CAST READY") == std::string::npos
				&& retries++ < 100) {
			return -100;
		}
		check(read_file(shot_dir + "/cast.out").find("CAST READY") != std::string::npos,
			"the cast client received frames of the window");
		if (view != nullptr) {
			view->close();
		}
		return 300;
	}
	case 7: {
		if ((titled("cast") != nullptr
				|| read_file(shot_dir + "/cast.out").find("CAST STOPPED") == std::string::npos)
				&& retries++ < 100) {
			return -100;
		}
		check(titled("cast") == nullptr, "the cast window closed");
		check(read_file(shot_dir + "/cast.out").find("CAST STOPPED") != std::string::npos,
			"the compositor ended the session with its window");
		open_window("full");
		open_window("beside");
		return 800;
	}

	// A fullscreen desktop: the window closes while it holds one.
	case 8: {
		View *view = titled("full");
		if ((view == nullptr || titled("beside") == nullptr) && retries++ < 100) {
			return -100;
		}
		check(view != nullptr && titled("beside") != nullptr, "two windows share the desktop");
		if (view == nullptr) {
			return stop();
		}
		desktops_before = server->canvas.desktops.size();
		view->set_fullscreen(true);
		return 400;
	}
	case 9: {
		View *view = titled("full");
		check(view != nullptr && view->fullscreen
				&& server->canvas.fullscreen_desktops.area(*view).has_value()
				&& server->canvas.desktops.size() == desktops_before + 1,
			"fullscreen over another window made a desktop of its own");
		if (view != nullptr) {
			view->close();
		}
		return 300;
	}
	case 10: {
		if ((titled("full") != nullptr || server->canvas.desktops.size() != desktops_before)
				&& retries++ < 100) {
			return -100;
		}
		check(titled("full") == nullptr, "the fullscreen window closed");
		check(server->canvas.desktops.size() == desktops_before,
			std::format("its desktop went with it ({} desktops, {} before)",
				server->canvas.desktops.size(), desktops_before));
		check(titled("beside") != nullptr, "the window beside it is untouched");
		finished = true;
		wl_display_terminate(server->display);
		return -1;
	}
	default:
		return -1;
	}
}

int handle_timer() {
	t += 1000;
	int delay = run_step();
	if (delay < -1) {
		wl_event_source_timer_update(timer, -delay);
		return 0;
	}
	if (delay == -1) {
		return 0;
	}
	step++;
	retries = 0;
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

} // namespace

int main() {
	if (std::getenv("CANVAS_CAST_CLIENT") == nullptr) {
		std::fprintf(stderr, "viewgone: $CANVAS_CAST_CLIENT is not set (meson passes it)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/viewgone", root);
	mkdir_p(shot_dir);
	setenv("XDG_CONFIG_HOME", std::format("{}/xdg-config", shot_dir).c_str(), 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *data) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display);
	// A compositor that stops by itself ends the run early: without this
	// the scenario would still exit 0.
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	if (cast_pid > 0) {
		kill(cast_pid, SIGTERM);
	}
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	std::println("{}: {} failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
	return failures != 0;
}
