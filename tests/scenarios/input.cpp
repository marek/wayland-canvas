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
View *dragged = nullptr; // the window dragged onto the other's slot
View *closed_view = nullptr; // compared by address only, after it is freed
View *second = nullptr; // the window beside "a" on Desktop 1
Rect box_a{}, box_second{}; // Desktop 1's grid boxes, captured for later checks
Rect box_d{}; // the pinned desktop's window box, for the abandon check
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

// The expected grid cells of a desktop's frame for n windows.
std::vector<Rect> grid_cells(const Desktop &desktop, std::size_t n) {
	const wlr_box &frame = desktop.frame();
	return tile_layout(
		Rect{double(frame.x), double(frame.y), double(frame.width), double(frame.height)},
		n, config::tile_gap);
}

// The view's frame sits in one of the cells (clients snap to their own
// units, so a side may sit a few px short of the cell).
bool fits_cell(View *view, const std::vector<Rect> &cells) {
	Rect f = view->frame_rect();
	for (const Rect &cell : cells) {
		if (std::fabs(f.x - cell.x) < 2 && std::fabs(f.y - cell.y) < 2
				&& cell.w - f.w < 20 && cell.h - f.h < 24) {
			return true;
		}
	}
	return false;
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
		double cx0 = o->camera.current().cx;
		swipe(3, 300, 0); // fingers right: canvas follows, camera moves left
		check(o->camera.current().cx < cx0 - 250,
			std::format("3-finger swipe events from the device panned ({:.0f})",
				o->camera.current().cx - cx0));
		return 1200;
	}
	case 18:
		swipe(4, 0, -300);
		return 600;
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
		check(a && second && !a->frame_rect().intersects(second->frame_rect()),
			"the desktop's windows tiled into the grid");
		check(a->tiled && second->tiled, "tiled windows are told they are tiled");
		shot("tiled-on");
		return 300;
	}
	case 25: { // the Tile switch again: tiling off
		Desktop *d1 = server->canvas.desktops.by_number(1);
		box_a = a->frame_rect();
		box_second = second->frame_rect();
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d1);
		click(layout->tile_button.x + layout->tile_button.width / 2.0,
			layout->tile_button.y + layout->tile_button.height / 2.0, BTN_LEFT);
		return 400;
	}
	case 26: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		Rect fa = a->frame_rect(), fs = second->frame_rect();
		check(d1 && !d1->tiling(), "the Tile switch switched tiling off");
		check(fa.x == box_a.x && fa.y == box_a.y, std::format("window a stayed"
			" where the grid left it ({:.0f},{:.0f}, was {:.0f},{:.0f}; untiled it"
			" re-fits its own size)", fa.x, fa.y, box_a.x, box_a.y));
		check(fs.x == box_second.x && fs.y == box_second.y, std::format("the other"
			" window stayed where the grid left it ({:.0f},{:.0f}, was {:.0f},{:.0f})",
			fs.x, fs.y, box_second.x, box_second.y));
		check(!a->tiled && !second->tiled,
			"switching off told the windows they are no longer tiled");
		shot("tiled-off");
		return 300;
	}
	case 27: { // `t` in the overview targets the desktop: tiling back on
		Desktop *d1 = server->canvas.desktops.by_number(1);
		o->overview.set_target(d1->frame(), d1->id(), false);
		key(KEY_T, true);
		key(KEY_T, false);
		check(d1->tiling(), "`t` in the overview switched tiling back on");
		return 300;
	}
	case 28: // leave the overview onto Desktop 1
		o->overview.exit();
		return 700;
	case 29: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		check(!o->overview.active() && o->camera.current().zoom == 1,
			"left the overview onto Desktop 1");
		Camera want = camera_for_box(d1->frame());
		check(std::fabs(o->camera.current().cx - want.cx) < 1
			&& std::fabs(o->camera.current().cy - want.cy) < 1, "the camera rests on Desktop 1");
		check(d1->tiling(), "tiling stayed on through the travel");
		box_a = a->frame_rect();
		box_second = second->frame_rect();
		shot("tiled-two"); // the 2-window grid before the next window joins
		spawn("foot -T c sh -c 'echo c; exec sleep 600'");
		return 400;
	}
	case 30:
		if (mapped() < 3 && retries++ < 100) {
			return -100;
		}
		check(mapped() == 3, "a window opened while the tiled desktop was shown");
		return 500; // the grid resize needs the client commits
	case 31: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		View *c = titled("c");
		std::vector<Rect> cells = grid_cells(*d1, 3);
		check(c && server->canvas.desktop_of(*c) == d1 && fits_cell(c, cells),
			"the new window joined the grid in a slot");
		check(fits_cell(a, cells) && fits_cell(second, cells),
			"the others re-flowed around it");
		check(a->frame_rect() != box_a || second->frame_rect() != box_second,
			"joining re-flowed the whole grid");
		shot("tiled-new-window");
		return 300;
	}
	case 32: // close one: the grid re-flows
		titled("c")->close();
		return 600;
	case 33: {
		if (mapped() < 2 && retries++ < 100) {
			return -100;
		}
		Desktop *d1 = server->canvas.desktops.by_number(1);
		check(mapped() == 2 && titled("c") == nullptr, "the window closed");
		check(fits_cell(a, grid_cells(*d1, 2)) && fits_cell(second, grid_cells(*d1, 2)),
			"closing one re-flowed the grid");
		return 300;
	}
	case 34: { // minimize via the title bar button: out of the grid
		wlr_box f = a->frame_box();
		click(f.x + f.width - config::button_width * 2.5, f.y + 16, BTN_LEFT);
		return 400;
	}
	case 35: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		check(a->minimized && !a->tiled, "minimize took the window out of the grid");
		check(fits_cell(second, grid_cells(*d1, 1)),
			"the remaining window re-flowed to fill the desktop");
		chord(KEY_M, SUPER_SHIFT); // restore it
		return 500;
	}
	case 36: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		std::vector<Rect> cells = grid_cells(*d1, 2);
		check(!a->minimized && fits_cell(a, cells) && fits_cell(second, cells),
			"restoring it put it back in the grid");
		return 300;
	}
	case 37: // maximize: out of the grid while in that state
		chord(KEY_UP, SUPER);
		return 500;
	case 38: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		check(a->maximized && !a->tiled, "maximize took the window out of the grid");
		check(fits_cell(second, grid_cells(*d1, 1)),
			"the freed window re-flowed to fill the desktop");
		shot("tiled-maximized-out");
		return 300;
	}
	case 39: // leave maximize: back in the grid
		chord(KEY_UP, SUPER);
		return 500;
	case 40: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		std::vector<Rect> cells = grid_cells(*d1, 2);
		check(!a->maximized && a->tiled && fits_cell(a, cells) && fits_cell(second, cells),
			"leaving maximize put the window back in the grid");
		return 300;
	}
	case 41: // fullscreen: out of the grid while in that state
		chord(KEY_F, SUPER);
		return 500;
	case 42: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		check(a->fullscreen && !a->tiled, "fullscreen took the window out of the grid");
		check(fits_cell(second, grid_cells(*d1, 1)),
			"the freed window re-flowed to fill the desktop");
		return 300;
	}
	case 43: // leave fullscreen: back in the grid
		chord(KEY_F, SUPER);
		return 500;
	case 44: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		std::vector<Rect> cells = grid_cells(*d1, 2);
		check(!a->fullscreen && a->tiled, "leaving fullscreen tiled the window again");
		check(fits_cell(a, cells) && fits_cell(second, cells),
			std::format("leaving fullscreen put the window back in the grid"
				" (a {:.0f},{:.0f} {:.0f}x{:.0f}; other {:.0f},{:.0f} {:.0f}x{:.0f})",
				a->frame_rect().x, a->frame_rect().y, a->frame_rect().w, a->frame_rect().h,
				second->frame_rect().x, second->frame_rect().y, second->frame_rect().w,
				second->frame_rect().h));
		return 300;
	}
	case 45: { // drag a window onto the other's slot: they reorder
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		dragged = left;
		View *right = left == a ? second : a;
		wlr_box lf = left->frame_box(), rf = right->frame_box();
		// Drop so the dragged window's centre lands inside the other's cell,
		// right of its centre; the grab sits (80, 16) inside the frame.
		double drop_x = rf.x + rf.width / 2.0 + 40 - lf.width / 2.0 + 80;
		double drop_y = rf.y + rf.height / 2.0 - lf.height / 2.0 + 16;
		drag(lf.x + 80, lf.y + 16, drop_x, drop_y);
		return 500;
	}
	case 46: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		View *other = dragged == a ? second : a;
		std::vector<Rect> cells = grid_cells(*d1, 2);
		Rect fd = dragged->frame_rect(), fo = other->frame_rect();
		check(fd.x > fo.x, std::format("dragging a window onto the other's slot"
			" reordered the grid (the dragged {} now at x={:.0f}, the other at x={:.0f})",
			dragged == a ? "a" : "other", fd.x, fo.x));
		check(dragged->tiled && fits_cell(dragged, cells) && fits_cell(other, cells),
			"the dragged window went back into the grid");
		shot("tiled-reordered");
		return 300;
	}
	case 47: // pan to a free view, then Super+T: pinned with tiling on
		swipe(3, 900, 0);
		return 1600;
	case 48: {
		check(o->desktop_showing() == nullptr, "panned onto a free view");
		chord(KEY_T, SUPER);
		Desktop *d3 = server->canvas.desktops.by_number(3);
		check(d3 && d3->tiling() && o->desktop_showing() == d3,
			"Super+T pinned the free view as a desktop with tiling on");
		check(server->hint.text == "Tiling on for Desktop 3",
			"the hint names the new tiled desktop");
		spawn("foot -T d sh -c 'echo d; exec sleep 600'");
		return 400;
	}
	case 49: {
		if (mapped() < 3 && retries++ < 100) {
			return -100;
		}
		Desktop *d3 = server->canvas.desktops.by_number(3);
		View *dv = titled("d");
		check(dv && server->canvas.desktop_of(*dv) == d3,
			"the first window on the empty tiled desktop joined it");
		check(fits_cell(dv, grid_cells(*d3, 1)),
			"the first window filled the desktop's area");
		box_d = dv->frame_rect();
		shot("tiled-first-fills");
		return 400;
	}
	case 50: // abandon the tiled desktop: its window stays, no longer tiled
		chord(KEY_F4, SUPER_CTRL);
		return 400;
	case 51: {
		View *dv = titled("d");
		Rect fd = dv->frame_rect();
		check(server->canvas.desktops.by_number(3) == nullptr,
			"Super+Ctrl+F4 abandoned the tiled desktop");
		check(dv && !dv->tiled && server->canvas.desktop_of(*dv) == nullptr,
			"its window was told it is no longer tiled and belongs to no desktop");
		check(fd.x == box_d.x && fd.y == box_d.y, std::format("its window stayed"
			" where the grid left it ({:.0f},{:.0f}, was {:.0f},{:.0f})",
			fd.x, fd.y, box_d.x, box_d.y));
		shot("abandoned-tiled");
		return 300;
	}
	case 52: { // combine the tiled Desktop 1 into the untiled Desktop 2
		Desktop *d1 = server->canvas.desktops.by_number(1);
		Desktop *d2 = server->canvas.desktops.by_number(2);
		Rect a0 = a->frame_rect(), s0 = second->frame_rect();
		int dx = d2->frame().x - d1->frame().x;
		int dy = d2->frame().y - d1->frame().y;
		server->canvas.combine(d2->id(), d1->id(), d1->frame());
		check(server->canvas.desktops.by_number(1) == nullptr,
			"the tiled desktop combined into the untiled one");
		check(a->frame_rect().x == a0.x + dx && a->frame_rect().y == a0.y + dy
			&& second->frame_rect().x == s0.x + dx && second->frame_rect().y == s0.y + dy,
			"its windows moved with the translation, keeping their layout");
		check(!a->tiled && !second->tiled
			&& server->canvas.desktop_of(*a) == d2,
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
