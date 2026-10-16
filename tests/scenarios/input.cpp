// End-to-end device-level input scenario: drives the compositor through fake
// wlr_pointer / wlr_keyboard devices announced via backend new_input, i.e.
// the same path libinput takes. Writes only under $CANVAS_TMP (meson passes
// the repo's tmp/).
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
#include <ranges>
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
#include "platform/output.h"
#include "render/render.h"
#include "core/server.h"
#include "core/wlr.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false; // the run reached its end; see main()
uint32_t t = 50000;
pid_t grim_pid = -1;
wlr_pointer ptr{};
wlr_keyboard kbd{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
wlr_box saved{};
int saved_x = 0;
View *closed_view = nullptr; // compared by address only, after it is freed
View *second = nullptr; // the window beside "a" on Desktop 1
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

View *titled(const char *title) {
	for (View *v : server->views) {
		if (v->mapped && v->wlr->title && std::strcmp(v->wlr->title, title) == 0) {
			return v;
		}
	}
	return nullptr;
}

int mapped() {
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

// The board's slices, as fractions of its area.
constexpr Slice WHOLE{0, 0, 1, 1};
constexpr Slice LEFT{0, 0, 0.5, 1}, RIGHT{0.5, 0, 1, 1};
constexpr Slice TOP{0, 0, 1, 0.5}, BOTTOM{0, 0.5, 1, 1};
constexpr Slice TL{0, 0, 0.5, 0.5}, BL{0, 0.5, 0.5, 1};

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

void click(double x, double y, uint32_t code) {
	move_to(x, y);
	button(code, true);
	button(code, false);
}

void drag(double x0, double y0, double x1, double y1) {
	move_to(x0, y0);
	button(BTN_LEFT, true);
	for (int i = 1; i <= 10; i++) {
		move_to(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
	}
	button(BTN_LEFT, false);
}

void key(uint32_t code, bool pressed) {
	t += 8;
	wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

// Press mods..., tap k, release mods.
void chord(uint32_t k, std::span<const uint32_t> mods) {
	for (uint32_t m : mods) {
		key(m, true);
	}
	key(k, true);
	key(k, false);
	for (uint32_t m : std::views::reverse(mods)) {
		key(m, false);
	}
}

void swipe(uint32_t fingers, double dx, double dy) {
	t += 8;
	wlr_pointer_swipe_begin_event b = {.pointer = &ptr, .time_msec = t, .fingers = fingers};
	wl_signal_emit_mutable(&ptr.events.swipe_begin, &b);
	for (int i = 0; i < 20; i++) {
		t += 8;
		wlr_pointer_swipe_update_event u = {.pointer = &ptr, .time_msec = t,
			.fingers = fingers, .dx = dx / 20, .dy = dy / 20};
		wl_signal_emit_mutable(&ptr.events.swipe_update, &u);
	}
	t += 8;
	wlr_pointer_swipe_end_event e = {.pointer = &ptr, .time_msec = t};
	wl_signal_emit_mutable(&ptr.events.swipe_end, &e);
}

constexpr uint32_t SUPER[] = {KEY_LEFTMETA};
constexpr uint32_t SUPER_SHIFT[] = {KEY_LEFTMETA, KEY_LEFTSHIFT};
constexpr uint32_t SUPER_CTRL[] = {KEY_LEFTMETA, KEY_LEFTCTRL};
constexpr uint32_t SUPER_ALT[] = {KEY_LEFTMETA, KEY_LEFTALT};
constexpr uint32_t SUPER_CTRL_SHIFT[] = {KEY_LEFTMETA, KEY_LEFTCTRL,
	KEY_LEFTSHIFT};

int run_step() {
	Output *o = out();
	View *a = titled("a"), *b = titled("b");
	switch (step) {
	case 0:
		ptr = wlr_pointer{};
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		spawn("foot -T a sh -c 'echo a; exec sleep 600'");
		return 400;
	case 1:
		spawn("foot -T b sh -c 'echo b; exec sleep 600'");
		return 300;
	case 2:
		if (mapped() < 2 && retries++ < 100) return -100;
		check(a && b && server->focused_view == b, "two windows, newest focused");
		return 100;
	case 3: { // title bar drag moves the window
		wlr_box f = b->frame_box();
		saved_x = b->x;
		int saved_y = b->y;
		drag(f.x + 80, f.y + 16, f.x + 180, f.y + 66);
		check(b->x == saved_x + 100 && b->y == saved_y + 50,
			std::format("title bar drag moved the window by (100,50) -> ({},{})", b->x - saved_x,
				b->y - saved_y));
		return 200;
	}
	case 4: { // double-click title toggles maximize
		wlr_box f = b->frame_box();
		move_to(f.x + 80, f.y + 16);
		click(f.x + 80, f.y + 16, BTN_LEFT);
		click(f.x + 80, f.y + 16, BTN_LEFT);
		check(b->maximized, "double-click on the title maximized");
		return 400;
	}
	case 5: { // the maximize button now shows restore; click it
		wlr_box f = b->frame_box();
		shot("maximized");
		click(f.x + f.width - config::button_width * 1.5, f.y + 16, BTN_LEFT);
		check(!b->maximized, "restore button restored the window");
		return 400;
	}
	case 6: { // resize from the top-left corner: right/bottom edges must stay put
		saved = b->content_box();
		wlr_box f = b->frame_box();
		drag(f.x - 3, f.y - 3, f.x - 83, f.y - 53);
		return 400;
	}
	case 7: {
		wlr_box c = b->content_box();
		// foot snaps to whole character cells, so allow up to one cell short.
		check(c.width >= saved.width + 60 && c.height >= saved.height + 30 &&
			c.x + c.width == saved.x + saved.width && c.y + c.height == saved.y + saved.height,
			std::format("top-left corner drag resized {}x{} -> {}x{} with the bottom-right corner fixed",
				saved.width, saved.height, c.width, c.height));
		return 100;
	}
	case 8: { // minimize button
		wlr_box f = b->frame_box();
		move_to(f.x + f.width - config::button_width * 2.5, f.y + 16);
		check(b->deco.hover == DecoPart::minimize, "hovering the minimize button highlights it");
		shot("hover-minimize");
		return 300;
	}
	case 9: {
		wlr_box f = b->frame_box();
		click(f.x + f.width - config::button_width * 2.5, f.y + 16, BTN_LEFT);
		check(b->minimized && server->focused_view == a, "minimize button hid it, focus moved to a");
		chord(KEY_M, SUPER_SHIFT);
		check(!b->minimized && server->focused_view == b, "Super+Shift+M restored it");
		return 200;
	}
	case 10: { // close button
		wlr_box f = b->frame_box();
		move_to(f.x + f.width - config::button_width * 0.5, f.y + 16);
		shot("hover-close");
		return 300;
	}
	case 11: {
		wlr_box f = b->frame_box();
		click(f.x + 60, f.y + 16, BTN_LEFT); // a title click: b is now the double-click candidate
		closed_view = b;
		click(f.x + f.width - config::button_width * 0.5, f.y + 16, BTN_LEFT);
		return 600;
	}
	case 12:
		check(mapped() == 1 && titled("b") == nullptr, "close button closed the window");
		check(server->input.last_title_view != closed_view,
			"a closed window is no longer the double-click candidate");
		chord(KEY_ENTER, SUPER);
		return 300;
	case 13:
		if (mapped() < 2 && retries++ < 100) return -100;
		check(mapped() == 2, "Super+Return launched a terminal");
		chord(KEY_TAB, SUPER);
		return 500;
	case 14:
		check(o->overview.active(), "Super+Tab opened the overview");
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		return 500;
	case 15:
		check(!o->overview.active() && o->camera.current().zoom == 1,
			"Escape went back");
		for (int i = 0; i < 3; i++) chord(KEY_RIGHT, SUPER_ALT);
		return 600;
	case 16:
		chord(KEY_D, SUPER_CTRL);
		check(server->canvas.desktops.size() == 2,
			"Super+Alt+Right x3 then Super+Ctrl+D pinned Desktop 2");
		chord(KEY_LEFT, SUPER_CTRL);
		return 600;
	case 17: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		check(d1 && o->desktop_showing() == d1, "Super+Ctrl+Left jumped back to Desktop 1");
		move_to(640, 360);
		swipe(3, 300, 0); // fingers right: hop onto the desktop on the right
		return 1200;
	}
	case 18: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		check(d2 && o->desktop_showing() == d2
			&& o->camera.current().cx == d2->frame().x + d2->frame().width / 2.0,
			std::format("3-finger swipe events from the device hopped onto Desktop 2 ({:.0f})",
				o->camera.current().cx));
		swipe(4, 0, -300);
		return 600;
	}
	case 19:
		check(o->overview.active(), "4-finger swipe events opened the overview");
		shot("overview");
		return 300;
	case 20: { // right-click on empty canvas below the desktops: new desktop
		Desktop *d1 = server->canvas.desktops.by_number(1);
		Point p = o->canvas_to_local(d1->frame().x + d1->frame().width / 2.0,
			d1->frame().y + d1->frame().height * 1.9);
		click(p.x, p.y, BTN_RIGHT);
		check(server->canvas.desktops.size() == 3,
			"right-click on empty canvas added Desktop 3");
		return 400;
	}
	case 21: { // drag Desktop 3 by its tab onto Desktop 2: combine
		shot("three-desktops");
		Desktop *d2 = server->canvas.desktops.by_number(2);
		Desktop *d3 = server->canvas.desktops.by_number(3);
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d3);
		Point tx = o->canvas_to_local(d2->frame().x, d2->frame().y);
		Point sx = o->canvas_to_local(d3->frame().x, d3->frame().y);
		drag(layout->tab.x + 30, layout->tab.y + 14, layout->tab.x + 30 + (tx.x - sx.x),
			layout->tab.y + 14 + (tx.y - sx.y));
		check(server->canvas.desktops.size() == 2
			&& server->canvas.desktops.by_number(3) == nullptr,
			"dropping a frame onto another combined them");
		return 400;
	}
	case 22: { // drag Desktop 2's frame over the windows of Desktop 1 (frames move, windows don't)
		Desktop *d2 = server->canvas.desktops.by_number(2);
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d2);
		int ax = a->x;
		wlr_box before = d2->frame();
		drag(layout->tab.x + 30, layout->tab.y + 14, layout->tab.x + 30, layout->tab.y + 14 + 40);
		check(d2->frame().y > before.y && a->x == ax, "dragging a frame moves only the frame");
		return 300;
	}
	case 23: { // click the Tile switch on Desktop 1's tab: tiling on
		Desktop *d1 = server->canvas.desktops.by_number(1);
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d1);
		click(layout->tile_button.x + layout->tile_button.width / 2.0,
			layout->tile_button.y + layout->tile_button.height / 2.0, BTN_LEFT);
		return 500; // clients commit the new sizes asynchronously
	}
	case 24: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		for (View *v : server->views) {
			if (v->mapped && v != a) {
				second = v;
			}
		}
		check(d1 && second && d1->tiling(), "the tab's Tile switch switched tiling on");
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		View *right = left == a ? second : a;
		check(a && second && on_slice(left, slice_frame_of(*d1, LEFT))
				&& on_slice(right, slice_frame_of(*d1, RIGHT)),
			"the desktop's two windows took the board's halves");
		check(a->tiled && second->tiled, "tiled windows are told they are tiled");
		shot("tiled-halves");
		return 300;
	}
	case 25: { // leave the overview onto the empty Desktop 2
		Desktop *d2 = server->canvas.desktops.by_number(2);
		Point c = d2->centre();
		o->overview.target_point(c.x, c.y);
		o->overview.exit();
		return 700;
	}
	case 26: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		if ((o->overview.active() || o->desktop_showing() != d2) && retries++ < 100) {
			return -100;
		}
		check(!o->overview.active() && o->desktop_showing() == d2,
			"left the overview onto Desktop 2");
		// An empty desktop takes the switch too; its first window fills it.
		chord(KEY_T, SUPER);
		check(d2->tiling() && server->hint.text == "Tiling on for Desktop 2",
			"Super+T switched tiling on for the empty desktop");
		spawn("foot -T c sh -c 'echo c; exec sleep 600'");
		return 400;
	}
	case 27:
		if (mapped() < 3 && retries++ < 100) {
			return -100;
		}
		return 500; // the client commits the slice size
	case 28: { // Super+Left takes the whole board's window to the left half
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c");
		check(c && c->board == d2->id() && c->tiled
				&& on_slice(c, slice_frame_of(*d2, WHOLE)),
			"the first window on the empty tiled desktop filled it");
		shot("tiled-first-fills");
		chord(KEY_LEFT, SUPER);
		return 500;
	}
	case 29:
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), LEFT)),
			"Super+Left took the window to the left half");
		chord(KEY_DOWN, SUPER);
		return 500;
	case 30: // Left then Down reaches the bottom-left quarter
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), BL)),
			"Super+Left then Super+Down took the window to the bottom-left quarter");
		shot("slice-bl");
		chord(KEY_UP, SUPER); // the opposite key widens back to the left half
		return 500;
	case 31:
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), LEFT)),
			"Super+Up widened the bottom-left quarter back to the left half");
		chord(KEY_RIGHT, SUPER); // and on to the whole board
		return 500;
	case 32:
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), WHOLE)),
			"Super+Right widened the left half back to the whole board");
		chord(KEY_DOWN, SUPER); // the other order: Down first, then Left
		return 500;
	case 33:
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), BOTTOM)),
			"Super+Down took the whole board's window to the bottom half");
		chord(KEY_LEFT, SUPER);
		return 500;
	case 34: // both orders land on the bottom-left quarter
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), BL)),
			"Super+Down then Super+Left also took the window to the bottom-left quarter");
		// A second window takes the largest free slice: the top half wins the
		// tie with the right one by reading order.
		spawn("foot -T d sh -c 'echo d; exec sleep 600'");
		return 400;
	case 35: {
		if (mapped() < 4 && retries++ < 100) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d");
		check(d && on_slice(d, slice_frame_of(*d2, TOP))
				&& on_slice(c, slice_frame_of(*d2, BL)),
			"the new window took the largest free slice, the top half");
		// Focus c through its title bar and grow it over d. The frame is in
		// canvas coordinates; the pointer takes output-local ones.
		wlr_box f = c->frame_box();
		Point title = o->canvas_to_local(f.x + 80, f.y + 16);
		click(title.x, title.y, BTN_LEFT);
		return 300;
	}
	case 36:
		check(server->focused_view == titled("c"), "the clicked window is focused");
		chord(KEY_UP, SUPER); // bottom-left quarter widens to the left half, over d
		return 500;
	case 37: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d");
		check(on_slice(c, slice_frame_of(*d2, LEFT))
				&& on_slice(d, slice_frame_of(*d2, RIGHT)),
			"the grown slice pushed the covered window into the free space");
		check(!c->frame_rect().intersects(d->frame_rect()), "tiled windows do not overlap");
		shot("slice-push");
		// A third window with no free space left: it halves a held slice.
		spawn("foot -T e sh -c 'echo e; exec sleep 600'");
		return 400;
	}
	case 38: {
		if (mapped() < 5 && retries++ < 100) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *e = titled("e");
		check(e && on_slice(e, slice_frame_of(*d2, BL))
				&& on_slice(c, slice_frame_of(*d2, TL))
				&& on_slice(d, slice_frame_of(*d2, RIGHT)),
			"with no free slice left, the new window halved the left half");
		shot("slice-halved");
		titled("e")->close(); // its slice frees up
		return 600;
	}
	case 39: {
		if (mapped() > 4 && retries++ < 100) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d");
		check(titled("e") == nullptr && on_slice(c, slice_frame_of(*d2, TL))
				&& on_slice(d, slice_frame_of(*d2, RIGHT)),
			"the closed window's slice stayed free, the others kept theirs");
		spawn("foot -T f sh -c 'echo f; exec sleep 600'");
		return 400;
	}
	case 40: {
		if (mapped() < 5 && retries++ < 100) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(f && on_slice(f, slice_frame_of(*d2, BL))
				&& on_slice(c, slice_frame_of(*d2, TL))
				&& on_slice(d, slice_frame_of(*d2, RIGHT)),
			"the next window took the closed window's slice");
		shot("slice-refilled");
		// Drag c onto d's slice: they swap. Canvas coordinates in, the
		// pointer takes output-local ones.
		wlr_box fc = c->frame_box(), fd = d->frame_box();
		Point from = o->canvas_to_local(fc.x + 80, fc.y + 16);
		Point to = o->canvas_to_local(
			fd.x + fd.width / 2.0 - fc.width / 2.0 + 80,
			fd.y + fd.height / 2.0 - fc.height / 2.0 + 16);
		drag(from.x, from.y, to.x, to.y);
		return 500;
	}
	case 41: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(on_slice(c, slice_frame_of(*d2, RIGHT))
				&& on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"dragging a window onto the other's slice swapped them");
		check(c->tiled && d->tiled && f->tiled, "both windows stayed tiled");
		shot("slice-swap");
		// Double-click c's title: maximize keeps the slice reserved.
		wlr_box frame = c->frame_box();
		Point title = o->canvas_to_local(frame.x + 80, frame.y + 16);
		click(title.x, title.y, BTN_LEFT);
		click(title.x, title.y, BTN_LEFT);
		return 500;
	}
	case 42: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(c && c->maximized && !c->tiled && c->board == d2->id()
				&& c->slice == RIGHT,
			"maximize left the board but kept the window's slice");
		check(on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"maximize moved no other window: no re-layout happens anymore");
		shot("slice-maximized");
		wlr_box frame = c->frame_box();
		Point restore = o->canvas_to_local(
			frame.x + frame.width - config::button_width * 1.5, frame.y + 16);
		click(restore.x, restore.y, BTN_LEFT);
		return 500;
	}
	case 43: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(!c->maximized && c->tiled && c->board == d2->id()
				&& on_slice(c, slice_frame_of(*d2, RIGHT))
				&& on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"leaving maximize put the window back on its reserved slice");
		// Minimize: the slice frees up but is remembered.
		wlr_box frame = c->frame_box();
		Point mini = o->canvas_to_local(
			frame.x + frame.width - config::button_width * 2.5, frame.y + 16);
		click(mini.x, mini.y, BTN_LEFT);
		return 400;
	}
	case 44: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(c && c->minimized && !c->tiled && c->board == d2->id()
				&& c->slice == RIGHT,
			"minimize freed the slice but kept it remembered");
		check(on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"minimize moved no other window either");
		chord(KEY_M, SUPER_SHIFT); // restore it
		return 500;
	}
	case 45: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(!c->minimized && c->tiled && on_slice(c, slice_frame_of(*d2, RIGHT))
				&& on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"the restored window took its own slice back, still free");
		// Hop right off Desktop 2 onto a free view, carrying c along.
		swipe(3, 900, 0);
		return 1400;
	}
	case 46: {
		if (o->desktop_showing() != nullptr && retries++ < 100) {
			return -100;
		}
		check(o->desktop_showing() == nullptr, "3-finger swipe hopped onto a free view");
		chord(KEY_RIGHT, SUPER_CTRL_SHIFT); // move the focused window here
		return 600;
	}
	case 47: {
		View *c = titled("c");
		check(c && !c->tiled && !c->board.has_value()
				&& server->canvas.desktop_of(*c) == nullptr,
			"moving out freed the window's slice and left the board");
		chord(KEY_UP, SUPER); // a floating window still maximizes
		return 500;
	}
	case 48: {
		View *c = titled("c");
		Rect fr = c->frame_rect(), vp = o->viewport();
		check(c->maximized && std::fabs(fr.x - vp.x) < 1 && std::fabs(fr.y - vp.y) < 1
				&& std::fabs(fr.w - vp.w) < 1 && std::fabs(fr.h - vp.h) < 1,
			"Super+Up still toggles maximize on a floating desktop");
		shot("floating-maximized");
		chord(KEY_UP, SUPER); // and back
		return 500;
	}
	case 49: {
		View *c = titled("c");
		check(c && !c->maximized, "Super+Up restored the floating window");
		// Pin this free view as a desktop with tiling on: c joins it.
		chord(KEY_T, SUPER);
		Desktop *d3 = server->canvas.desktops.by_number(3);
		check(d3 && d3->tiling() && o->desktop_showing() == d3,
			"Super+T pinned the free view as a desktop with tiling on");
		check(server->hint.text == "Tiling on for Desktop 3",
			"the hint names the new tiled desktop");
		return 400;
	}
	case 50: {
		Desktop *d3 = server->canvas.desktops.by_number(3);
		View *c = titled("c");
		check(c && c->board == d3->id() && on_slice(c, slice_frame_of(*d3, WHOLE)),
			"the window standing there joined the new desktop's board");
		// Back to Desktop 1 (via 2): its tiling survived, boxes unmoved.
		chord(KEY_LEFT, SUPER_CTRL);
		return 800;
	}
	case 51: {
		if (o->desktop_showing() != server->canvas.desktops.by_number(2)
				&& retries++ < 100) {
			return -100;
		}
		chord(KEY_LEFT, SUPER_CTRL);
		return 800;
	}
	case 52: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		if (o->desktop_showing() != d1 && retries++ < 100) {
			return -100;
		}
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		View *right = left == a ? second : a;
		check(d1 && o->desktop_showing() == d1 && d1->tiling(),
			"back on Desktop 1, tiling still on");
		check(on_slice(left, slice_frame_of(*d1, LEFT))
				&& on_slice(right, slice_frame_of(*d1, RIGHT)),
			"and its two windows kept their slices all along");
		// `t` in the overview switches tiling off: boxes stay, boards drop.
		chord(KEY_TAB, SUPER);
		return 700;
	}
	case 53: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		if (!o->overview.active() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active() && o->overview.target().desktop == d1->id(),
			"the overview targets Desktop 1");
		key(KEY_T, true);
		key(KEY_T, false);
		return 500;
	}
	case 54: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		View *right = left == a ? second : a;
		Rect fl = left->frame_rect(), fr = right->frame_rect();
		check(d1 && !d1->tiling() && !a->tiled && !second->tiled
				&& !a->board.has_value() && !second->board.has_value(),
			"`t` in the overview switched tiling off, boards dropped");
		check(on_slice(left, slice_frame_of(*d1, LEFT))
				&& on_slice(right, slice_frame_of(*d1, RIGHT)),
			"switching off left the windows where the board had them");
		check(fl.x == left->frame_rect().x && fr.x == right->frame_rect().x,
			"not a pixel moved when the board went away");
		shot("tiled-off-kept");
		// The tab's Tile switch brings it back: re-placed as they stood.
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d1);
		click(layout->tile_button.x + layout->tile_button.width / 2.0,
			layout->tile_button.y + layout->tile_button.height / 2.0, BTN_LEFT);
		return 500;
	}
	case 55: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		View *right = left == a ? second : a;
		check(d1 && d1->tiling() && a->tiled && second->tiled
				&& on_slice(left, slice_frame_of(*d1, LEFT))
				&& on_slice(right, slice_frame_of(*d1, RIGHT)),
			"the tab's Tile switch laid the standing windows back on the same slices");
		// And off again, for the combine below.
		key(KEY_T, true);
		key(KEY_T, false);
		return 500;
	}
	case 56: { // combine the tiled Desktop 2 into the untiled Desktop 1
		Desktop *d1 = server->canvas.desktops.by_number(1);
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *d = titled("d"), *f = titled("f");
		Rect d0 = d->frame_rect(), f0 = f->frame_rect();
		check(d1 && d2 && !d1->tiling() && d2->tiling(),
			"Desktop 1 untiled, Desktop 2 still tiled, before the combine");
		int dx = d1->frame().x - d2->frame().x;
		int dy = d1->frame().y - d2->frame().y;
		server->canvas.combine(d1->id(), d2->id(), d2->frame());
		check(server->canvas.desktops.by_number(2) == nullptr,
			"the tiled desktop combined into the untiled one");
		check(d->frame_rect().x == d0.x + dx && d->frame_rect().y == d0.y + dy
				&& f->frame_rect().x == f0.x + dx && f->frame_rect().y == f0.y + dy,
			"its windows moved with the translation, keeping their layout");
		check(!d->tiled && !f->tiled
				&& server->canvas.desktop_of(*d) == d1,
			"no longer tiled, they belong to the target desktop");
		shot("combined-untiled");
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
	t += 1000; // real time passes between steps (double-click detection)
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
	shot_dir = std::format("{}/scenarios/input", root);
	mkdir_p(shot_dir);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	setenv("TERMINAL", "foot", 1);
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
