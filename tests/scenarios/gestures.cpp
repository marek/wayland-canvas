// End-to-end gesture scenario: the real compositor in-process on the headless
// backend, real foot clients, synthetic touchpad gestures through the Gesture
// entry points, screenshots. Writes only under $CANVAS_TMP (meson passes the
// repo's tmp/).
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <initializer_list>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <glib.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core/box.h"
#include "core/config.h"
#include "core/geometry.h"
#include "platform/output.h"
#include "render/render.h"
#include "core/server.h"
#include "core/wlr.h"

// The allocation counter alloc-counter.c adds; absent in sanitizer builds.
extern "C" unsigned long long canvas_allocations(void) __attribute__((weak));

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false; // the run reached its end; see main()
uint32_t t_ms = 10000;
pid_t grim_pid = -1;
std::optional<DesktopId> d1, d2, d3;
Camera cam0{}, super_start{};
Rect board_one{}, board_two{}; // the boxes the board left, for the off check
// The windows' frames before a zoom, for the checks that a zoom re-lays out
// nothing; and the zoom rounds' progress and what they changed.
std::vector<std::pair<View *, Rect>> kept;
int zoom_round = 0;
std::string zoom_changes;
wlr_box frame0{}; // Desktop 1's frame at 100 % before any zoom, for the drift check
Camera scrolled_from{}; // the camera before the 25 % scroll
Camera swipe_from{}; // the camera before a zoom swipe
Camera hop_from{}; // where the camera was going when a hop began
Camera look_from{}; // the view a look away began from
std::optional<DesktopId> hop_a, hop_b, hop_c; // the row the normal-view hops walk
std::vector<View *> launched_before; // the views before the overview launch
wlr_box launch_box{}; // the overview target a late launch was made for
std::string tmp_root;
wlr_pointer ptr{};
wlr_keyboard kbd{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
// The gesture client: what it has said, and the step's wait for it.
struct Reporter {
	pid_t pid = -1;
	int out = -1;
	std::string said;
};
Reporter reporter;
int report_retries = 0; // the wait for the client's report; steps move, it resets
bool acted = false; // the step's action is done and only its wait repeats
std::size_t report_mark = 0; // how much the client had said before the action
Point pointer_was{}; // where the pointer stood before it parked on the client
std::optional<Point> overview_click; // the canvas point the overview click was made on
std::string shot_dir;

// Prints PASS/FAIL with the note; a failure counts and names its source line.
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

Output *out() {
	return server->outputs.front().get();
}

// The desktop a stored id names; null once it is gone.
Desktop *desk(std::optional<DesktopId> id) {
	return server->canvas.desktops.find(id);
}

View *view_titled(const char *title) {
	for (View *v : server->views) {
		if (v->mapped && v->shell_title() && std::strcmp(v->shell_title(), title) == 0) {
			return v;
		}
	}
	return nullptr;
}

int mapped_views() {
	int n = 0;
	for (View *v : server->views) {
		n += v->mapped;
	}
	return n;
}

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

void swipe(uint32_t fingers, double dx, double dy, int steps, bool cancelled = false) {
	server->gestures.swipe_begin(t_ms, fingers);
	for (int i = 0; i < steps; i++) {
		t_ms += 8;
		server->gestures.swipe_update(t_ms, dx / steps, dy / steps);
	}
	t_ms += 8;
	server->gestures.swipe_end(t_ms, cancelled);
}

void pinch(uint32_t fingers, double scale, int steps) {
	server->gestures.pinch_begin(t_ms, fingers);
	for (int i = 1; i <= steps; i++) {
		t_ms += 8;
		server->gestures.pinch_update(t_ms, 0, 0, 1 + (scale - 1) * i / steps, 0);
	}
	t_ms += 8;
	server->gestures.pinch_end(t_ms, false);
}

// Fake-device helpers: a keyboard press (the scenario's way of holding
// Super), pointer motion, buttons and pinch events, all through the
// paths real devices take — so a held Super is a real modifier and a
// pinch release is a real gesture end, not a direct call into Gestures.
void key(uint32_t code, bool pressed) {
	t_ms += 8;
	wlr_keyboard_key_event ev = {.time_msec = t_ms, .keycode = code,
		.update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

// Presses and releases each key in turn: typing into the launcher.
void tap_keys(std::initializer_list<uint32_t> codes) {
	for (uint32_t code : codes) {
		key(code, true);
		key(code, false);
	}
}

// Remembers the mapped windows, so launched_view() finds the next one.
void remember_views() {
	launched_before.assign(server->views.begin(), server->views.end());
}

// The mapped window that was not there at remember_views(), or null.
View *launched_view() {
	for (View *v : server->views) {
		if (v->mapped && std::ranges::find(launched_before, v) == launched_before.end()) {
			return v;
		}
	}
	return nullptr;
}

// Pointer at output-local logical (x, y) on the single 1280x720 output.
void move_to(double x, double y) {
	t_ms += 8;
	wlr_pointer_motion_absolute_event ev = {
		.pointer = &ptr, .time_msec = t_ms, .x = x / 1280.0, .y = y / 720.0};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

// A button press or release through the device path.
void click(uint32_t button, bool pressed) {
	t_ms += 8;
	wlr_pointer_button_event ev = {.pointer = &ptr, .time_msec = t_ms,
		.button = button,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			: WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&ptr.events.button, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

// A 2-finger pinch driven through the device path.
void pinch_down(uint32_t fingers) {
	t_ms += 8;
	wlr_pointer_pinch_begin_event ev = {
		.pointer = &ptr, .time_msec = t_ms, .fingers = fingers};
	wl_signal_emit_mutable(&ptr.events.pinch_begin, &ev);
}

void pinch_move(double scale) {
	t_ms += 8;
	wlr_pointer_pinch_update_event ev = {.pointer = &ptr, .time_msec = t_ms,
		.fingers = 2, .dx = 0, .dy = 0, .scale = scale, .rotation = 0};
	wl_signal_emit_mutable(&ptr.events.pinch_update, &ev);
}

void pinch_up(bool cancelled) {
	t_ms += 8;
	wlr_pointer_pinch_end_event ev = {
		.pointer = &ptr, .time_msec = t_ms, .cancelled = cancelled};
	wl_signal_emit_mutable(&ptr.events.pinch_end, &ev);
}

// A 2-finger scroll tick through the device path: vertical, from fingers,
// as a touchpad with natural scrolling (the default) sends it.
void scroll(double delta) {
	t_ms += 8;
	wlr_pointer_axis_event ev = {.pointer = &ptr, .time_msec = t_ms,
		.source = WL_POINTER_AXIS_SOURCE_FINGER,
		.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
		.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED,
		.delta = delta, .delta_discrete = 0};
	wl_signal_emit_mutable(&ptr.events.axis, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

// A 3- or 4-finger swipe driven through the device path.
void swipe_down(uint32_t fingers) {
	t_ms += 8;
	wlr_pointer_swipe_begin_event ev = {
		.pointer = &ptr, .time_msec = t_ms, .fingers = fingers};
	wl_signal_emit_mutable(&ptr.events.swipe_begin, &ev);
}

void swipe_move(uint32_t fingers, double dx, double dy) {
	t_ms += 8;
	wlr_pointer_swipe_update_event ev = {.pointer = &ptr, .time_msec = t_ms,
		.fingers = fingers, .dx = dx, .dy = dy};
	wl_signal_emit_mutable(&ptr.events.swipe_update, &ev);
}

void swipe_up(bool cancelled) {
	t_ms += 8;
	wlr_pointer_swipe_end_event ev = {
		.pointer = &ptr, .time_msec = t_ms, .cancelled = cancelled};
	wl_signal_emit_mutable(&ptr.events.swipe_end, &ev);
}

// The gesture client (tests/clients/gesture-client.c): a square in the
// output's corner that says, on stdout, every swipe and pinch the compositor
// forwards to it. A forwarded gesture is checked by what the client says.
bool start_reporter() {
	const char *bin = std::getenv("CANVAS_GESTURE_CLIENT");
	int fds[2];
	if (bin == nullptr || *bin == '\0' || pipe(fds) != 0) {
		return false;
	}
	reporter.pid = fork();
	if (reporter.pid == 0) {
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, nullptr);
		close(fds[0]);
		dup2(fds[1], 1);
		close(fds[1]);
		execl(bin, bin, static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	if (reporter.pid < 0) {
		close(fds[0]);
		return false;
	}
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	reporter.out = fds[0];
	return true;
}

void stop_reporter() {
	if (reporter.pid > 0) {
		kill(reporter.pid, SIGKILL);
		waitpid(reporter.pid, nullptr, 0);
		reporter.pid = -1;
	}
	if (reporter.out >= 0) {
		close(reporter.out);
		reporter.out = -1;
	}
}

// Everything the client has said so far.
const std::string &reported() {
	char buf[512];
	ssize_t got = 0;
	while (reporter.out >= 0 && (got = read(reporter.out, buf, sizeof buf)) > 0) {
		reporter.said.append(buf, static_cast<std::size_t>(got));
	}
	return reporter.said;
}

// Whether the client has said `line`, a whole line.
bool client_said(std::string_view line) {
	return ("\n" + reported()).find("\n" + std::string{line} + "\n") != std::string::npos;
}

// The swipe and pinch lines the client said from byte `from` on, joined by '|'.
std::string gestures_reported(std::size_t from) {
	const std::string &all = reported();
	std::string lines;
	for (std::size_t at = from; at < all.size();) {
		std::size_t end = all.find('\n', at);
		if (end == std::string::npos) {
			break;
		}
		std::string_view line{all.data() + at, end - at};
		if (line.starts_with("SWIPE ") || line.starts_with("PINCH ")) {
			lines += lines.empty() ? "" : "|";
			lines += line;
		}
		at = end + 1;
	}
	return lines;
}

// Puts the fake pointer over the client's square, so that what the
// compositor forwards goes to it, and starts the count of what it says.
void park_on_reporter() {
	pointer_was = {server->cursor->x - out()->lx, server->cursor->y - out()->ly};
	report_mark = reported().size();
	move_to(2, 2);
}

void leave_reporter() {
	move_to(pointer_was.x, pointer_was.y);
}

// Whether the step keeps waiting for the client to have said `want`.
bool awaiting(const std::string &want) {
	return gestures_reported(report_mark) != want && report_retries++ < 100;
}

void check_client_got(const std::string &want, std::string_view what,
		std::source_location where = std::source_location::current()) {
	std::string got = gestures_reported(report_mark);
	check(got == want, got == want ? std::string{what}
		: std::format("{} (the client said: {})", what, got), where);
}

// Whether the camera is still easing: a check of where it ended waits.
bool camera_easing() {
	return out()->camera.panning() || out()->camera.zooming();
}

// Whether two cameras show the same canvas, to a hundredth of a pixel.
bool same_camera(const Camera &a, const Camera &b) {
	return std::fabs(a.cx - b.cx) < 0.01 && std::fabs(a.cy - b.cy) < 0.01
		&& std::fabs(a.zoom - b.zoom) < 1e-6;
}

bool inside(const Rect &r, const wlr_box &b) {
	return r.x >= b.x - 0.5 && r.y >= b.y - 0.5 && r.x + r.w <= b.x + b.width + 0.5 &&
		r.y + r.h <= b.y + b.height + 0.5;
}

// The output-local centre of a mapped window on the screen, if any.
std::optional<Point> window_on_screen() {
	for (View *v : server->views) {
		if (!v->mapped) {
			continue;
		}
		Rect f = v->frame_rect();
		Point p = out()->canvas_to_local(f.x + f.w / 2, f.y + f.h / 2);
		if (p.x > 0 && p.x < 1280 && p.y > 0 && p.y < 720) {
			return p;
		}
	}
	return std::nullopt;
}

// Whether a window (with a margin for its title bar) covers the canvas point.
bool window_covers(const Point &c) {
	for (View *v : server->views) {
		Rect f = v->frame_rect();
		if (v->mapped && c.x >= f.x - 16 && c.x <= f.x + f.w + 16
				&& c.y >= f.y - 48 && c.y <= f.y + f.h + 16) {
			return true;
		}
	}
	return false;
}

// An output-local point over the empty canvas: no window under it, with a
// margin for its title bar.
std::optional<Point> empty_spot() {
	for (double y = 80; y < 680; y += 40) {
		for (double x = 40; x < 1240; x += 40) {
			if (!window_covers(out()->local_to_canvas(x, y))) {
				return Point{x, y};
			}
		}
	}
	return std::nullopt;
}

// An output-local point on the open overview's empty canvas, left of
// Desktop 1: under no desktop and no window, above the minimized shelf.
std::optional<Point> empty_overview_spot() {
	wlr_box frame = desk(d1)->frame();
	for (double y = 80; y < 600; y += 40) {
		for (double x = 40; x < 1240; x += 40) {
			Point c = out()->local_to_canvas(x, y);
			if (c.x < frame.x && server->canvas.desktops.at(c.x, c.y) == nullptr
					&& !window_covers(c)) {
				return Point{x, y};
			}
		}
	}
	return std::nullopt;
}

// The board's slices, as fractions of its area.
constexpr Slice LEFT{0, 0, 0.5, 1}, RIGHT{0.5, 0, 1, 1};
constexpr Slice TL{0, 0, 0.5, 0.5}, BL{0, 0.5, 0.5, 1};
constexpr Slice RT{0.5, 0, 1, 0.5}, RB{0.5, 0.5, 1, 1};

// A slice's expected frame on a desktop's board: the slice laid into the
// desktop's frame, rounded the way the compositor rounds it (the scenarios
// run with no bar, so the frame is the whole tiling area).
wlr_box slice_frame_of(const Desktop &desktop, Slice slice) {
	return box_snapped(slice_box(rect_of_box(desktop.frame()), slice, config::tile_gap));
}

// The view's frame lies on its slice (clients snap to their own units, so a
// side may sit a few px short of the slice's box).
bool on_slice(View *view, const wlr_box &expected) {
	Rect f = view->frame_rect();
	Rect e = rect_of_box(expected);
	return std::fabs(f.x - e.x) < 3 && std::fabs(f.y - e.y) < 3
		&& e.w - f.w < 12 && e.h - f.h < 12;
}

// The slice a view holds on its board; none for a window with no seat.
std::optional<Slice> slice_of(const View *view) {
	return view->board_seat.transform(&BoardSeat::slice);
}

// Remembers these windows' frames, to compare after a zoom.
void keep(std::initializer_list<View *> views) {
	kept.clear();
	for (View *view : views) {
		kept.emplace_back(view, view->frame_rect());
	}
}

// The kept windows whose frame moved, resized or unmapped since keep(),
// named with their sizes; empty when nothing changed.
std::string changed_boxes() {
	std::string out;
	for (const auto &[view, box] : kept) {
		Rect now = view->frame_rect();
		if (!view->mapped || now.x != box.x || now.y != box.y || now.w != box.w
				|| now.h != box.h) {
			out += std::format(" ({} {:.0f}x{:.0f} at {:.0f},{:.0f} -> {:.0f}x{:.0f} at {:.0f},{:.0f}{})",
				view->shell_title() != nullptr ? view->shell_title() : "?", box.w, box.h,
				box.x, box.y, now.w, now.h, now.x, now.y, view->mapped ? "" : ", unmapped");
		}
	}
	return out;
}

// Whether every kept window was told only the output's scale.
bool told_output_scale() {
	for (const auto &[view, box] : kept) {
		if (std::fabs(view->told_scale - out()->wlr->scale) > 1e-9) {
			return false;
		}
	}
	return true;
}

// Super + 2-finger pinch through the device path, from the shown desktop's
// scale `from` to settle at `to`.
void super_pinch_to(double from, double to) {
	key(KEY_LEFTMETA, true);
	pinch_down(2);
	pinch_move(to / from);
	pinch_up(false);
	key(KEY_LEFTMETA, false);
}

// Whether the overview help line names `word` for the options in force.
bool help_names(const char *word) {
	std::string_view text = overview_help_text(server->gestures.options());
	return text.find(word) != std::string_view::npos;
}

// The frame a desktop made beside desktop 1 has at grid cell (cx, cy):
// screen-sized, directly beside the anchor's frame across the desktop gap,
// centred along that side, each further cell one frame and one gap on.
wlr_box beside_frame(const wlr_box &anchor, int cx, int cy) {
	Output *o = out();
	auto along = [](int start, int extent, int length, int step) {
		if (step == 0) {
			return start + (extent - length) / 2;
		}
		if (step > 0) {
			return start + extent + config::desktop_gap
				+ (step - 1) * (length + config::desktop_gap);
		}
		return start + step * (length + config::desktop_gap);
	};
	return wlr_box{along(anchor.x, anchor.width, o->width, cx),
		along(anchor.y, anchor.height, o->height, cy), o->width, o->height};
}

// One desktop Super+N made: numbered `number`, its frame at `expected`,
// overlapping no other desktop, and the one shown after its chord.
void check_made_desktop(int number, const wlr_box &expected) {
	Desktop *made = server->canvas.desktops.by_number(number);
	bool at = made != nullptr && made->frame().x == expected.x
		&& made->frame().y == expected.y && made->frame().width == expected.width
		&& made->frame().height == expected.height;
	bool clear = made != nullptr;
	if (made != nullptr) {
		for (const Desktop &other : server->canvas.desktops.all()) {
			clear = clear
				&& (&other == made || !made->rect().intersects(other.rect()));
		}
	}
	check(made != nullptr && at && clear && out()->desktop_showing() == made,
		std::format("Super+{} made desktop {} at {},{} clear of the others and shown",
			number, number, expected.x, expected.y));
}

// The allocation steps: a drag and a pinch each run until `counted_frames`
// frames have gone by after `warm_ticks` ticks (which open the hint's
// texture, the first title of a shelf and the like). A tick's input damages
// the output once, so a tick is about a frame.
constexpr int warm_ticks = 5;
constexpr unsigned long counted_frames = 100;
constexpr int max_ticks = 1000; // a run whose frames never come ends anyway
int tick = 0;
unsigned long long allocations_before = 0;
unsigned long frames_seen = 0, frames_before = 0;
wl_listener frame_counter{};
Point grab_point{};
Rect dragged_from{};
double drag_travel = 0; // the furthest the window got from where it began
pid_t subscriber_pid = -1;

// First in each tick of a run: opens the counted span after the warm-up and
// reports, once it is over, the allocations and frames it held.
bool run_over(unsigned long long &made, unsigned long &frames) {
	if (tick == warm_ticks) {
		allocations_before = canvas_allocations();
		frames_before = frames_seen;
	}
	frames = frames_seen - frames_before;
	if (tick <= warm_ticks || (frames < counted_frames && tick < max_ticks)) {
		return false;
	}
	made = canvas_allocations() - allocations_before;
	return true;
}

// The pointer holding the title bar walks a triangle wave from the grab point.
void drag_tick(View &view) {
	int phase = tick % 40;
	double w = phase < 20 ? phase : 40 - phase;
	move_to(grab_point.x + 6 * w, grab_point.y + 3 * w);
	drag_travel = std::fmax(drag_travel, std::fabs(view.frame_rect().x - dragged_from.x));
	tick++;
}

// Presses the left button on `view`'s title bar, at the camera it is shown by.
void grab_title(View &view) {
	wlr_box f = view.frame_box();
	dragged_from = view.frame_rect();
	drag_travel = 0;
	grab_point = out()->canvas_to_local(f.x + 80, f.y + 16);
	move_to(grab_point.x, grab_point.y);
	click(BTN_LEFT, true);
	tick = 0;
}

std::string file_text(const std::string &path) {
	std::string text;
	if (FILE *f = std::fopen(path.c_str(), "r")) {
		char buf[4096];
		std::size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
			text.append(buf, n);
		}
		std::fclose(f);
	}
	return text;
}

int cascade_before = 0;
constexpr int cascade_windows = 9;

int run_step() {
	Output *o = out();
	View *one = view_titled("one"), *two = view_titled("two"), *three = view_titled("three");
	switch (step) {
	case 0:
		// The fake devices the checks drive: a keyboard that holds Super and
		// presses chords, a pointer that clicks and parks.
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		check(start_reporter(), "the gesture client started");
		spawn("foot -T one sh -c 'echo window one; exec sleep 600'");
		spawn("foot -T two sh -c 'echo window two; exec sleep 600'");
		spawn("foot -T three sh -c 'echo window three; exec sleep 600'");
		return 300;
	case 1:
		if ((mapped_views() < 3 || !client_said("MAPPED")) && retries++ < 100) {
			return -100; // retry
		}
		check(client_said("MAPPED"), "the gesture client's square is mapped");
		check(mapped_views() == 3, std::format("three foot windows mapped ({})", mapped_views()));
		check(server->canvas.desktops.size() == 1, "Desktop 1 exists at start");
		d1 = server->canvas.desktops.all().front().id();
		check(three && one && two && three->deco.ssd, "foot negotiated server-side decorations");
		shot("01-three-windows");
		return 400;
	case 2: {
		// Title bar hit-testing at the close/maximize/minimize buttons.
		wlr_box f = three->frame_box();
		auto hits = [&](DecoPart part) {
			Point at = button_centre(f, part);
			return three->deco.hit(at.x, at.y, 1).part == part;
		};
		check(hits(DecoPart::close), "close button hit");
		check(hits(DecoPart::maximize), "maximize button hit");
		check(hits(DecoPart::minimize), "minimize button hit");
		check(three->deco.hit(f.x + 60, f.y + 10, 1).part == DecoPart::title, "title hit");
		DecoHit h = three->deco.hit(f.x + f.width + 3, f.y + f.height + 3, 1);
		check(h.part == DecoPart::resize &&
			h.edges == static_cast<uint32_t>(WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM),
			"bottom-right resize corner");
		three->set_maximized(true);
		return 500;
	}
	case 3: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		Rect fr = three->frame_rect();
		Rect vp = o->viewport();
		check(std::fabs(fr.x - vp.x) < 1 && std::fabs(fr.w - vp.w) < 1 &&
			std::fabs(fr.y - vp.y) < 1 && std::fabs(fr.h - vp.h) < 1,
			std::format("maximized frame fills the screen ({:.0f},{:.0f} {:.0f}x{:.0f})", fr.x,
				fr.y, fr.w, fr.h));
		shot("02-maximized");
		return 400;
	}
	case 4:
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		three->set_maximized(false);
		cam0 = o->camera.current();
		// A swipe short of the hop threshold moves nothing: no free pan.
		swipe(3, 20, 0, 4);
		check(o->camera.current().cx == cam0.cx && !o->camera.panning(),
			"a 3-finger swipe short of the hop threshold moves nothing");
		// Fingers right, no desktop that way: the view stays on Desktop 1.
		swipe(3, 600, 0, 30);
		return 600;
	case 5:
		check(!o->overview.active() && o->camera.current().cx == cam0.cx
			&& o->camera.current().cy == cam0.cy && o->camera.current().zoom == 1
			&& !o->camera.panning() && o->interactive()
			&& server->hint.text == "No desktop that way",
			std::format("with no desktop that way, a 3-finger swipe stays on the desktop"
				" (dx={:.0f}, hint {})", o->camera.current().cx - cam0.cx, server->hint.text));
		// The free area to the right is a pan away (a drag, a scroll, or
		// Super+Alt+arrows for the user).
		o->pan(o->width, 0);
		shot("03-free-area");
		return 400;
	case 6: {
		o->pin_desktop();
		check(server->canvas.desktops.size() == 2, "pinned the free area as Desktop 2");
		Desktop *showing = o->desktop_showing();
		d2 = showing->id();
		check(showing && showing != desk(d1) && showing->number() == 2,
			"Desktop 2 is what we're looking at");
		three->move(showing->frame().x + 200, showing->frame().y + 150);
		check(server->canvas.desktop_of(*three) == showing,
			"window three now belongs to Desktop 2");
		return 300;
	}
	case 7:
		swipe(4, 0, -300, 20); // 4 fingers up: overview
		return 700;
	case 8:
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active(), "4-finger swipe up opened the overview");
		check(o->camera.current().zoom < 0.5,
			std::format("overview is zoomed out (zoom {:.3f})",
				o->camera.current().zoom));
		check(o->overview.target().desktop == d2,
			"overview target starts at the current desktop");
		shot("04-overview");
		return 400;
	case 9:
		swipe(3, -80, 0, 8); // 3 fingers left: jump to the desktop on the left
		return 500;
	case 10:
		check(o->overview.target().desktop == d1, "3-finger left jumped to Desktop 1");
		shot("05-hop-left");
		return 400;
	case 11:
		swipe(3, 0, -80, 8); // up: no desktop that way -> free spot one screen up
		return 400;
	case 12:
		check(!o->overview.target().desktop.has_value()
			&& o->overview.target().box.y == desk(d1)->frame().y - o->height,
			"no desktop above: target moved one screen up to a free spot");
		shot("06-free-spot-target");
		return 600;
	case 13:
		swipe(3, 0, 80, 8); // back down onto Desktop 1
		check(o->overview.target().desktop == d1, "3-finger down returned to Desktop 1");
		pinch(3, 1.5, 10); // spread: switch tiling on for Desktop 1
		return 500;
	case 14: {
		Rect a = one->frame_rect(), b = two->frame_rect();
		check(inside(a, desk(d1)->frame()) && inside(b, desk(d1)->frame()),
			"tiled windows stay inside Desktop 1");
		check(!a.intersects(b), "tiled windows do not overlap");
		// The first joiner took the whole board, the second halved it: two
		// side-by-side halves, whoever of the two stood higher up.
		View *left = a.x <= b.x ? one : two;
		View *right = left == one ? two : one;
		check(on_slice(left, slice_frame_of(*desk(d1), LEFT))
				&& on_slice(right, slice_frame_of(*desk(d1), RIGHT)),
			"the two windows took the board's halves");
		check(desk(d1)->tiling(), "the spread switched tiling on for Desktop 1");
		check(one->tiled && two->tiled, "tiled windows are told they are tiled");
		shot("07-tiled");
		board_one = a;
		board_two = b;
		pinch(3, 1.5, 10); // spread again: tiling off
		return 400;
	}
	case 15:
		check(!desk(d1)->tiling(), "a second spread switched tiling off");
		check(one->frame_rect().x == board_one.x && one->frame_rect().y == board_one.y
			&& two->frame_rect().x == board_two.x && two->frame_rect().y == board_two.y,
			"switching tiling off kept the windows where the board left them"
			" (untiled, they may re-fit their own size)");
		check(!one->tiled && !two->tiled,
			"switching tiling off told the windows they are no longer tiled");
		shot("07-untiled");
		pinch(3, 1.5, 10); // and back on: the combine below joins the board
		return 400;
	case 16: {
		check(desk(d1)->tiling(), "the third spread switched tiling on again");
		pinch(3, 0.6, 10); // gather: combine nearest desktop (2) into Desktop 1
		check(server->canvas.desktops.size() == 1,
			"pinch-in combined Desktop 2 into Desktop 1");
		check(server->canvas.desktop_of(*three) == desk(d1) && three->tiled,
			"the combined-in window joined Desktop 1's board");
		d2 = std::nullopt;
		return 500;
	}
	case 17: {
		// The 3-window board: the left half's holder keeps its top half for
		// the newcomer, which takes the bottom; the right half is untouched.
		View *left = one->frame_rect().x <= two->frame_rect().x ? one : two;
		View *right = left == one ? two : one;
		check(on_slice(left, slice_frame_of(*desk(d1), TL))
				&& on_slice(right, slice_frame_of(*desk(d1), RIGHT))
				&& on_slice(three, slice_frame_of(*desk(d1), BL)),
			"combine into a tiled desktop: the newcomer halved the left half");
		shot("08-combined-into-board");
		return 400;
	}
	case 18: {
		const wlr_box &frame = desk(d1)->frame();
		o->overview.new_desktop(frame.x + frame.width * 1.8, frame.y + frame.height / 2.0);
		d3 = o->overview.target().desktop;
		check(d3.has_value() && server->canvas.desktops.size() == 2,
			"new desktop created in the overview");
		return 400;
	}
	case 19:
		shot("09-new-desktop");
		swipe(4, 320, 0, 20); // 4-finger sideways: abandon the target desktop
		return 400;
	case 20: {
		check(server->canvas.desktops.size() == 1
			&& server->canvas.desktops.find(o->overview.target().desktop) == nullptr,
			"4-finger sideways swipe abandoned the desktop");
		check(mapped_views() == 3, "abandoning kept every window");
		// Abandoning (by key) the desktop a swipe is sliding away must end that swipe cleanly.
		const wlr_box &frame = desk(d1)->frame();
		o->overview.new_desktop(frame.x + frame.width * 1.8, frame.y + frame.height / 2.0);
		server->gestures.swipe_begin(t_ms += 8, 4);
		for (int i = 0; i < 6; i++) {
			server->gestures.swipe_update(t_ms += 8, 20, 0);
		}
		check(std::holds_alternative<Gestures::Abandon>(server->gestures.state()),
			"4-finger sideways swipe is sliding a desktop away");
		o->overview.abandon(o->overview.target().desktop);
		for (int i = 0; i < 6; i++) {
			server->gestures.swipe_update(t_ms += 8, 20, 0);
		}
		server->gestures.swipe_end(t_ms += 8, false);
		check(server->canvas.desktops.size() == 1
			&& std::holds_alternative<Gestures::Idle>(server->gestures.state()),
			"abandoning the swiped desktop mid-swipe ends the swipe cleanly");
		// A desktop created after an abandon reuses the freed number, never
		// the abandoned identity.
		std::optional<DesktopId> abandoned = o->overview.target().desktop;
		o->overview.new_desktop(frame.x + frame.width * 1.8, frame.y + frame.height / 2.0);
		Desktop *reused = server->canvas.desktops.by_number(2);
		check(abandoned.has_value()
			&& server->canvas.desktops.find(abandoned) == nullptr
			&& reused != nullptr && reused->id() != *abandoned && reused->number() == 2,
			"the freed number is reused, the abandoned id is not");
		o->overview.set_target(desk(d1)->frame(), d1, true);
		return 400;
	}
	case 21:
		swipe(4, 0, 300, 20); // 4-finger down: zoom into the target
		return 700;
	case 22: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(!o->overview.active() && o->camera.current().zoom == 1,
			"4-finger down returned to normal mode");
		Camera want = camera_for_box(desk(d1)->frame());
		check(std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1,
			"landed on Desktop 1");
		shot("10-desktop1");
		return 400;
	}
	case 23:
		one->set_minimized(true);
		check(one->minimized && server->focused_view != one,
			"minimize hides the window and moves focus");
		o->overview.enter();
		return 600;
	case 24:
		shot("11-overview-shelf");
		return 400;
	case 25: {
		if (!acted) {
			acted = true;
			// The shelf's thumbnail takes the click where it is drawn.
			Shelf shelf = overview_shelf_layout(*o);
			check(!shelf.empty() && shelf.front().view == one,
				"the minimized window sits on the shelf");
			if (!shelf.empty()) {
				const wlr_box &thumb = shelf.front().box;
				move_to(thumb.x + thumb.width / 2.0, thumb.y + thumb.height / 2.0);
				click(BTN_LEFT, true);
				click(BTN_LEFT, false);
			}
			check(!one->minimized && server->focused_view == one,
				"a click on the shelf's thumbnail restores the window");
			// Restoring a window leaves the overview: open it again to click.
			if (!o->overview.active()) {
				o->overview.enter();
			}
			return -600;
		}
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		// A click on the overview's empty canvas, left of Desktop 1.
		std::optional<Point> spot = empty_overview_spot();
		check(o->overview.active() && spot.has_value(),
			"the overview shows empty canvas left of Desktop 1");
		if (spot.has_value()) {
			overview_click = o->local_to_canvas(spot->x, spot->y);
			move_to(spot->x, spot->y);
			click(BTN_LEFT, true);
			click(BTN_LEFT, false);
		}
		return 700;
	}
	case 26: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		check(overview_click.has_value() && !o->overview.active()
				&& !o->overview.target().desktop.has_value()
				&& std::fabs(o->camera.current().cx - overview_click->x) < 1
				&& std::fabs(o->camera.current().cy - overview_click->y) < 1,
			"clicking empty overview canvas lands on a free area");
		shot("12-free-area");
		return 400;
	}
	case 27:
		// Super+Ctrl+Right on the keyboard: the nearest desktop to the right.
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTCTRL, true);
		key(KEY_RIGHT, true);
		key(KEY_RIGHT, false);
		key(KEY_LEFTCTRL, false);
		key(KEY_LEFTMETA, false);
		return 600;
	case 28: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Camera want = camera_for_box(desk(d1)->frame());
		check(std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1,
			"keyboard jump to the desktop in that direction");
		return 400; // the scale scenario continues from here
	}
	case 29:
		// A pointer that parks the cursor off-centre, for the Super pinch.
		move_to(900, 300);
		check(std::fabs(server->cursor->x - 900) < 0.5
			&& std::fabs(server->cursor->y - 300) < 0.5,
			"the fake pointer placed the cursor");
		super_start = o->camera.current();
		keep({one, two, three}); // Desktop 1's board, before any zoom
		frame0 = desk(d1)->frame();
		return 300;
	case 30: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		// Super + 2-finger pinch from the normal view: the shown desktop
		// scales with the fingers. Held open a step for a mid-pinch shot.
		key(KEY_LEFTMETA, true);
		pinch_down(2);
		check(std::holds_alternative<Gestures::DesktopScale>(server->gestures.state()),
			"Super + 2-finger pinch begins scaling the shown desktop");
		pinch_move(0.8);
		Camera cam = o->camera.current();
		check(std::fabs(cam.zoom - 0.8) < 1e-9
			&& std::fabs(cam.cx - super_start.cx) < 1e-9,
			std::format("the live zoom follows the fingers ({:.2f})",
				cam.zoom));
		check(desk(d1)->frame().width == std::lround(o->width / 0.8)
			&& desk(d1)->frame().height == std::lround(o->height / 0.8),
			"the frame is the base size divided by the scale");
		check(!o->overview.active(), "the pinch stays in the normal view");
		shot("13-super-pinch-mid");
		return 400;
	}
	case 31: {
		// Released just off a step: the scale settles on the nearest
		// 5 % step (a pinch to 0.78 settles at 0.80).
		pinch_move(0.78);
		pinch_up(false);
		check(std::fabs(desk(d1)->scale() - 0.8) < 1e-9,
			"released, the scale settles on the nearest step (0.80)");
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(),
			"releasing Super after a Super pinch does not open the launcher");
		return 400;
	}
	case 32: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		Camera want = camera_for_box(desk(d1)->frame(), desk(d1)->scale());
		check(std::fabs(o->camera.current().zoom - want.zoom) < 1e-9
			&& std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1,
			"the camera came to rest on the desktop at its scale");
		check(o->interactive(), "the resting zoom is interactive");
		std::string changed = changed_boxes();
		check(changed.empty(), std::format(
			"settled at 80 %, the tiled board kept every box{}", changed));
		check(told_output_scale(),
			"the clients were not told the scale: still the output's");
		check(server->hint.text == "Desktop 1 · 80 %",
			"the settle is named like a completed action");
		shot("14-desktop-80");
		return 400;
	}
	case 33:
		spawn(std::format("foot -T marker sh -c 'cat > {}/scenarios/gestures/marker'",
			tmp_root).c_str());
		return 300;
	case 34: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		// A window opened on the 80 % desktop: the pointer reaches it
		// through the camera, landing where it points.
		View *marker = view_titled("marker");
		if ((marker == nullptr || !marker->mapped) && retries++ < 100) {
			return -100; // retry until mapped
		}
		Camera cam = o->camera.drawn();
		double z = cam.zoom;
		double left = cam.cx - o->width / (2.0 * z);
		double top = cam.cy - o->height / (2.0 * z);
		wlr_box f = marker->frame_box();
		double sx = (f.x + f.width / 2.0 - left) * z;
		double sy = (f.y + f.height / 2.0 - top) * z;
		move_to(sx, sy);
		// Where the click lands, computed the way the camera does it.
		double cx = left + sx / z, cy = top + sy / z;
		check(std::fabs(server->seat->pointer_state.sx - (cx - marker->x)) < 1.0
			&& std::fabs(server->seat->pointer_state.sy - (cy - marker->y)) < 1.0,
			std::format("the pointer lands where it points ({:.1f},{:.1f} surface-local)",
				server->seat->pointer_state.sx, server->seat->pointer_state.sy));
		check(server->seat->pointer_state.focused_surface == marker->surface(),
			"the pointer focuses the window through the camera");
		click(BTN_LEFT, true);
		click(BTN_LEFT, false);
		check(server->focused_view == marker, "the click focused the window");
		shot("15-click-80");
		return 300;
	}
	case 35:
		key(KEY_O, true);
		key(KEY_O, false);
		key(KEY_K, true);
		key(KEY_K, false);
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 400;
	case 36: {
		// What was typed reached the client through the camera.
		std::string path = std::format("{}/scenarios/gestures/marker", tmp_root);
		FILE *f = std::fopen(path.c_str(), "r");
		char buf[16] = {};
		bool got = f != nullptr && std::fgets(buf, sizeof(buf), f) != nullptr;
		if (f != nullptr) {
			std::fclose(f);
		}
		check(got && std::string_view(buf) == "ok\n",
			std::format("typing on the 80 % desktop reached the client ({})", buf));
		// The marker joined the board by halving the largest held slice, the
		// right half: its holder keeps the top, the marker takes the bottom.
		// A join moves nobody else: the others kept their boxes.
		View *marker = view_titled("marker");
		View *right = one->frame_rect().x <= two->frame_rect().x ? two : one;
		bool others_kept = marker != nullptr;
		for (const auto &[view, box] : kept) {
			if (view != right) {
				Rect now = view->frame_rect();
				others_kept = others_kept && view->mapped && now.x == box.x
					&& now.y == box.y && now.w == box.w && now.h == box.h;
			}
		}
		check(marker != nullptr && marker->seated_on(*d1) && marker->tiled
				&& inside(marker->frame_rect(), desk(d1)->frame()),
			"the window opened on the 80 % desktop joined the board in its frame");
		check(on_slice(marker, slice_frame_of(*desk(d1), RB))
				&& on_slice(right, slice_frame_of(*desk(d1), RT)) && others_kept,
			"it halved the right half; every other window kept its box");
		keep({one, two, three, marker});
		// Near 100 % catches 100 % exactly: 100 is one of the steps.
		key(KEY_LEFTMETA, true);
		pinch_down(2);
		pinch_move(1.27); // raw 0.8 x 1.27 = 1.016: nearest step is 1.00
		pinch_up(false);
		check(desk(d1)->scale() == 1.0,
			"a pinch settling near 100 % catches 100 % exactly");
		key(KEY_LEFTMETA, false);
		return 400;
	}
	case 37: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->camera.current().zoom == 1 && o->interactive(),
			"back at 100 %: zoom 1, interactive");
		std::string changed = changed_boxes();
		check(changed.empty() && told_output_scale(), std::format(
			"back at 100 %, every window kept its box and the output's scale{}", changed));
		// Spread past 100 % for the 125 % frame.
		key(KEY_LEFTMETA, true);
		pinch_down(2);
		pinch_move(1.25);
		pinch_up(false);
		check(std::fabs(desk(d1)->scale() - 1.25) < 1e-9,
			"spreading further settles at 125 %");
		key(KEY_LEFTMETA, false);
		return 400;
	}
	case 38: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(std::fabs(o->camera.current().zoom - 1.25) < 1e-9 && o->interactive()
			&& desk(d1)->frame().width == std::lround(o->width / 1.25),
			"the desktop rests at 125 %, its frame the base size over 1.25");
		std::string changed = changed_boxes();
		check(changed.empty() && told_output_scale(), std::format(
			"at 125 %, every window kept its box and the output's scale{}", changed));
		shot("16-desktop-125");
		return 400;
	}
	case 39:
		// The overview from a scaled desktop: its tab names the scale.
		o->overview.enter();
		return 600;
	case 40:
		check(o->overview.active(), "the overview opened over the 125 % desktop");
		shot("17-overview-125");
		o->overview.exit();
		return 700;
	case 41:
		// Fingers down, no desktop below: the view stays.
		hop_from = o->camera.destination();
		swipe(3, 0, 300, 10);
		return 600;
	case 42: {
		Camera cam = o->camera.current();
		check(cam.cx == hop_from.cx && cam.cy == hop_from.cy && cam.zoom == 1.25
			&& o->desktop_showing() == desk(d1) && o->interactive(),
			"a swipe toward no desktop leaves the 125 % desktop where it is");
		// A free view two screens down, clear of Desktop 1's frame at every
		// scale the later steps give it (the view pinned there next grows as
		// it scales down to 90 %); a pan keeps the zoom.
		o->pan(0, 2 * o->height / 1.25);
		cam = o->camera.current();
		check(o->desktop_showing() == nullptr && cam.zoom == 1.25 && o->interactive(),
			std::format("the free view below keeps the zoom, interactive ({:.2f})", cam.zoom));
		// Super + pinch on a free view: it is pinned as a desktop first, at
		// the view's zoom.
		std::size_t before = server->canvas.desktops.size();
		key(KEY_LEFTMETA, true);
		pinch_down(2);
		check(server->canvas.desktops.size() == before + 1
			&& std::holds_alternative<Gestures::DesktopScale>(
				server->gestures.state())
			&& o->desktop_showing() != nullptr
			&& std::fabs(o->desktop_showing()->scale() - 1.25) < 1e-9,
			"a Super pinch on a free view pins it as a desktop first, at its zoom");
		pinch_move(0.72); // raw 1.25 x 0.72 = 0.9
		check(std::fabs(o->camera.current().zoom - 0.9) < 1e-9,
			"the pinned desktop scales with the fingers");
		shot("18-free-view-pinned");
		return 400;
	}
	case 43: {
		pinch_up(false);
		check(o->desktop_showing() != nullptr
			&& std::fabs(o->desktop_showing()->scale() - 0.9) < 1e-9,
			"the pinned desktop settled at 90 %");
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(),
			"no launcher from a Super pinch on a free view");
		return 400;
	}
	case 44: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		// Without Super a 2-finger pinch still belongs to the client.
		if (!acted) {
			acted = true;
			Camera start = o->camera.current();
			park_on_reporter();
			pinch_down(2);
			check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
				"2-finger pinch without Super forwards to the client");
			pinch_move(1.5);
			pinch_move(0.6);
			pinch_up(false);
			leave_reporter();
			check(std::holds_alternative<Gestures::Idle>(server->gestures.state()),
				"the forwarded pinch ended cleanly");
			check(std::fabs(o->camera.current().zoom - start.zoom) < 1e-9
				&& std::fabs(o->camera.current().cx - start.cx) < 1e-9,
				"without Super the canvas did not scale");
		}
		const std::string want = "PINCH BEGIN 2|PINCH UPDATE 0.0 0.0 1.50 0.0"
			"|PINCH UPDATE 0.0 0.0 0.60 0.0|PINCH END 0";
		if (awaiting(want)) {
			return -50;
		}
		check_client_got(want, "the client under the pointer received the 2-finger"
			" pinch without Super");
		shot("19-pinch-without-super");
		return 400; // the tiled-scale scenario continues
	}
	case 45:
		// Back to Desktop 1 for the tiling interaction: Super+Ctrl+Right's
		// path, at its scale.
		o->goto_desktop(*desk(d1));
		return 700;
	case 46: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		Camera want = camera_for_box(desk(d1)->frame(), desk(d1)->scale());
		check(std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1
			&& std::fabs(o->camera.current().zoom - want.zoom) < 1e-9,
			"going to a desktop eases onto it at its scale");
		// Desktop 1 has been tiled since the combine; the board the marker
		// halved a slice of at 80 % has kept its boxes since.
		check(desk(d1)->tiling(), "tiling is on for Desktop 1");
		std::string changed = changed_boxes();
		check(changed.empty(), std::format(
			"the board kept its boxes through the settles at 100 % and 125 %{}", changed));
		shot("20-tiled-125");
		return 400;
	}
	case 47: {
		// Super + pinch on a tiled desktop: the settled scale leaves the
		// board alone.
		key(KEY_LEFTMETA, true);
		pinch_down(2);
		pinch_move(0.64); // raw 1.25 x 0.64 = 0.8
		pinch_up(false);
		check(std::fabs(desk(d1)->scale() - 0.8) < 1e-9,
			"the tiled desktop settled at 80 %");
		key(KEY_LEFTMETA, false);
		return 400;
	}
	case 48: {
		std::string changed = changed_boxes();
		check(changed.empty(), std::format(
			"settled at 80 %, the tiled board kept every box{}", changed));
		shot("21-tiled-80");
		// Tiling off and a maximized window: the zoom rounds below cover
		// floating and maximized windows alike.
		key(KEY_LEFTMETA, true);
		key(KEY_T, true);
		key(KEY_T, false);
		key(KEY_LEFTMETA, false);
		return 600; // untiled, foot may re-fit its own size
	}
	case 49:
		check(!desk(d1)->tiling(), "Super+T switched tiling off");
		three->set_maximized(true);
		return 500;
	case 50: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		Rect fr = three->frame_rect();
		Rect vp = o->viewport();
		check(three->mode == WindowMode::maximized
			&& std::fabs(fr.x - vp.x) < 1 && std::fabs(fr.y - vp.y) < 1
			&& std::fabs(fr.w - vp.w) < 1 && std::fabs(fr.h - vp.h) < 1,
			std::format("maximized at 80 %, the window fills the screen ({:.0f}x{:.0f})",
				fr.w, fr.h));
		keep({one, two, three, view_titled("marker")});
		zoom_round = 0;
		zoom_changes.clear();
		return 300;
	}
	case 51: {
		// Twelve settles, six zooms in and out between 50 % and 200 %, on
		// uneven steps too: no zoom changes a window, however many there
		// are. Odd rounds look at the settle before them, 450 ms on, once
		// anything it set off would have landed.
		static constexpr std::array targets{2.0, 0.5, 1.35, 0.7, 1.15, 0.5,
			2.0, 0.85, 1.5, 0.6, 1.25, 0.6};
		std::size_t round = static_cast<std::size_t>(zoom_round / 2);
		if (zoom_round % 2 == 1) {
			std::string changed = changed_boxes();
			bool told = told_output_scale();
			if (std::fabs(desk(d1)->scale() - targets[round]) > 1e-9 || !changed.empty()
					|| !told) {
				zoom_changes += std::format(" [round {} at {:.2f}:{}{}]", round + 1,
					desk(d1)->scale(), changed, told ? "" : " clients told a scale");
			}
			if (round == 0) {
				shot("22-desktop-200");
			} else if (round == 1) {
				shot("23-desktop-50");
			}
			zoom_round++;
			return -100;
		}
		if (round == targets.size()) {
			check(zoom_changes.empty(), std::format(
				"twelve settles between 50 % and 200 % changed no window{}", zoom_changes));
			return 300;
		}
		super_pinch_to(desk(d1)->scale(), targets[round]);
		zoom_round++;
		return -450;
	}
	case 52: {
		// Super+0: back to 100 % from 60 %.
		check(std::fabs(desk(d1)->scale() - 0.6) < 1e-9, "the zoom rounds ended at 60 %");
		key(KEY_LEFTMETA, true);
		key(KEY_0, true);
		key(KEY_0, false);
		key(KEY_LEFTMETA, false);
		check(desk(d1)->scale() == 1.0 && server->hint.text == "Desktop 1 · 100 %",
			"Super+0 returned the desktop shown to 100 %, named in a hint");
		check(!server->launcher.is_open(), "Super+0 opens no launcher");
		return 700;
	}
	case 53: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		Camera want = camera_for_box(desk(d1)->frame(), 1.0);
		check(o->camera.current().zoom == 1 && o->interactive()
			&& std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1,
			"the camera eased with it and rests on the desktop at 100 %");
		std::string changed = changed_boxes();
		check(changed.empty() && told_output_scale()
			&& three->mode == WindowMode::maximized, std::format(
			"the reset changed no window either, the maximized one included{}", changed));
		const wlr_box &f = desk(d1)->frame();
		check(f.x == frame0.x && f.y == frame0.y && f.width == frame0.width
			&& f.height == frame0.height,
			std::format("after every zoom, back at 100 % the frame is where it began"
				" ({},{} vs {},{})", f.x, f.y, frame0.x, frame0.y));
		shot("24-reset-super-0");
		super_pinch_to(1.0, 1.25);
		return 500;
	}
	case 54:
		check(std::fabs(desk(d1)->scale() - 1.25) < 1e-9, "spread to 125 % again");
		o->overview.enter();
		return 600;
	case 55: {
		// `0` in the overview: the target desktop back to 100 %.
		check(o->overview.active() && o->overview.target().desktop == d1,
			"the overview targets the 125 % desktop");
		key(KEY_0, true);
		key(KEY_0, false);
		const wlr_box &f = desk(d1)->frame();
		const wlr_box &t = o->overview.target().box;
		check(desk(d1)->scale() == 1.0 && f.width == o->width && f.height == o->height,
			"0 in the overview returned the target desktop to 100 %, its frame the base size");
		check(t.x == f.x && t.y == f.y && t.width == f.width && t.height == f.height,
			"the overview's target follows the resized frame");
		check(server->hint.text == "Desktop 1 · 100 %", "the overview's reset is named in a hint");
		shot("25-overview-reset");
		o->overview.exit();
		return 700;
	}
	case 56: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		Camera want = camera_for_box(desk(d1)->frame(), 1.0);
		check(!o->overview.active() && o->camera.current().zoom == 1 && o->interactive()
			&& std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1,
			"leaving the overview rests on the desktop at 100 %");
		std::string changed = changed_boxes();
		check(changed.empty() && told_output_scale(), std::format(
			"no window changed through the overview's reset{}", changed));
		return 400;
	}
	case 57:
		// The board through a zoom: laid out at 50 % (the maximized window
		// stays out of it), then shown at 200 %, where the frame's edges
		// pass the outer windows' centres.
		super_pinch_to(1.0, 0.5);
		return 450;
	case 58:
		key(KEY_LEFTMETA, true);
		key(KEY_T, true);
		key(KEY_T, false);
		key(KEY_LEFTMETA, false);
		return 600;
	case 59: {
		View *marker = view_titled("marker");
		check(desk(d1)->tiling() && one->seated_on(*d1) && two->seated_on(*d1)
			&& marker != nullptr && marker->seated_on(*d1) && !three->board_seat.has_value(),
			"Super+T tiled the 50 % desktop, the maximized window off the board");
		keep({one, two, marker});
		super_pinch_to(0.5, 2.0);
		return 450;
	}
	case 60: {
		std::string changed = changed_boxes();
		check(std::fabs(desk(d1)->scale() - 2.0) < 1e-9 && changed.empty(), std::format(
			"from 50 % to 200 % the board kept every box{}", changed));
		// At 200 % the frame no longer covers every boarded window's centre:
		// the board, not the centre, keeps them the desktop's.
		int outside = 0;
		for (const auto &[view, box] : kept) {
			outside += !desk(d1)->contains(box.cx(), box.cy());
		}
		check(outside > 0, std::format("at 200 % {} boarded window(s) lie past the frame's edge",
			outside));
		spawn("foot -T late sh -c 'echo late; exec sleep 600'");
		return 300;
	}
	case 61: {
		View *late = view_titled("late");
		if ((late == nullptr || !late->mapped) && retries++ < 100) {
			return -100; // retry until mapped
		}
		return 700; // the join's configures land
	}
	case 62: {
		// A window opened at 200 % joins the board there: it halves the
		// largest held slice, the right half, and moves nobody else.
		View *late = view_titled("late");
		View *marker = view_titled("marker");
		// The join already moved the halved slice's holder onto its kept
		// half: after it, exactly one window holds the top-right quarter.
		View *rholder = slice_of(one) == RT ? one
			: slice_of(two) == RT ? two : marker;
		bool others_kept = late != nullptr && rholder != nullptr;
		for (const auto &[view, box] : kept) {
			if (view != rholder) {
				Rect now = view->frame_rect();
				others_kept = others_kept && view->mapped && now.x == box.x
					&& now.y == box.y && now.w == box.w && now.h == box.h;
			}
		}
		check(late != nullptr && late->tiled && late->seated_on(*d1)
				&& inside(late->frame_rect(), desk(d1)->frame()),
			"a window opened at 200 % joined the board in its frame");
		check(rholder != nullptr && slice_of(rholder) == RT && slice_of(late) == RB
				&& others_kept,
			"it halved the right half; every other window kept its box");
		shot("26-tiled-200-join");
		return 400;
	}
	case 63:
		// Views at any zoom: back to 100 %, then down to the 25 % floor.
		key(KEY_LEFTMETA, true);
		key(KEY_0, true);
		key(KEY_0, false);
		key(KEY_LEFTMETA, false);
		return 700;
	case 64:
		super_pinch_to(1.0, 0.25);
		return 500;
	case 65: {
		check(std::fabs(desk(d1)->scale() - 0.25) < 1e-9 && o->interactive(),
			std::format("the scale reaches down to 25 % ({:.2f})", desk(d1)->scale()));
		shot("27-desktop-25");
		// A 2-finger scroll over the empty canvas moves nothing without
		// Super; with Super held it looks away, over a window too.
		std::optional<Point> spot = empty_spot();
		std::optional<Point> window = window_on_screen();
		check(spot.has_value() && window.has_value(),
			"the 25 % view shows empty canvas and a window");
		scrolled_from = o->camera.current();
		if (spot.has_value()) {
			move_to(spot->x, spot->y);
		}
		for (int i = 0; i < 4; i++) {
			scroll(40);
		}
		check(o->camera.current() == scrolled_from,
			"a 2-finger scroll over empty canvas without Super moves nothing");
		if (window.has_value()) {
			move_to(window->x, window->y);
		}
		key(KEY_LEFTMETA, true);
		for (int i = 0; i < 4; i++) {
			scroll(40);
		}
		key(KEY_LEFTMETA, false);
		return 300;
	}
	case 66: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		if (!acted) {
			acted = true;
			Camera cam = o->camera.current();
			check(cam.zoom == 0.25 && o->interactive() && std::fabs(cam.cy - scrolled_from.cy) > 100,
				std::format("a Super 2-finger scroll over a window pans the 25 % view at its"
					" zoom and leaves it interactive (zoom {:.2f}, panned {:.0f})", cam.zoom,
					cam.cy - scrolled_from.cy));
			// A 2-finger pinch without Super belongs to the client under the
			// pointer at any zoom, and never zooms the view.
			park_on_reporter();
			pinch_down(2);
			check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
				"a 2-finger pinch without Super is forwarded at 25 %");
			pinch_move(1.6);
			pinch_up(false);
			leave_reporter();
			check(o->camera.current().zoom == 0.25, "the plain pinch left the zoom at 25 %");
		}
		const std::string want = "PINCH BEGIN 2|PINCH UPDATE 0.0 0.0 1.60 0.0|PINCH END 0";
		if (awaiting(want)) {
			return -50;
		}
		check_client_got(want, "at 25 % the client under the pointer received the"
			" 2-finger pinch");
		// A 3-finger swipe left off the 25 % desktop, where no desktop lies.
		hop_from = o->camera.destination();
		swipe(3, -900, 0, 30);
		return 600;
	}
	case 67: {
		Camera cam = o->camera.current();
		check(cam.cx == hop_from.cx && cam.cy == hop_from.cy && cam.zoom == 0.25
			&& !o->camera.panning() && o->interactive(),
			std::format("a swipe toward no desktop leaves the 25 % view where it is"
				" (dx {:.0f})", cam.cx - hop_from.cx));
		// A free view a screen to the left; a pan keeps the zoom.
		o->pan(-o->width / 0.25, 0);
		cam = o->camera.current();
		check(o->desktop_showing() == nullptr && cam.zoom == 0.25 && o->interactive(),
			std::format("the free view beside it keeps the 25 % zoom, interactive ({:.2f})",
				cam.zoom));
		shot("28-free-view-25");
		return 300;
	}
	case 68:
		o->overview.enter();
		return 700;
	case 69:
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active() && o->camera.current().zoom < 0.25,
			std::format("the overview opens further out than the 25 % view (zoom {:.3f})",
				o->camera.current().zoom));
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 800;
	case 70:
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(!o->overview.active() && std::fabs(o->camera.current().zoom - 0.25) < 1e-9
			&& o->interactive(),
			std::format("leaving the overview into the free view it came from keeps its zoom"
				" ({:.2f})", o->camera.current().zoom));
		key(KEY_LEFTMETA, true);
		key(KEY_0, true);
		key(KEY_0, false);
		key(KEY_LEFTMETA, false);
		return 700;
	case 71:
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->camera.current().zoom == 1 && o->interactive()
			&& server->hint.text == "Free area · 100 %",
			std::format("Super+0 returns a free view to 100 % (zoom {:.2f}, hint {})",
				o->camera.current().zoom, server->hint.text));
		return 300;
	case 72: {
		// Hops in the normal view, on a row far from the rest: A, B at 80 %
		// and C, a screen or so apart.
		int w = o->width, h = o->height;
		int x0 = 100000, y0 = 100000;
		hop_a = server->canvas.desktops.create(wlr_box{x0, y0, w, h}).id();
		hop_b = server->canvas.desktops.create(
			wlr_box{x0 + 2 * w, y0, w * 5 / 4, h * 5 / 4}, 0.8).id();
		hop_c = server->canvas.desktops.create(wlr_box{x0 + 4 * w, y0, w, h}).id();
		o->camera.jump(camera_for_box(desk(hop_a)->frame(), 1));
		return 300;
	}
	case 73:
		check(o->desktop_showing() == desk(hop_a) && o->interactive(), "on desktop A of the row");
		// Two quick swipes right: the second starts from where the first
		// lands, so they walk two desktops.
		swipe(3, 200, 0, 8);
		swipe(3, 200, 0, 8);
		return 900;
	case 74: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Camera want = camera_for_box(desk(hop_c)->frame(), 1);
		Camera cam = o->camera.current();
		check(cam.cx == want.cx && cam.cy == want.cy && cam.zoom == 1 && o->interactive(),
			std::format("two quick 3-finger swipes walked two desktops, A to B to C"
				" (off by {:.0f},{:.0f})", cam.cx - want.cx, cam.cy - want.cy));
		shot("29-hopped-to-c");
		server->hint.set("");
		swipe(3, 300, 0, 10); // nothing right of C
		return 600;
	}
	case 75: {
		Camera c = camera_for_box(desk(hop_c)->frame(), 1);
		Camera cam = o->camera.current();
		check(cam.cx == c.cx && cam.cy == c.cy && cam.zoom == 1 && !o->camera.panning()
			&& o->interactive() && server->hint.text == "No desktop that way",
			std::format("with no desktop right of C, a swipe stays on C and says so (hint {})",
				server->hint.text));
		swipe(3, -1500, 0, 60); // a long swipe left: one hop all the same
		return 900;
	}
	case 76: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Camera b = camera_for_box(desk(hop_b)->frame(), 0.8);
		Camera cam = o->camera.current();
		check(cam.cx == b.cx && cam.cy == b.cy && std::fabs(cam.zoom - 0.8) < 1e-9
			&& o->interactive(),
			std::format("a long swipe hops once, onto B at its 80 % (zoom {:.2f})", cam.zoom));
		shot("30-hopped-to-b-80");
		// A sloppy 3-finger swipe the touchpad reports as a pinch: its
		// travel hops all the same, here right onto C.
		server->gestures.pinch_begin(t_ms, 3);
		for (int i = 0; i < 10; i++) {
			t_ms += 8;
			server->gestures.pinch_update(t_ms, 30, 0, 1.02, 0);
		}
		t_ms += 8;
		server->gestures.pinch_end(t_ms, false);
		return 900;
	}
	case 77: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Camera c = camera_for_box(desk(hop_c)->frame(), 1);
		check(o->camera.current().cx == c.cx && o->desktop_showing() == desk(hop_c),
			"a 3-finger pinch with travel in the normal view hops like a swipe, onto C");
		// A free view three screens below B (the earlier desktops lie up and
		// to the left, inside an upward swipe's cone): down, nothing; up, B.
		Camera b = camera_for_box(desk(hop_b)->frame(), 0.8);
		hop_from = Camera{.cx = b.cx, .cy = b.cy + 3 * o->height, .zoom = 1};
		o->camera.jump(hop_from);
		server->hint.set("");
		swipe(3, 0, 300, 10);
		return 600;
	}
	case 78: {
		Camera cam = o->camera.current();
		check(o->desktop_showing() == nullptr && cam.cx == hop_from.cx && cam.cy == hop_from.cy
			&& !o->camera.panning() && server->hint.text == "No desktop that way",
			"on a free view, a swipe toward no desktop stays and says so");
		swipe(3, 0, -300, 10);
		return 900;
	}
	case 79: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Camera b = camera_for_box(desk(hop_b)->frame(), 0.8);
		Camera cam = o->camera.current();
		check(cam.cx == b.cx && cam.cy == b.cy && std::fabs(cam.zoom - 0.8) < 1e-9
			&& o->desktop_showing() == desk(hop_b),
			"from a free view, a swipe hops onto the desktop that way, B at its 80 %");
		// B tiled, so the launch below would show if a tiled desktop the
		// overview came from captured the new window.
		key(KEY_LEFTMETA, true);
		key(KEY_T, true);
		key(KEY_T, false);
		key(KEY_LEFTMETA, false);
		return 400;
	}
	case 80:
		check(desk(hop_b)->tiling(), "Super+T tiled B");
		o->overview.enter();
		return 700;
	case 81:
		check(o->overview.active() && o->overview.target().desktop == hop_b,
			"the overview opened over B, its target");
		swipe(3, 0, 80, 8); // nothing below B: the target moves to a free spot
		return 500;
	case 82: {
		check(o->overview.active() && !o->overview.target().desktop.has_value(),
			"the overview's target moved to a free spot below B");
		// Launch an app from the overview: Super+Space opens the launcher.
		key(KEY_LEFTMETA, true);
		key(KEY_SPACE, true);
		key(KEY_SPACE, false);
		key(KEY_LEFTMETA, false);
		tap_keys({KEY_F, KEY_O, KEY_O, KEY_T});
		return 300;
	}
	case 83:
		check(server->launcher.is_open(), "Super+Space opened the launcher over the overview");
		remember_views();
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 300;
	case 84: {
		View *launched = launched_view();
		if (launched == nullptr && retries++ < 100) {
			return -100;
		}
		wlr_box target = o->overview.target().box;
		Rect fr = launched != nullptr ? launched->frame_rect() : Rect{};
		check(launched != nullptr && o->overview.active()
			&& wlr_box_contains_point(&target, fr.cx(), fr.cy())
			&& !launched->board_seat.has_value(),
			std::format("an app launched from the overview opened in its target, the free"
				" spot, not on B (centre {:.0f},{:.0f}; target {},{} {}x{})", fr.cx(), fr.cy(),
				target.x, target.y, target.width, target.height));
		shot("31-launched-into-target");
		return 300;
	}
	case 85: // an app that takes a second to appear, launched from the overview
		key(KEY_LEFTMETA, true);
		key(KEY_SPACE, true);
		key(KEY_SPACE, false);
		key(KEY_LEFTMETA, false);
		tap_keys({KEY_T, KEY_A, KEY_R, KEY_D, KEY_Y});
		return 300;
	case 86:
		check(server->launcher.is_open(), "the launcher opened over the overview again");
		remember_views();
		launch_box = o->overview.target().box;
		tap_keys({KEY_ENTER, KEY_ESC}); // launch, then Escape back to B at once
		return 300;
	case 87:
		check(!o->overview.active() && launched_view() == nullptr,
			"Escape closed the overview before the launched window appeared");
		return 100;
	case 88: {
		View *tardy = launched_view();
		if (tardy == nullptr && retries++ < 100) {
			return -100;
		}
		Rect fr = tardy != nullptr ? tardy->frame_rect() : Rect{};
		check(tardy != nullptr && wlr_box_contains_point(&launch_box, fr.cx(), fr.cy())
				&& o->desktop_showing() == desk(hop_b),
			std::format("the late window opened in the free spot it was launched for, not on B"
				" where the overview closed to (centre {:.0f},{:.0f})", fr.cx(), fr.cy()));
		check(tardy != nullptr && server->focused_view != tardy
				&& server->hint.text == "Opened in a free area",
			std::format("unseen, it left the keyboard alone, and a hint said where (hint {})",
				server->hint.text));
		return 300;
	}
	case 89: // on B, far from the canvas origin: a window asking to start maximized
		remember_views();
		spawn("exec foot --maximized -T maxhere");
		return 300;
	case 90: {
		View *maxhere = launched_view();
		if (maxhere == nullptr && retries++ < 100) {
			return -100;
		}
		Rect fr = maxhere != nullptr ? maxhere->frame_rect() : Rect{};
		check(maxhere != nullptr && maxhere->mode == WindowMode::maximized
				&& o->viewport().contains(fr.cx(), fr.cy()) && server->focused_view == maxhere,
			std::format("a window starting maximized filled the screen being looked at, and"
				" took the keyboard (centre {:.0f},{:.0f})", fr.cx(), fr.cy()));
		shot("32-started-maximized-here");
		o->overview.enter();
		return 700;
	}
	case 91:
		check(o->overview.active() && o->overview.target().desktop == hop_b,
			"the overview opened over B again");
		swipe(3, 0, 80, 8); // the target down to the free spot below B again
		return 500;
	case 92:
		check(!o->overview.target().desktop.has_value(),
			"the overview's target moved to the free spot below B again");
		key(KEY_LEFTMETA, true);
		key(KEY_SPACE, true);
		key(KEY_SPACE, false);
		key(KEY_LEFTMETA, false);
		tap_keys({KEY_B, KEY_E, KEY_L, KEY_A, KEY_T, KEY_E, KEY_D});
		return 300;
	case 93:
		check(server->launcher.is_open(), "the launcher opened over the overview a third time");
		remember_views();
		launch_box = o->overview.target().box;
		tap_keys({KEY_ENTER, KEY_ESC});
		return 300;
	case 94: {
		View *belated = launched_view();
		if (belated == nullptr && retries++ < 100) {
			return -100;
		}
		Rect fr = belated != nullptr ? belated->frame_rect() : Rect{};
		check(belated != nullptr && belated->mode == WindowMode::maximized
				&& !o->overview.active()
				&& wlr_box_contains_point(&launch_box, fr.cx(), fr.cy())
				&& o->desktop_showing() == desk(hop_b),
			std::format("a late window starting maximized filled the free spot it was launched"
				" for, not B (centre {:.0f},{:.0f})", fr.cx(), fr.cy()));
		return 0;
	}
	case 95:
		// The four-finger-hops preset: every 3 and 4 swaps roles.
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		server->gestures.set_options(
			{.swipes = settings::SwipePreset::four_finger_hops, .pinch_scale = true});
		check(server->gestures.options().swipes == settings::SwipePreset::four_finger_hops,
			"set_options installed the four-finger-hops preset");
		check(help_names("4 fingers") && help_names("3 down")
				&& help_names("4-finger pinch"),
			"the help line names the four-finger preset's gestures");
		swipe(4, -200, 0, 8); // 4 fingers left: a hop, like 3 fingers today
		return 900;
	case 96: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Camera a = camera_for_box(desk(hop_a)->frame(), 1);
		Camera cam = o->camera.current();
		check(cam.cx == a.cx && cam.cy == a.cy && std::fabs(cam.zoom - 1) < 1e-9
				&& o->desktop_showing() == desk(hop_a),
			std::format("a 4-finger swipe hopped from B onto A in the normal view"
				" (off by {:.0f},{:.0f})", cam.cx - a.cx, cam.cy - a.cy));
		shot("33-four-finger-hop");
		swipe(3, 0, -300, 20); // 3 fingers up: the overview
		return 700;
	}
	case 97:
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active() && o->camera.current().zoom < 0.5,
			std::format("a 3-finger swipe up opened the overview (zoom {:.3f})",
				o->camera.current().zoom));
		check(o->overview.target().desktop == hop_a, "the overview targets A");
		swipe(4, 80, 0, 8); // 4 fingers right: a hop, in the overview too
		return 500;
	case 98:
		check(o->overview.active() && o->overview.target().desktop == hop_b,
			"a 4-finger swipe in the overview hopped the target onto B");
		swipe(3, 0, 300, 20); // 3 fingers down: into the target
		return 800;
	case 99: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		Camera b = camera_for_box(desk(hop_b)->frame(), desk(hop_b)->scale());
		check(!o->overview.active() && o->desktop_showing() == desk(hop_b)
				&& std::fabs(o->camera.current().cx - b.cx) < 1
				&& std::fabs(o->camera.current().zoom - b.zoom) < 1e-9,
			"a 3-finger swipe down went into the target, B");
		o->overview.enter(); // for the abandon and the pinch previews
		return 700;
	}
	case 100: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active() && o->overview.target().desktop == hop_b,
			"the overview opened over B again");
		// A 3-finger sideways swipe abandons: a fresh desktop, the pointer
		// over it so the swipe has a desktop under the pointer.
		const wlr_box &frame = desk(hop_b)->frame();
		o->overview.new_desktop(frame.x + frame.width * 1.8,
			frame.y + frame.height / 2.0);
		std::optional<DesktopId> doomed = o->overview.target().desktop;
		check(doomed.has_value() && doomed != hop_b, "a fresh desktop to abandon");
		Camera cam = o->camera.drawn();
		double z = cam.zoom;
		double left = cam.cx - o->width / (2.0 * z);
		double top = cam.cy - o->height / (2.0 * z);
		const wlr_box &dframe = desk(doomed)->frame();
		move_to((dframe.x + dframe.width / 2.0 - left) * z,
			(dframe.y + dframe.height / 2.0 - top) * z);
		swipe(3, 320, 0, 20);
		return 500;
	}
	case 101: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		std::optional<DesktopId> doomed = o->overview.target().desktop;
		check(doomed.has_value()
				&& server->canvas.desktops.find(doomed) == nullptr,
			"a 3-finger sideways swipe abandoned the desktop in the overview");
		shot("37-overview-help-four-finger");
		o->overview.set_target(desk(hop_a)->frame(), hop_a, false);
		Camera cam = o->camera.drawn();
		double z = cam.zoom;
		double left = cam.cx - o->width / (2.0 * z);
		double top = cam.cy - o->height / (2.0 * z);
		const wlr_box &aframe = desk(hop_a)->frame();
		move_to((aframe.x + aframe.width / 2.0 - left) * z,
			(aframe.y + aframe.height / 2.0 - top) * z);
		server->gestures.pinch_begin(t_ms, 4);
		for (int i = 1; i <= 10; i++) {
			t_ms += 8;
			server->gestures.pinch_update(t_ms, 0, 0, 1 + 0.5 * i / 10, 0);
		}
		check(server->gestures.pinch() != nullptr
				&& server->hint.text == std::format("Release to tile Desktop {}",
					desk(hop_a)->number()),
			std::format("a 4-finger spread previews tiling (hint {})",
				server->hint.text));
		t_ms += 8;
		server->gestures.pinch_end(t_ms, true); // cancelled: the preview is the point
		server->gestures.pinch_begin(t_ms, 4);
		for (int i = 1; i <= 10; i++) {
			t_ms += 8;
			server->gestures.pinch_update(t_ms, 0, 0, 1 - 0.4 * i / 10, 0);
		}
		check(server->gestures.pinch() != nullptr
				&& server->hint.text == std::format(
					"Release to combine Desktop {} into Desktop {}",
					desk(hop_b)->number(), desk(hop_a)->number()),
			std::format("a 4-finger gather previews combining (hint {})",
				server->hint.text));
		t_ms += 8;
		server->gestures.pinch_end(t_ms, true);
		return 400;
	}
	case 102:
		// Out of the overview, onto B, for the `off` preset.
		o->overview.set_target(desk(hop_b)->frame(), hop_b, false);
		o->overview.exit();
		return 700;
	case 103: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		if (!acted) {
			acted = true;
			Camera b = camera_for_box(desk(hop_b)->frame(), desk(hop_b)->scale());
			check(!o->overview.active() && o->desktop_showing() == desk(hop_b)
					&& std::fabs(o->camera.current().cx - b.cx) < 1,
				"back on B in the normal view");
			// `off`: no 3- or 4-finger compositor gesture; the client under the
			// pointer gets them, like 2-finger ones.
			server->gestures.set_options(
				{.swipes = settings::SwipePreset::off, .pinch_scale = true});
			check(!help_names("finger"),
				"with the preset off the help line names only keys");
			Camera before = o->camera.current();
			park_on_reporter();
			swipe_down(3);
			check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
				"a 3-finger swipe in the normal view is forwarded to the client");
			swipe_move(3, 60, 0);
			swipe_up(false);
			swipe_down(4);
			check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
				"a 4-finger swipe in the normal view is forwarded to the client");
			swipe_move(4, 0, -60);
			swipe_up(false);
			leave_reporter();
			Camera now = o->camera.current();
			check(!o->overview.active() && now.cx == before.cx && now.cy == before.cy
					&& now.zoom == before.zoom && !o->camera.panning()
					&& o->desktop_showing() == desk(hop_b),
				"with the preset off, 3- and 4-finger swipes left the camera and"
					" the overview state alone");
		}
		const std::string want = "SWIPE BEGIN 3|SWIPE UPDATE 60.0 0.0|SWIPE END 0"
			"|SWIPE BEGIN 4|SWIPE UPDATE 0.0 -60.0|SWIPE END 0";
		if (awaiting(want)) {
			return -50;
		}
		check_client_got(want, "with the preset off, the client under the pointer"
			" received the 3- and the 4-finger swipe");
		o->overview.enter();
		return 700;
	}
	case 104: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		// In the overview the compositor owns every gesture: with no preset,
		// 3- and 4-finger gestures do nothing at all.
		check(o->overview.active(), "the overview opened with the preset off");
		std::optional<DesktopId> target_before = o->overview.target().desktop;
		Camera before = o->camera.current();
		server->hint.set("");
		server->gestures.swipe_begin(t_ms += 8, 3);
		check(std::holds_alternative<Gestures::Ignored>(server->gestures.state()),
			"a 3-finger swipe in the overview does nothing");
		server->gestures.swipe_update(t_ms += 8, 60, 60);
		server->gestures.swipe_end(t_ms += 8, false);
		server->gestures.swipe_begin(t_ms += 8, 4);
		check(std::holds_alternative<Gestures::Ignored>(server->gestures.state()),
			"a 4-finger swipe in the overview does nothing");
		server->gestures.swipe_update(t_ms += 8, 60, 60);
		server->gestures.swipe_end(t_ms += 8, false);
		server->gestures.pinch_begin(t_ms += 8, 3);
		check(std::holds_alternative<Gestures::Ignored>(server->gestures.state()),
			"a 3-finger pinch in the overview does nothing");
		server->gestures.pinch_update(t_ms += 8, 0, 0, 1.5, 0);
		server->gestures.pinch_end(t_ms += 8, false);
		Camera now = o->camera.current();
		check(o->overview.active() && now.cx == before.cx && now.cy == before.cy
				&& now.zoom == before.zoom && server->hint.text.empty()
				&& o->overview.target().desktop == target_before
				&& std::holds_alternative<Gestures::Idle>(server->gestures.state()),
			"in the overview, 3- and 4-finger gestures did nothing at all");
		shot("36-overview-help-off");
		return 400;
	}
	case 105:
		o->overview.exit();
		return 700;
	case 106: {
		// pinch-scale off: a Super + 2-finger pinch is a plain pinch.
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		if (!acted) {
			acted = true;
			check(!o->overview.active() && o->desktop_showing() == desk(hop_b)
					&& o->interactive(), "back on B for the pinch-scale switch");
			server->gestures.set_options(
				{.swipes = settings::SwipePreset::three_finger_hops, .pinch_scale = false});
			Camera before = o->camera.current();
			double scale_before = desk(hop_b)->scale();
			park_on_reporter();
			key(KEY_LEFTMETA, true);
			pinch_down(2);
			check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
				"with pinch-scale off, a Super + 2-finger pinch is forwarded to the"
					" client");
			pinch_move(1.5);
			pinch_up(false);
			key(KEY_LEFTMETA, false);
			leave_reporter();
			Camera now = o->camera.current();
			check(now.cx == before.cx && now.cy == before.cy && now.zoom == before.zoom
					&& desk(hop_b)->scale() == scale_before
					&& !server->launcher.is_open(),
				"with pinch-scale off, the Super pinch scaled no desktop and opened"
					" no launcher");
		}
		const std::string want = "PINCH BEGIN 2|PINCH UPDATE 0.0 0.0 1.50 0.0|PINCH END 0";
		if (awaiting(want)) {
			return -50;
		}
		check_client_got(want, "with pinch-scale off, the client under the pointer"
			" received the Super + 2-finger pinch");
		return 400;
	}
	case 107:
		o->overview.enter();
		return 700;
	case 108: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		// In the overview the same pinch zooms the overview camera.
		check(o->overview.active(), "the overview opened for the pinch-scale switch");
		double zoom_before = o->camera.current().zoom;
		double scale_before = desk(hop_b)->scale();
		key(KEY_LEFTMETA, true);
		pinch_down(2);
		check(std::holds_alternative<Gestures::OverviewZoom>(server->gestures.state()),
			"in the overview, the Super pinch is the overview's zoom");
		pinch_move(1.6);
		check(std::fabs(o->camera.current().zoom - zoom_before) > 1e-9
				&& std::holds_alternative<Gestures::OverviewZoom>(
					server->gestures.state()),
			std::format("the pinch zoomed the overview camera ({:.3f} -> {:.3f})",
				zoom_before, o->camera.current().zoom));
		pinch_up(false);
		key(KEY_LEFTMETA, false);
		check(desk(hop_b)->scale() == scale_before && !server->launcher.is_open(),
			"the overview's Super pinch scaled no desktop and opened no launcher");
		shot("34-overview-super-pinch-plain");
		return 400;
	}
	case 109:
		// The defaults restored: 3 and 4 back in their own roles.
		o->overview.exit();
		server->gestures.set_options(GestureOptions{});
		check(server->gestures.options().swipes == settings::SwipePreset::three_finger_hops
				&& server->gestures.options().pinch_scale,
			"the default options restored");
		check(help_names("3 fingers") && help_names("4 down")
				&& help_names("3-finger pinch"),
			"the help line names the default preset's gestures");
		return 700;
	case 110:
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		check(!o->overview.active() && o->desktop_showing() == desk(hop_b),
			"back on B with the defaults");
		swipe(3, 200, 0, 8); // 3 fingers right: a hop again
		return 900;
	case 111: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Camera c = camera_for_box(desk(hop_c)->frame(), 1);
		Camera cam = o->camera.current();
		check(cam.cx == c.cx && cam.cy == c.cy && std::fabs(cam.zoom - 1) < 1e-9
				&& o->desktop_showing() == desk(hop_c),
			std::format("with the defaults back, a 3-finger swipe hopped onto C"
				" (off by {:.0f},{:.0f})", cam.cx - c.cx, cam.cy - c.cy));
		shot("35-default-restored");
		// hop-swipe away: the same swipe hops the other way
		// (docs/intents/input/gestures.md).
		server->gestures.set_options({.swipes = settings::SwipePreset::three_finger_hops,
			.pinch_scale = true, .hops_away = true});
		swipe(3, 200, 0, 8); // 3 fingers right, hopping away: back onto B
		return 900;
	}
	case 112: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Camera b = camera_for_box(desk(hop_b)->frame(), 0.8);
		Camera cam = o->camera.current();
		check(cam.cx == b.cx && cam.cy == b.cy && o->desktop_showing() == desk(hop_b),
			std::format("with hop-swipe away, a 3-finger swipe right hopped left, onto B"
				" (off by {:.0f},{:.0f})", cam.cx - b.cx, cam.cy - b.cy));
		server->gestures.set_options(GestureOptions{});
		check(!server->gestures.options().hops_away, "hops go toward the fingers again");
		return 0;
	}
	// Looking away (docs/intents/canvas/infinite-canvas.md), from B at 80 %.
	case 113: { // a drag over empty canvas pans only with Super
		// B is tiled full: Super+Alt+Up first shows the empty canvas above
		// the row, one look away the drag then carries on.
		look_from = o->camera.destination();
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTALT, true);
		key(KEY_UP, true);
		key(KEY_UP, false);
		key(KEY_LEFTALT, false);
		key(KEY_LEFTMETA, false);
		std::optional<Point> spot = empty_spot();
		check(spot.has_value(), "looking up from B shows the empty canvas above the row");
		Point p = spot.value_or(Point{640, 120});
		Camera up = o->camera.destination();
		move_to(p.x, p.y);
		click(BTN_LEFT, true);
		for (int i = 1; i <= 10; i++) {
			move_to(p.x + 20 * i, p.y + 10 * i);
		}
		click(BTN_LEFT, false);
		check(o->camera.destination() == up,
			"a drag over empty canvas without Super moves nothing");
		key(KEY_LEFTMETA, true);
		move_to(p.x, p.y);
		click(BTN_LEFT, true);
		for (int i = 1; i <= 10; i++) {
			move_to(p.x + 20 * i, p.y + 10 * i);
		}
		click(BTN_LEFT, false);
		key(KEY_LEFTMETA, false);
		Camera cam = o->camera.current();
		double moved_x = (up.cx - cam.cx) * cam.zoom;
		double moved_y = (up.cy - cam.cy) * cam.zoom;
		check(std::fabs(moved_x - 200) < 1.5 && std::fabs(moved_y - 100) < 1.5
				&& cam.zoom == look_from.zoom && o->interactive(),
			std::format("a Super drag over empty canvas looks away, the content following"
				" the pointer at the desktop's zoom (moved {:.1f},{:.1f} px)", moved_x, moved_y));
		shot("36-looked-away");
		tap_keys({KEY_LEFTMETA, KEY_LEFTMETA});
		return 600;
	}
	case 114: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(o->camera.current() == look_from && o->desktop_showing() == desk(hop_b),
			"double Super eased back to desktop B at its 80 %");
		// Super+Alt+Right looks away by a third of a screen; double Super
		// comes back from it too.
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTALT, true);
		key(KEY_RIGHT, true);
		key(KEY_RIGHT, false);
		key(KEY_LEFTALT, false);
		key(KEY_LEFTMETA, false);
		check(o->camera.destination() != look_from, "Super+Alt+Right looked away");
		tap_keys({KEY_LEFTMETA, KEY_LEFTMETA});
		return 600;
	}
	case 115: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(o->camera.current() == look_from,
			"double Super came back from Super+Alt+Right");
		// A hop after a look away: where it lands is the view kept.
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTALT, true);
		key(KEY_RIGHT, true);
		key(KEY_RIGHT, false);
		key(KEY_LEFTALT, false);
		key(KEY_LEFTCTRL, true);
		key(KEY_RIGHT, true);
		key(KEY_RIGHT, false);
		key(KEY_LEFTCTRL, false);
		key(KEY_LEFTMETA, false);
		return 900;
	}
	case 116: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(o->desktop_showing() == desk(hop_c), "the hop after the look away landed on C");
		hop_from = o->camera.destination();
		tap_keys({KEY_LEFTMETA, KEY_LEFTMETA});
		return 600;
	}
	case 117: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(o->camera.destination() == hop_from && o->desktop_showing() == desk(hop_c),
			"after the hop, double Super stays on C: the hop made it the view kept");
		// The overview is another move: what it brings back is kept, even
		// the very view the look away had reached.
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTALT, true);
		key(KEY_RIGHT, true);
		key(KEY_RIGHT, false);
		key(KEY_LEFTALT, false);
		key(KEY_LEFTMETA, false);
		hop_from = o->camera.destination();
		o->overview.enter();
		return 700;
	}
	case 118:
		check(o->overview.active(), "the overview opened over the look away");
		tap_keys({KEY_ESC});
		return 800;
	case 119: {
		if ((o->camera.panning() || o->camera.zooming()) && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(!o->overview.active() && o->camera.destination() == hop_from,
			"Escape brought the view back from the overview where the look away had it");
		tap_keys({KEY_LEFTMETA, KEY_LEFTMETA});
		return 600;
	}
	case 120: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(o->camera.destination() == hop_from,
			"after the overview, double Super stays: the overview made that the view kept");
		return 0;
	}
	case 121: {
		// Super+N makes a missing desktop beside desktop 1: from desktop 1
		// alone, desktops 2-5 land above, right, below and left of it.
		std::vector<DesktopId> others;
		for (const Desktop &d : server->canvas.desktops.all()) {
			if (d.number() != 1) {
				others.push_back(d.id());
			}
		}
		for (DesktopId id : others) {
			o->overview.abandon(id);
		}
		// Super+1 goes to desktop 1, making it at its own place when even
		// that is gone.
		key(KEY_LEFTMETA, true);
		key(KEY_1, true);
		key(KEY_1, false);
		key(KEY_LEFTMETA, false);
		return 700;
	}
	case 122: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(server->canvas.desktops.size() == 1
				&& o->desktop_showing() == server->canvas.desktops.by_number(1),
			std::format("Super+N starts from desktop 1 alone ({} desktops)",
				server->canvas.desktops.size()));
		key(KEY_LEFTMETA, true);
		key(KEY_2, true);
		key(KEY_2, false);
		key(KEY_LEFTMETA, false);
		return 700;
	}
	case 123: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
		check_made_desktop(2, beside_frame(anchor, 0, -1));
		key(KEY_LEFTMETA, true);
		key(KEY_3, true);
		key(KEY_3, false);
		key(KEY_LEFTMETA, false);
		return 700;
	}
	case 124: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
		check_made_desktop(3, beside_frame(anchor, 1, 0));
		key(KEY_LEFTMETA, true);
		key(KEY_4, true);
		key(KEY_4, false);
		key(KEY_LEFTMETA, false);
		return 700;
	}
	case 125: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
		check_made_desktop(4, beside_frame(anchor, 0, 1));
		key(KEY_LEFTMETA, true);
		key(KEY_5, true);
		key(KEY_5, false);
		key(KEY_LEFTMETA, false);
		return 700;
	}
	case 126: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
		check_made_desktop(5, beside_frame(anchor, -1, 0));
		shot("37-super-n-made-beside");
		return 500;
	}
	case 127: {
		// The up slot already covered: Super+2 passes over it to the next
		// free slot, right of desktop 1.
		wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
		wlr_box up = beside_frame(anchor, 0, -1);
		Desktop *three_desk = server->canvas.desktops.by_number(3);
		Desktop *two_desk = server->canvas.desktops.by_number(2);
		check(two_desk != nullptr && three_desk != nullptr,
			"desktops 2 and 3 exist before the covered-slot round");
		if (two_desk == nullptr || three_desk == nullptr) {
			return 0;
		}
		o->overview.abandon(two_desk->id());
		server->canvas.desktops.move(three_desk->id(), up.x, up.y);
		check(server->canvas.desktops.by_number(2) == nullptr
				&& three_desk->frame().x == up.x && three_desk->frame().y == up.y,
			"desktop 2 abandoned, desktop 3 moved onto the up slot");
		key(KEY_LEFTMETA, true);
		key(KEY_2, true);
		key(KEY_2, false);
		key(KEY_LEFTMETA, false);
		return 700;
	}
	case 128: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
		check_made_desktop(2, beside_frame(anchor, 1, 0));
		shot("38-super-n-passed-slot");
		return 0;
	}
	case 129: // a tab's label follows its desktop on an ordinary tab and the target's alike
		o->overview.enter();
		return 700;
	case 130: {
		// Desktop 3's ordinary tab was drawn as the overview opened; now it
		// is the target, and is scaled while it is.
		Desktop *d3 = server->canvas.desktops.by_number(3);
		check(o->overview.active() && d3 != nullptr,
			"the overview opened with desktop 3 on the canvas");
		if (d3 == nullptr) {
			return 0;
		}
		o->overview.set_target(d3->frame(), d3->id(), false);
		return 400;
	}
	case 131:
		server->canvas.settle_scale(server->canvas.desktops.by_number(3)->id(), 0.7);
		return 400;
	case 132: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		o->overview.set_target(d2->frame(), d2->id(), false);
		return 400;
	}
	case 133: {
		Desktop *d3 = server->canvas.desktops.by_number(3);
		float scale = o->wlr->scale;
		CachedText label = desktop_label(*server, *d3, scale, false);
		uint32_t drawn = label.texture != nullptr ? label.texture->width : 0;
		check(label.texture != nullptr
				&& drawn == static_cast<uint32_t>(std::ceil(label.w * scale)),
			std::format("scaled while it was the target, desktop 3's ordinary tab draws "
				"its label from its own text ({} px for {} logical)", drawn, label.w));
		shot("39-tab-labels");
		return 300;
	}
	case 134:
		server->canvas.settle_scale(server->canvas.desktops.by_number(3)->id(), 1.0);
		o->overview.exit();
		return 700;
	// Each direction switch turns its own gesture and no other
	// (docs/intents/input/gestures.md).
	case 135: { // hops away: four fingers up still open the overview
		if ((o->camera.panning() || !o->interactive()) && retries++ < 100) {
			return -100;
		}
		server->gestures.set_options({.hops_away = true});
		swipe(4, 0, -300, 10);
		return 900;
	}
	case 136: {
		if (!o->overview.active() && retries++ < 30) {
			return -100;
		}
		check(o->overview.active(), "with hop-swipe away, four fingers up still open the overview");
		// overview-swipe down: in the overview, four fingers up go back in.
		server->gestures.set_options({.overview_down = true});
		swipe(4, 0, -300, 10);
		return 900;
	}
	case 137: {
		if ((o->overview.active() || !o->interactive()) && retries++ < 30) {
			return -100;
		}
		check(!o->overview.active(), "with overview-swipe down, four fingers up went back in");
		swipe(4, 0, 300, 10);
		return 900;
	}
	case 138: {
		if (!o->overview.active() && retries++ < 30) {
			return -100;
		}
		check(o->overview.active(), "and four fingers down opened the overview");
		server->gestures.set_options(GestureOptions{});
		o->overview.exit();
		return 800;
	}
	case 139: { // pan: Super + a scroll looks away the way the pan switch says
		if ((o->camera.panning() || !o->interactive()) && retries++ < 100) {
			return -100;
		}
		scrolled_from = o->camera.current();
		key(KEY_LEFTMETA, true);
		for (int i = 0; i < 4; i++) {
			scroll(40);
		}
		key(KEY_LEFTMETA, false);
		return 300;
	}
	case 140: {
		double moved = o->camera.current().cy - scrolled_from.cy;
		check(moved > 100, std::format("with pan natural, Super and fingers scrolling up "
			"move the content up, the view down ({:+.0f})", moved));
		o->camera.jump(scrolled_from);
		server->conf.pan_traditional = true;
		key(KEY_LEFTMETA, true);
		for (int i = 0; i < 4; i++) {
			scroll(40);
		}
		key(KEY_LEFTMETA, false);
		return 300;
	}
	case 141: {
		double moved = o->camera.current().cy - scrolled_from.cy;
		check(moved < -100, std::format("with pan traditional, the same scroll moves the "
			"view up instead ({:+.0f})", moved));
		server->conf.pan_traditional = false;
		return 300;
	}
	case 142: {
		// A tiled desktop with two windows on its board, on a row of its own.
		int w = o->width, h = o->height;
		int x0 = 200000, y0 = 200000;
		check(one != nullptr && two != nullptr && one->mapped && two->mapped
				&& !one->minimized && !two->minimized
				&& one->mode == WindowMode::normal && two->mode == WindowMode::normal,
			"two plain windows are left to carry onto a board");
		if (one == nullptr || two == nullptr) {
			return 0;
		}
		DesktopId boarded = server->canvas.desktops.create(wlr_box{x0, y0, w, h}).id();
		one->move(x0 + 100, y0 + 100);
		two->move(x0 + w / 2, y0 + 100);
		server->canvas.toggle_tiling(boarded);
		check(desk(boarded)->tiling() && one->seated_on(boarded) && two->seated_on(boarded)
				&& one->tiled && two->tiled,
			"tiling the desktop put both windows on its board");
		o->overview.abandon(boarded);
		check(server->canvas.desktops.find(boarded) == nullptr,
			"the tiled desktop is abandoned");
		check(!one->board_seat.has_value() && !two->board_seat.has_value()
				&& !one->tiled && !two->tiled,
			"abandoning a tiled desktop released its windows' board seats");
		return 300;
	}
	case 143: {
		// Frames allocate nothing (cpp-standards.md): a window drag, one
		// with a client subscribed, and a pinch, each over a hundred frames.
		if (canvas_allocations == nullptr) {
			std::println("SKIP: allocations per frame (no counter in a sanitizer build)");
			step = 153;
			return 1;
		}
		{
			// The counter sees what C++ and glib allocate, or its zeros mean nothing.
			unsigned long long probe = canvas_allocations();
			char *plain = new char[64];
			char *glib_text = g_strdup("counted");
			asm volatile("" : : "r"(plain), "r"(glib_text) : "memory");
			delete[] plain;
			g_free(glib_text);
			check(canvas_allocations() - probe == 2,
				"the allocation counter counts an operator new and a g_strdup");
		}
		frame_counter.notify = [](wl_listener *, void *) { frames_seen++; };
		wl_signal_add(&o->wlr->events.frame, &frame_counter);
		if (o->overview.active()) {
			o->overview.exit();
		}
		server->hint.set({});
		check(one != nullptr && one->mapped && !one->minimized
				&& two != nullptr && two->mapped && !two->minimized,
			"two windows are left to drag and to shelve");
		return 700;
	}
	case 144: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		Rect f = one->frame_rect();
		o->camera.jump(Camera{.cx = f.x + f.w / 2, .cy = f.y + f.h / 2, .zoom = 1});
		return 300;
	}
	case 145:
		grab_title(*one);
		return 100;
	case 146: {
		unsigned long long made = 0;
		unsigned long frames = 0;
		if (!run_over(made, frames)) {
			drag_tick(*one);
			return -17;
		}
		click(BTN_LEFT, false);
		check(drag_travel >= 60, std::format("the window followed the drag ({:.0f} px)",
			drag_travel));
		check(frames >= counted_frames && made == 0,
			std::format("a window drag allocated {} times over {} frames", made, frames));
		return 300;
	}
	case 147: {
		// The same drag with a client subscribed: the window list, which a
		// drag does not change, is compared and never serialised.
		std::string path = shot_dir + "/subscriber.out";
		subscriber_pid = fork();
		if (subscriber_pid == 0) {
			sigset_t empty; // the compositor blocks SIGTERM for its signal source
			sigemptyset(&empty);
			sigprocmask(SIG_SETMASK, &empty, nullptr);
			int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) {
				_exit(126);
			}
			execlp(std::getenv("WAYLAND_CANVAS_BIN"), "wayland-canvas", "msg",
				"subscribe", static_cast<char *>(nullptr));
			_exit(127);
		}
		return 600;
	}
	case 148:
		server->focus_view(three); // the subscriber is told; activate() would pan the camera
		grab_title(*one);
		return 100;
	case 149: {
		unsigned long long made = 0;
		unsigned long frames = 0;
		if (!run_over(made, frames)) {
			drag_tick(*one);
			return -17;
		}
		click(BTN_LEFT, false);
		kill(subscriber_pid, SIGTERM);
		waitpid(subscriber_pid, nullptr, 0);
		check(file_text(shot_dir + "/subscriber.out").find("\"event\":\"windows\"")
				!= std::string::npos,
			"the subscriber was told the window list changed with the focus");
		check(drag_travel >= 60, std::format("the window followed the drag ({:.0f} px)",
			drag_travel));
		check(frames >= counted_frames && made == 0,
			std::format("a window drag with a subscriber allocated {} times over {} frames",
				made, frames));
		return 300;
	}
	case 150:
		// A pinch over a desktop of the overview, with a minimized window on
		// the shelf: tabs, shelf title and the release preview are drawn.
		server->hint.set({});
		two->set_minimized(true);
		o->overview.enter();
		return 800;
	case 151: {
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		const Desktop &pinched = server->canvas.desktops.all().front();
		o->overview.set_target(pinched.frame(), pinched.id(), false);
		Camera cam = o->camera.drawn();
		double left = cam.cx - o->width / (2.0 * cam.zoom);
		double top = cam.cy - o->height / (2.0 * cam.zoom);
		move_to((pinched.frame().x + pinched.frame().width / 2.0 - left) * cam.zoom,
			(pinched.frame().y + pinched.frame().height / 2.0 - top) * cam.zoom);
		pinch_down(3);
		tick = 0;
		return 100;
	}
	case 152: {
		unsigned long long made = 0;
		unsigned long frames = 0;
		if (!run_over(made, frames)) {
			pinch_move(1.35 + 0.0004 * (tick % 100));
			tick++;
			return -17;
		}
		bool previewing = server->gestures.pinch() != nullptr && !server->hint.text.empty();
		pinch_up(true);
		check(previewing, "the pinch previewed what release would do");
		check(frames >= counted_frames && made == 0,
			std::format("a pinch allocated {} times over {} frames", made, frames));
		return 300;
	}
	case 153:
		wl_list_remove(&frame_counter.link);
		o->overview.exit();
		two->set_minimized(false);
		return 700;
	case 154: {
		// New windows wider than a third of the screen cascade to its right
		// edge in a few steps; nine of them go past it.
		if (o->camera.panning() && retries++ < 100) {
			return -100;
		}
		cascade_before = mapped_views();
		for (int i = 0; i < cascade_windows; i++) {
			spawn("foot -T cascade -w 900x200 sh -c 'exec sleep 600'");
		}
		return 500;
	}
	case 155: {
		if (mapped_views() < cascade_before + cascade_windows && retries++ < 100) {
			return -100;
		}
		Rect vp = o->viewport();
		wlr_box area = {static_cast<int>(std::round(vp.x)), static_cast<int>(std::round(vp.y)),
			static_cast<int>(std::round(vp.w)), static_cast<int>(std::round(vp.h))};
		wlr_box usable = o->usable_canvas_box(area, o->camera.drawn().zoom);
		int opened = 0, outside = 0, apart = 0;
		double furthest = usable.x;
		std::vector<Point> spots;
		for (View *v : server->views) {
			if (!v->mapped || v->shell_title() == nullptr
					|| std::strcmp(v->shell_title(), "cascade") != 0) {
				continue;
			}
			Rect f = v->frame_rect();
			opened++;
			outside += !inside(f, usable);
			furthest = std::fmax(furthest, f.x + f.w);
			bool taken = std::ranges::any_of(spots, [&](const Point &p) {
				return p.x == f.x && p.y == f.y;
			});
			apart += !taken;
			spots.push_back({f.x, f.y});
		}
		check(opened == cascade_windows,
			std::format("{} wide windows opened ({})", cascade_windows, opened));
		check(outside == 0, std::format("{} of {} cascaded windows are outside the usable area "
			"(furthest right edge {:.0f}, the area ends at {})", outside, opened, furthest,
			usable.x + usable.width));
		check(furthest > usable.x + usable.width - config::cascade_step,
			std::format("the cascade went to the right edge (furthest right edge {:.0f} of {})",
				furthest, usable.x + usable.width));
		check(apart < opened,
			"once the edge is reached, the next window opens at the first spot again");
		shot("38-cascade");
		return 300;
	}
	// A zoom swipe released short of the commit travel completes when the
	// fingers were fast and springs back when they were slow; one that was
	// cancelled springs back whatever it travelled.
	case 156: {
		if ((camera_easing() || !o->interactive()) && retries++ < 100) {
			return -100;
		}
		static_assert(60 < config::overview_swipe_distance * config::overview_commit);
		swipe_from = o->camera.current();
		swipe(4, 0, -50, 50); // slow: 50 units over 400 ms
		return 900;
	}
	case 157: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(!o->overview.active() && same_camera(o->camera.current(), swipe_from),
			"a short, slow 4-finger swipe up sprang back to the normal view");
		swipe(4, 0, -60, 5); // fast: 60 units over 40 ms
		return 900;
	}
	case 158: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active() && o->camera.current().zoom < 0.5,
			"a short, fast 4-finger swipe up opened the overview");
		swipe_from = o->camera.current();
		swipe(4, 0, 50, 50);
		return 900;
	}
	case 159: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active() && same_camera(o->camera.current(), swipe_from),
			"a short, slow 4-finger swipe down sprang back to the overview");
		swipe(4, 0, 50, 4);
		return 900;
	}
	case 160: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(!o->overview.active() && o->camera.current().zoom == 1,
			"a short, fast 4-finger swipe down left the overview");
		swipe_from = o->camera.current();
		swipe(4, 0, -150, 10, true);
		return 900;
	}
	case 161: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(!o->overview.active() && same_camera(o->camera.current(), swipe_from),
			"a cancelled 4-finger swipe up sprang back past the commit travel");
		o->overview.enter();
		return 900;
	}
	case 162: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		swipe_from = o->camera.current();
		swipe(4, 0, 150, 10, true);
		return 900;
	}
	case 163: {
		if (camera_easing() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active() && same_camera(o->camera.current(), swipe_from),
			"a cancelled 4-finger swipe down sprang back past the commit travel");
		o->overview.exit();
		return 900;
	}
	case 164: { // a second output at scale 2 joins the first, at scale 1
		wlr_backend *headless = nullptr;
		wlr_multi_for_each_backend(server->backend,
			[](wlr_backend *backend, void *data) {
				if (wlr_backend_is_headless(backend)) {
					*static_cast<wlr_backend **>(data) = backend;
				}
			}, &headless);
		check(headless != nullptr && wlr_headless_add_output(headless, 640, 480) != nullptr,
			"a second output was added for the text cache");
		return 600;
	}
	case 165: {
		if (server->outputs.size() < 2 && retries++ < 50) {
			return -100;
		}
		Output *second = server->outputs.back().get();
		wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_scale(&state, 2);
		wlr_output_commit_state(second->wlr, &state);
		wlr_output_state_finish(&state);
		second->camera.jump(o->camera.current());
		check(server->outputs.size() == 2 && second->wlr->scale == 2 && o->wlr->scale == 1,
			"two outputs, at scales 1 and 2");
		return 400;
	}
	case 166: { // both in the overview, with a minimized window on the shelf
		two->set_minimized(true);
		o->overview.enter();
		server->outputs.back()->overview.enter();
		return 900;
	}
	case 167: {
		// Frames are rendered here, one after the other, so that none is
		// skipped and the hint's 2 s cannot run out between them.
		auto frames = [](int n) {
			for (int i = 0; i < n; i++) {
				for (const std::unique_ptr<Output> &output : server->outputs) {
					server->painter.render(*output);
				}
			}
		};
		const config::Palette &pal = server->theme.colors();
		server->hint.set("Text cache check");
		unsigned long before = server->text_cache.rebuilt();
		frames(2);
		unsigned long warm = server->text_cache.rebuilt();
		check(warm > before, std::format("the first frames built their textures ({} new)",
			warm - before));
		check(server->text_cache.find_size(panel_text_spec("Text cache check", 1, pal))
				&& server->text_cache.find_size(panel_text_spec("Text cache check", 2, pal)),
			"the hint is cached at scale 1 and at scale 2");
		frames(100);
		check(server->text_cache.rebuilt() == warm,
			std::format("no texture was rebuilt across 100 unchanged frames on both outputs "
				"({} rebuilt)", server->text_cache.rebuilt() - warm));
		server->hint.set("Another line");
		frames(1);
		check(server->text_cache.rebuilt() - warm == 2,
			std::format("a changed hint builds one texture per scale, and the counter sees "
				"it ({} built)", server->text_cache.rebuilt() - warm));
		two->set_minimized(false);
		o->overview.exit();
		server->outputs.back()->overview.exit();
		return 700;
	}
	default:
		finished = true;
		std::println("{}: {} failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
		wl_display_terminate(server->display);
		return -1;
	}
}

int handle_timer() {
	if (grim_pid > 0) {
		if (waitpid(grim_pid, nullptr, WNOHANG) == 0) {
			wl_event_source_timer_update(timer, 30);
			return 0;
		}
		grim_pid = -1;
	}
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
	report_retries = 0;
	acted = false;
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

void write_entry(const std::string &path, const char *name, const char *exec) {
	FILE *f = std::fopen(path.c_str(), "w");
	if (f == nullptr) {
		std::fprintf(stderr, "fopen %s: %s\n", path.c_str(), std::strerror(errno));
		std::exit(2);
	}
	std::fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n", name, exec);
	std::fclose(f);
}

// The launcher entries of the late launches, in $XDG_DATA_HOME/applications:
// each window appears a second after its launch, once the overview it was
// launched from has closed.
void install_late_entries(const std::string &xdg_dir) {
	std::string apps = xdg_dir + "/applications";
	mkdir_p(apps);
	// foot as the shell's child (`; true` keeps sh from exec'ing it), so the
	// trace walks up from the client to the launch.
	write_entry(apps + "/tardy-terminal.desktop", "Tardy Terminal",
		"sh -c \"sleep 1; foot -T tardy; true\"");
	write_entry(apps + "/belated-maximized.desktop", "Belated Maximized",
		"sh -c \"sleep 1; exec foot --maximized -T belated\"");
}

} // namespace

int main() {
	wlr_log_init(WLR_ERROR, nullptr);
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp"; // meson passes the repo's tmp/ via env
	shot_dir = std::format("{}/scenarios/gestures", root);
	tmp_root = root;
	mkdir_p(shot_dir);
	std::string xdg_dir = std::format("{}/scenarios/gestures/xdg", root);
	install_late_entries(xdg_dir);
	// Before the server starts: the launcher scans $XDG_DATA_HOME/applications first.
	setenv("XDG_DATA_HOME", xdg_dir.c_str(), 1);
	// Before the server starts: canvas.conf lives in this run's scratch,
	// never in the user's real ~/.config.
	setenv("XDG_CONFIG_HOME",
		std::format("{}/scenarios/gestures/xdg-config", root).c_str(), 1);
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
	// A compositor that stops by itself (its own shutdown, a lost backend)
	// ends the run early: without this the scenario would still exit 0.
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	wl_event_source_remove(timer);
	stop_reporter();
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	return failures != 0;
}
