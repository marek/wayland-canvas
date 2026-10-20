// End-to-end X11 scenario: X11 clients of the scenarios' own
// (tests/clients/x11-client.c) through Xwayland, which starts with them and
// not before. The window is a view like any other — a title bar, a place
// on the canvas, a click and a key reaching the client, maximize from the
// title bar, and the × closing it through WM_DELETE_WINDOW. X's own picture
// follows the canvas: a window opened over another takes its clicks, the
// one under it takes clicks where it shows and once raised, a minimized one
// takes none, and windows past X's screen or larger than it at 25 % take
// clicks at their own points; a click after a jump, and a press in the same
// instant as one, land where they were aimed. The window resizes (by
// Super+right-drag, by its frame's left edge, by its own
// _NET_WM_MOVERESIZE request), the session locks and unlocks over it, and
// Super+Shift+4 shoots it, the compositor answering `msg state` after each.
// Writes only under $CANVAS_TMP, with canvas.conf in its own scratch config
// dir.
#include <cairo.h>
#include <cerrno>
#include <cmath>
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
#include <variant>
#include <vector>
#include "core/config.h"
#include "core/server.h"
#include "core/wlr.h"
#include "platform/output.h"
#include "windows/view.h"
#include "windows/view_x11.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
constexpr int last_step = 120; // the step that ends the run; a step that cannot go on jumps here
bool finished = false;
uint32_t t = 90000;
// An X11 client of the scenario's own, and what it has said on stdout.
struct X11Client {
	pid_t pid = -1;
	int out = -1;
	std::string lines;
};
// Most steps need one client; the stacking steps a second beside it.
X11Client client, other;
Camera home{}; // the camera the run starts at
Point main_spot{}; // where the raised main window's click is aimed, its own point
std::size_t desktops_before = 0; // the desktops before a fullscreen step
wlr_box resized_from{}; // the X11 window's content box before the resize drag
Point drag_from{}; // where the resize drag's button went down
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

std::string read_text(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		return "";
	}
	std::string out;
	char buf[256];
	std::size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.append(buf, n);
	}
	std::fclose(f);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}

void write_text(const std::string &path, std::string_view text) {
	FILE *f = std::fopen(path.c_str(), "w");
	if (f == nullptr) {
		return;
	}
	std::fwrite(text.data(), 1, text.size(), f);
	std::fclose(f);
}

// Starts `c` at the scenario's size; later arguments override it.
bool start_client(X11Client &c, std::vector<std::string> args = {}) {
	const char *bin = std::getenv("CANVAS_X11_CLIENT");
	if (bin == nullptr || *bin == '\0') {
		return false;
	}
	args.insert(args.begin(), std::format("--size={}x{}", client_w, client_h));
	std::vector<char *> argv{const_cast<char *>("x11-client")};
	for (std::string &arg : args) {
		argv.push_back(arg.data());
	}
	argv.push_back(nullptr);
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}
	c.pid = fork();
	if (c.pid == 0) {
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, nullptr);
		close(fds[0]);
		dup2(fds[1], 1);
		close(fds[1]);
		execv(bin, argv.data());
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	c.out = fds[0];
	c.lines.clear();
	return c.pid > 0;
}

std::string_view client_said(X11Client &c = client) {
	char buf[512];
	ssize_t got = 0;
	while (c.out >= 0 && (got = read(c.out, buf, sizeof buf)) > 0) {
		c.lines.append(buf, static_cast<std::size_t>(got));
	}
	return c.lines;
}

void reap_client(X11Client &c = client) {
	if (c.pid > 0) {
		kill(c.pid, SIGKILL);
		waitpid(c.pid, nullptr, 0);
		c.pid = -1;
	}
	if (c.out >= 0) {
		close(c.out);
		c.out = -1;
	}
}

// True once the client has exited; reaped here, so the answer holds for
// every later call.
bool client_gone(X11Client &c = client) {
	if (c.pid <= 0) {
		return true;
	}
	if (waitpid(c.pid, nullptr, WNOHANG) == c.pid) {
		c.pid = -1;
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

// Super with one key, as the shortcuts are pressed.
void super_key(uint32_t code) {
	key(KEY_LEFTMETA, true);
	key(code, true);
	key(code, false);
	key(KEY_LEFTMETA, false);
}

// ---- the control socket, the locker and the shots ----

// The scenarios' locker, with its stdout on a pipe.
pid_t locker_pid = -1;
int locker_out = -1;
std::string locker_lines;
std::string pictures_dir; // the scratch Pictures: every shot lands here
int state_asks = 0;

bool start_locker() {
	const char *bin = std::getenv("CANVAS_LOCK_CLIENT");
	if (bin == nullptr || *bin == '\0') {
		return false;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}
	locker_pid = fork();
	if (locker_pid == 0) {
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, nullptr);
		close(fds[0]);
		dup2(fds[1], 1);
		close(fds[1]);
		execl(bin, "lock-client", static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	locker_out = fds[0];
	locker_lines.clear();
	return locker_pid > 0;
}

std::string_view locker_said() {
	char buf[512];
	ssize_t got = 0;
	while (locker_out >= 0 && (got = read(locker_out, buf, sizeof buf)) > 0) {
		locker_lines.append(buf, static_cast<std::size_t>(got));
	}
	return locker_lines;
}

void reap_locker() {
	if (locker_pid > 0) {
		kill(locker_pid, SIGKILL);
		waitpid(locker_pid, nullptr, 0);
		locker_pid = -1;
	}
	if (locker_out >= 0) {
		close(locker_out);
		locker_out = -1;
	}
}

// A `msg state` round trip on the compositor's own control socket, run
// through its own spawn so this process never blocks; the answer is read a
// step later by state_answered().
void ask_state() {
	state_asks++;
	spawn(std::format("\"{}\" msg state > '{}/state-{}.out' 2>&1; "
			"echo $? > '{}/state-{}.code'",
		std::getenv("WAYLAND_CANVAS_BIN"), shot_dir, state_asks, shot_dir,
		state_asks).c_str());
}

bool state_answered() {
	return !read_text(std::format("{}/state-{}.code", shot_dir, state_asks)).empty();
}

// The last ask's answer: the compositor is alive and says what it holds.
void check_alive(std::string_view after) {
	std::string code = read_text(std::format("{}/state-{}.code", shot_dir, state_asks));
	std::string said = read_text(std::format("{}/state-{}.out", shot_dir, state_asks));
	check(code == "0" && said.find("\"windows\"") != std::string::npos,
		std::format("the compositor answers `msg state` after {}", after));
}

// The newest file the shot hotkey saved, "" when there is none.
std::string newest_shot() {
	std::string newest;
	std::filesystem::file_time_type newest_time{};
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(
			pictures_dir + "/Screenshots", ec)) {
		if (!entry.path().filename().string().starts_with("Screenshot")) {
			continue;
		}
		std::filesystem::file_time_type time = entry.last_write_time(ec);
		if (newest.empty() || time > newest_time) {
			newest = entry.path().string();
			newest_time = time;
		}
	}
	return newest;
}

// A PNG's size and the share of its pixels that are the colour `rgb`.
struct PngLook {
	int width = 0, height = 0;
	double share = -1;
};

PngLook look_at_png(const std::string &path, uint32_t rgb) {
	cairo_surface_t *image = cairo_image_surface_create_from_png(path.c_str());
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(image);
		return {};
	}
	cairo_surface_flush(image);
	PngLook look;
	look.width = cairo_image_surface_get_width(image);
	look.height = cairo_image_surface_get_height(image);
	int stride = cairo_image_surface_get_stride(image);
	const unsigned char *data = cairo_image_surface_get_data(image);
	long hits = 0;
	for (int y = 0; y < look.height; y++) {
		const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * stride);
		for (int x = 0; x < look.width; x++) {
			if ((row[x] & 0x00ffffffu) == rgb) {
				hits++;
			}
		}
	}
	cairo_surface_destroy(image);
	look.share = look.width * look.height > 0
		? static_cast<double>(hits) / (look.width * look.height) : -1;
	return look;
}

// Whether the interactive resize grab is on.
bool resizing() {
	return std::holds_alternative<PointerGrab::Resize>(server->input.grab.state());
}

// Whether `now` still ends where `from` did, at the right and at the bottom,
// within a pixel: the edges a drag by the left or top edge leaves alone.
bool far_edges_held(const wlr_box &from, const wlr_box &now) {
	return std::abs(now.x + now.width - (from.x + from.width)) <= 1
		&& std::abs(now.y + now.height - (from.y + from.height)) <= 1;
}

View *x11_view() {
	for (View *view : server->views) {
		if (view->mapped && view->as_x11() != nullptr) {
			return view;
		}
	}
	return nullptr;
}

Output &output() {
	return *server->outputs.front();
}

// The mapped X11 window of that title.
View *titled(std::string_view title) {
	for (View *view : server->views) {
		if (view->mapped && view->as_x11() != nullptr && view->shell_title() != nullptr
				&& title == view->shell_title()) {
			return view;
		}
	}
	return nullptr;
}

// The output-local logical point of a point of the window's own surface.
Point on_screen(const View &view, double lx, double ly) {
	return output().canvas_to_local(view.x + lx, view.y + ly);
}

// Every point a client said a button arrived at, in order.
std::vector<Point> clicks_said(X11Client &c) {
	std::vector<Point> out;
	std::string text{client_said(c)};
	for (std::size_t at = text.find("BUTTON "); at != std::string::npos;
			at = text.find("BUTTON ", at + 1)) {
		int button = 0, x = 0, y = 0;
		if (std::sscanf(text.c_str() + at, "BUTTON %d %d,%d", &button, &x, &y) == 3) {
			out.push_back(Point{static_cast<double>(x), static_cast<double>(y)});
		}
	}
	return out;
}

std::string said_clicks(X11Client &c) {
	std::string out;
	for (Point p : clicks_said(c)) {
		out += std::format("{}{:.0f},{:.0f}", out.empty() ? "" : " ", p.x, p.y);
	}
	return out.empty() ? "none" : out;
}

// A click arrives on whole pixels: within two of where it was aimed.
bool close_to(Point got, Point want) {
	return std::abs(got.x - want.x) <= 2 && std::abs(got.y - want.y) <= 2;
}

// Whether the screen's camera is going to rest on `desktop`.
bool camera_on(const Desktop *desktop) {
	if (desktop == nullptr) {
		return false;
	}
	Camera to = output().camera.destination();
	Point centre = desktop->centre();
	return std::abs(to.cx - centre.x) < 1 && std::abs(to.cy - centre.y) < 1;
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

// Whether `over`'s frame is centred on `under`'s, within a pixel.
bool centred_on(const View &over, const View &under) {
	Rect a = under.frame_rect(), b = over.frame_rect();
	return std::abs(a.cx() - b.cx()) <= 1 && std::abs(a.cy() - b.cy()) <= 1;
}

int run_step() {
	switch (step) {
	case 0: { // no X11 client yet: Xwayland has nothing to show
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		home = output().camera.drawn();
		check(std::getenv("DISPLAY") != nullptr,
			"the session names an X display for X11 clients");
		check(x11_view() == nullptr, "no X11 window before a client runs");
		check(start_client(client), "the scenario's X11 client started");
		return 1500;
	}
	case 1: { // the window is a view like any other
		View *view = x11_view();
		if (view == nullptr && retries++ < 200) {
			return -100;
		}
		check(view != nullptr, "the X11 window mapped as a view");
		if (view == nullptr) {
			step = last_step - 1;
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
		Point maximize = button_centre(frame, DecoPart::maximize);
		click(maximize.x, maximize.y);
		return 1200;
	}
	case 5: {
		View *view = x11_view();
		if (view != nullptr && !view->maximized && retries++ < 50) {
			return -100;
		}
		check(view != nullptr && view->maximized,
			"the title bar's maximize button maximized the X11 window");
		check(view != nullptr && client_said().find(std::format("CONFIGURED {}x{}",
				view->as_x11()->wlr->width,
				view->as_x11()->wlr->height)) != std::string_view::npos,
			"the client was told its new size");
		return 600;
	}
	case 6: { // the × closes it through WM_DELETE_WINDOW
		View *view = x11_view();
		wlr_box frame = frame_on_screen(*view);
		Point close = button_centre(frame, DecoPart::close);
		click(close.x, close.y);
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
		// A menu of the client's own: an override-redirect window.
		check(start_client(client, {"--popup"}), "an X11 client with a menu started");
		return 1500;
	}
	case 9: {
		if (client_said().find("POPUP") == std::string_view::npos && retries++ < 200) {
			return -100;
		}
		check(client_said().find("POPUP") != std::string_view::npos,
			"the client mapped its menu");
		return 800;
	}
	case 10: { // the menu is where the client put it, and undecorated
		View *menu = nullptr;
		View *window = nullptr;
		for (View *view : server->views) {
			if (!view->mapped || view->as_x11() == nullptr) {
				continue;
			}
			(view->client_placed() ? menu : window) = view;
		}
		if (menu == nullptr && retries++ < 50) {
			return -100;
		}
		check(menu != nullptr, "the menu is on the canvas as its own surface");
		check(window != nullptr, "beside the window it belongs to");
		if (menu != nullptr) {
			check(menu->deco.titlebar() == 0,
				"an override-redirect window wears no title bar");
			check(menu->x == 120 + server->x11_offset_x
					&& menu->y == 140 + server->x11_offset_y, std::format(
				"it sits where its client put it, through X's offset ({},{})",
				menu->x, menu->y));
			check(server->focused_view != menu,
				"and it took no focus from the window");
		}
		reap_client();
		return 800;
	}
	case 11: { // the clipboard: X11 to wayland
		check(start_client(client, {"--offer=from-x11"}),
			"an X11 client owning the clipboard started");
		return 1500;
	}
	case 12: {
		if (client_said().find("OFFERED") == std::string_view::npos && retries++ < 200) {
			return -100;
		}
		check(client_said().find("OFFERED") != std::string_view::npos,
			"the client owns the X11 clipboard");
		// X11 hands its selection over for the window that holds the
		// focus: the client's own window is clicked first.
		if (View *view = x11_view()) {
			wlr_box frame = frame_on_screen(*view);
			click(frame.x + frame.width / 2,
				frame.y + config::titlebar_height + 20);
		}
		// Each ask writes its own file: a file left by an ask that found
		// nothing must never be read as this one's answer.
		spawn(std::format("wl-paste -n > '{}/paste-0.txt' 2>/dev/null",
			shot_dir).c_str());
		return 2000;
	}
	case 13: {
		std::string pasted = read_text(std::format("{}/paste-{}.txt", shot_dir,
			retries));
		if (!pasted.starts_with("from-x11") && retries++ < 120) {
			// The offer crosses the bridge once the compositor holds the
			// X11 selection, which follows the focus: the window is
			// clicked again and the clipboard asked again.
			if (View *view = x11_view()) {
				wlr_box frame = frame_on_screen(*view);
				click(frame.x + frame.width / 2,
					frame.y + config::titlebar_height + 20);
			}
			spawn(std::format("wl-paste -n > '{}/paste-{}.txt' 2>/dev/null",
				shot_dir, retries).c_str());
			return -500;
		}
		check(pasted.starts_with("from-x11"), std::format(
			"what an X11 client copied pastes in a wayland client ('{}')", pasted));
		reap_client();
		return 500;
	}
	case 14: { // and wayland to X11
		spawn("wl-copy from-wayland");
		return 1000;
	}
	case 15: {
		check(start_client(client, {"--paste"}), "an X11 client reading the clipboard started");
		return 2000;
	}
	case 16: {
		if (client_said().find("PASTED from-wayland") == std::string_view::npos
				&& retries++ < 200) {
			// The selection crosses to the window that holds the focus:
			// the client's own window is clicked as it comes up.
			if (View *view = x11_view()) {
				wlr_box frame = frame_on_screen(*view);
				click(frame.x + frame.width / 2,
					frame.y + config::titlebar_height + 20);
			}
			return -200;
		}
		std::string_view said = client_said();
		check(said.find("PASTED from-wayland") != std::string_view::npos,
			std::format("what a wayland client copied pastes in an X11 client ({})",
				said.substr(said.find("PASTED"),
					said.find('\n', said.find("PASTED")) - said.find("PASTED"))));
		reap_client();
		return 300;
	}
	case 17: { // a machine whose Xwayland is missing: no X11, and the
		// compositor runs all the same. A compositor of its own, headless
		// and on its own display, so this run's X server is untouched.
		check(server->xwayland != nullptr,
			"this run has an X server, its binary being there");
		std::string log = shot_dir + "/xwayland-missing.log";
		spawn(std::format(
			"env WLR_BACKENDS=headless WLR_RENDERER=gles2 "
			"WLR_XWAYLAND='{}/not-an-xwayland' WAYLAND_DISPLAY= "
			"XDG_CONFIG_HOME='{}/xdg-config' '{}' -B "
			"-s \"sleep 2; '{}' msg quit\" > '{}' 2>&1; echo done >> '{}'",
			shot_dir, shot_dir, std::getenv("WAYLAND_CANVAS_BIN"),
			std::getenv("WAYLAND_CANVAS_BIN"), log, log).c_str());
		return 2000;
	}
	case 18: {
		std::string log = read_text(shot_dir + "/xwayland-missing.log");
		if (log.find("done") == std::string::npos && retries++ < 100) {
			return -300;
		}
		check(log.find("done") != std::string::npos,
			"a compositor whose Xwayland binary is missing starts and stops");
		check(log.find("Running wayland-canvas on WAYLAND_DISPLAY=")
				!= std::string::npos,
			"it served its own display while it ran");
		return 300;
	}
	case 19: { // X's picture follows the canvas: a window opened over another
		reap_client();
		check(start_client(client, {"--size=600x400", "--title=x11-main"}),
			"a main X11 window started");
		return 1500;
	}
	case 20: {
		if (titled("x11-main") == nullptr && retries++ < 200) {
			return -100;
		}
		check(start_client(other, {"--size=300x200", "--title=x11-dialog"}),
			"a dialog-sized X11 window started after it");
		return 1500;
	}
	case 21: {
		View *dialog = titled("x11-dialog");
		if (dialog == nullptr && retries++ < 200) {
			return -100;
		}
		View *main = titled("x11-main");
		check(main != nullptr && dialog != nullptr && server->views.front() == dialog,
			"both mapped, the second on top");
		if (main == nullptr || dialog == nullptr) {
			step = last_step - 1;
			return 100;
		}
		return 600; // a frame tells X the stacking and the places
	}
	case 22: {
		Point p = on_screen(*titled("x11-dialog"), 150, 100);
		click(p.x, p.y);
		key(KEY_A, true);
		key(KEY_A, false);
		return 800;
	}
	case 23: {
		std::vector<Point> top = clicks_said(other);
		check(top.size() == 1 && close_to(top.back(), {150, 100}), std::format(
			"a click on the window opened over another reaches it ({})",
			said_clicks(other)));
		check(clicks_said(client).empty(), "and not the window under it");
		check(client_said(other).find("KEY") != std::string_view::npos,
			"its keys reach it too");
		Point p = on_screen(*titled("x11-main"), 30, 30);
		click(p.x, p.y);
		return 800;
	}
	case 24: {
		std::vector<Point> under = clicks_said(client);
		check(under.size() == 1 && close_to(under.back(), {30, 30}), std::format(
			"the window under it takes clicks where it shows ({})", said_clicks(client)));
		// Raised by that click, it covers the other where both are.
		View *main = titled("x11-main"), *dialog = titled("x11-dialog");
		main_spot = Point{dialog->x + 150.0 - main->x, dialog->y + 100.0 - main->y};
		Point p = on_screen(*dialog, 150, 100);
		click(p.x, p.y);
		return 800;
	}
	case 25: {
		std::vector<Point> under = clicks_said(client);
		check(under.size() == 2 && close_to(under.back(), main_spot)
				&& clicks_said(other).size() == 1, std::format(
			"raised, it takes the click where it covers the other ({})",
			said_clicks(client)));
		titled("x11-main")->set_minimized(true);
		return 600;
	}
	case 26: {
		Point p = on_screen(*titled("x11-dialog"), 150, 100);
		click(p.x, p.y);
		return 800;
	}
	case 27: {
		std::vector<Point> top = clicks_said(other);
		check(top.size() == 2 && close_to(top.back(), {150, 100})
				&& clicks_said(client).size() == 2, std::format(
			"a minimized window takes no click meant for the one it covered ({})",
			said_clicks(other)));
		reap_client(client);
		reap_client(other);
		// The camera looks past X's screen, which is the layout's box.
		output().camera.jump(Camera{home.cx + 3000, home.cy, 1});
		check(start_client(client, {"--size=300x200", "--title=x11-far"}),
			"an X11 window started past X's screen");
		return 1500;
	}
	case 28: {
		View *distant = titled("x11-far");
		if (distant == nullptr && retries++ < 200) {
			return -100;
		}
		check(distant != nullptr && distant->x > output().width, std::format(
			"it opened where the camera looks ({})", distant != nullptr ? distant->x : 0));
		if (distant == nullptr) {
			step = last_step - 1;
			return 100;
		}
		return 600;
	}
	case 29: { // the pointer comes in as a pointer does, then clicks
		Point p = on_screen(*titled("x11-far"), 150, 100);
		move_to(p.x - 3, p.y - 3);
		return 300;
	}
	case 30: {
		Point p = on_screen(*titled("x11-far"), 150, 100);
		click(p.x, p.y);
		return 800;
	}
	case 31: {
		std::vector<Point> distant = clicks_said(client);
		check(distant.size() == 1 && close_to(distant.back(), {150, 100}), std::format(
			"a window past X's screen takes the click at its own point ({})",
			said_clicks(client)));
		reap_client(client);
		// At 25 %, a window larger than X's screen.
		output().camera.jump(Camera{home.cx, home.cy, 0.25});
		check(start_client(client, {"--size=4000x2400", "--title=x11-big"}),
			"an X11 window larger than X's screen started");
		return 1500;
	}
	case 32: {
		View *big = titled("x11-big");
		if (big == nullptr && retries++ < 200) {
			return -100;
		}
		check(big != nullptr, "it mapped at 25 %");
		if (big == nullptr) {
			step = last_step - 1;
			return 100;
		}
		return 600;
	}
	case 33: {
		Point p = on_screen(*titled("x11-big"), 3900, 2300);
		move_to(p.x - 2, p.y - 2);
		return 300;
	}
	case 34: {
		Point p = on_screen(*titled("x11-big"), 3900, 2300);
		click(p.x, p.y);
		return 300;
	}
	case 35: {
		Point p = on_screen(*titled("x11-big"), 100, 100);
		move_to(p.x + 2, p.y + 2);
		return 300;
	}
	case 36: {
		Point p = on_screen(*titled("x11-big"), 100, 100);
		click(p.x, p.y);
		return 800;
	}
	case 37: {
		std::vector<Point> big = clicks_said(client);
		check(big.size() == 2 && close_to(big[0], {3900, 2300})
				&& close_to(big[1], {100, 100}), std::format(
			"at 25 %, the larger window takes clicks at its far corners ({})",
			said_clicks(client)));
		// A jump across the window, then a press with no motion between: X
		// saw the jump before its windows moved, and is told it again.
		Point p = on_screen(*titled("x11-big"), 3900, 100);
		move_to(p.x, p.y);
		return 400;
	}
	case 38: {
		button(BTN_LEFT, true);
		button(BTN_LEFT, false);
		return 800;
	}
	case 39: {
		std::vector<Point> big = clicks_said(client);
		check(big.size() == 3 && close_to(big.back(), {3900, 100}), std::format(
			"a click after a jump, with no motion between, lands where it was aimed ({})",
			said_clicks(client)));
		// A jump and a press in the same instant: the press waits until X
		// has taken in the moves the jump sent.
		Point p = on_screen(*titled("x11-big"), 100, 2300);
		move_to(p.x, p.y);
		button(BTN_LEFT, true);
		button(BTN_LEFT, false);
		return 800;
	}
	case 40: {
		std::vector<Point> big = clicks_said(client);
		check(big.size() == 4 && close_to(big.back(), {100, 2300}), std::format(
			"a jump and a press in the same instant land where they were aimed ({})",
			said_clicks(client)));
		reap_client(client);
		output().camera.jump(home);
		return 300;
	}
	case 41: { // a modal dialog floats over its window on a tiled desktop
		super_key(KEY_T);
		check(start_client(client, {"--size=600x400", "--title=x11-tiled", "--dialog"}),
			"an X11 window with a modal dialog started on a tiled desktop");
		return 1500;
	}
	case 42: {
		if (titled("x11-tiled") == nullptr && retries++ < 200) {
			return -100;
		}
		return 800; // tiled by now
	}
	case 43: { // its first key opens the dialog
		key(KEY_D, true);
		key(KEY_D, false);
		return 600;
	}
	case 44: {
		if (titled("x11-tiled-dialog") == nullptr && retries++ < 200) {
			return -100;
		}
		return 600;
	}
	case 45: {
		View *main = titled("x11-tiled"), *dialog = titled("x11-tiled-dialog");
		check(main != nullptr && main->tiled && main->board.has_value(),
			"the window joined the tiled desktop's board");
		check(dialog != nullptr && !dialog->tiled && !dialog->board.has_value(),
			"its modal dialog floats, off the board");
		check(main != nullptr && dialog != nullptr && centred_on(*dialog, *main),
			"centred over the window it blocks");
		// Switching tiling off and on lays the desktop out again: the modal
		// dialog still floats over its window.
		super_key(KEY_T);
		return 600;
	}
	case 46: {
		super_key(KEY_T);
		return 600;
	}
	case 47: {
		View *main = titled("x11-tiled"), *dialog = titled("x11-tiled-dialog");
		check(main != nullptr && main->tiled && main->board.has_value()
				&& dialog != nullptr && !dialog->tiled && !dialog->board.has_value(),
			"switched off and on, tiling leaves the modal dialog floating");
		// Going to a desktop gives the keyboard to the window last used there.
		super_key(KEY_2);
		return 1000;
	}
	case 48: {
		check(start_client(other, {"--size=300x200", "--title=x11-two"}),
			"an X11 window started on desktop 2");
		return 1500;
	}
	case 49: {
		if (titled("x11-two") == nullptr && retries++ < 200) {
			return -100;
		}
		check(titled("x11-two") != nullptr && server->focused_view == titled("x11-two"),
			"it took the keyboard as it opened");
		super_key(KEY_1);
		return 1000;
	}
	case 50: {
		check(server->focused_view != nullptr
				&& server->focused_view == titled("x11-tiled-dialog"),
			"Super+1 gave the keyboard to the window last used on desktop 1");
		super_key(KEY_2);
		return 1000;
	}
	case 51: {
		check(server->focused_view != nullptr && server->focused_view == titled("x11-two"),
			"Super+2 gave it back to the window on desktop 2");
		reap_client(client);
		reap_client(other);
		return 300;
	}
	case 52: { // fullscreen over another window: a desktop of its own
		check(start_client(other, {"--size=400x300", "--title=x11-beside"}),
			"a window to be covered started on desktop 2");
		return 1500;
	}
	case 53: {
		if (titled("x11-beside") == nullptr && retries++ < 200) {
			return -100;
		}
		check(start_client(client, {"--size=500x300", "--title=x11-fs"}),
			"a window to go fullscreen started beside it");
		return 1500;
	}
	case 54: {
		if (titled("x11-fs") == nullptr && retries++ < 200) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *fs = titled("x11-fs"), *beside = titled("x11-beside");
		check(fs != nullptr && beside != nullptr && d2 != nullptr
				&& server->canvas.desktop_of(*fs) == d2
				&& server->canvas.desktop_of(*beside) == d2 && server->focused_view == fs,
			"both stand on desktop 2, the newer focused");
		desktops_before = server->canvas.desktops.size();
		super_key(KEY_F);
		return 800;
	}
	case 55: {
		View *fs = titled("x11-fs"), *beside = titled("x11-beside");
		Desktop *d2 = server->canvas.desktops.by_number(2);
		Desktop *own = fs != nullptr ? server->canvas.desktop_of(*fs) : nullptr;
		check(fs != nullptr && fs->fullscreen && own != nullptr && own != d2
				&& server->canvas.desktops.size() == desktops_before + 1,
			"Super+F over another window took it to a new desktop of its own");
		bool clear = own != nullptr;
		for (const Desktop &d : server->canvas.desktops.all()) {
			clear = clear && (&d == own || !own->rect().intersects(d.rect()));
		}
		check(clear, "beside the others, overlapping none");
		check(beside != nullptr && server->canvas.desktop_of(*beside) == d2,
			"the covered window stayed on desktop 2");
		check(camera_on(own) && server->focused_view == fs,
			"the screen and the keyboard went with it");
		super_key(KEY_F);
		return 800;
	}
	case 56: {
		View *fs = titled("x11-fs");
		Desktop *d2 = server->canvas.desktops.by_number(2);
		check(fs != nullptr && !fs->fullscreen && server->canvas.desktop_of(*fs) == d2
				&& server->canvas.desktops.size() == desktops_before,
			"Super+F again brought it back to desktop 2, and its desktop went");
		check(camera_on(d2), "the screen came back to desktop 2 with it");
		reap_client(other);
		return 600;
	}
	case 57: { // alone on its desktop: fullscreen in place
		if (titled("x11-beside") != nullptr && retries++ < 100) {
			return -100;
		}
		super_key(KEY_F);
		return 800;
	}
	case 58: {
		View *fs = titled("x11-fs");
		Desktop *d2 = server->canvas.desktops.by_number(2);
		check(fs != nullptr && fs->fullscreen && server->canvas.desktop_of(*fs) == d2
				&& server->canvas.desktops.size() == desktops_before,
			"alone on its desktop, it went fullscreen in place");
		super_key(KEY_F);
		return 600;
	}
	case 59: {
		reap_client(client);
		return 300;
	}
	// ---- an interactive resize, an unlock and a window shot of an X11 window:
	// each asks the window's shell for something and each is followed by a
	// `msg state` round trip, which only a live compositor answers.
	case 60: {
		reap_client(other);
		check(start_client(client, {"--title=x11-act"}),
			"an X11 window to resize, lock under and shoot started");
		return 1500;
	}
	case 61: { // (a) Super is held, the pointer comes in over the window
		View *view = titled("x11-act");
		if (view == nullptr && retries++ < 200) {
			return -100;
		}
		check(view != nullptr, "the window mapped");
		if (view == nullptr) {
			step = last_step - 1;
			return 100;
		}
		resized_from = view->content_box();
		wlr_box frame = frame_on_screen(*view);
		// The lower right quarter: the right and bottom edges move.
		drag_from = Point{frame.x + frame.width * 0.75, frame.y + frame.height * 0.75};
		key(KEY_LEFTMETA, true);
		move_to(drag_from.x, drag_from.y);
		return 300;
	}
	case 62: { // the button goes down, and the grab is awaited before the pointer travels
		button(BTN_RIGHT, true);
		return 300;
	}
	case 63: {
		if (!resizing() && retries++ < 50) {
			return -100;
		}
		check(resizing(), "Super+right-drag over the X11 window began a resize");
		move_to(drag_from.x + 20, drag_from.y + 15);
		return 100;
	}
	case 64: {
		move_to(drag_from.x + 40, drag_from.y + 30);
		return 100;
	}
	case 65: {
		move_to(drag_from.x + 60, drag_from.y + 45);
		return 100;
	}
	case 66: {
		move_to(drag_from.x + 80, drag_from.y + 60);
		return 300;
	}
	case 67: { // the release ends the grab
		button(BTN_RIGHT, false);
		ask_state();
		return 600;
	}
	case 68: {
		View *view = titled("x11-act");
		// The drag is in screen pixels: the window grows by as many canvas
		// pixels as they cover at the zoom the drag began at.
		double zoom = output().camera.drawn().zoom;
		int want_w = resized_from.width + static_cast<int>(std::round(80 / zoom));
		int want_h = resized_from.height + static_cast<int>(std::round(60 / zoom));
		bool larger = view != nullptr
			&& std::abs(view->content_box().width - want_w) <= 1
			&& std::abs(view->content_box().height - want_h) <= 1;
		if ((!state_answered() || !larger) && retries++ < 50) {
			return -100;
		}
		check_alive("the resize");
		key(KEY_LEFTMETA, false);
		check(!resizing(), "the release ended the resize");
		check(larger, std::format("the window is larger by the drag ({}x{} to {}x{})",
			resized_from.width, resized_from.height,
			view != nullptr ? view->content_box().width : 0,
			view != nullptr ? view->content_box().height : 0));
		return 300;
	}
	case 69: { // (b) the window holds the focus
		View *view = titled("x11-act");
		if (view == nullptr) {
			step = last_step - 1;
			return 100;
		}
		wlr_box frame = frame_on_screen(*view);
		click(frame.x + frame.width / 2.0, frame.y + config::titlebar_height + 20);
		return 600;
	}
	case 70: { // ...and the session locks
		View *view = titled("x11-act");
		check(view != nullptr && server->focused_view == view,
			"a click on the window focused it");
		check(start_locker(), "the scenarios' locker started");
		return 600;
	}
	case 71: {
		if ((!server->lock.locked()
				|| locker_said().find("LOCKED") == std::string_view::npos)
				&& retries++ < 100) {
			return -100;
		}
		check(server->lock.locked(), "the locker took the session");
		check(locker_said().find("LOCKED") != std::string_view::npos,
			"the compositor confirmed the lock");
		ask_state();
		return 600;
	}
	case 72: {
		if (!state_answered() && retries++ < 50) {
			return -100;
		}
		check_alive("the lock");
		kill(locker_pid, SIGUSR1); // unlock and destroy
		return 600;
	}
	case 73: {
		if (server->lock.locked() && retries++ < 100) {
			return -100;
		}
		check(!server->lock.locked(), "the locker unlocked the session");
		ask_state();
		return 600;
	}
	case 74: {
		if (!state_answered() && retries++ < 50) {
			return -100;
		}
		check_alive("the unlock");
		View *view = titled("x11-act");
		check(view != nullptr
				&& server->seat->keyboard_state.focused_surface == view->surface(),
			"after the unlock the keyboard is on the X11 window's surface");
		reap_locker();
		return 300;
	}
	case 75: { // (c) Super+Shift+4, the pointer on the window
		View *view = titled("x11-act");
		if (view == nullptr) {
			step = last_step - 1;
			return 100;
		}
		wlr_box frame = frame_on_screen(*view);
		move_to(frame.x + frame.width / 2.0, frame.y + config::titlebar_height + 20);
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTSHIFT, true);
		key(KEY_4, true);
		key(KEY_4, false);
		key(KEY_LEFTSHIFT, false);
		key(KEY_LEFTMETA, false);
		return 600;
	}
	case 76: { // ...and a click on the window shoots it
		check(server->capture_ui.picking(), "Super+Shift+4 opened the picker");
		wlr_box frame = frame_on_screen(*titled("x11-act"));
		click(frame.x + frame.width / 2.0, frame.y + config::titlebar_height + 20);
		ask_state();
		return 1500;
	}
	case 77: {
		std::string shot = newest_shot();
		if ((shot.empty() || !state_answered()) && retries++ < 50) {
			return -100;
		}
		check_alive("the window shot");
		check(!shot.empty(), "the click saved a window shot");
		View *view = titled("x11-act");
		PngLook look = look_at_png(shot, 0xc04040);
		wlr_box frame = view != nullptr ? view->frame_box() : wlr_box{};
		check(view != nullptr && look.width == frame.width && look.height == frame.height,
			std::format("the shot has the window frame's size ({}x{}, frame {}x{})",
				look.width, look.height, frame.width, frame.height));
		check(look.share > 0.7, std::format(
			"and shows the client's colour ({:.2f} of it)", look.share));
		return 300;
	}
	case 78: { // by the frame's left edge: the window grows leftwards, the right edge holds
		View *view = titled("x11-act");
		if (view == nullptr) {
			step = last_step - 1;
			return 100;
		}
		resized_from = view->content_box();
		wlr_box frame = frame_on_screen(*view);
		drag_from = Point{frame.x - config::resize_margin / 2.0, frame.y + frame.height / 2.0};
		move_to(drag_from.x, drag_from.y);
		return 300;
	}
	case 79: {
		button(BTN_LEFT, true);
		return 300;
	}
	case 80: {
		if (!resizing() && retries++ < 50) {
			return -100;
		}
		check(resizing(), "a press on the X11 window's left edge began a resize");
		move_to(drag_from.x - 30, drag_from.y);
		return 100;
	}
	case 81: {
		move_to(drag_from.x - 60, drag_from.y);
		return 100;
	}
	case 82: {
		move_to(drag_from.x - 90, drag_from.y);
		return 300;
	}
	case 83: {
		button(BTN_LEFT, false);
		ask_state();
		return 600;
	}
	case 84: {
		View *view = titled("x11-act");
		double zoom = output().camera.drawn().zoom;
		int want_w = resized_from.width + static_cast<int>(std::round(90 / zoom));
		bool grown = view != nullptr
			&& std::abs(view->content_box().width - want_w) <= 1;
		if ((!state_answered() || !grown) && retries++ < 50) {
			return -100;
		}
		check_alive("the left-edge resize");
		check(!resizing(), "the release ended the resize");
		check(grown, std::format("the window grew by the drag ({} to {})", resized_from.width,
			view != nullptr ? view->content_box().width : 0));
		check(view != nullptr && far_edges_held(resized_from, view->content_box()),
			"the right and bottom edges held after the release");
		return 400;
	}
	case 85: { // the client has redrawn at its new size by now
		View *view = titled("x11-act");
		check(view != nullptr && far_edges_held(resized_from, view->content_box()),
			"and after the client's next commit");
		return 100;
	}
	case 86: { // a window that asks for a resize of its own
		reap_client(client);
		check(start_client(client, {"--title=x11-ask", "--moveresize"}),
			"an X11 client that asks to be resized started");
		return 1500;
	}
	case 87: {
		View *view = titled("x11-ask");
		if (view == nullptr && retries++ < 200) {
			return -100;
		}
		check(view != nullptr, "its window mapped");
		if (view == nullptr) {
			step = last_step - 1;
			return 100;
		}
		resized_from = view->content_box();
		wlr_box frame = frame_on_screen(*view);
		drag_from = Point{frame.x + frame.width / 2.0,
			frame.y + config::titlebar_height + 20.0};
		move_to(drag_from.x, drag_from.y);
		return 300;
	}
	case 88: { // the press reaches the client, which asks for the resize
		button(BTN_LEFT, true);
		return 300;
	}
	case 89: {
		if (!resizing() && retries++ < 50) {
			return -100;
		}
		check(client_said().find("MOVERESIZE") != std::string_view::npos,
			"the client asked the window manager to resize it");
		check(resizing(), "the client's own request began the same resize");
		move_to(drag_from.x - 40, drag_from.y - 30);
		return 100;
	}
	case 90: {
		move_to(drag_from.x - 80, drag_from.y - 60);
		return 300;
	}
	case 91: {
		button(BTN_LEFT, false);
		ask_state();
		return 600;
	}
	case 92: {
		View *view = titled("x11-ask");
		double zoom = output().camera.drawn().zoom;
		int want_w = resized_from.width + static_cast<int>(std::round(80 / zoom));
		int want_h = resized_from.height + static_cast<int>(std::round(60 / zoom));
		bool grown = view != nullptr
			&& std::abs(view->content_box().width - want_w) <= 1
			&& std::abs(view->content_box().height - want_h) <= 1;
		if ((!state_answered() || !grown) && retries++ < 50) {
			return -100;
		}
		check_alive("the client's own resize request");
		check(!resizing(), "the release ended the resize");
		check(grown, std::format("the window grew by the drag ({}x{} to {}x{})",
			resized_from.width, resized_from.height,
			view != nullptr ? view->content_box().width : 0,
			view != nullptr ? view->content_box().height : 0));
		check(view != nullptr && far_edges_held(resized_from, view->content_box()),
			"the right and bottom edges held, the top left corner having moved");
		return 400;
	}
	case 93: {
		View *view = titled("x11-ask");
		check(view != nullptr && far_edges_held(resized_from, view->content_box()),
			"and after the client's next commit");
		reap_client(client);
		return 300;
	}
	// ---- a modal dialog whose hints reach the window manager after it maps: the
	// kind they decide is laid out when they arrive, as at the map.
	case 94: {
		output().camera.jump(home);
		return 300;
	}
	case 95: {
		Desktop *shown = output().desktop_showing();
		check(shown != nullptr, "the screen shows a desktop");
		if (shown != nullptr && !shown->tiling()) {
			super_key(KEY_T);
		}
		check(start_client(client, {"--size=600x400", "--title=x11-late", "--dialog-late"}),
			"an X11 window that opens its modal dialog bare started on a tiled desktop");
		return 1500;
	}
	case 96: {
		if (titled("x11-late") == nullptr && retries++ < 200) {
			return -100;
		}
		return 800; // tiled by now
	}
	case 97: { // its first key opens the dialog with no hints
		key(KEY_D, true);
		key(KEY_D, false);
		return 600;
	}
	case 98: {
		if (titled("x11-late-dialog") == nullptr && retries++ < 200) {
			return -100;
		}
		return 600;
	}
	case 99: { // mapped bare it is a window like any: the board holds it
		View *main = titled("x11-late"), *dialog = titled("x11-late-dialog");
		check(main != nullptr && main->tiled && main->board.has_value(),
			"the window joined the tiled desktop's board");
		check(dialog != nullptr && dialog->kind() == ViewKind::normal && dialog->tiled
				&& dialog->board.has_value(),
			"its dialog, mapped before its hints, joined the board as a plain window");
		// The second key gives the dialog its hints, after the map.
		key(KEY_D, true);
		key(KEY_D, false);
		return 600;
	}
	case 100: { // the hints are in wlroots, and so was the layout when they arrived
		View *dialog = titled("x11-late-dialog");
		const X11View *shell = dialog != nullptr ? dialog->as_x11() : nullptr;
		bool arrived = shell != nullptr && shell->wlr->parent != nullptr
			&& shell->wlr->modal;
		if (!arrived && retries++ < 100) {
			return -100;
		}
		check(arrived, "the dialog's hints, sent after its map, reached the window manager");
		View *main = titled("x11-late");
		check(dialog != nullptr && dialog->kind() == ViewKind::modal,
			"the dialog is a modal one now");
		check(dialog != nullptr && !dialog->tiled && !dialog->board.has_value(),
			"its modal dialog floats, off the board");
		check(main != nullptr && dialog != nullptr && centred_on(*dialog, *main),
			"centred over the window it blocks");
		super_key(KEY_T);
		return 600;
	}
	case 101: {
		super_key(KEY_T);
		return 600;
	}
	case 102: {
		View *main = titled("x11-late"), *dialog = titled("x11-late-dialog");
		check(main != nullptr && main->tiled && main->board.has_value()
				&& dialog != nullptr && !dialog->tiled && !dialog->board.has_value(),
			"switched off and on, tiling leaves the late modal dialog floating");
		reap_client(client);
		return 300;
	}
	case last_step: {
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
	for (const char *name : {"CANVAS_X11_CLIENT", "CANVAS_LOCK_CLIENT", "WAYLAND_CANVAS_BIN"}) {
		if (std::getenv(name) == nullptr) {
			std::fprintf(stderr, "xwayland: $%s is not set (meson passes it)\n", name);
			return 2;
		}
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/xwayland", root);
	std::error_code ec;
	std::filesystem::remove_all(shot_dir, ec);
	mkdir_p(shot_dir);
	// Nothing touches the real Pictures: the user dir points into the scratch
	// tree. glib caches the user dirs, so this happens before the server exists.
	pictures_dir = std::format("{}/pictures", shot_dir);
	std::string config_home = std::format("{}/xdg-config", shot_dir);
	mkdir_p(pictures_dir);
	mkdir_p(config_home);
	write_text(std::format("{}/user-dirs.dirs", config_home),
		std::format("XDG_PICTURES_DIR=\"{}\"\n", pictures_dir));
	setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
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
	reap_client(client);
	reap_client(other);
	reap_locker();
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	return failures != 0;
}
