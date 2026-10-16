// End-to-end gesture scenario: the real compositor in-process on the headless
// backend, real foot clients, synthetic touchpad gestures through the Gesture
// entry points, screenshots. Writes only under $CANVAS_TMP (meson passes the
// repo's tmp/).
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core/config.h"
#include "core/geometry.h"
#include "platform/output.h"
#include "render/render.h"
#include "core/server.h"
#include "core/wlr.h"

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
Camera hop_from{}; // where the camera was going when a hop began
std::optional<DesktopId> hop_a, hop_b, hop_c; // the row the normal-view hops walk
std::vector<View *> launched_before; // the views before the overview launch
std::string tmp_root;
wlr_pointer ptr{};
wlr_keyboard kbd{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
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
		if (v->mapped && v->wlr->title && std::strcmp(v->wlr->title, title) == 0) {
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

void swipe(uint32_t fingers, double dx, double dy, int steps) {
	server->gestures.swipe_begin(t_ms, fingers);
	for (int i = 0; i < steps; i++) {
		t_ms += 8;
		server->gestures.swipe_update(t_ms, dx / steps, dy / steps);
	}
	t_ms += 8;
	server->gestures.swipe_end(t_ms, false);
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

// A 2-finger scroll tick through the device path: vertical, from fingers.
void scroll(double delta) {
	t_ms += 8;
	wlr_pointer_axis_event ev = {.pointer = &ptr, .time_msec = t_ms,
		.source = WL_POINTER_AXIS_SOURCE_FINGER,
		.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
		.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
		.delta = delta, .delta_discrete = 0};
	wl_signal_emit_mutable(&ptr.events.axis, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

bool inside(const Rect &r, const wlr_box &b) {
	return r.x >= b.x - 0.5 && r.y >= b.y - 0.5 && r.x + r.w <= b.x + b.width + 0.5 &&
		r.y + r.h <= b.y + b.height + 0.5;
}

// The board's slices, as fractions of its area.
constexpr Slice LEFT{0, 0, 0.5, 1}, RIGHT{0.5, 0, 1, 1};
constexpr Slice TL{0, 0, 0.5, 0.5}, BL{0, 0.5, 0.5, 1};
constexpr Slice RT{0.5, 0, 1, 0.5}, RB{0.5, 0.5, 1, 1};

// A slice's expected frame on a desktop's board: the slice laid into the
// desktop's frame, rounded the way the compositor rounds it (the scenarios
// run with no bar, so the frame is the whole tiling area).
wlr_box slice_frame_of(const Desktop &desktop, Slice slice) {
	const wlr_box &frame = desktop.frame();
	Rect area{double(frame.x), double(frame.y), double(frame.width),
		double(frame.height)};
	Rect r = slice_box(area, slice, config::tile_gap);
	return wlr_box{
		static_cast<int>(std::round(r.x)),
		static_cast<int>(std::round(r.y)),
		static_cast<int>(std::round(r.x + r.w)) - static_cast<int>(std::round(r.x)),
		static_cast<int>(std::round(r.y + r.h)) - static_cast<int>(std::round(r.y)),
	};
}

// The view's frame lies on its slice (clients snap to their own units, so a
// side may sit a few px short of the slice's box).
bool on_slice(View *view, const wlr_box &expected) {
	Rect f = view->frame_rect();
	Rect e{double(expected.x), double(expected.y), double(expected.width),
		double(expected.height)};
	return std::fabs(f.x - e.x) < 3 && std::fabs(f.y - e.y) < 3
		&& e.w - f.w < 12 && e.h - f.h < 12;
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
				view->wlr->title != nullptr ? view->wlr->title : "?", box.w, box.h,
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

int run_step() {
	Output *o = out();
	View *one = view_titled("one"), *two = view_titled("two"), *three = view_titled("three");
	switch (step) {
	case 0:
		spawn("foot -T one sh -c 'echo window one; exec sleep 600'");
		spawn("foot -T two sh -c 'echo window two; exec sleep 600'");
		spawn("foot -T three sh -c 'echo window three; exec sleep 600'");
		return 300;
	case 1:
		if (mapped_views() < 3 && retries++ < 100) {
			return -100; // retry
		}
		check(mapped_views() == 3, std::format("three foot windows mapped ({})", mapped_views()));
		check(server->canvas.desktops.size() == 1, "Desktop 1 exists at start");
		d1 = server->canvas.desktops.all().front().id();
		check(three && one && two && three->deco.ssd, "foot negotiated server-side decorations");
		shot("01-three-windows");
		return 400;
	case 2: {
		// Title bar hit-testing at the close/maximize/minimize buttons.
		wlr_box f = three->frame_box();
		check(three->deco.hit(f.x + f.width - 10, f.y + 10, 1).part == DecoPart::close,
			"close button hit");
		check(three->deco.hit(f.x + f.width - 50, f.y + 10, 1).part == DecoPart::maximize,
			"maximize button hit");
		check(three->deco.hit(f.x + f.width - 100, f.y + 10, 1).part == DecoPart::minimize,
			"minimize button hit");
		check(three->deco.hit(f.x + 60, f.y + 10, 1).part == DecoPart::title, "title hit");
		DecoHit h = three->deco.hit(f.x + f.width + 3, f.y + f.height + 3, 1);
		check(h.part == DecoPart::resize &&
			h.edges == static_cast<uint32_t>(WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM),
			"bottom-right resize corner");
		three->set_maximized(true);
		return 500;
	}
	case 3: {
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
	case 25:
		one->activate();
		check(!one->minimized, "restore from minimized");
		o->overview.target_point(desk(d1)->frame().x - 3000, desk(d1)->frame().y + 2000);
		o->overview.exit();
		return 700;
	case 26: {
		check(!o->overview.active() && !o->overview.target().desktop.has_value() &&
			std::fabs(o->camera.current().cx - (desk(d1)->frame().x - 3000)) < 1,
			"clicking empty overview canvas lands on a free area");
		shot("12-free-area");
		return 400;
	}
	case 27:
		o->goto_direction(1, 0); // Super+Ctrl+Right: nearest desktop to the right
		return 600;
	case 28: {
		Camera want = camera_for_box(desk(d1)->frame());
		check(std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1,
			"keyboard jump to the desktop in that direction");
		return 400; // the scale scenario continues from here
	}
	case 29:
		// Fake devices for the Super pinch: a keyboard that can hold
		// Super, a pointer that parks the cursor off-centre.
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		move_to(900, 300);
		check(std::fabs(server->cursor->x - 900) < 0.5
			&& std::fabs(server->cursor->y - 300) < 0.5,
			"the fake pointer placed the cursor");
		super_start = o->camera.current();
		keep({one, two, three}); // Desktop 1's board, before any zoom
		frame0 = desk(d1)->frame();
		return 300;
	case 30: {
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
		check(server->seat->pointer_state.focused_surface == marker->wlr->base->surface,
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
		check(marker != nullptr && marker->board == d1 && marker->tiled
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
		// Without Super a 2-finger pinch still belongs to the client.
		Camera start = o->camera.current();
		pinch_down(2);
		check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
			"2-finger pinch without Super forwards to the client");
		pinch_move(1.5);
		pinch_move(0.6);
		pinch_up(false);
		check(std::holds_alternative<Gestures::Idle>(server->gestures.state()),
			"the forwarded pinch ended cleanly");
		check(std::fabs(o->camera.current().zoom - start.zoom) < 1e-9
			&& std::fabs(o->camera.current().cx - start.cx) < 1e-9,
			"without Super the canvas did not scale");
		shot("19-pinch-without-super");
		return 400; // the tiled-scale scenario continues
	}
	case 45:
		// Back to Desktop 1 for the tiling interaction: Super+Ctrl+Right's
		// path, at its scale.
		o->goto_desktop(*desk(d1));
		return 700;
	case 46: {
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
		Rect fr = three->frame_rect();
		Rect vp = o->viewport();
		check(three->maximized && std::fabs(fr.x - vp.x) < 1 && std::fabs(fr.y - vp.y) < 1
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
		Camera want = camera_for_box(desk(d1)->frame(), 1.0);
		check(o->camera.current().zoom == 1 && o->interactive()
			&& std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1,
			"the camera eased with it and rests on the desktop at 100 %");
		std::string changed = changed_boxes();
		check(changed.empty() && told_output_scale() && three->maximized, std::format(
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
		check(desk(d1)->tiling() && one->board == d1 && two->board == d1
			&& marker != nullptr && marker->board == d1 && !three->board.has_value(),
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
		View *rholder = one->slice == RT ? one
			: two->slice == RT ? two : marker;
		bool others_kept = late != nullptr && rholder != nullptr;
		for (const auto &[view, box] : kept) {
			if (view != rholder) {
				Rect now = view->frame_rect();
				others_kept = others_kept && view->mapped && now.x == box.x
					&& now.y == box.y && now.w == box.w && now.h == box.h;
			}
		}
		check(late != nullptr && late->tiled && late->board == d1
				&& inside(late->frame_rect(), desk(d1)->frame()),
			"a window opened at 200 % joined the board in its frame");
		check(rholder != nullptr && rholder->slice == RT && late->slice == RB
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
		// A 2-finger scroll over the empty canvas right of the windows.
		scrolled_from = o->camera.current();
		for (int i = 0; i < 4; i++) {
			scroll(40);
		}
		return 300;
	}
	case 66: {
		Camera cam = o->camera.current();
		check(cam.zoom == 0.25 && o->interactive() && std::fabs(cam.cy - scrolled_from.cy) > 100,
			std::format("a 2-finger scroll pans the 25 % view at its zoom and leaves it"
				" interactive (zoom {:.2f}, panned {:.0f})", cam.zoom, cam.cy - scrolled_from.cy));
		// A 2-finger pinch without Super belongs to the client under the
		// pointer at any zoom, and never zooms the view.
		pinch_down(2);
		check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
			"a 2-finger pinch without Super is forwarded at 25 %");
		pinch_move(1.6);
		pinch_up(false);
		check(o->camera.current().zoom == 0.25, "the plain pinch left the zoom at 25 %");
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
		check(o->overview.active() && o->camera.current().zoom < 0.25,
			std::format("the overview opens further out than the 25 % view (zoom {:.3f})",
				o->camera.current().zoom));
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 800;
	case 70:
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
		// Launch an app from the overview: a Super tap opens the launcher.
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		for (uint32_t code : {KEY_F, KEY_O, KEY_O, KEY_T}) {
			key(code, true);
			key(code, false);
		}
		return 300;
	}
	case 83:
		check(server->launcher.is_open(), "a Super tap opened the launcher over the overview");
		launched_before.clear();
		for (View *v : server->views) {
			launched_before.push_back(v);
		}
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 300;
	case 84: {
		View *launched = nullptr;
		for (View *v : server->views) {
			if (v->mapped && std::find(launched_before.begin(), launched_before.end(), v)
					== launched_before.end()) {
				launched = v;
			}
		}
		if (launched == nullptr && retries++ < 100) {
			return -100;
		}
		wlr_box target = o->overview.target().box;
		Rect fr = launched != nullptr ? launched->frame_rect() : Rect{};
		check(launched != nullptr && o->overview.active()
			&& wlr_box_contains_point(&target, fr.cx(), fr.cy())
			&& !launched->board.has_value(),
			std::format("an app launched from the overview opened in its target, the free"
				" spot, not on B (centre {:.0f},{:.0f}; target {},{} {}x{})", fr.cx(), fr.cy(),
				target.x, target.y, target.width, target.height));
		shot("31-launched-into-target");
		return 0;
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
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

} // namespace

int main() {
	wlr_log_init(WLR_ERROR, nullptr);
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp"; // meson passes the repo's tmp/ via env
	shot_dir = std::format("{}/scenarios/gestures", root);
	tmp_root = root;
	mkdir_p(shot_dir);
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
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	return failures != 0;
}
