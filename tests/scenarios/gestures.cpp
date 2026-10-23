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
#include <glib.h>
#include <initializer_list>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "canvas/overview_chrome.h"
#include "core/box.h"
#include "core/config.h"
#include "core/geometry.h"
#include "core/process.h"
#include "core/server.h"
#include "core/wlr.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "platform/output.h"

// The allocation counter alloc-counter.c adds; absent in sanitizer builds.
extern "C" unsigned long long canvas_allocations(void) __attribute__((weak));

using namespace harness;

namespace {

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
// The gesture client (tests/clients/gesture-client.c): a square in the
// output's corner that says, on stdout, every swipe and pinch the compositor
// forwards to it. A forwarded gesture is checked by what the client says.
Child reporter;
std::size_t report_mark = 0; // how much the client had said before the action
Point pointer_was{}; // where the pointer stood before it parked on the client
std::optional<Point> overview_click; // the canvas point the overview click was made on

// The desktop a stored id names; null once it is gone.
Desktop *desk(std::optional<DesktopId> id) {
	return server->canvas.desktops.find(id);
}

int mapped_views() {
	int n = 0;
	for (View *v : server->views) {
		n += v->mapped;
	}
	return n;
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

// The swipe and pinch lines the client said from byte `from` on, joined by '|'.
std::string gestures_reported(std::size_t from) {
	std::string_view all = reporter.said();
	std::string lines;
	for (std::size_t at = from; at < all.size();) {
		std::size_t end = all.find('\n', at);
		if (end == std::string_view::npos) {
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
	pointer_was = {server->cursor->x - output().lx, server->cursor->y - output().ly};
	report_mark = reporter.said().size();
	move_to(2, 2);
}

void leave_reporter() {
	move_to(pointer_was.x, pointer_was.y);
}

void check_client_got(const std::string &want, std::string_view what,
		std::source_location where = std::source_location::current()) {
	std::string got = gestures_reported(report_mark);
	check(got == want, got == want ? std::string{what}
		: std::format("{} (the client said: {})", what, got), where);
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
		Point p = output().canvas_to_local(f.x + f.w / 2, f.y + f.h / 2);
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
			if (!window_covers(output().local_to_canvas(x, y))) {
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
			Point c = output().local_to_canvas(x, y);
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
		if (std::fabs(view->told_scale - output().wlr->scale) > 1e-9) {
			return false;
		}
	}
	return true;
}

// Super + 2-finger pinch through the device path, from the shown desktop's
// scale `from` to settle at `to`.
void super_pinch_to(double from, double to) {
	key(KEY_LEFTMETA, true);
	touchpad_pinch_begin(2);
	touchpad_pinch_update(2, to / from);
	touchpad_pinch_end(false);
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
	Output *o = &output();
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
	check(made != nullptr && at && clear && output().desktop_showing() == made,
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
Child subscriber; // a client subscribed to the compositor's events

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
	grab_point = output().canvas_to_local(f.x + 80, f.y + 16);
	move_to(grab_point.x, grab_point.y);
	button(BTN_LEFT, true);
	tick = 0;
}

int cascade_before = 0;
constexpr int cascade_windows = 9;

Result devices_and_windows() {
	// The fake devices the checks drive: a keyboard that holds Super and
	// presses chords, a pointer that clicks and parks.
	attach_pointer();
	attach_keyboard();
	check(reporter.start_client("CANVAS_GESTURE_CLIENT"), "the gesture client started");
	spawn("foot -T one sh -c 'echo window one; exec sleep 600'");
	spawn("foot -T two sh -c 'echo window two; exec sleep 600'");
	spawn("foot -T three sh -c 'echo window three; exec sleep 600'");
	return after(300);
}

Result windows_mapped() {
	View *one = titled("one");
	View *two = titled("two");
	View *three = titled("three");
	if (auto wait = wait_for(mapped_views() >= 3 && reporter.said_line("MAPPED"))) {
		return *wait;
	}
	check(reporter.said_line("MAPPED"), "the gesture client's square is mapped");
	check(mapped_views() == 3, std::format("three foot windows mapped ({})", mapped_views()));
	check(server->canvas.desktops.size() == 1, "Desktop 1 exists at start");
	d1 = server->canvas.desktops.all().front().id();
	check(three && one && two && three->deco.ssd, "foot negotiated server-side decorations");
	shot("01-three-windows");
	return after(400);
}

Result decoration_hits() {
	View *three = titled("three");
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
	return after(500);
}

Result maximized_frame_fills_the_screen() {
	Output *o = &output();
	View *three = titled("three");
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Rect fr = three->frame_rect();
	Rect vp = o->viewport();
	check(std::fabs(fr.x - vp.x) < 1 && std::fabs(fr.w - vp.w) < 1 &&
		std::fabs(fr.y - vp.y) < 1 && std::fabs(fr.h - vp.h) < 1,
		std::format("maximized frame fills the screen ({:.0f},{:.0f} {:.0f}x{:.0f})", fr.x,
			fr.y, fr.w, fr.h));
	shot("02-maximized");
	return after(400);
}

Result a_short_swipe_moves_nothing() {
	Output *o = &output();
	View *three = titled("three");
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	three->set_maximized(false);
	cam0 = o->camera.current();
	// A swipe short of the hop threshold moves nothing: no free pan.
	swipe(3, 20, 0, 4);
	check(o->camera.current().cx == cam0.cx && !o->camera.panning(),
		"a 3-finger swipe short of the hop threshold moves nothing");
	// Fingers right, no desktop that way: the view stays on Desktop 1.
	swipe(3, 600, 0, 30);
	return after(600);
}

Result no_desktop_that_way() {
	Output *o = &output();
	check(!o->overview.active() && o->camera.current().cx == cam0.cx
		&& o->camera.current().cy == cam0.cy && o->camera.current().zoom == 1
		&& !o->camera.panning() && o->interactive()
		&& server->hint.text() == "No desktop that way",
		std::format("with no desktop that way, a 3-finger swipe stays on the desktop"
			" (dx={:.0f}, hint {})", o->camera.current().cx - cam0.cx, server->hint.text()));
	// The free area to the right is a pan away (a drag, a scroll, or
	// Super+Alt+arrows for the user).
	o->pan(o->width, 0);
	shot("03-free-area");
	return after(400);
}

Result free_area_pinned_as_desktop_2() {
	Output *o = &output();
	View *three = titled("three");
	o->pin_desktop();
	check(server->canvas.desktops.size() == 2, "pinned the free area as Desktop 2");
	Desktop *showing = o->desktop_showing();
	d2 = showing->id();
	check(showing && showing != desk(d1) && showing->number() == 2,
		"Desktop 2 is what we're looking at");
	three->move(showing->frame().x + 200, showing->frame().y + 150);
	check(server->canvas.desktop_of(*three) == showing,
		"window three now belongs to Desktop 2");
	return after(300);
}

Result four_fingers_up() {
	swipe(4, 0, -300, 20); // 4 fingers up: overview
	return after(700);
}

Result overview_opened() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->overview.active(), "4-finger swipe up opened the overview");
	check(o->camera.current().zoom < 0.5,
		std::format("overview is zoomed out (zoom {:.3f})",
			o->camera.current().zoom));
	check(o->overview.target().desktop == d2,
		"overview target starts at the current desktop");
	shot("04-overview");
	return after(400);
}

Result three_fingers_left() {
	swipe(3, -80, 0, 8); // 3 fingers left: jump to the desktop on the left
	return after(500);
}

Result hopped_left() {
	Output *o = &output();
	check(o->overview.target().desktop == d1, "3-finger left jumped to Desktop 1");
	shot("05-hop-left");
	return after(400);
}

Result three_fingers_up() {
	swipe(3, 0, -80, 8); // up: no desktop that way -> free spot one screen up
	return after(400);
}

Result target_moved_to_a_free_spot() {
	Output *o = &output();
	check(!o->overview.target().desktop.has_value()
		&& o->overview.target().box.y == desk(d1)->frame().y - o->height,
		"no desktop above: target moved one screen up to a free spot");
	shot("06-free-spot-target");
	return after(600);
}

Result hopped_back_down() {
	Output *o = &output();
	swipe(3, 0, 80, 8); // back down onto Desktop 1
	check(o->overview.target().desktop == d1, "3-finger down returned to Desktop 1");
	pinch(3, 1.5, 10); // spread: switch tiling on for Desktop 1
	return after(500);
}

Result tiled_windows_stay_inside() {
	View *one = titled("one");
	View *two = titled("two");
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
	return after(400);
}

Result tiling_switched_off() {
	View *one = titled("one");
	View *two = titled("two");
	check(!desk(d1)->tiling(), "a second spread switched tiling off");
	check(one->frame_rect().x == board_one.x && one->frame_rect().y == board_one.y
		&& two->frame_rect().x == board_two.x && two->frame_rect().y == board_two.y,
		"switching tiling off kept the windows where the board left them"
		" (untiled, they may re-fit their own size)");
	check(!one->tiled && !two->tiled,
		"switching tiling off told the windows they are no longer tiled");
	shot("07-untiled");
	pinch(3, 1.5, 10); // and back on: the combine below joins the board
	return after(400);
}

Result tiling_back_on_then_combined() {
	View *three = titled("three");
	check(desk(d1)->tiling(), "the third spread switched tiling on again");
	pinch(3, 0.6, 10); // gather: combine nearest desktop (2) into Desktop 1
	check(server->canvas.desktops.size() == 1,
		"pinch-in combined Desktop 2 into Desktop 1");
	check(server->canvas.desktop_of(*three) == desk(d1) && three->tiled,
		"the combined-in window joined Desktop 1's board");
	d2 = std::nullopt;
	return after(500);
}

Result combined_into_a_tiled_board() {
	View *one = titled("one");
	View *two = titled("two");
	View *three = titled("three");
	// The 3-window board: the left half's holder keeps its top half for
	// the newcomer, which takes the bottom; the right half is untouched.
	View *left = one->frame_rect().x <= two->frame_rect().x ? one : two;
	View *right = left == one ? two : one;
	check(on_slice(left, slice_frame_of(*desk(d1), TL))
			&& on_slice(right, slice_frame_of(*desk(d1), RIGHT))
			&& on_slice(three, slice_frame_of(*desk(d1), BL)),
		"combine into a tiled desktop: the newcomer halved the left half");
	shot("08-combined-into-board");
	return after(400);
}

Result new_desktop_in_the_overview() {
	Output *o = &output();
	const wlr_box &frame = desk(d1)->frame();
	o->overview.new_desktop(frame.x + frame.width * 1.8, frame.y + frame.height / 2.0);
	d3 = o->overview.target().desktop;
	check(d3.has_value() && server->canvas.desktops.size() == 2,
		"new desktop created in the overview");
	return after(400);
}

Result new_desktop_swiped_away() {
	shot("09-new-desktop");
	swipe(4, 320, 0, 20); // 4-finger sideways: abandon the target desktop
	return after(400);
}

Result desktop_abandoned() {
	Output *o = &output();
	check(server->canvas.desktops.size() == 1
		&& server->canvas.desktops.find(o->overview.target().desktop) == nullptr,
		"4-finger sideways swipe abandoned the desktop");
	check(mapped_views() == 3, "abandoning kept every window");
	// Abandoning (by key) the desktop a swipe is sliding away must end that swipe cleanly.
	const wlr_box &frame = desk(d1)->frame();
	o->overview.new_desktop(frame.x + frame.width * 1.8, frame.y + frame.height / 2.0);
	server->gestures.swipe_begin(event_time(), 4);
	for (int i = 0; i < 6; i++) {
		server->gestures.swipe_update(event_time(), 20, 0);
	}
	check(std::holds_alternative<Gestures::Abandon>(server->gestures.state()),
		"4-finger sideways swipe is sliding a desktop away");
	server->canvas.abandon(o->overview.target().desktop);
	for (int i = 0; i < 6; i++) {
		server->gestures.swipe_update(event_time(), 20, 0);
	}
	server->gestures.swipe_end(event_time(), false);
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
	return after(400);
}

Result four_fingers_down() {
	swipe(4, 0, 300, 20); // 4-finger down: zoom into the target
	return after(700);
}

Result back_in_the_normal_view() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!o->overview.active() && o->camera.current().zoom == 1,
		"4-finger down returned to normal mode");
	Camera want = camera_for_box(desk(d1)->frame());
	check(std::fabs(o->camera.current().cx - want.cx) < 1
		&& std::fabs(o->camera.current().cy - want.cy) < 1,
		"landed on Desktop 1");
	shot("10-desktop1");
	return after(400);
}

Result window_minimized() {
	Output *o = &output();
	View *one = titled("one");
	one->set_minimized(true);
	check(one->minimized && server->focused_view != one,
		"minimize hides the window and moves focus");
	o->overview.enter();
	return after(600);
}

Result shelf_shot() {
	shot("11-overview-shelf");
	return after(400);
}

Result minimized_window_on_the_shelf() {
	Output *o = &output();
	View *one = titled("one");
	// The shelf's thumbnail takes the click where it is drawn.
	Shelf shelf = overview_shelf_layout(*o);
	check(!shelf.empty() && shelf.front().view == one,
		"the minimized window sits on the shelf");
	if (!shelf.empty()) {
		const wlr_box &thumb = shelf.front().box;
		move_to(thumb.x + thumb.width / 2.0, thumb.y + thumb.height / 2.0);
		button(BTN_LEFT, true);
		button(BTN_LEFT, false);
	}
	check(!one->minimized && server->focused_view == one,
		"a click on the shelf's thumbnail restores the window");
	// Restoring a window leaves the overview: open it again to click.
	if (!o->overview.active()) {
		o->overview.enter();
	}
	return after(600);
}

Result overview_canvas_click() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	// A click on the overview's empty canvas, left of Desktop 1.
	std::optional<Point> spot = empty_overview_spot();
	check(o->overview.active() && spot.has_value(),
		"the overview shows empty canvas left of Desktop 1");
	if (spot.has_value()) {
		overview_click = o->local_to_canvas(spot->x, spot->y);
		move_to(spot->x, spot->y);
		button(BTN_LEFT, true);
		button(BTN_LEFT, false);
	}
	return after(700);
}

Result free_area_made_by_the_click() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(overview_click.has_value() && !o->overview.active()
			&& !o->overview.target().desktop.has_value()
			&& std::fabs(o->camera.current().cx - overview_click->x) < 1
			&& std::fabs(o->camera.current().cy - overview_click->y) < 1,
		"clicking empty overview canvas lands on a free area");
	shot("12-free-area");
	return after(400);
}

Result keyboard_jump_right() {
	// Super+Ctrl+Right on the keyboard: the nearest desktop to the right.
	key(KEY_LEFTMETA, true);
	key(KEY_LEFTCTRL, true);
	key(KEY_RIGHT, true);
	key(KEY_RIGHT, false);
	key(KEY_LEFTCTRL, false);
	key(KEY_LEFTMETA, false);
	return after(600);
}

Result keyboard_jump_landed() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Camera want = camera_for_box(desk(d1)->frame());
	check(std::fabs(o->camera.current().cx - want.cx) < 1
		&& std::fabs(o->camera.current().cy - want.cy) < 1,
		"keyboard jump to the desktop in that direction");
	return after(400); // the scale scenario continues from here
}

Result pointer_placed() {
	Output *o = &output();
	View *one = titled("one");
	View *two = titled("two");
	View *three = titled("three");
	// A pointer that parks the cursor off-centre, for the Super pinch.
	move_to(900, 300);
	check(std::fabs(server->cursor->x - 900) < 0.5
		&& std::fabs(server->cursor->y - 300) < 0.5,
		"the fake pointer placed the cursor");
	super_start = o->camera.current();
	keep({one, two, three}); // Desktop 1's board, before any zoom
	frame0 = desk(d1)->frame();
	return after(300);
}

Result super_pinch_scaling() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	// Super + 2-finger pinch from the normal view: the shown desktop
	// scales with the fingers. Held open a step for a mid-pinch shot.
	key(KEY_LEFTMETA, true);
	touchpad_pinch_begin(2);
	check(std::holds_alternative<Gestures::DesktopScale>(server->gestures.state()),
		"Super + 2-finger pinch begins scaling the shown desktop");
	touchpad_pinch_update(2, 0.8);
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
	return after(400);
}

Result pinch_released() {
	// Released just off a step: the scale settles on the nearest
	// 5 % step (a pinch to 0.78 settles at 0.80).
	touchpad_pinch_update(2, 0.78);
	touchpad_pinch_end(false);
	check(std::fabs(desk(d1)->scale() - 0.8) < 1e-9,
		"released, the scale settles on the nearest step (0.80)");
	key(KEY_LEFTMETA, false);
	check(!server->launcher.is_open(),
		"releasing Super after a Super pinch does not open the launcher");
	return after(400);
}

Result desktop_at_80_percent() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
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
	check(server->hint.text() == "Desktop 1 · 80 %",
		"the settle is named like a completed action");
	shot("14-desktop-80");
	return after(400);
}

Result marker_window_opened() {
	spawn(std::format("foot -T marker sh -c 'cat > {}/marker'", scratch()).c_str());
	return after(300);
}

Result pointer_through_the_camera() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	// A window opened on the 80 % desktop: the pointer reaches it
	// through the camera, landing where it points.
	View *marker = titled("marker");
	if (auto wait = wait_for(marker != nullptr && marker->mapped)) {
		return *wait;
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
	button(BTN_LEFT, true);
	button(BTN_LEFT, false);
	check(server->focused_view == marker, "the click focused the window");
	shot("15-click-80");
	return after(300);
}

Result typing_into_the_marker() {
	key(KEY_O, true);
	key(KEY_O, false);
	key(KEY_K, true);
	key(KEY_K, false);
	key(KEY_ENTER, true);
	key(KEY_ENTER, false);
	return after(400);
}

Result typing_reached_the_marker() {
	View *one = titled("one");
	View *two = titled("two");
	View *three = titled("three");
	// What was typed reached the client through the camera.
	std::string path = std::format("{}/marker", scratch());
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
	View *marker = titled("marker");
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
	touchpad_pinch_begin(2);
	touchpad_pinch_update(2, 1.27); // raw 0.8 x 1.27 = 1.016: nearest step is 1.00
	touchpad_pinch_end(false);
	check(desk(d1)->scale() == 1.0,
		"a pinch settling near 100 % catches 100 % exactly");
	key(KEY_LEFTMETA, false);
	return after(400);
}

Result back_at_100_percent() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->camera.current().zoom == 1 && o->interactive(),
		"back at 100 %: zoom 1, interactive");
	std::string changed = changed_boxes();
	check(changed.empty() && told_output_scale(), std::format(
		"back at 100 %, every window kept its box and the output's scale{}", changed));
	// Spread past 100 % for the 125 % frame.
	key(KEY_LEFTMETA, true);
	touchpad_pinch_begin(2);
	touchpad_pinch_update(2, 1.25);
	touchpad_pinch_end(false);
	check(std::fabs(desk(d1)->scale() - 1.25) < 1e-9,
		"spreading further settles at 125 %");
	key(KEY_LEFTMETA, false);
	return after(400);
}

Result desktop_at_125_percent() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(std::fabs(o->camera.current().zoom - 1.25) < 1e-9 && o->interactive()
		&& desk(d1)->frame().width == std::lround(o->width / 1.25),
		"the desktop rests at 125 %, its frame the base size over 1.25");
	std::string changed = changed_boxes();
	check(changed.empty() && told_output_scale(), std::format(
		"at 125 %, every window kept its box and the output's scale{}", changed));
	shot("16-desktop-125");
	return after(400);
}

Result overview_over_the_scaled_desktop() {
	Output *o = &output();
	// The overview from a scaled desktop: its tab names the scale.
	o->overview.enter();
	return after(600);
}

Result overview_tab_names_the_scale() {
	Output *o = &output();
	check(o->overview.active(), "the overview opened over the 125 % desktop");
	shot("17-overview-125");
	o->overview.exit();
	return after(700);
}

Result swipe_down_toward_nothing() {
	Output *o = &output();
	// Fingers down, no desktop below: the view stays.
	hop_from = o->camera.destination();
	swipe(3, 0, 300, 10);
	return after(600);
}

Result free_view_pinned() {
	Output *o = &output();
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
	touchpad_pinch_begin(2);
	check(server->canvas.desktops.size() == before + 1
		&& std::holds_alternative<Gestures::DesktopScale>(
			server->gestures.state())
		&& o->desktop_showing() != nullptr
		&& std::fabs(o->desktop_showing()->scale() - 1.25) < 1e-9,
		"a Super pinch on a free view pins it as a desktop first, at its zoom");
	touchpad_pinch_update(2, 0.72); // raw 1.25 x 0.72 = 0.9
	check(std::fabs(o->camera.current().zoom - 0.9) < 1e-9,
		"the pinned desktop scales with the fingers");
	shot("18-free-view-pinned");
	return after(400);
}

Result pinned_desktop_settled() {
	Output *o = &output();
	touchpad_pinch_end(false);
	check(o->desktop_showing() != nullptr
		&& std::fabs(o->desktop_showing()->scale() - 0.9) < 1e-9,
		"the pinned desktop settled at 90 %");
	key(KEY_LEFTMETA, false);
	check(!server->launcher.is_open(),
		"no launcher from a Super pinch on a free view");
	return after(400);
}

Result pinch_without_super() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	// Without Super a 2-finger pinch still belongs to the client.
	act_when(true, [&] {
		Camera start = o->camera.current();
		park_on_reporter();
		touchpad_pinch_begin(2);
		check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
			"2-finger pinch without Super forwards to the client");
		touchpad_pinch_update(2, 1.5);
		touchpad_pinch_update(2, 0.6);
		touchpad_pinch_end(false);
		leave_reporter();
		check(std::holds_alternative<Gestures::Idle>(server->gestures.state()),
			"the forwarded pinch ended cleanly");
		check(std::fabs(o->camera.current().zoom - start.zoom) < 1e-9
			&& std::fabs(o->camera.current().cx - start.cx) < 1e-9,
			"without Super the canvas did not scale");
	});
	const std::string want = "PINCH BEGIN 2|PINCH UPDATE 0.0 0.0 1.50 0.0"
		"|PINCH UPDATE 0.0 0.0 0.60 0.0|PINCH END 0";
	if (auto wait = wait_for(gestures_reported(report_mark) == want,
			{.poll_ms = 50, .budget_ms = 5000})) {
		return *wait;
	}
	check_client_got(want, "the client under the pointer received the 2-finger"
		" pinch without Super");
	shot("19-pinch-without-super");
	return after(400); // the tiled-scale scenario continues
}

Result back_to_desktop_1() {
	Output *o = &output();
	// Back to Desktop 1 for the tiling interaction: Super+Ctrl+Right's
	// path, at its scale.
	o->goto_desktop(*desk(d1));
	return after(700);
}

Result eased_onto_desktop_1() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
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
	return after(400);
}

Result tiled_desktop_at_80_percent() {
	// Super + pinch on a tiled desktop: the settled scale leaves the
	// board alone.
	key(KEY_LEFTMETA, true);
	touchpad_pinch_begin(2);
	touchpad_pinch_update(2, 0.64); // raw 1.25 x 0.64 = 0.8
	touchpad_pinch_end(false);
	check(std::fabs(desk(d1)->scale() - 0.8) < 1e-9,
		"the tiled desktop settled at 80 %");
	key(KEY_LEFTMETA, false);
	return after(400);
}

Result tiled_board_at_80_percent() {
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
	return after(600); // untiled, foot may re-fit its own size
}

Result tiling_switched_off_at_80_percent() {
	View *three = titled("three");
	check(!desk(d1)->tiling(), "Super+T switched tiling off");
	three->set_maximized(true);
	return after(500);
}

Result maximized_at_80_percent() {
	Output *o = &output();
	View *one = titled("one");
	View *two = titled("two");
	View *three = titled("three");
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Rect fr = three->frame_rect();
	Rect vp = o->viewport();
	check(three->mode == WindowMode::maximized
		&& std::fabs(fr.x - vp.x) < 1 && std::fabs(fr.y - vp.y) < 1
		&& std::fabs(fr.w - vp.w) < 1 && std::fabs(fr.h - vp.h) < 1,
		std::format("maximized at 80 %, the window fills the screen ({:.0f}x{:.0f})",
			fr.w, fr.h));
	keep({one, two, three, titled("marker")});
	zoom_round = 0;
	zoom_changes.clear();
	return after(300);
}

Result twelve_settles() {
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
		return again(100);
	}
	if (round == targets.size()) {
		check(zoom_changes.empty(), std::format(
			"twelve settles between 50 % and 200 % changed no window{}", zoom_changes));
		return after(300);
	}
	super_pinch_to(desk(d1)->scale(), targets[round]);
	zoom_round++;
	return again(450);
}

Result zoom_rounds_ended() {
	// Super+0: back to 100 % from 60 %.
	check(std::fabs(desk(d1)->scale() - 0.6) < 1e-9, "the zoom rounds ended at 60 %");
	key(KEY_LEFTMETA, true);
	key(KEY_0, true);
	key(KEY_0, false);
	key(KEY_LEFTMETA, false);
	check(desk(d1)->scale() == 1.0 && server->hint.text() == "Desktop 1 · 100 %",
		"Super+0 returned the desktop shown to 100 %, named in a hint");
	check(!server->launcher.is_open(), "Super+0 opens no launcher");
	return after(700);
}

Result reset_to_100_percent() {
	Output *o = &output();
	View *three = titled("three");
	if (auto wait = wait_for(settled())) {
		return *wait;
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
	return after(500);
}

Result spread_to_125_percent() {
	Output *o = &output();
	check(std::fabs(desk(d1)->scale() - 1.25) < 1e-9, "spread to 125 % again");
	o->overview.enter();
	return after(600);
}

Result overview_reset() {
	Output *o = &output();
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
	check(server->hint.text() == "Desktop 1 · 100 %", "the overview's reset is named in a hint");
	shot("25-overview-reset");
	o->overview.exit();
	return after(700);
}

Result overview_left_at_100_percent() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Camera want = camera_for_box(desk(d1)->frame(), 1.0);
	check(!o->overview.active() && o->camera.current().zoom == 1 && o->interactive()
		&& std::fabs(o->camera.current().cx - want.cx) < 1
		&& std::fabs(o->camera.current().cy - want.cy) < 1,
		"leaving the overview rests on the desktop at 100 %");
	std::string changed = changed_boxes();
	check(changed.empty() && told_output_scale(), std::format(
		"no window changed through the overview's reset{}", changed));
	return after(400);
}

Result board_through_a_zoom() {
	// The board through a zoom: laid out at 50 % (the maximized window
	// stays out of it), then shown at 200 %, where the frame's edges
	// pass the outer windows' centres.
	super_pinch_to(1.0, 0.5);
	return after(450);
}

Result super_t_at_50_percent() {
	key(KEY_LEFTMETA, true);
	key(KEY_T, true);
	key(KEY_T, false);
	key(KEY_LEFTMETA, false);
	return after(600);
}

Result board_tiled_at_50_percent() {
	View *one = titled("one");
	View *two = titled("two");
	View *three = titled("three");
	View *marker = titled("marker");
	check(desk(d1)->tiling() && one->seated_on(*d1) && two->seated_on(*d1)
		&& marker != nullptr && marker->seated_on(*d1) && !three->board_seat.has_value(),
		"Super+T tiled the 50 % desktop, the maximized window off the board");
	keep({one, two, marker});
	super_pinch_to(0.5, 2.0);
	return after(450);
}

Result board_kept_to_200_percent() {
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
	return after(300);
}

Result late_window_mapped() {
	View *late = titled("late");
	if (auto wait = wait_for(late != nullptr && late->mapped)) {
		return *wait;
	}
	return after(700); // the join's configures land
}

Result late_window_joined_the_board() {
	View *one = titled("one");
	View *two = titled("two");
	// A window opened at 200 % joins the board there: it halves the
	// largest held slice, the right half, and moves nobody else.
	View *late = titled("late");
	View *marker = titled("marker");
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
	return after(400);
}

Result reset_before_the_floor() {
	// Views at any zoom: back to 100 %, then down to the 25 % floor.
	key(KEY_LEFTMETA, true);
	key(KEY_0, true);
	key(KEY_0, false);
	key(KEY_LEFTMETA, false);
	return after(700);
}

Result pinch_to_the_floor() {
	super_pinch_to(1.0, 0.25);
	return after(500);
}

Result scale_reached_the_floor() {
	Output *o = &output();
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
		touchpad_scroll(40);
	}
	check(o->camera.current() == scrolled_from,
		"a 2-finger scroll over empty canvas without Super moves nothing");
	if (window.has_value()) {
		move_to(window->x, window->y);
	}
	key(KEY_LEFTMETA, true);
	for (int i = 0; i < 4; i++) {
		touchpad_scroll(40);
	}
	key(KEY_LEFTMETA, false);
	return after(300);
}

Result scroll_and_pinch_at_25_percent() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	act_when(true, [&] {
		Camera cam = o->camera.current();
		check(cam.zoom == 0.25 && o->interactive() && std::fabs(cam.cy - scrolled_from.cy) > 100,
			std::format("a Super 2-finger scroll over a window pans the 25 % view at its"
				" zoom and leaves it interactive (zoom {:.2f}, panned {:.0f})", cam.zoom,
				cam.cy - scrolled_from.cy));
		// A 2-finger pinch without Super belongs to the client under the
		// pointer at any zoom, and never zooms the view.
		park_on_reporter();
		touchpad_pinch_begin(2);
		check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
			"a 2-finger pinch without Super is forwarded at 25 %");
		touchpad_pinch_update(2, 1.6);
		touchpad_pinch_end(false);
		leave_reporter();
		check(o->camera.current().zoom == 0.25, "the plain pinch left the zoom at 25 %");
	});
	const std::string want = "PINCH BEGIN 2|PINCH UPDATE 0.0 0.0 1.60 0.0|PINCH END 0";
	if (auto wait = wait_for(gestures_reported(report_mark) == want,
			{.poll_ms = 50, .budget_ms = 5000})) {
		return *wait;
	}
	check_client_got(want, "at 25 % the client under the pointer received the"
		" 2-finger pinch");
	// A 3-finger swipe left off the 25 % desktop, where no desktop lies.
	hop_from = o->camera.destination();
	swipe(3, -900, 0, 30);
	return after(600);
}

Result swipe_toward_nothing_at_25_percent() {
	Output *o = &output();
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
	return after(300);
}

Result overview_from_25_percent() {
	Output *o = &output();
	o->overview.enter();
	return after(700);
}

Result overview_opened_further_out() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->overview.active() && o->camera.current().zoom < 0.25,
		std::format("the overview opens further out than the 25 % view (zoom {:.3f})",
			o->camera.current().zoom));
	key(KEY_ENTER, true);
	key(KEY_ENTER, false);
	return after(800);
}

Result overview_left_into_the_free_view() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!o->overview.active() && std::fabs(o->camera.current().zoom - 0.25) < 1e-9
		&& o->interactive(),
		std::format("leaving the overview into the free view it came from keeps its zoom"
			" ({:.2f})", o->camera.current().zoom));
	key(KEY_LEFTMETA, true);
	key(KEY_0, true);
	key(KEY_0, false);
	key(KEY_LEFTMETA, false);
	return after(700);
}

Result free_area_at_100_percent() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->camera.current().zoom == 1 && o->interactive()
		&& server->hint.text() == "Free area · 100 %",
		std::format("Super+0 returns a free view to 100 % (zoom {:.2f}, hint {})",
			o->camera.current().zoom, server->hint.text()));
	return after(300);
}

Result a_row_of_desktops() {
	Output *o = &output();
	// Hops in the normal view, on a row far from the rest: A, B at 80 %
	// and C, a screen or so apart.
	int w = o->width, h = o->height;
	int x0 = 100000, y0 = 100000;
	hop_a = server->canvas.desktops.create(wlr_box{x0, y0, w, h}).id();
	hop_b = server->canvas.desktops.create(
		wlr_box{x0 + 2 * w, y0, w * 5 / 4, h * 5 / 4}, 0.8).id();
	hop_c = server->canvas.desktops.create(wlr_box{x0 + 4 * w, y0, w, h}).id();
	o->camera.jump(camera_for_box(desk(hop_a)->frame(), 1));
	return after(300);
}

Result on_desktop_a() {
	Output *o = &output();
	check(o->desktop_showing() == desk(hop_a) && o->interactive(), "on desktop A of the row");
	// Two quick swipes right: the second starts from where the first
	// lands, so they walk two desktops.
	swipe(3, 200, 0, 8);
	swipe(3, 200, 0, 8);
	return after(900);
}

Result two_quick_swipes() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Camera want = camera_for_box(desk(hop_c)->frame(), 1);
	Camera cam = o->camera.current();
	check(cam.cx == want.cx && cam.cy == want.cy && cam.zoom == 1 && o->interactive(),
		std::format("two quick 3-finger swipes walked two desktops, A to B to C"
			" (off by {:.0f},{:.0f})", cam.cx - want.cx, cam.cy - want.cy));
	shot("29-hopped-to-c");
	server->hint.set("");
	swipe(3, 300, 0, 10); // nothing right of C
	return after(600);
}

Result no_desktop_past_c() {
	Output *o = &output();
	Camera c = camera_for_box(desk(hop_c)->frame(), 1);
	Camera cam = o->camera.current();
	check(cam.cx == c.cx && cam.cy == c.cy && cam.zoom == 1 && !o->camera.panning()
		&& o->interactive() && server->hint.text() == "No desktop that way",
		std::format("with no desktop right of C, a swipe stays on C and says so (hint {})",
			server->hint.text()));
	swipe(3, -1500, 0, 60); // a long swipe left: one hop all the same
	return after(900);
}

Result long_swipe_hopped_once() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Camera b = camera_for_box(desk(hop_b)->frame(), 0.8);
	Camera cam = o->camera.current();
	check(cam.cx == b.cx && cam.cy == b.cy && std::fabs(cam.zoom - 0.8) < 1e-9
		&& o->interactive(),
		std::format("a long swipe hops once, onto B at its 80 % (zoom {:.2f})", cam.zoom));
	shot("30-hopped-to-b-80");
	// A sloppy 3-finger swipe the touchpad reports as a pinch: its
	// travel hops all the same, here right onto C.
	server->gestures.pinch_begin(event_time(), 3);
	for (int i = 0; i < 10; i++) {
		event_time();
		server->gestures.pinch_update(event_time(), 30, 0, 1.02, 0);
	}
	event_time();
	server->gestures.pinch_end(event_time(), false);
	return after(900);
}

Result pinch_with_travel_hopped() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
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
	return after(600);
}

Result nothing_below_the_free_view() {
	Output *o = &output();
	Camera cam = o->camera.current();
	check(o->desktop_showing() == nullptr && cam.cx == hop_from.cx && cam.cy == hop_from.cy
		&& !o->camera.panning() && server->hint.text() == "No desktop that way",
		"on a free view, a swipe toward no desktop stays and says so");
	swipe(3, 0, -300, 10);
	return after(900);
}

Result up_from_the_free_view() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
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
	return after(400);
}

Result b_tiled() {
	Output *o = &output();
	check(desk(hop_b)->tiling(), "Super+T tiled B");
	o->overview.enter();
	return after(700);
}

Result overview_over_b() {
	Output *o = &output();
	check(o->overview.active() && o->overview.target().desktop == hop_b,
		"the overview opened over B, its target");
	swipe(3, 0, 80, 8); // nothing below B: the target moves to a free spot
	return after(500);
}

Result target_moved_below_b() {
	Output *o = &output();
	check(o->overview.active() && !o->overview.target().desktop.has_value(),
		"the overview's target moved to a free spot below B");
	// Launch an app from the overview: Super+Space opens the launcher.
	key(KEY_LEFTMETA, true);
	key(KEY_SPACE, true);
	key(KEY_SPACE, false);
	key(KEY_LEFTMETA, false);
	tap_keys({KEY_F, KEY_O, KEY_O, KEY_T});
	return after(300);
}

Result launcher_over_the_overview() {
	check(server->launcher.is_open(), "Super+Space opened the launcher over the overview");
	remember_views();
	key(KEY_ENTER, true);
	key(KEY_ENTER, false);
	return after(300);
}

Result app_launched_into_the_target() {
	Output *o = &output();
	View *launched = launched_view();
	if (auto wait = wait_for(launched != nullptr)) {
		return *wait;
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
	return after(300);
}

// an app that takes a second to appear, launched from the overview
Result launcher_before_the_late_window() {
	key(KEY_LEFTMETA, true);
	key(KEY_SPACE, true);
	key(KEY_SPACE, false);
	key(KEY_LEFTMETA, false);
	tap_keys({KEY_T, KEY_A, KEY_R, KEY_D, KEY_Y});
	return after(300);
}

Result launcher_over_the_overview_again() {
	Output *o = &output();
	check(server->launcher.is_open(), "the launcher opened over the overview again");
	remember_views();
	launch_box = o->overview.target().box;
	tap_keys({KEY_ENTER, KEY_ESC}); // launch, then Escape back to B at once
	return after(300);
}

Result overview_closed_early() {
	Output *o = &output();
	check(!o->overview.active() && launched_view() == nullptr,
		"Escape closed the overview before the launched window appeared");
	return after(100);
}

Result late_window_in_the_free_spot() {
	Output *o = &output();
	View *tardy = launched_view();
	if (auto wait = wait_for(tardy != nullptr)) {
		return *wait;
	}
	Rect fr = tardy != nullptr ? tardy->frame_rect() : Rect{};
	check(tardy != nullptr && wlr_box_contains_point(&launch_box, fr.cx(), fr.cy())
			&& o->desktop_showing() == desk(hop_b),
		std::format("the late window opened in the free spot it was launched for, not on B"
			" where the overview closed to (centre {:.0f},{:.0f})", fr.cx(), fr.cy()));
	check(tardy != nullptr && server->focused_view != tardy
			&& server->hint.text() == "Opened in a free area",
		std::format("unseen, it left the keyboard alone, and a hint said where (hint {})",
			server->hint.text()));
	return after(300);
}

// on B, far from the canvas origin: a window asking to start maximized
Result maximized_window_launched() {
	remember_views();
	spawn("exec foot --maximized -T maxhere");
	return after(300);
}

Result maximized_window_filled_the_view() {
	Output *o = &output();
	View *maxhere = launched_view();
	if (auto wait = wait_for(maxhere != nullptr)) {
		return *wait;
	}
	Rect fr = maxhere != nullptr ? maxhere->frame_rect() : Rect{};
	check(maxhere != nullptr && maxhere->mode == WindowMode::maximized
			&& o->viewport().contains(fr.cx(), fr.cy()) && server->focused_view == maxhere,
		std::format("a window starting maximized filled the screen being looked at, and"
			" took the keyboard (centre {:.0f},{:.0f})", fr.cx(), fr.cy()));
	shot("32-started-maximized-here");
	o->overview.enter();
	return after(700);
}

Result overview_over_b_again() {
	Output *o = &output();
	check(o->overview.active() && o->overview.target().desktop == hop_b,
		"the overview opened over B again");
	swipe(3, 0, 80, 8); // the target down to the free spot below B again
	return after(500);
}

Result target_moved_below_b_again() {
	Output *o = &output();
	check(!o->overview.target().desktop.has_value(),
		"the overview's target moved to the free spot below B again");
	key(KEY_LEFTMETA, true);
	key(KEY_SPACE, true);
	key(KEY_SPACE, false);
	key(KEY_LEFTMETA, false);
	tap_keys({KEY_B, KEY_E, KEY_L, KEY_A, KEY_T, KEY_E, KEY_D});
	return after(300);
}

Result launcher_a_third_time() {
	Output *o = &output();
	check(server->launcher.is_open(), "the launcher opened over the overview a third time");
	remember_views();
	launch_box = o->overview.target().box;
	tap_keys({KEY_ENTER, KEY_ESC});
	return after(300);
}

Result late_maximized_window() {
	Output *o = &output();
	View *belated = launched_view();
	if (auto wait = wait_for(belated != nullptr)) {
		return *wait;
	}
	Rect fr = belated != nullptr ? belated->frame_rect() : Rect{};
	check(belated != nullptr && belated->mode == WindowMode::maximized
			&& !o->overview.active()
			&& wlr_box_contains_point(&launch_box, fr.cx(), fr.cy())
			&& o->desktop_showing() == desk(hop_b),
		std::format("a late window starting maximized filled the free spot it was launched"
			" for, not B (centre {:.0f},{:.0f})", fr.cx(), fr.cy()));
	return after(0);
}

Result four_finger_hops_installed() {
	// The four-finger-hops preset: every 3 and 4 swaps roles.
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	server->gestures.set_options(
		{.swipes = settings::SwipePreset::four_finger_hops, .pinch_scale = true});
	check(server->gestures.options().swipes == settings::SwipePreset::four_finger_hops,
		"set_options installed the four-finger-hops preset");
	check(help_names("4 fingers") && help_names("3 down")
			&& help_names("4-finger pinch"),
		"the help line names the four-finger preset's gestures");
	swipe(4, -200, 0, 8); // 4 fingers left: a hop, like 3 fingers today
	return after(900);
}

Result four_finger_hop() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Camera a = camera_for_box(desk(hop_a)->frame(), 1);
	Camera cam = o->camera.current();
	check(cam.cx == a.cx && cam.cy == a.cy && std::fabs(cam.zoom - 1) < 1e-9
			&& o->desktop_showing() == desk(hop_a),
		std::format("a 4-finger swipe hopped from B onto A in the normal view"
			" (off by {:.0f},{:.0f})", cam.cx - a.cx, cam.cy - a.cy));
	shot("33-four-finger-hop");
	swipe(3, 0, -300, 20); // 3 fingers up: the overview
	return after(700);
}

Result overview_by_three_fingers_up() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->overview.active() && o->camera.current().zoom < 0.5,
		std::format("a 3-finger swipe up opened the overview (zoom {:.3f})",
			o->camera.current().zoom));
	check(o->overview.target().desktop == hop_a, "the overview targets A");
	swipe(4, 80, 0, 8); // 4 fingers right: a hop, in the overview too
	return after(500);
}

Result four_finger_hop_in_the_overview() {
	Output *o = &output();
	check(o->overview.active() && o->overview.target().desktop == hop_b,
		"a 4-finger swipe in the overview hopped the target onto B");
	swipe(3, 0, 300, 20); // 3 fingers down: into the target
	return after(800);
}

Result three_fingers_down_into_b() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Camera b = camera_for_box(desk(hop_b)->frame(), desk(hop_b)->scale());
	check(!o->overview.active() && o->desktop_showing() == desk(hop_b)
			&& std::fabs(o->camera.current().cx - b.cx) < 1
			&& std::fabs(o->camera.current().zoom - b.zoom) < 1e-9,
		"a 3-finger swipe down went into the target, B");
	o->overview.enter(); // for the abandon and the pinch previews
	return after(700);
}

Result overview_over_b_for_a_fresh_desktop() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
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
	return after(500);
}

Result sideways_swipe_abandoned_it() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
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
	server->gestures.pinch_begin(event_time(), 4);
	for (int i = 1; i <= 10; i++) {
		event_time();
		server->gestures.pinch_update(event_time(), 0, 0, 1 + 0.5 * i / 10, 0);
	}
	check(server->gestures.pinch() != nullptr
			&& server->hint.text() == std::format("Release to tile Desktop {}",
				desk(hop_a)->number()),
		std::format("a 4-finger spread previews tiling (hint {})",
			server->hint.text()));
	event_time();
	server->gestures.pinch_end(event_time(), true); // cancelled: the preview is the point
	server->gestures.pinch_begin(event_time(), 4);
	for (int i = 1; i <= 10; i++) {
		event_time();
		server->gestures.pinch_update(event_time(), 0, 0, 1 - 0.4 * i / 10, 0);
	}
	check(server->gestures.pinch() != nullptr
			&& server->hint.text() == std::format(
				"Release to combine Desktop {} into Desktop {}",
				desk(hop_b)->number(), desk(hop_a)->number()),
		std::format("a 4-finger gather previews combining (hint {})",
			server->hint.text()));
	event_time();
	server->gestures.pinch_end(event_time(), true);
	return after(400);
}

Result out_of_the_overview_onto_b() {
	Output *o = &output();
	// Out of the overview, onto B, for the `off` preset.
	o->overview.set_target(desk(hop_b)->frame(), hop_b, false);
	o->overview.exit();
	return after(700);
}

Result swipes_forwarded_with_the_preset_off() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	act_when(true, [&] {
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
		touchpad_swipe_begin(3);
		check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
			"a 3-finger swipe in the normal view is forwarded to the client");
		touchpad_swipe_update(3, 60, 0);
		touchpad_swipe_end(false);
		touchpad_swipe_begin(4);
		check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
			"a 4-finger swipe in the normal view is forwarded to the client");
		touchpad_swipe_update(4, 0, -60);
		touchpad_swipe_end(false);
		leave_reporter();
		Camera now = o->camera.current();
		check(!o->overview.active() && now.cx == before.cx && now.cy == before.cy
				&& now.zoom == before.zoom && !o->camera.panning()
				&& o->desktop_showing() == desk(hop_b),
			"with the preset off, 3- and 4-finger swipes left the camera and"
				" the overview state alone");
	});
	const std::string want = "SWIPE BEGIN 3|SWIPE UPDATE 60.0 0.0|SWIPE END 0"
		"|SWIPE BEGIN 4|SWIPE UPDATE 0.0 -60.0|SWIPE END 0";
	if (auto wait = wait_for(gestures_reported(report_mark) == want,
			{.poll_ms = 50, .budget_ms = 5000})) {
		return *wait;
	}
	check_client_got(want, "with the preset off, the client under the pointer"
		" received the 3- and the 4-finger swipe");
	o->overview.enter();
	return after(700);
}

Result overview_with_the_preset_off() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	// In the overview the compositor owns every gesture: with no preset,
	// 3- and 4-finger gestures do nothing at all.
	check(o->overview.active(), "the overview opened with the preset off");
	std::optional<DesktopId> target_before = o->overview.target().desktop;
	Camera before = o->camera.current();
	server->hint.set("");
	server->gestures.swipe_begin(event_time(), 3);
	check(std::holds_alternative<Gestures::Ignored>(server->gestures.state()),
		"a 3-finger swipe in the overview does nothing");
	server->gestures.swipe_update(event_time(), 60, 60);
	server->gestures.swipe_end(event_time(), false);
	server->gestures.swipe_begin(event_time(), 4);
	check(std::holds_alternative<Gestures::Ignored>(server->gestures.state()),
		"a 4-finger swipe in the overview does nothing");
	server->gestures.swipe_update(event_time(), 60, 60);
	server->gestures.swipe_end(event_time(), false);
	server->gestures.pinch_begin(event_time(), 3);
	check(std::holds_alternative<Gestures::Ignored>(server->gestures.state()),
		"a 3-finger pinch in the overview does nothing");
	server->gestures.pinch_update(event_time(), 0, 0, 1.5, 0);
	server->gestures.pinch_end(event_time(), false);
	Camera now = o->camera.current();
	check(o->overview.active() && now.cx == before.cx && now.cy == before.cy
			&& now.zoom == before.zoom && server->hint.text().empty()
			&& o->overview.target().desktop == target_before
			&& std::holds_alternative<Gestures::Idle>(server->gestures.state()),
		"in the overview, 3- and 4-finger gestures did nothing at all");
	shot("36-overview-help-off");
	return after(400);
}

Result overview_left_for_the_pinch_switch() {
	Output *o = &output();
	o->overview.exit();
	return after(700);
}

Result super_pinch_forwarded_without_pinch_scale() {
	Output *o = &output();
	// pinch-scale off: a Super + 2-finger pinch is a plain pinch.
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	act_when(true, [&] {
		check(!o->overview.active() && o->desktop_showing() == desk(hop_b)
				&& o->interactive(), "back on B for the pinch-scale switch");
		server->gestures.set_options(
			{.swipes = settings::SwipePreset::three_finger_hops, .pinch_scale = false});
		Camera before = o->camera.current();
		double scale_before = desk(hop_b)->scale();
		park_on_reporter();
		key(KEY_LEFTMETA, true);
		touchpad_pinch_begin(2);
		check(std::holds_alternative<Gestures::Forwarded>(server->gestures.state()),
			"with pinch-scale off, a Super + 2-finger pinch is forwarded to the"
				" client");
		touchpad_pinch_update(2, 1.5);
		touchpad_pinch_end(false);
		key(KEY_LEFTMETA, false);
		leave_reporter();
		Camera now = o->camera.current();
		check(now.cx == before.cx && now.cy == before.cy && now.zoom == before.zoom
				&& desk(hop_b)->scale() == scale_before
				&& !server->launcher.is_open(),
			"with pinch-scale off, the Super pinch scaled no desktop and opened"
				" no launcher");
	});
	const std::string want = "PINCH BEGIN 2|PINCH UPDATE 0.0 0.0 1.50 0.0|PINCH END 0";
	if (auto wait = wait_for(gestures_reported(report_mark) == want,
			{.poll_ms = 50, .budget_ms = 5000})) {
		return *wait;
	}
	check_client_got(want, "with pinch-scale off, the client under the pointer"
		" received the Super + 2-finger pinch");
	return after(400);
}

Result overview_for_the_pinch_switch() {
	Output *o = &output();
	o->overview.enter();
	return after(700);
}

Result overview_zoomed_by_the_pinch() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	// In the overview the same pinch zooms the overview camera.
	check(o->overview.active(), "the overview opened for the pinch-scale switch");
	double zoom_before = o->camera.current().zoom;
	double scale_before = desk(hop_b)->scale();
	key(KEY_LEFTMETA, true);
	touchpad_pinch_begin(2);
	check(std::holds_alternative<Gestures::OverviewZoom>(server->gestures.state()),
		"in the overview, the Super pinch is the overview's zoom");
	touchpad_pinch_update(2, 1.6);
	check(std::fabs(o->camera.current().zoom - zoom_before) > 1e-9
			&& std::holds_alternative<Gestures::OverviewZoom>(
				server->gestures.state()),
		std::format("the pinch zoomed the overview camera ({:.3f} -> {:.3f})",
			zoom_before, o->camera.current().zoom));
	touchpad_pinch_end(false);
	key(KEY_LEFTMETA, false);
	check(desk(hop_b)->scale() == scale_before && !server->launcher.is_open(),
		"the overview's Super pinch scaled no desktop and opened no launcher");
	shot("34-overview-super-pinch-plain");
	return after(400);
}

Result defaults_restored() {
	Output *o = &output();
	// The defaults restored: 3 and 4 back in their own roles.
	o->overview.exit();
	server->gestures.set_options(GestureOptions{});
	check(server->gestures.options().swipes == settings::SwipePreset::three_finger_hops
			&& server->gestures.options().pinch_scale,
		"the default options restored");
	check(help_names("3 fingers") && help_names("4 down")
			&& help_names("3-finger pinch"),
		"the help line names the default preset's gestures");
	return after(700);
}

Result back_on_b_with_the_defaults() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!o->overview.active() && o->desktop_showing() == desk(hop_b),
		"back on B with the defaults");
	swipe(3, 200, 0, 8); // 3 fingers right: a hop again
	return after(900);
}

Result swipe_hopped_onto_c() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
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
	return after(900);
}

Result hop_swipe_away_hopped_left() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Camera b = camera_for_box(desk(hop_b)->frame(), 0.8);
	Camera cam = o->camera.current();
	check(cam.cx == b.cx && cam.cy == b.cy && o->desktop_showing() == desk(hop_b),
		std::format("with hop-swipe away, a 3-finger swipe right hopped left, onto B"
			" (off by {:.0f},{:.0f})", cam.cx - b.cx, cam.cy - b.cy));
	server->gestures.set_options(GestureOptions{});
	check(!server->gestures.options().hops_away, "hops go toward the fingers again");
	return after(0);
}

// Looking away (docs/intents/canvas/infinite-canvas.md), from B at 80 %.
// a drag over empty canvas pans only with Super
Result look_away() {
	Output *o = &output();
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
	button(BTN_LEFT, true);
	for (int i = 1; i <= 10; i++) {
		move_to(p.x + 20 * i, p.y + 10 * i);
	}
	button(BTN_LEFT, false);
	check(o->camera.destination() == up,
		"a drag over empty canvas without Super moves nothing");
	key(KEY_LEFTMETA, true);
	move_to(p.x, p.y);
	button(BTN_LEFT, true);
	for (int i = 1; i <= 10; i++) {
		move_to(p.x + 20 * i, p.y + 10 * i);
	}
	button(BTN_LEFT, false);
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
	return after(600);
}

Result double_super_came_back() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
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
	return after(600);
}

Result double_super_after_super_alt_right() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
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
	return after(900);
}

Result hop_after_the_look_away() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->desktop_showing() == desk(hop_c), "the hop after the look away landed on C");
	hop_from = o->camera.destination();
	tap_keys({KEY_LEFTMETA, KEY_LEFTMETA});
	return after(600);
}

Result double_super_after_the_hop() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
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
	return after(700);
}

Result overview_over_the_look_away() {
	Output *o = &output();
	check(o->overview.active(), "the overview opened over the look away");
	tap_keys({KEY_ESC});
	return after(800);
}

Result escape_brought_the_view_back() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!o->overview.active() && o->camera.destination() == hop_from,
		"Escape brought the view back from the overview where the look away had it");
	tap_keys({KEY_LEFTMETA, KEY_LEFTMETA});
	return after(600);
}

Result double_super_after_the_overview() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->camera.destination() == hop_from,
		"after the overview, double Super stays: the overview made that the view kept");
	return after(0);
}

Result only_desktop_1_left() {
	// Super+N makes a missing desktop beside desktop 1: from desktop 1
	// alone, desktops 2-5 land above, right, below and left of it.
	std::vector<DesktopId> others;
	for (const Desktop &d : server->canvas.desktops.all()) {
		if (d.number() != 1) {
			others.push_back(d.id());
		}
	}
	for (DesktopId id : others) {
		server->canvas.abandon(id);
	}
	// Super+1 goes to desktop 1, making it at its own place when even
	// that is gone.
	key(KEY_LEFTMETA, true);
	key(KEY_1, true);
	key(KEY_1, false);
	key(KEY_LEFTMETA, false);
	return after(700);
}

Result super_n_from_desktop_1_alone() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(server->canvas.desktops.size() == 1
			&& o->desktop_showing() == server->canvas.desktops.by_number(1),
		std::format("Super+N starts from desktop 1 alone ({} desktops)",
			server->canvas.desktops.size()));
	key(KEY_LEFTMETA, true);
	key(KEY_2, true);
	key(KEY_2, false);
	key(KEY_LEFTMETA, false);
	return after(700);
}

Result desktop_2_made() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
	check_made_desktop(2, beside_frame(anchor, 0, -1));
	key(KEY_LEFTMETA, true);
	key(KEY_3, true);
	key(KEY_3, false);
	key(KEY_LEFTMETA, false);
	return after(700);
}

Result desktop_3_made() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
	check_made_desktop(3, beside_frame(anchor, 1, 0));
	key(KEY_LEFTMETA, true);
	key(KEY_4, true);
	key(KEY_4, false);
	key(KEY_LEFTMETA, false);
	return after(700);
}

Result desktop_4_made() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
	check_made_desktop(4, beside_frame(anchor, 0, 1));
	key(KEY_LEFTMETA, true);
	key(KEY_5, true);
	key(KEY_5, false);
	key(KEY_LEFTMETA, false);
	return after(700);
}

Result desktop_5_made() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
	check_made_desktop(5, beside_frame(anchor, -1, 0));
	shot("37-super-n-made-beside");
	return after(500);
}

Result up_slot_covered() {
	// The up slot already covered: Super+2 passes over it to the next
	// free slot, right of desktop 1.
	wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
	wlr_box up = beside_frame(anchor, 0, -1);
	Desktop *three_desk = server->canvas.desktops.by_number(3);
	Desktop *two_desk = server->canvas.desktops.by_number(2);
	check(two_desk != nullptr && three_desk != nullptr,
		"desktops 2 and 3 exist before the covered-slot round");
	if (two_desk == nullptr || three_desk == nullptr) {
		return after(0);
	}
	server->canvas.abandon(two_desk->id());
	server->canvas.move_desktop(three_desk->id(), up.x, up.y);
	check(server->canvas.desktops.by_number(2) == nullptr
			&& three_desk->frame().x == up.x && three_desk->frame().y == up.y,
		"desktop 2 abandoned, desktop 3 moved onto the up slot");
	key(KEY_LEFTMETA, true);
	key(KEY_2, true);
	key(KEY_2, false);
	key(KEY_LEFTMETA, false);
	return after(700);
}

Result super_2_passed_the_slot() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	wlr_box anchor = server->canvas.desktops.by_number(1)->frame();
	check_made_desktop(2, beside_frame(anchor, 1, 0));
	shot("38-super-n-passed-slot");
	return after(0);
}

// A tab's label follows its desktop on an ordinary tab and the target's alike
Result overview_for_the_tab_labels() {
	Output *o = &output();
	o->overview.enter();
	return after(700);
}

Result desktop_3_targeted() {
	Output *o = &output();
	// Desktop 3's ordinary tab was drawn as the overview opened; now it
	// is the target, and is scaled while it is.
	Desktop *d3 = server->canvas.desktops.by_number(3);
	check(o->overview.active() && d3 != nullptr,
		"the overview opened with desktop 3 on the canvas");
	if (d3 == nullptr) {
		return after(0);
	}
	o->overview.set_target(d3->frame(), d3->id(), false);
	return after(400);
}

Result desktop_3_scaled() {
	server->canvas.settle_scale(server->canvas.desktops.by_number(3)->id(), 0.7);
	return after(400);
}

Result desktop_2_targeted() {
	Output *o = &output();
	Desktop *d2 = server->canvas.desktops.by_number(2);
	o->overview.set_target(d2->frame(), d2->id(), false);
	return after(400);
}

Result label_drawn_from_its_own_text() {
	Output *o = &output();
	Desktop *d3 = server->canvas.desktops.by_number(3);
	float scale = o->wlr->scale;
	CachedText label = desktop_label(*server, *d3, scale, false);
	uint32_t drawn = label.texture != nullptr ? label.texture->width : 0;
	check(label.texture != nullptr
			&& drawn == static_cast<uint32_t>(std::ceil(label.w * scale)),
		std::format("scaled while it was the target, desktop 3's ordinary tab draws "
			"its label from its own text ({} px for {} logical)", drawn, label.w));
	shot("39-tab-labels");
	return after(300);
}

Result scale_reset_and_overview_left() {
	Output *o = &output();
	server->canvas.settle_scale(server->canvas.desktops.by_number(3)->id(), 1.0);
	o->overview.exit();
	return after(700);
}

// Each direction switch turns its own gesture and no other
// (docs/intents/input/gestures.md).
// hops away: four fingers up still open the overview
Result hop_swipe_away_four_fingers_up() {
	Output *o = &output();
	if (auto wait = wait_for(settled() && o->interactive())) {
		return *wait;
	}
	server->gestures.set_options({.hops_away = true});
	swipe(4, 0, -300, 10);
	return after(900);
}

Result overview_opened_by_hop_swipe_away() {
	Output *o = &output();
	if (auto wait = wait_for(o->overview.active(), {.budget_ms = 3000})) {
		return *wait;
	}
	check(o->overview.active(), "with hop-swipe away, four fingers up still open the overview");
	// overview-swipe down: in the overview, four fingers up go back in.
	server->gestures.set_options({.overview_down = true});
	swipe(4, 0, -300, 10);
	return after(900);
}

Result overview_swipe_down_went_back() {
	Output *o = &output();
	if (auto wait = wait_for(!o->overview.active() && o->interactive(), {.budget_ms = 3000})) {
		return *wait;
	}
	check(!o->overview.active(), "with overview-swipe down, four fingers up went back in");
	swipe(4, 0, 300, 10);
	return after(900);
}

Result four_fingers_down_opened_it() {
	Output *o = &output();
	if (auto wait = wait_for(o->overview.active(), {.budget_ms = 3000})) {
		return *wait;
	}
	check(o->overview.active(), "and four fingers down opened the overview");
	server->gestures.set_options(GestureOptions{});
	o->overview.exit();
	return after(800);
}

// pan: Super + a scroll looks away the way the pan switch says
Result overview_left_for_the_pan_options() {
	Output *o = &output();
	if (auto wait = wait_for(settled() && o->interactive())) {
		return *wait;
	}
	scrolled_from = o->camera.current();
	key(KEY_LEFTMETA, true);
	for (int i = 0; i < 4; i++) {
		touchpad_scroll(40);
	}
	key(KEY_LEFTMETA, false);
	return after(300);
}

Result pan_natural() {
	Output *o = &output();
	double moved = o->camera.current().cy - scrolled_from.cy;
	check(moved > 100, std::format("with pan natural, Super and fingers scrolling up "
		"move the content up, the view down ({:+.0f})", moved));
	o->camera.jump(scrolled_from);
	server->conf.resolved.pan_traditional = true;
	key(KEY_LEFTMETA, true);
	for (int i = 0; i < 4; i++) {
		touchpad_scroll(40);
	}
	key(KEY_LEFTMETA, false);
	return after(300);
}

Result pan_traditional() {
	Output *o = &output();
	double moved = o->camera.current().cy - scrolled_from.cy;
	check(moved < -100, std::format("with pan traditional, the same scroll moves the "
		"view up instead ({:+.0f})", moved));
	server->conf.resolved.pan_traditional = false;
	return after(300);
}

Result plain_windows_carried_onto_a_board() {
	Output *o = &output();
	View *one = titled("one");
	View *two = titled("two");
	// A tiled desktop with two windows on its board, on a row of its own.
	int w = o->width, h = o->height;
	int x0 = 200000, y0 = 200000;
	check(one != nullptr && two != nullptr && one->mapped && two->mapped
			&& !one->minimized && !two->minimized
			&& one->mode == WindowMode::normal && two->mode == WindowMode::normal,
		"two plain windows are left to carry onto a board");
	if (one == nullptr || two == nullptr) {
		return after(0);
	}
	DesktopId boarded = server->canvas.desktops.create(wlr_box{x0, y0, w, h}).id();
	one->move(x0 + 100, y0 + 100);
	two->move(x0 + w / 2, y0 + 100);
	server->canvas.toggle_tiling(boarded);
	check(desk(boarded)->tiling() && one->seated_on(boarded) && two->seated_on(boarded)
			&& one->tiled && two->tiled,
		"tiling the desktop put both windows on its board");
	wlr_box frame = desk(boarded)->frame();
	int one_x = one->x, one_y = one->y, two_x = two->x, two_y = two->y;
	server->canvas.move_desktop(boarded, frame.x + 300, frame.y + 40);
	check(desk(boarded)->frame().x == frame.x + 300
			&& one->x == one_x + 300 && one->y == one_y + 40
			&& two->x == two_x + 300 && two->y == two_y + 40,
		"moving a tiled desktop carried both windows with its frame");
	server->canvas.abandon(boarded);
	check(server->canvas.desktops.find(boarded) == nullptr,
		"the tiled desktop is abandoned");
	check(!one->board_seat.has_value() && !two->board_seat.has_value()
			&& !one->tiled && !two->tiled,
		"abandoning a tiled desktop released its windows' board seats");
	return after(300);
}

Result allocation_counter() {
	Output *o = &output();
	View *one = titled("one");
	View *two = titled("two");
	// Frames allocate nothing (cpp-standards.md): a window drag, one
	// with a client subscribed, and a pinch, each over a hundred frames.
	if (canvas_allocations == nullptr) {
		std::println("SKIP: allocations per frame (no counter in a sanitizer build)");
		return go_to("overview left after the pinch");
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
	return after(700);
}

Result camera_on_the_window() {
	Output *o = &output();
	View *one = titled("one");
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Rect f = one->frame_rect();
	o->camera.jump(Camera{.cx = f.x + f.w / 2, .cy = f.y + f.h / 2, .zoom = 1});
	return after(300);
}

Result title_grabbed() {
	View *one = titled("one");
	grab_title(*one);
	return after(100);
}

Result window_drag_allocations() {
	View *one = titled("one");
	unsigned long long made = 0;
	unsigned long frames = 0;
	if (!run_over(made, frames)) {
		drag_tick(*one);
		return again(17);
	}
	button(BTN_LEFT, false);
	check(drag_travel >= 60, std::format("the window followed the drag ({:.0f} px)",
		drag_travel));
	check(frames >= counted_frames && made == 0,
		std::format("a window drag allocated {} times over {} frames", made, frames));
	return after(300);
}

Result subscriber_started() {
	// The same drag with a client subscribed: the window list, which a
	// drag does not change, is compared and never serialised.
	subscriber.start({std::getenv("WAYLAND_CANVAS_BIN"), "msg", "subscribe"});
	return after(600);
}

Result title_grabbed_again() {
	View *one = titled("one");
	View *three = titled("three");
	server->focus_view(three); // the subscriber is told; activate() would pan the camera
	grab_title(*one);
	return after(100);
}

Result window_drag_with_a_subscriber() {
	View *one = titled("one");
	unsigned long long made = 0;
	unsigned long frames = 0;
	if (!run_over(made, frames)) {
		drag_tick(*one);
		return again(17);
	}
	button(BTN_LEFT, false);
	subscriber.stop();
	check(subscriber.said().find("\"event\":\"windows\"")
			!= std::string_view::npos,
		"the subscriber was told the window list changed with the focus");
	check(drag_travel >= 60, std::format("the window followed the drag ({:.0f} px)",
		drag_travel));
	check(frames >= counted_frames && made == 0,
		std::format("a window drag with a subscriber allocated {} times over {} frames",
			made, frames));
	return after(300);
}

Result overview_for_the_pinch() {
	Output *o = &output();
	View *two = titled("two");
	// A pinch over a desktop of the overview, with a minimized window on
	// the shelf: tabs, shelf title and the release preview are drawn.
	server->hint.set({});
	two->set_minimized(true);
	o->overview.enter();
	return after(800);
}

Result pinch_started() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	const Desktop &pinched = server->canvas.desktops.all().front();
	o->overview.set_target(pinched.frame(), pinched.id(), false);
	Camera cam = o->camera.drawn();
	double left = cam.cx - o->width / (2.0 * cam.zoom);
	double top = cam.cy - o->height / (2.0 * cam.zoom);
	move_to((pinched.frame().x + pinched.frame().width / 2.0 - left) * cam.zoom,
		(pinched.frame().y + pinched.frame().height / 2.0 - top) * cam.zoom);
	touchpad_pinch_begin(3);
	tick = 0;
	return after(100);
}

Result pinch_allocations() {
	unsigned long long made = 0;
	unsigned long frames = 0;
	if (!run_over(made, frames)) {
		touchpad_pinch_update(2, 1.35 + 0.0004 * (tick % 100));
		tick++;
		return again(17);
	}
	bool previewing = server->gestures.pinch() != nullptr && !server->hint.text().empty();
	touchpad_pinch_end(true);
	check(previewing, "the pinch previewed what release would do");
	check(frames >= counted_frames && made == 0,
		std::format("a pinch allocated {} times over {} frames", made, frames));
	return after(300);
}

Result overview_left_after_the_pinch() {
	Output *o = &output();
	View *two = titled("two");
	if (frame_counter.link.next != nullptr) {
		wl_list_remove(&frame_counter.link);
	}
	o->overview.exit();
	two->set_minimized(false);
	return after(700);
}

Result wide_windows_launched() {
	// New windows wider than a third of the screen cascade to its right
	// edge in a few steps; nine of them go past it.
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	cascade_before = mapped_views();
	for (int i = 0; i < cascade_windows; i++) {
		spawn("foot -T cascade -w 900x200 sh -c 'exec sleep 600'");
	}
	return after(500);
}

Result wide_windows_cascaded() {
	Output *o = &output();
	if (auto wait = wait_for(mapped_views() >= cascade_before + cascade_windows)) {
		return *wait;
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
	return after(300);
}

// A zoom swipe released short of the commit travel completes when the
// fingers were fast and springs back when they were slow; one that was
// cancelled springs back whatever it travelled.
Result slow_swipe_up() {
	Output *o = &output();
	if (auto wait = wait_for(settled() && o->interactive())) {
		return *wait;
	}
	static_assert(60 < config::overview_swipe_distance * config::overview_commit);
	swipe_from = o->camera.current();
	swipe(4, 0, -50, 50); // slow: 50 units over 400 ms
	return after(900);
}

Result slow_swipe_up_sprang_back() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!o->overview.active() && same_camera(o->camera.current(), swipe_from),
		"a short, slow 4-finger swipe up sprang back to the normal view");
	swipe(4, 0, -60, 5); // fast: 60 units over 40 ms
	return after(900);
}

Result fast_swipe_up_opened_the_overview() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->overview.active() && o->camera.current().zoom < 0.5,
		"a short, fast 4-finger swipe up opened the overview");
	swipe_from = o->camera.current();
	swipe(4, 0, 50, 50);
	return after(900);
}

Result slow_swipe_down_sprang_back() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->overview.active() && same_camera(o->camera.current(), swipe_from),
		"a short, slow 4-finger swipe down sprang back to the overview");
	swipe(4, 0, 50, 4);
	return after(900);
}

Result fast_swipe_down_left_the_overview() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!o->overview.active() && o->camera.current().zoom == 1,
		"a short, fast 4-finger swipe down left the overview");
	swipe_from = o->camera.current();
	swipe(4, 0, -150, 10, true);
	return after(900);
}

Result cancelled_swipe_up_sprang_back() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!o->overview.active() && same_camera(o->camera.current(), swipe_from),
		"a cancelled 4-finger swipe up sprang back past the commit travel");
	o->overview.enter();
	return after(900);
}

Result overview_for_the_cancelled_swipe_down() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	swipe_from = o->camera.current();
	swipe(4, 0, 150, 10, true);
	return after(900);
}

Result cancelled_swipe_down_sprang_back() {
	Output *o = &output();
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(o->overview.active() && same_camera(o->camera.current(), swipe_from),
		"a cancelled 4-finger swipe down sprang back past the commit travel");
	o->overview.exit();
	return after(900);
}

// A second output at scale 2 joins the first, at scale 1
Result second_output_added() {
	check(add_output(640, 480) != nullptr, "a second output was added for the text cache");
	return after(600);
}

Result outputs_at_scales_1_and_2() {
	Output *o = &output();
	if (auto wait = wait_for(server->outputs.size() >= 2, {.budget_ms = 5000})) {
		return *wait;
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
	return after(400);
}

// both in the overview, with a minimized window on the shelf
Result both_in_the_overview() {
	Output *o = &output();
	View *two = titled("two");
	two->set_minimized(true);
	o->overview.enter();
	server->outputs.back()->overview.enter();
	return after(900);
}

Result text_cache_frames() {
	Output *o = &output();
	View *two = titled("two");
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
	return after(700);
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
	if (!begin("gestures", {"WAYLAND_CANVAS_BIN", "CANVAS_GESTURE_CLIENT"})) {
		return 2;
	}
	// Before the server starts: the launcher scans $XDG_DATA_HOME/applications first.
	install_late_entries(scratch_env("XDG_DATA_HOME", "xdg"));
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	return run(options, {
		{"devices and windows", devices_and_windows},
		{"windows mapped", windows_mapped},
		{"decoration hits", decoration_hits},
		{"maximized frame fills the screen", maximized_frame_fills_the_screen},
		{"a short swipe moves nothing", a_short_swipe_moves_nothing},
		{"no desktop that way", no_desktop_that_way},
		{"free area pinned as desktop 2", free_area_pinned_as_desktop_2},
		{"four fingers up", four_fingers_up},
		{"overview opened", overview_opened},
		{"three fingers left", three_fingers_left},
		{"hopped left", hopped_left},
		{"three fingers up", three_fingers_up},
		{"target moved to a free spot", target_moved_to_a_free_spot},
		{"hopped back down", hopped_back_down},
		{"tiled windows stay inside", tiled_windows_stay_inside},
		{"tiling switched off", tiling_switched_off},
		{"tiling back on then combined", tiling_back_on_then_combined},
		{"combined into a tiled board", combined_into_a_tiled_board},
		{"new desktop in the overview", new_desktop_in_the_overview},
		{"new desktop swiped away", new_desktop_swiped_away},
		{"desktop abandoned", desktop_abandoned},
		{"four fingers down", four_fingers_down},
		{"back in the normal view", back_in_the_normal_view},
		{"window minimized", window_minimized},
		{"shelf shot", shelf_shot},
		{"minimized window on the shelf", minimized_window_on_the_shelf},
		{"overview canvas click", overview_canvas_click},
		{"free area made by the click", free_area_made_by_the_click},
		{"keyboard jump right", keyboard_jump_right},
		{"keyboard jump landed", keyboard_jump_landed},
		{"pointer placed", pointer_placed},
		{"super pinch scaling", super_pinch_scaling},
		{"pinch released", pinch_released},
		{"desktop at 80 percent", desktop_at_80_percent},
		{"marker window opened", marker_window_opened},
		{"pointer through the camera", pointer_through_the_camera},
		{"typing into the marker", typing_into_the_marker},
		{"typing reached the marker", typing_reached_the_marker},
		{"back at 100 percent", back_at_100_percent},
		{"desktop at 125 percent", desktop_at_125_percent},
		{"overview over the scaled desktop", overview_over_the_scaled_desktop},
		{"overview tab names the scale", overview_tab_names_the_scale},
		{"swipe down toward nothing", swipe_down_toward_nothing},
		{"free view pinned", free_view_pinned},
		{"pinned desktop settled", pinned_desktop_settled},
		{"pinch without super", pinch_without_super},
		{"back to desktop 1", back_to_desktop_1},
		{"eased onto desktop 1", eased_onto_desktop_1},
		{"tiled desktop at 80 percent", tiled_desktop_at_80_percent},
		{"tiled board at 80 percent", tiled_board_at_80_percent},
		{"tiling switched off at 80 percent", tiling_switched_off_at_80_percent},
		{"maximized at 80 percent", maximized_at_80_percent},
		{"twelve settles", twelve_settles},
		{"zoom rounds ended", zoom_rounds_ended},
		{"reset to 100 percent", reset_to_100_percent},
		{"spread to 125 percent", spread_to_125_percent},
		{"overview reset", overview_reset},
		{"overview left at 100 percent", overview_left_at_100_percent},
		{"board through a zoom", board_through_a_zoom},
		{"super t at 50 percent", super_t_at_50_percent},
		{"board tiled at 50 percent", board_tiled_at_50_percent},
		{"board kept to 200 percent", board_kept_to_200_percent},
		{"late window mapped", late_window_mapped},
		{"late window joined the board", late_window_joined_the_board},
		{"reset before the floor", reset_before_the_floor},
		{"pinch to the floor", pinch_to_the_floor},
		{"scale reached the floor", scale_reached_the_floor},
		{"scroll and pinch at 25 percent", scroll_and_pinch_at_25_percent},
		{"swipe toward nothing at 25 percent", swipe_toward_nothing_at_25_percent},
		{"overview from 25 percent", overview_from_25_percent},
		{"overview opened further out", overview_opened_further_out},
		{"overview left into the free view", overview_left_into_the_free_view},
		{"free area at 100 percent", free_area_at_100_percent},
		{"a row of desktops", a_row_of_desktops},
		{"on desktop a", on_desktop_a},
		{"two quick swipes", two_quick_swipes},
		{"no desktop past c", no_desktop_past_c},
		{"long swipe hopped once", long_swipe_hopped_once},
		{"pinch with travel hopped", pinch_with_travel_hopped},
		{"nothing below the free view", nothing_below_the_free_view},
		{"up from the free view", up_from_the_free_view},
		{"b tiled", b_tiled},
		{"overview over b", overview_over_b},
		{"target moved below b", target_moved_below_b},
		{"launcher over the overview", launcher_over_the_overview},
		{"app launched into the target", app_launched_into_the_target},
		{"launcher before the late window", launcher_before_the_late_window},
		{"launcher over the overview again", launcher_over_the_overview_again},
		{"overview closed early", overview_closed_early},
		{"late window in the free spot", late_window_in_the_free_spot},
		{"maximized window launched", maximized_window_launched},
		{"maximized window filled the view", maximized_window_filled_the_view},
		{"overview over b again", overview_over_b_again},
		{"target moved below b again", target_moved_below_b_again},
		{"launcher a third time", launcher_a_third_time},
		{"late maximized window", late_maximized_window},
		{"four finger hops installed", four_finger_hops_installed},
		{"four finger hop", four_finger_hop},
		{"overview by three fingers up", overview_by_three_fingers_up},
		{"four finger hop in the overview", four_finger_hop_in_the_overview},
		{"three fingers down into b", three_fingers_down_into_b},
		{"overview over b for a fresh desktop", overview_over_b_for_a_fresh_desktop},
		{"sideways swipe abandoned it", sideways_swipe_abandoned_it},
		{"out of the overview onto b", out_of_the_overview_onto_b},
		{"swipes forwarded with the preset off", swipes_forwarded_with_the_preset_off},
		{"overview with the preset off", overview_with_the_preset_off},
		{"overview left for the pinch switch", overview_left_for_the_pinch_switch},
		{"super pinch forwarded without pinch scale", super_pinch_forwarded_without_pinch_scale},
		{"overview for the pinch switch", overview_for_the_pinch_switch},
		{"overview zoomed by the pinch", overview_zoomed_by_the_pinch},
		{"defaults restored", defaults_restored},
		{"back on b with the defaults", back_on_b_with_the_defaults},
		{"swipe hopped onto c", swipe_hopped_onto_c},
		{"hop swipe away hopped left", hop_swipe_away_hopped_left},
		{"look away", look_away},
		{"double super came back", double_super_came_back},
		{"double super after super alt right", double_super_after_super_alt_right},
		{"hop after the look away", hop_after_the_look_away},
		{"double super after the hop", double_super_after_the_hop},
		{"overview over the look away", overview_over_the_look_away},
		{"escape brought the view back", escape_brought_the_view_back},
		{"double super after the overview", double_super_after_the_overview},
		{"only desktop 1 left", only_desktop_1_left},
		{"super n from desktop 1 alone", super_n_from_desktop_1_alone},
		{"desktop 2 made", desktop_2_made},
		{"desktop 3 made", desktop_3_made},
		{"desktop 4 made", desktop_4_made},
		{"desktop 5 made", desktop_5_made},
		{"up slot covered", up_slot_covered},
		{"super 2 passed the slot", super_2_passed_the_slot},
		{"overview for the tab labels", overview_for_the_tab_labels},
		{"desktop 3 targeted", desktop_3_targeted},
		{"desktop 3 scaled", desktop_3_scaled},
		{"desktop 2 targeted", desktop_2_targeted},
		{"label drawn from its own text", label_drawn_from_its_own_text},
		{"scale reset and overview left", scale_reset_and_overview_left},
		{"hop swipe away four fingers up", hop_swipe_away_four_fingers_up},
		{"overview opened by hop swipe away", overview_opened_by_hop_swipe_away},
		{"overview swipe down went back", overview_swipe_down_went_back},
		{"four fingers down opened it", four_fingers_down_opened_it},
		{"overview left for the pan options", overview_left_for_the_pan_options},
		{"pan natural", pan_natural},
		{"pan traditional", pan_traditional},
		{"plain windows carried onto a board", plain_windows_carried_onto_a_board},
		{"allocation counter", allocation_counter},
		{"camera on the window", camera_on_the_window},
		{"title grabbed", title_grabbed},
		{"window drag allocations", window_drag_allocations},
		{"subscriber started", subscriber_started},
		{"title grabbed again", title_grabbed_again},
		{"window drag with a subscriber", window_drag_with_a_subscriber},
		{"overview for the pinch", overview_for_the_pinch},
		{"pinch started", pinch_started},
		{"pinch allocations", pinch_allocations},
		{"overview left after the pinch", overview_left_after_the_pinch},
		{"wide windows launched", wide_windows_launched},
		{"wide windows cascaded", wide_windows_cascaded},
		{"slow swipe up", slow_swipe_up},
		{"slow swipe up sprang back", slow_swipe_up_sprang_back},
		{"fast swipe up opened the overview", fast_swipe_up_opened_the_overview},
		{"slow swipe down sprang back", slow_swipe_down_sprang_back},
		{"fast swipe down left the overview", fast_swipe_down_left_the_overview},
		{"cancelled swipe up sprang back", cancelled_swipe_up_sprang_back},
		{"overview for the cancelled swipe down", overview_for_the_cancelled_swipe_down},
		{"cancelled swipe down sprang back", cancelled_swipe_down_sprang_back},
		{"second output added", second_output_added},
		{"outputs at scales 1 and 2", outputs_at_scales_1_and_2},
		{"both in the overview", both_in_the_overview},
		{"text cache frames", text_cache_frames},
	});
}
