// End-to-end X11 scenario: an X11 client of the scenarios' own
// (tests/clients/x11-client.c) through Xwayland, which starts with it and
// not before. The window is a view like any other — a title bar, a place
// on the canvas, a click and a key reaching the client, maximize from the
// title bar, and the × closing it through WM_DELETE_WINDOW. Writes only
// under $CANVAS_TMP, with canvas.conf in its own scratch config dir.
#include <cairo.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
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
#include <sys/wait.h>
#include <unistd.h>
#include "core/config.h"
#include "core/server.h"
#include "core/wlr.h"
#include "platform/output.h"
#include "windows/view.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false;
uint32_t t = 90000;
pid_t client_pid = -1;
int client_out = -1;
std::string client_lines;
std::string shot_dir;
wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
wlr_keyboard kbd{};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};

constexpr int client_w = 400, client_h = 300;

void check(bool cond, std::string_view note,
	std::source_location where = std::source_location::current()) {
	std::println("{}: {}", cond ? "PASS" : "FAIL", note);
	std::fflush(stdout);
	if (!cond) {
		failures++;
		std::println(stderr, "  at {}:{} in {}", where.file_name(), where.line(),
			where.function_name());
	}
}

void mkdir_p(const std::string &path) {
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
}

bool start_client() {
	const char *bin = std::getenv("CANVAS_X11_CLIENT");
	if (bin == nullptr || *bin == '\0') {
		return false;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}
	client_pid = fork();
	if (client_pid == 0) {
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, nullptr);
		close(fds[0]);
		dup2(fds[1], 1);
		close(fds[1]);
		execl(bin, "x11-client",
			std::format("--size={}x{}", client_w, client_h).c_str(),
			static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	client_out = fds[0];
	client_lines.clear();
	return client_pid > 0;
}

std::string_view client_said() {
	char buf[512];
	ssize_t got = 0;
	while (client_out >= 0 && (got = read(client_out, buf, sizeof buf)) > 0) {
		client_lines.append(buf, static_cast<std::size_t>(got));
	}
	return client_lines;
}

void reap_client() {
	if (client_pid > 0) {
		kill(client_pid, SIGKILL);
		waitpid(client_pid, nullptr, 0);
		client_pid = -1;
	}
	if (client_out >= 0) {
		close(client_out);
		client_out = -1;
	}
}

// True once the client has exited; reaped here, so the answer holds for
// every later call.
bool client_gone() {
	if (client_pid <= 0) {
		return true;
	}
	if (waitpid(client_pid, nullptr, WNOHANG) == client_pid) {
		client_pid = -1;
		return true;
	}
	return false;
}

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
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			: WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&ptr.events.button, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

void click(double x, double y) {
	move_to(x, y);
	button(BTN_LEFT, true);
	button(BTN_LEFT, false);
}

void key(uint32_t code, bool pressed) {
	t += 8;
	wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

View *x11_view() {
	for (View *view : server->views) {
		if (view->mapped && view->is_x11()) {
			return view;
		}
	}
	return nullptr;
}

Output &output() {
	return *server->outputs.front();
}

// The window's frame in output-local logical pixels.
wlr_box frame_on_screen(View &view) {
	Camera cam = output().camera.drawn();
	wlr_box frame = view.frame_box();
	Point top_left = output().canvas_to_local(frame.x, frame.y);
	return wlr_box{int(std::round(top_left.x)), int(std::round(top_left.y)),
		int(std::round(frame.width * cam.zoom)),
		int(std::round(frame.height * cam.zoom))};
}

int run_step() {
	switch (step) {
	case 0: { // no X11 client yet: Xwayland has nothing to show
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		check(std::getenv("DISPLAY") != nullptr,
			"the session names an X display for X11 clients");
		check(x11_view() == nullptr, "no X11 window before a client runs");
		check(start_client(), "the scenario's X11 client started");
		return 1500;
	}
	case 1: { // the window is a view like any other
		View *view = x11_view();
		if (view == nullptr && retries++ < 200) {
			return -100;
		}
		check(view != nullptr, "the X11 window mapped as a view");
		if (view == nullptr) {
			step = 90;
			return 100;
		}
		check(client_said().find("MAPPED") != std::string_view::npos,
			"the client says it mapped its window");
		check(view->shell_title() != nullptr
				&& std::string_view{view->shell_title()} == "canvas-x11",
			"the view carries the X11 window's title");
		check(view->deco.titlebar() == config::titlebar_height,
			"it is drawn with a title bar, like any other window");
		check(server->canvas.desktop_of(*view) != nullptr
				|| view->frame_rect().intersects(output().viewport()),
			"it opened where the user is looking");
		return 600;
	}
	case 2: { // a click and a key reach the client
		View *view = x11_view();
		wlr_box frame = frame_on_screen(*view);
		click(frame.x + frame.width / 2, frame.y + config::titlebar_height + 20);
		key(KEY_A, true);
		key(KEY_A, false);
		return 800;
	}
	case 3: {
		std::string_view said = client_said();
		if ((said.find("BUTTON") == std::string_view::npos
				|| said.find("KEY") == std::string_view::npos) && retries++ < 50) {
			return -100;
		}
		check(said.find("BUTTON") != std::string_view::npos,
			"a click reached the X11 client");
		check(said.find("KEY") != std::string_view::npos,
			"a key reached the X11 client");
		return 300;
	}
	case 4: { // maximize from the title bar
		View *view = x11_view();
		wlr_box frame = frame_on_screen(*view);
		// The maximize button is the middle of the three, each one
		// button_width wide at the frame's right edge.
		click(frame.x + frame.width - config::button_width * 1.5,
			frame.y + config::titlebar_height / 2);
		return 1200;
	}
	case 5: {
		View *view = x11_view();
		if (view != nullptr && !view->maximized && retries++ < 50) {
			return -100;
		}
		check(view != nullptr && view->maximized,
			"the title bar's maximize button maximized the X11 window");
		check(client_said().find("CONFIGURED") != std::string_view::npos,
			"the client was told its new size");
		return 600;
	}
	case 6: { // the × closes it through WM_DELETE_WINDOW
		View *view = x11_view();
		wlr_box frame = frame_on_screen(*view);
		click(frame.x + frame.width - config::button_width / 2,
			frame.y + config::titlebar_height / 2);
		return 1200;
	}
	case 7: {
		if (!client_gone() && retries++ < 100) {
			return -100;
		}
		check(client_said().find("CLOSING") != std::string_view::npos,
			"the close button asked the client to close itself");
		check(client_gone(), "the X11 client ended");
		return 600;
	}
	case 8: {
		check(x11_view() == nullptr, "its view went with it");
		reap_client();
		return 300;
	}
	case 90: {
		finished = true;
		wl_display_terminate(server->display);
		return -1;
	}
	}
	finished = true;
	wl_display_terminate(server->display);
	return -1;
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
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (std::getenv("CANVAS_X11_CLIENT") == nullptr) {
		std::fprintf(stderr, "xwayland: $CANVAS_X11_CLIENT is not set "
			"(meson passes it)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/xwayland", root);
	std::error_code ec;
	std::filesystem::remove_all(shot_dir, ec);
	mkdir_p(shot_dir);
	setenv("XDG_CONFIG_HOME", std::format("{}/xdg-config", shot_dir).c_str(), 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false;
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display);
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	reap_client();
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	return failures != 0;
}
