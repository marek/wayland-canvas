// End-to-end touch and pen scenario (docs/intents/input/touch-and-pen.md):
// a fake touchscreen and a fake pen tablet announced on the backend, the
// tests' own client reporting what reaches it (tests/clients/touch-client.c),
// and, where this machine has Xwayland, the tests' X11 client. A tap and a
// drag reach a window as touch; one finger drags a window by its title bar;
// a finger held still on the overview's empty canvas makes a desktop there,
// as a right click does; two fingers on the canvas look away and pinch a
// desktop's scale; three fingers hop. A finger on a window as the session
// locks is cancelled for it, and locked, touch reaches the locker only - as
// a click, the locker taking no touch. A client without touch support gets
// a tap as a click. The pen reaches a tablet client with pressure and tilt,
// a client without tablet support as the pointer, and drags a title bar;
// a tap and the pen's tip reach an X11 window as its press. Writes only
// under $CANVAS_TMP; canvas.conf lives in its own scratch config dir.
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <ranges>
#include <signal.h>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
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
std::string shot_dir;

wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
wlr_keyboard kbd{};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
wlr_touch touch{};
const wlr_touch_impl touch_impl = {.name = "fake-touchscreen"};
wlr_tablet tablet{};
const wlr_tablet_impl tablet_impl = {.name = "fake-tablet"};
wlr_tablet_tool pen{};

constexpr int screen_w = 1280, screen_h = 720;

// A client of the tests' own, its stdout read line by line.
struct Client {
	pid_t pid = -1;
	int out = -1;
	std::string lines;
};
Client app, plain, x11, locker;

// State carried between steps.
Camera home{};
wlr_box frame_before{};
std::size_t desktops_before = 0, said_before = 0, locker_before = 0;
std::optional<DesktopId> shown; // the desktop on screen when a pinch began

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
	for (std::size_t i = 1; i < path.size(); i++) {
		if (path[i] == '/') {
			mkdir(path.substr(0, i).c_str(), 0755);
		}
	}
	if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
		std::fprintf(stderr, "mkdir %s: %s\n", path.c_str(), std::strerror(errno));
	}
}

// ------------------------------------------------------------ clients

bool start(Client &c, const char *env, std::vector<std::string> args) {
	const char *bin = std::getenv(env);
	if (bin == nullptr || *bin == '\0') {
		return false;
	}
	std::vector<char *> argv{const_cast<char *>(bin)};
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

std::string_view said(Client &c) {
	char buf[512];
	ssize_t got = 0;
	while (c.out >= 0 && (got = read(c.out, buf, sizeof buf)) > 0) {
		c.lines.append(buf, static_cast<std::size_t>(got));
	}
	return c.lines;
}

// Whether `c` has said `line` (a whole line) since `from`.
bool said_line(Client &c, std::string_view line, std::size_t from = 0) {
	std::string_view all = said(c);
	for (std::size_t at = all.find(line, from); at != std::string_view::npos;
			at = all.find(line, at + 1)) {
		bool starts = at == 0 || all[at - 1] == '\n';
		bool ends = at + line.size() == all.size() || all[at + line.size()] == '\n';
		if (starts && ends) {
			return true;
		}
	}
	return false;
}

// What `c` has said since `from`, for a failure's note.
std::string since(Client &c, std::size_t from) {
	std::string_view all = said(c);
	std::string out{from < all.size() ? all.substr(from) : std::string_view{}};
	std::ranges::replace(out, '\n', '|');
	return out;
}

// Whether the X11 client said "BUTTON <button> <x>,<y>" since `from`, within
// a pixel of `want`: X takes a position as a whole pixel, and a touch goes
// through Xwayland's 0..65535 device range and back, which can come out a
// pixel short.
bool said_button_near(Client &c, int button, Point want, std::size_t from) {
	std::string_view all = said(c);
	std::string head = std::format("BUTTON {} ", button);
	for (std::size_t at = all.find(head, from); at != std::string_view::npos;
			at = all.find(head, at + 1)) {
		int x = 0, y = 0;
		if (std::sscanf(all.data() + at + head.size(), "%d,%d", &x, &y) == 2
				&& std::abs(x - want.x) <= 1 && std::abs(y - want.y) <= 1) {
			return true;
		}
	}
	return false;
}

void reap(Client &c) {
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

// ------------------------------------------------------- input fakes

void key(uint32_t code, bool pressed) {
	t += 8;
	wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

void super_key(uint32_t code) {
	key(KEY_LEFTMETA, true);
	key(code, true);
	key(code, false);
	key(KEY_LEFTMETA, false);
}

// Fingers at output-local logical points; the touchscreen maps to the one output.
void finger_down(int32_t id, double x, double y) {
	t += 8;
	wlr_touch_down_event ev = {.touch = &touch, .time_msec = t, .touch_id = id,
		.x = x / screen_w, .y = y / screen_h};
	wl_signal_emit_mutable(&touch.events.down, &ev);
	wl_signal_emit_mutable(&touch.events.frame, nullptr);
}

void touch_frame() {
	wl_signal_emit_mutable(&touch.events.frame, nullptr);
}

// A finger's move; fingers moving together share one frame, as libinput sends them.
void finger_move(int32_t id, double x, double y, bool frame = true) {
	t += 8;
	wlr_touch_motion_event ev = {.touch = &touch, .time_msec = t, .touch_id = id,
		.x = x / screen_w, .y = y / screen_h};
	wl_signal_emit_mutable(&touch.events.motion, &ev);
	if (frame) {
		touch_frame();
	}
}

void finger_up(int32_t id) {
	t += 8;
	wlr_touch_up_event ev = {.touch = &touch, .time_msec = t, .touch_id = id};
	wl_signal_emit_mutable(&touch.events.up, &ev);
	wl_signal_emit_mutable(&touch.events.frame, nullptr);
}

// Fingers starting at `from`, moved together by (dx, dy) in steps, then lifted.
void fingers_drag(std::span<const Point> from, double dx, double dy, int steps = 10) {
	for (std::size_t i = 0; i < from.size(); i++) {
		finger_down(static_cast<int32_t>(10 + i), from[i].x, from[i].y);
	}
	for (int s = 1; s <= steps; s++) {
		for (std::size_t i = 0; i < from.size(); i++) {
			finger_move(static_cast<int32_t>(10 + i), from[i].x + dx * s / steps,
				from[i].y + dy * s / steps, false);
		}
		touch_frame();
	}
	for (std::size_t i = 0; i < from.size(); i++) {
		finger_up(static_cast<int32_t>(10 + i));
	}
}

void pen_proximity(double x, double y, bool in) {
	t += 8;
	wlr_tablet_tool_proximity_event ev = {.tablet = &tablet, .tool = &pen, .time_msec = t,
		.x = x / screen_w, .y = y / screen_h,
		.state = in ? WLR_TABLET_TOOL_PROXIMITY_IN : WLR_TABLET_TOOL_PROXIMITY_OUT};
	wl_signal_emit_mutable(&tablet.events.proximity, &ev);
}

void pen_move(double x, double y, std::optional<double> pressure = std::nullopt,
		std::optional<Point> tilt = std::nullopt) {
	t += 8;
	wlr_tablet_tool_axis_event ev = {.tablet = &tablet, .tool = &pen, .time_msec = t,
		.updated_axes = WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y,
		.x = x / screen_w, .y = y / screen_h};
	if (pressure.has_value()) {
		ev.updated_axes |= WLR_TABLET_TOOL_AXIS_PRESSURE;
		ev.pressure = *pressure;
	}
	if (tilt.has_value()) {
		ev.updated_axes |= WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y;
		ev.tilt_x = tilt->x;
		ev.tilt_y = tilt->y;
	}
	wl_signal_emit_mutable(&tablet.events.axis, &ev);
}

void pen_tip(double x, double y, bool down) {
	t += 8;
	wlr_tablet_tool_tip_event ev = {.tablet = &tablet, .tool = &pen, .time_msec = t,
		.x = x / screen_w, .y = y / screen_h,
		.state = down ? WLR_TABLET_TOOL_TIP_DOWN : WLR_TABLET_TOOL_TIP_UP};
	wl_signal_emit_mutable(&tablet.events.tip, &ev);
}

void pen_button(uint32_t button, bool pressed) {
	t += 8;
	wlr_tablet_tool_button_event ev = {.tablet = &tablet, .tool = &pen, .time_msec = t,
		.button = button, .state = pressed ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED};
	wl_signal_emit_mutable(&tablet.events.button, &ev);
}

// ---------------------------------------------------------- the scene

Output &output() {
	return *server->outputs.front();
}

bool settled() {
	return !output().camera.panning() && !output().camera.zooming();
}

View *titled(std::string_view title) {
	for (View *view : server->views) {
		if (view->mapped && view->shell_title() != nullptr && view->shell_title() == title) {
			return view;
		}
	}
	return nullptr;
}

// The output-local point of a point on the window's own surface.
Point on_screen(const View &view, double sx, double sy) {
	return output().canvas_to_local(view.x + sx, view.y + sy);
}

// A point on the window's title bar, clear of its buttons and edges.
Point on_title(View &view) {
	wlr_box frame = view.frame_box();
	return output().canvas_to_local(frame.x + 60, frame.y + 16);
}

Desktop *desk(int number) {
	return server->canvas.desktops.by_number(number);
}

// ------------------------------------------------------------- steps

int run_step() {
	switch (step) {
	case 0: { // the fake devices, then the client that reports touch and the pen
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		wlr_touch_init(&touch, &touch_impl, "fake-touchscreen");
		wl_signal_emit_mutable(&server->backend->events.new_input, &touch.base);
		wlr_tablet_init(&tablet, &tablet_impl, "fake-tablet");
		wl_signal_emit_mutable(&server->backend->events.new_input, &tablet.base);
		pen.type = WLR_TABLET_TOOL_TYPE_PEN;
		pen.pressure = true;
		pen.tilt = true;
		wl_signal_init(&pen.events.destroy);
		check(start(app, "CANVAS_TOUCH_CLIENT", {"--size=400x300", "--title=touch-app"}),
			"the touch client started");
		return 1000;
	}
	case 1: {
		View *view = titled("touch-app");
		if ((view == nullptr || !settled() || !said_line(app, "MAPPED 400x300"))
				&& retries++ < 200) {
			return -100;
		}
		check(view != nullptr, "the touch client's window mapped as a view");
		check((server->seat->capabilities & WL_SEAT_CAPABILITY_TOUCH) != 0,
			"with a touchscreen connected the seat offers touch");
		home = output().camera.current();
		return 300;
	}
	case 2: { // a tap
		View *view = titled("touch-app");
		if (view == nullptr) {
			return 300;
		}
		said_before = said(app).size();
		Point p = on_screen(*view, 100, 80);
		finger_down(1, p.x, p.y);
		finger_up(1);
		return 400;
	}
	case 3: {
		View *view = titled("touch-app");
		check(said_line(app, "TOUCH DOWN 1 100,80", said_before) && said_line(app, "TOUCH UP 1", said_before),
			std::format("a tap reached the window as touch, where it landed ({})",
				since(app, said_before)));
		check(view != nullptr && server->focused_view == view, "and focused it, as a click does");
		// A drag: the finger stays the window's, at its own point, past its
		// right edge too.
		said_before = said(app).size();
		if (view != nullptr) {
			Point a = on_screen(*view, 50, 50);
			finger_down(2, a.x, a.y);
			for (int s = 1; s <= 5; s++) {
				finger_move(2, a.x + 80 * s, a.y + 8 * s);
			}
			finger_up(2);
		}
		return 400;
	}
	case 4: {
		check(said_line(app, "TOUCH MOTION 2 370,82", said_before)
				&& said_line(app, "TOUCH MOTION 2 450,90", said_before),
			std::format("a drag's motion reached the window in its own coordinates, "
				"past its right edge too ({})",
				since(app, said_before)));
		// One finger on the title bar: the window moves with it.
		View *view = titled("touch-app");
		if (view == nullptr) {
			return 300;
		}
		frame_before = view->frame_box();
		Point p = on_title(*view);
		const Point from[] = {p};
		fingers_drag(from, 200, 100);
		return 500;
	}
	case 5: {
		View *view = titled("touch-app");
		wlr_box now = view != nullptr ? view->frame_box() : wlr_box{};
		check(view != nullptr && now.x - frame_before.x == 200 && now.y - frame_before.y == 100,
			std::format("one finger dragged the window by its title bar ({},{})",
				now.x - frame_before.x, now.y - frame_before.y));
		output().overview.enter();
		return 800;
	}
	case 6: { // a finger held still on the overview's empty canvas
		if (!settled() && retries++ < 100) {
			return -100;
		}
		desktops_before = std::ranges::distance(server->canvas.desktops.all());
		finger_down(3, 40, screen_h / 2.0);
		return config::touch_hold_ms + 300;
	}
	case 7: {
		finger_up(3);
		std::size_t now = std::ranges::distance(server->canvas.desktops.all());
		check(now == desktops_before + 1, std::format(
			"a finger held on the overview's empty canvas made a desktop there, as a "
			"right click does ({} -> {})", desktops_before, now));
		output().overview.exit();
		return 800;
	}
	case 8: { // two fingers on the canvas look away, the content following them
		if (!settled() && retries++ < 100) {
			return -100;
		}
		home = output().camera.current();
		said_before = said(app).size();
		const Point from[] = {{100, 620}, {220, 620}};
		fingers_drag(from, 150, -100);
		return 400;
	}
	case 9: {
		Camera now = output().camera.current();
		check(std::abs(now.cx - (home.cx - 150)) < 1 && std::abs(now.cy - (home.cy + 100)) < 1,
			std::format("two fingers looked away, the content following them "
				"(camera moved {:.0f},{:.0f})", now.cx - home.cx, now.cy - home.cy));
		check(said(app).find("TOUCH DOWN", said_before) == std::string::npos,
			std::format("and no client saw them ({})", since(app, said_before)));
		output().camera.jump(home);
		return 400;
	}
	case 10: { // two fingers spreading pinch the shown desktop's scale, as Super does
		shown = output().desktop_showing() != nullptr
			? std::optional<DesktopId>{output().desktop_showing()->id()} : std::nullopt;
		finger_down(20, 300, 620);
		finger_down(21, 400, 620);
		for (int s = 1; s <= 10; s++) {
			finger_move(20, 300 - 5 * s, 620, false);
			finger_move(21, 400 + 5 * s, 620, false);
			touch_frame();
		}
		finger_up(20);
		finger_up(21);
		return 1000;
	}
	case 11: {
		if (!settled() && retries++ < 100) {
			return -100;
		}
		Desktop *pinched = server->canvas.desktops.find(shown);
		check(pinched != nullptr && pinched->scale() > 1.5, std::format(
			"two fingers spreading scaled the shown desktop ({:.2f})",
			pinched != nullptr ? pinched->scale() : 0.0));
		if (pinched != nullptr) {
			server->canvas.settle_scale(pinched->id(), 1.0);
		}
		return 800;
	}
	case 12: // desktop 3 to the right of desktop 1, then back on desktop 1
		super_key(KEY_3);
		return 900;
	case 13:
		super_key(KEY_1);
		return 900;
	case 14: { // three fingers swiping right hop right, onto desktop 3
		if (!settled() && retries++ < 100) {
			return -100;
		}
		check(output().desktop_showing() == desk(1), "desktop 1 is shown before the swipe");
		const Point from[] = {{300, 620}, {360, 620}, {420, 620}};
		fingers_drag(from, 240, 0);
		return 900;
	}
	case 15: {
		if (!settled() && retries++ < 100) {
			return -100;
		}
		check(desk(3) != nullptr && output().desktop_showing() == desk(3),
			"three fingers swiping right hopped right, onto desktop 3");
		super_key(KEY_1);
		return 900;
	}
	case 16: { // four fingers swiping up open the overview
		if (!settled() && retries++ < 100) {
			return -100;
		}
		const Point from[] = {{300, 620}, {360, 620}, {420, 620}, {480, 620}};
		fingers_drag(from, 0, -320);
		return 900;
	}
	case 17: {
		if (!settled() && retries++ < 100) {
			return -100;
		}
		check(output().overview.active(), "four fingers swiping up opened the overview");
		output().overview.exit();
		return 800;
	}
	case 18: { // a finger on the window as the session locks is cancelled for it
		if (!settled() && retries++ < 100) {
			return -100;
		}
		View *view = titled("touch-app");
		said_before = said(app).size();
		if (view != nullptr) {
			Point p = on_screen(*view, 200, 150);
			finger_down(31, p.x, p.y);
		}
		check(start(locker, "CANVAS_LOCK_CLIENT", {}), "the scenarios' locker started");
		return 800;
	}
	case 19: { // locked, touch reaches the locker only
		if ((!server->lock.locked() || !said_line(locker, "LOCKED")) && retries++ < 100) {
			return -100;
		}
		check(server->lock.locked(), "the session is locked");
		check(said_line(app, "TOUCH CANCEL", said_before), std::format(
			"a finger on the window as the session locked was cancelled for it ({})",
			since(app, said_before)));
		View *view = titled("touch-app");
		said_before = said(app).size();
		locker_before = said(locker).size();
		if (view != nullptr) {
			Point p = on_screen(*view, 200, 150);
			finger_move(31, p.x + 30, p.y + 20); // the held finger goes on: to nobody
			finger_up(31);
			frame_before = view->frame_box();
			finger_down(30, p.x, p.y);
			finger_up(30);
			const Point from[] = {on_title(*view)};
			fingers_drag(from, 120, 60);
		}
		return 500;
	}
	case 20: {
		View *view = titled("touch-app");
		check(said(app).find("TOUCH", said_before) == std::string::npos,
			std::format("locked, the window got no touch ({})", since(app, said_before)));
		check(said_line(locker, "BUTTON 272", locker_before), std::format(
			"a tap reached the locker, which takes no touch, as a click ({})",
			since(locker, locker_before)));
		check(view != nullptr && view->frame_box().x == frame_before.x
				&& view->frame_box().y == frame_before.y,
			"and a title-bar drag moved nothing");
		if (locker.pid > 0) {
			kill(locker.pid, SIGUSR1);
		}
		return 800;
	}
	case 21: {
		if (server->lock.locked() && retries++ < 100) {
			return -100;
		}
		check(!server->lock.locked(), "the locker unlocked the session");
		reap(locker);
		// The pen over the tablet client: in range, pressure and tilt, the tip.
		View *view = titled("touch-app");
		said_before = said(app).size();
		if (view != nullptr) {
			Point a = on_screen(*view, 120, 90);
			Point b = on_screen(*view, 160, 120);
			pen_proximity(a.x, a.y, true);
			pen_move(a.x, a.y, 0.5, Point{10, -5});
			pen_tip(a.x, a.y, true);
			pen_move(b.x, b.y);
			pen_tip(b.x, b.y, false);
			pen_proximity(b.x, b.y, false);
		}
		return 500;
	}
	case 22: {
		std::string got = since(app, said_before);
		check(said_line(app, "TABLET IN", said_before) && said_line(app, "TABLET MOTION 120,90", said_before),
			std::format("the pen came in range of the tablet client where it was ({})", got));
		check(said_line(app, "TABLET PRESSURE 32767", said_before)
				|| said_line(app, "TABLET PRESSURE 32768", said_before),
			"with its pressure");
		check(said_line(app, "TABLET TILT 10,-5", said_before), "and its tilt");
		check(said_line(app, "TABLET DOWN", said_before) && said_line(app, "TABLET MOTION 160,120", said_before)
				&& said_line(app, "TABLET UP", said_before) && said_line(app, "TABLET OUT", said_before),
			"the tip went down, moved and lifted, and the pen left range");
		check(start(plain, "CANVAS_TOUCH_CLIENT",
				{"--size=300x200", "--title=touch-plain", "--no-tablet", "--no-touch"}),
			"a client without touch or tablet support started");
		return 1000;
	}
	case 23: {
		View *view = titled("touch-plain");
		if ((view == nullptr || !settled() || !said_line(plain, "MAPPED 300x200"))
				&& retries++ < 200) {
			return -100;
		}
		check(view != nullptr, "it mapped as a view");
		said_before = said(plain).size();
		if (view != nullptr) {
			// A finger, then the pen: both arrive as the pointer.
			Point f = on_screen(*view, 40, 30);
			finger_down(41, f.x, f.y);
			finger_up(41);
			Point p = on_screen(*view, 60, 60);
			pen_proximity(p.x, p.y, true);
			pen_tip(p.x, p.y, true);
			pen_tip(p.x, p.y, false);
			pen_button(BTN_STYLUS, true);
			pen_button(BTN_STYLUS, false);
			pen_proximity(p.x, p.y, false);
		}
		return 500;
	}
	case 24: {
		check(said_line(plain, "POINTER BUTTON 272 pressed 40,30", said_before)
				&& said_line(plain, "POINTER BUTTON 272 released 40,30", said_before),
			std::format("to a client without touch support a tap is a click where it "
				"landed ({})", since(plain, said_before)));
		check(said_line(plain, "POINTER BUTTON 272 pressed 60,60", said_before)
				&& said_line(plain, "POINTER BUTTON 272 released 60,60", said_before),
			"and without tablet support the pen's tip is the left button");
		check(said_line(plain, "POINTER BUTTON 273 pressed 60,60", said_before)
				&& said_line(plain, "POINTER BUTTON 273 released 60,60", said_before),
			"and its barrel button the right");
		// The tip on a title bar drags the window, as the mouse does.
		View *view = titled("touch-plain");
		if (view == nullptr) {
			return 300;
		}
		frame_before = view->frame_box();
		Point p = on_title(*view);
		pen_proximity(p.x, p.y, true);
		pen_tip(p.x, p.y, true);
		for (int s = 1; s <= 10; s++) {
			pen_move(p.x - 15 * s, p.y + 5 * s);
		}
		pen_tip(p.x - 150, p.y + 50, false);
		pen_proximity(p.x - 150, p.y + 50, false);
		return 500;
	}
	case 25: {
		View *view = titled("touch-plain");
		wlr_box now = view != nullptr ? view->frame_box() : wlr_box{};
		check(view != nullptr && now.x - frame_before.x == -150 && now.y - frame_before.y == 50,
			std::format("the pen's tip dragged a window by its title bar ({},{})",
				now.x - frame_before.x, now.y - frame_before.y));
		if (!start(x11, "CANVAS_X11_CLIENT", {"--size=300x200", "--title=x11-touch"})) {
			step = 29; // this machine has no X11 client: the X11 steps are skipped
			return 300;
		}
		return 1500;
	}
	case 26: {
		View *view = titled("x11-touch");
		if ((view == nullptr || !settled()) && retries++ < 200) {
			return -100;
		}
		check(view != nullptr, "the X11 client's window mapped as a view");
		said_before = said(x11).size();
		if (view != nullptr) {
			Point p = on_screen(*view, 80, 60);
			finger_down(40, p.x, p.y);
			finger_up(40);
		}
		return 600;
	}
	case 27: {
		check(said_button_near(x11, 1, {80, 60}, said_before),
			std::format("a tap reached the X11 window as its press ({})", since(x11, said_before)));
		said_before = said(x11).size();
		// The tip lifts in a later step: Xwayland reads the tip once per
		// tablet frame, and a down and an up in one frame cancel out.
		View *view = titled("x11-touch");
		if (view != nullptr) {
			Point p = on_screen(*view, 120, 90);
			pen_proximity(p.x, p.y, true);
			pen_tip(p.x, p.y, true);
		}
		return 200;
	}
	case 28: {
		View *view = titled("x11-touch");
		if (view != nullptr) {
			Point p = on_screen(*view, 120, 90);
			pen_tip(p.x, p.y, false);
			pen_proximity(p.x, p.y, false);
		}
		return 600;
	}
	case 29: {
		if (x11.pid > 0) {
			check(said_button_near(x11, 1, {120, 90}, said_before),
				std::format("the pen's tip reached the X11 window as its press ({})",
					since(x11, said_before)));
		}
		reap(x11);
		reap(plain);
		reap(app);
		return 300;
	}
	case 30: { // a touchscreen naming an output acts on that one
		wlr_backend *headless = nullptr;
		wlr_multi_for_each_backend(server->backend,
			[](wlr_backend *backend, void *data) {
				if (wlr_backend_is_headless(backend)) {
					*static_cast<wlr_backend **>(data) = backend;
				}
			}, &headless);
		std::free(touch.output_name);
		touch.output_name = strdup("HEADLESS-2");
		wlr_output *second = headless != nullptr
			? wlr_headless_add_output(headless, screen_w, screen_h) : nullptr;
		check(second != nullptr && std::string_view{second->name} == "HEADLESS-2",
			"a second output was added");
		return 800;
	}
	case 31: {
		// A quarter across: on the second output only if the touchscreen is
		// mapped to it, not to the first or to the whole layout.
		finger_down(50, screen_w / 4.0, screen_h / 2.0);
		finger_up(50);
		Output *under = server->output_at(server->cursor->x, server->cursor->y);
		check(under != nullptr && std::string_view{under->wlr->name} == "HEADLESS-2",
			std::format("a touchscreen naming the second output acts on it (the tap "
				"put the pointer on {})", under != nullptr ? under->wlr->name : "nothing"));
		return 300;
	}
	default:
		finished = true;
		std::println("{}: {} failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
		wl_display_terminate(server->display);
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
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr
			|| std::getenv("CANVAS_TOUCH_CLIENT") == nullptr
			|| std::getenv("CANVAS_LOCK_CLIENT") == nullptr) {
		std::fprintf(stderr, "touch: $WAYLAND_CANVAS_BIN, $CANVAS_TOUCH_CLIENT or "
			"$CANVAS_LOCK_CLIENT is not set (meson passes them)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/touch", root);
	mkdir_p(shot_dir);
	setenv("XDG_CONFIG_HOME", std::format("{}/xdg-config", shot_dir).c_str(), 1);
	// A stub systemctl first on PATH: a run that is not a login starts no
	// session services, and the machine's own user manager is never spoken to.
	std::string stub_bin = shot_dir + "/bin";
	mkdir_p(stub_bin);
	{
		std::string path = stub_bin + "/systemctl";
		FILE *f = std::fopen(path.c_str(), "w");
		if (f != nullptr) {
			std::fprintf(f, "#!/usr/bin/env bash\nexit 0\n");
			std::fclose(f);
			chmod(path.c_str(), 0755);
		}
	}
	setenv("PATH", std::format("{}:{}", stub_bin,
		std::getenv("PATH") != nullptr ? std::getenv("PATH") : "").c_str(), 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false; // the suites never spawn the shell
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
	reap(locker);
	reap(x11);
	reap(plain);
	reap(app);
	wl_event_source_remove(timer);
	wl_signal_emit_mutable(&pen.events.destroy, &pen);
	wlr_tablet_finish(&tablet);
	wlr_touch_finish(&touch);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	return failures != 0;
}
