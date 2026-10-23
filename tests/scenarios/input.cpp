// End-to-end device-level input scenario: drives the compositor through fake
// wlr_pointer / wlr_keyboard devices announced via backend new_input, i.e.
// the same path libinput takes. Writes only under $CANVAS_TMP (meson passes
// the repo's tmp/).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
#include "canvas/overview_chrome.h"
#include "core/box.h"
#include "core/config.h"
#include "core/process.h"
#include "core/server.h"
#include "core/wlr.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/control.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "platform/output.h"
#include "windows/view.h"

using namespace harness;

namespace {

Child x11; // the scenario's X11 client: its stdout says what keys reach it
wlr_box saved{};
View *closed_view = nullptr; // compared by address only, after it is freed
View *second = nullptr; // the window beside "a" on Desktop 1
View *held_view = nullptr; // the window a held button was pressed on
std::optional<DesktopId> right_made; // the desktop a right click made in the overview

std::string canvas_conf; // this run's scratch canvas.conf

// The board's slices, as fractions of its area.
constexpr Slice WHOLE{0, 0, 1, 1};
constexpr Slice LEFT{0, 0, 0.5, 1}, RIGHT{0.5, 0, 1, 1};
constexpr Slice TOP{0, 0, 1, 0.5}, BOTTOM{0, 0.5, 1, 1};
constexpr Slice TL{0, 0, 0.5, 0.5}, BL{0, 0.5, 0.5, 1};

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

Desktop *desk(int number) {
	return server->canvas.desktops.by_number(number);
}

int mapped_count() {
	int n = 0;
	for (View *v : server->views) {
		n += v->mapped;
	}
	return n;
}

// Desktop 1's two windows, the one further left first.
std::pair<View *, View *> desktop_1_pair() {
	View *a = titled("a");
	return a->frame_rect().x <= second->frame_rect().x
		? std::pair{a, second} : std::pair{second, a};
}

std::string in_scratch(std::string_view name) {
	return std::format("{}/{}", scratch(), name);
}

void spawn_window(std::string_view title) {
	spawn(std::format("foot -T {0} sh -c 'echo {0}; exec sleep 600'", title).c_str());
}

// A touchpad swipe as the device reports it: a begin, twenty updates and an
// end, through the pointer device's own events.
void device_swipe(int fingers, double dx, double dy) {
	touchpad_swipe_begin(fingers);
	for (int i = 0; i < 20; i++) {
		touchpad_swipe_update(fingers, dx / 20, dy / 20);
	}
	touchpad_swipe_end();
}

View *x11_view() {
	for (View *view : server->views) {
		if (view->mapped && view->as_x11() != nullptr) {
			return view;
		}
	}
	return nullptr;
}

// --------------------------------------------------- canvas.conf helpers

// Saves `text` over canvas.conf the way an editor does: written beside,
// renamed over, so the inotify reload sees one whole file.
bool save_conf(const std::string &text) {
	std::string beside = canvas_conf + ".hand";
	return write_file(beside, text) && rename(beside.c_str(), canvas_conf.c_str()) == 0;
}

// True when the running session's resolved keys for `action` (global
// scope) hold `chord`: the observable of a reload that applied.
bool holds_chord(std::string_view action, const settings::Chord &chord) {
	const std::vector<settings::ActionDef> &registry = settings::registry();
	for (std::size_t i = 0; i < registry.size(); i++) {
		if (registry[i].scope != settings::Scope::global
				|| registry[i].name != action) {
			continue;
		}
		for (settings::Chord held : server->conf.resolved.keys[i]) {
			if (held == chord) {
				return true;
			}
		}
	}
	return false;
}

// A client that sends `request` on the control socket and keeps listening
// through `quiet_polls` quiet polls; its answers are read from `out`. A
// shortcut capture's answer can wait for keys the scenario presses only
// after the client is forked, so it gets the long quiet count.
void ask_socket(std::string_view request, std::string_view out, int quiet_polls) {
	fork_control_client(request, out, {.quiet_polls = quiet_polls});
}

// ------------------------------------------------------------- the steps

Result devices_and_window_a() {
	attach_pointer();
	attach_keyboard();
	spawn_window("a");
	return after(400);
}

Result window_b() {
	spawn_window("b");
	return after(300);
}

Result two_windows() {
	if (auto wait = wait_for(mapped_count() >= 2)) {
		return *wait;
	}
	View *a = titled("a"), *b = titled("b");
	check(a && b && server->focused_view == b, "two windows, newest focused");
	return after(100);
}

Result title_bar_drag() {
	View *b = titled("b");
	int saved_x = b->x, saved_y = b->y;
	Point from = title_point(*b);
	drag(from.x, from.y, from.x + 100, from.y + 50);
	check(b->x == saved_x + 100 && b->y == saved_y + 50,
		std::format("title bar drag moved the window by (100,50) -> ({},{})", b->x - saved_x,
			b->y - saved_y));
	return after(200);
}

// Double-click on the title toggles maximize.
Result double_click_on_the_title() {
	View *b = titled("b");
	Point title = title_point(*b);
	click(title.x, title.y);
	click(title.x, title.y);
	check(b->mode == WindowMode::maximized, "double-click on the title maximized");
	return after(400);
}

// The maximize button now shows restore; click it.
Result the_restore_button() {
	View *b = titled("b");
	shot("maximized");
	Point restore = deco_point(*b, DecoPart::maximize);
	click(restore.x, restore.y);
	check(b->mode != WindowMode::maximized, "restore button restored the window");
	return after(400);
}

// Resize from the top-left corner: the right and bottom edges must stay put.
Result corner_resize() {
	View *b = titled("b");
	saved = b->content_box();
	wlr_box f = b->frame_box();
	Point corner = output().canvas_to_local(f.x - 3, f.y - 3);
	drag(corner.x, corner.y, corner.x - 80, corner.y - 50);
	return after(400);
}

Result the_corner_resize_landed() {
	View *b = titled("b");
	wlr_box c = b->content_box();
	// foot snaps to whole character cells, so allow up to one cell short.
	check(c.width >= saved.width + 60 && c.height >= saved.height + 30 &&
		c.x + c.width == saved.x + saved.width && c.y + c.height == saved.y + saved.height,
		std::format("top-left corner drag resized {}x{} -> {}x{} with the bottom-right corner fixed",
			saved.width, saved.height, c.width, c.height));
	return after(100);
}

Result hover_minimize() {
	View *b = titled("b");
	Point mini = deco_point(*b, DecoPart::minimize);
	move_to(mini.x, mini.y);
	check(b->deco.hover() == DecoPart::minimize, "hovering the minimize button highlights it");
	shot("hover-minimize");
	return after(300);
}

Result minimize_and_restore() {
	View *a = titled("a"), *b = titled("b");
	Point mini = deco_point(*b, DecoPart::minimize);
	click(mini.x, mini.y);
	check(b->minimized && server->focused_view == a, "minimize button hid it, focus moved to a");
	chord(KEY_M, mods::super_shift);
	check(!b->minimized && server->focused_view == b, "Super+Shift+M restored it");
	return after(200);
}

Result hover_close() {
	View *b = titled("b");
	Point close = deco_point(*b, DecoPart::close);
	move_to(close.x, close.y);
	shot("hover-close");
	return after(300);
}

Result close_button() {
	View *b = titled("b");
	Point title = title_point(*b, 60);
	click(title.x, title.y); // a title click: b is now the double-click candidate
	closed_view = b;
	Point close = deco_point(*b, DecoPart::close);
	click(close.x, close.y);
	return after(600);
}

Result closed_then_super_return() {
	check(mapped_count() == 1 && titled("b") == nullptr, "close button closed the window");
	check(server->input.router.last_title_view != closed_view,
		"a closed window is no longer the double-click candidate");
	chord(KEY_ENTER, mods::super);
	return after(300);
}

Result terminal_launched() {
	if (auto wait = wait_for(mapped_count() >= 2)) {
		return *wait;
	}
	check(mapped_count() == 2, "Super+Return launched a terminal");
	chord(KEY_TAB, mods::super);
	return after(500);
}

Result overview_opened_by_super_tab() {
	check(output().overview.active(), "Super+Tab opened the overview");
	tap(KEY_ESC);
	return after(500);
}

Result escape_went_back() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!output().overview.active() && output().camera.current().zoom == 1,
		"Escape went back");
	for (int i = 0; i < 3; i++) {
		chord(KEY_RIGHT, mods::super_alt);
	}
	return after(600);
}

Result pin_desktop_2() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	chord(KEY_D, mods::super_ctrl);
	check(server->canvas.desktops.size() == 2,
		"Super+Alt+Right x3 then Super+Ctrl+D pinned Desktop 2");
	chord(KEY_LEFT, mods::super_ctrl);
	return after(600);
}

Result jumped_back_then_a_swipe_right() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Desktop *d1 = desk(1);
	check(d1 && output().desktop_showing() == d1, "Super+Ctrl+Left jumped back to Desktop 1");
	move_to(screen_w / 2.0, screen_h / 2.0);
	device_swipe(3, 300, 0); // fingers right: hop onto the desktop on the right
	return after(1200);
}

Result hopped_then_a_swipe_up() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Output &o = output();
	Desktop *d2 = desk(2);
	check(d2 && o.desktop_showing() == d2
		&& o.camera.current().cx == d2->frame().x + d2->frame().width / 2.0,
		std::format("3-finger swipe events from the device hopped onto Desktop 2 ({:.0f})",
			o.camera.current().cx));
	device_swipe(4, 0, -300);
	return after(600);
}

Result overview_opened_by_a_swipe() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(output().overview.active(), "4-finger swipe events opened the overview");
	shot("overview");
	return after(300);
}

// A right click on empty canvas below the desktops: a new desktop.
Result right_click_makes_desktop_3() {
	Output &o = output();
	Desktop *d1 = desk(1);
	Point p = o.canvas_to_local(d1->frame().x + d1->frame().width / 2.0,
		d1->frame().y + d1->frame().height * 1.9);
	click(p.x, p.y, BTN_RIGHT);
	check(server->canvas.desktops.size() == 3,
		"right-click on empty canvas added Desktop 3");
	return after(400);
}

// Drag Desktop 3 by its tab onto Desktop 2: combine.
Result tab_drag_combines() {
	Output &o = output();
	shot("three-desktops");
	Desktop *d2 = desk(2), *d3 = desk(3);
	std::optional<TabLayout> layout = overview_tab_layout(o, *d3);
	Point tx = o.canvas_to_local(d2->frame().x, d2->frame().y);
	Point sx = o.canvas_to_local(d3->frame().x, d3->frame().y);
	drag(layout->tab.x + 30, layout->tab.y + 14, layout->tab.x + 30 + (tx.x - sx.x),
		layout->tab.y + 14 + (tx.y - sx.y));
	check(server->canvas.desktops.size() == 2 && desk(3) == nullptr,
		"dropping a frame onto another combined them");
	return after(400);
}

// Drag Desktop 2's frame over the windows of Desktop 1 (frames move, windows don't).
Result frame_drag() {
	View *a = titled("a");
	Desktop *d2 = desk(2);
	std::optional<TabLayout> layout = overview_tab_layout(output(), *d2);
	int ax = a->x;
	wlr_box before = d2->frame();
	drag(layout->tab.x + 30, layout->tab.y + 14, layout->tab.x + 30, layout->tab.y + 14 + 40);
	check(d2->frame().y > before.y && a->x == ax, "dragging a frame moves only the frame");
	return after(300);
}

// Click the Tile switch on Desktop 1's tab: tiling on.
Result tile_switch() {
	Desktop *d1 = desk(1);
	std::optional<TabLayout> layout = overview_tab_layout(output(), *d1);
	click(layout->tile_button.x + layout->tile_button.width / 2.0,
		layout->tile_button.y + layout->tile_button.height / 2.0);
	return after(500); // clients commit the new sizes asynchronously
}

Result tiling_took_the_halves() {
	Desktop *d1 = desk(1);
	View *a = titled("a");
	for (View *v : server->views) {
		if (v->mapped && v != a) {
			second = v;
		}
	}
	check(d1 && second && d1->tiling(), "the tab's Tile switch switched tiling on");
	auto [left, right] = desktop_1_pair();
	check(a && second && on_slice(left, slice_frame_of(*d1, LEFT))
			&& on_slice(right, slice_frame_of(*d1, RIGHT)),
		"the desktop's two windows took the board's halves");
	check(a->tiled && second->tiled, "tiled windows are told they are tiled");
	shot("tiled-halves");
	return after(300);
}

// Leave the overview onto the empty Desktop 2.
Result leave_the_overview() {
	Output &o = output();
	Point c = desk(2)->centre();
	o.overview.target_point(c.x, c.y);
	o.overview.exit();
	return after(700);
}

Result left_the_overview_then_window_c() {
	Output &o = output();
	Desktop *d2 = desk(2);
	if (auto wait = wait_for(!o.overview.active() && o.desktop_showing() == d2)) {
		return *wait;
	}
	check(!o.overview.active() && o.desktop_showing() == d2,
		"left the overview onto Desktop 2");
	// An empty desktop takes the switch too; its first window fills it.
	chord(KEY_T, mods::super);
	check(d2->tiling() && server->hint.text() == "Tiling on for Desktop 2",
		"Super+T switched tiling on for the empty desktop");
	spawn_window("c");
	return after(400);
}

Result window_c_mapped() {
	if (auto wait = wait_for(mapped_count() >= 3)) {
		return *wait;
	}
	return after(500); // the client commits the slice size
}

// Super+Left takes the whole board's window to the left half.
Result first_window_fills_the_desktop() {
	Desktop *d2 = desk(2);
	View *c = titled("c");
	check(c && c->seated_on(d2->id()) && c->tiled
			&& on_slice(c, slice_frame_of(*d2, WHOLE)),
		"the first window on the empty tiled desktop filled it");
	shot("tiled-first-fills");
	chord(KEY_LEFT, mods::super);
	return after(500);
}

Result super_left_took_the_left_half() {
	check(on_slice(titled("c"), slice_frame_of(*desk(2), LEFT)),
		"Super+Left took the window to the left half");
	chord(KEY_DOWN, mods::super);
	return after(500);
}

// Left then Down reaches the bottom-left quarter.
Result left_then_down() {
	check(on_slice(titled("c"), slice_frame_of(*desk(2), BL)),
		"Super+Left then Super+Down took the window to the bottom-left quarter");
	shot("slice-bl");
	chord(KEY_UP, mods::super); // the opposite key widens back to the left half
	return after(500);
}

Result up_widened_to_the_left_half() {
	check(on_slice(titled("c"), slice_frame_of(*desk(2), LEFT)),
		"Super+Up widened the bottom-left quarter back to the left half");
	chord(KEY_RIGHT, mods::super); // and on to the whole board
	return after(500);
}

Result right_widened_to_the_whole_board() {
	check(on_slice(titled("c"), slice_frame_of(*desk(2), WHOLE)),
		"Super+Right widened the left half back to the whole board");
	chord(KEY_DOWN, mods::super); // the other order: Down first, then Left
	return after(500);
}

Result down_took_the_bottom_half() {
	check(on_slice(titled("c"), slice_frame_of(*desk(2), BOTTOM)),
		"Super+Down took the whole board's window to the bottom half");
	chord(KEY_LEFT, mods::super);
	return after(500);
}

// Both orders land on the bottom-left quarter.
Result both_orders_agree() {
	check(on_slice(titled("c"), slice_frame_of(*desk(2), BL)),
		"Super+Down then Super+Left also took the window to the bottom-left quarter");
	// A second window takes the largest free slice: the top half wins the
	// tie with the right one by reading order.
	spawn_window("d");
	return after(400);
}

Result window_d_took_the_top_half() {
	if (auto wait = wait_for(mapped_count() >= 4)) {
		return *wait;
	}
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d");
	check(d && on_slice(d, slice_frame_of(*d2, TOP))
			&& on_slice(c, slice_frame_of(*d2, BL)),
		"the new window took the largest free slice, the top half");
	// Focus c through its title bar and grow it over d.
	Point title = title_point(*c);
	click(title.x, title.y);
	return after(300);
}

Result c_focused_then_grown_over_d() {
	check(server->focused_view == titled("c"), "the clicked window is focused");
	chord(KEY_UP, mods::super); // bottom-left quarter widens to the left half, over d
	return after(500);
}

Result growing_pushed_d_aside() {
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d");
	check(on_slice(c, slice_frame_of(*d2, LEFT))
			&& on_slice(d, slice_frame_of(*d2, RIGHT)),
		"the grown slice pushed the covered window into the free space");
	check(!c->frame_rect().intersects(d->frame_rect()), "tiled windows do not overlap");
	shot("slice-push");
	// A third window with no free space left: it halves a held slice.
	spawn_window("e");
	return after(400);
}

Result window_e_halved_a_slice() {
	if (auto wait = wait_for(mapped_count() >= 5)) {
		return *wait;
	}
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d"), *e = titled("e");
	check(e && on_slice(e, slice_frame_of(*d2, BL))
			&& on_slice(c, slice_frame_of(*d2, TL))
			&& on_slice(d, slice_frame_of(*d2, RIGHT)),
		"with no free slice left, the new window halved the left half");
	shot("slice-halved");
	titled("e")->close(); // its slice frees up
	return after(600);
}

Result e_closed_and_f_opens() {
	if (auto wait = wait_for(mapped_count() <= 4)) {
		return *wait;
	}
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d");
	check(titled("e") == nullptr && on_slice(c, slice_frame_of(*d2, TL))
			&& on_slice(d, slice_frame_of(*d2, RIGHT)),
		"the closed window's slice stayed free, the others kept theirs");
	spawn_window("f");
	return after(400);
}

Result window_f_took_the_freed_slice() {
	if (auto wait = wait_for(mapped_count() >= 5)) {
		return *wait;
	}
	Output &o = output();
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d"), *f = titled("f");
	check(f && on_slice(f, slice_frame_of(*d2, BL))
			&& on_slice(c, slice_frame_of(*d2, TL))
			&& on_slice(d, slice_frame_of(*d2, RIGHT)),
		"the next window took the closed window's slice");
	shot("slice-refilled");
	// Drag c onto d's slice: they swap. Canvas coordinates in, the pointer
	// takes output-local ones.
	wlr_box fc = c->frame_box(), fd = d->frame_box();
	Point from = title_point(*c);
	Point to = o.canvas_to_local(
		fd.x + fd.width / 2.0 - fc.width / 2.0 + 80,
		fd.y + fd.height / 2.0 - fc.height / 2.0 + 16);
	drag(from.x, from.y, to.x, to.y);
	return after(500);
}

Result swapped_then_maximize() {
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d"), *f = titled("f");
	check(on_slice(c, slice_frame_of(*d2, RIGHT))
			&& on_slice(d, slice_frame_of(*d2, TL))
			&& on_slice(f, slice_frame_of(*d2, BL)),
		"dragging a window onto the other's slice swapped them");
	check(c->tiled && d->tiled && f->tiled, "both windows stayed tiled");
	shot("slice-swap");
	// Double-click c's title: maximize keeps the slice reserved.
	Point title = title_point(*c);
	click(title.x, title.y);
	click(title.x, title.y);
	return after(500);
}

Result maximized_then_restore() {
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d"), *f = titled("f");
	check(c && c->mode == WindowMode::maximized && !c->tiled && c->seated_on(d2->id())
			&& slice_of(c) == RIGHT,
		"maximize left the board but kept the window's slice");
	check(on_slice(d, slice_frame_of(*d2, TL))
			&& on_slice(f, slice_frame_of(*d2, BL)),
		"maximize moved no other window: no re-layout happens anymore");
	shot("slice-maximized");
	Point restore = deco_point(*c, DecoPart::maximize);
	click(restore.x, restore.y);
	return after(500);
}

Result restored_then_minimize() {
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d"), *f = titled("f");
	check(c->mode != WindowMode::maximized && c->tiled && c->seated_on(d2->id())
			&& on_slice(c, slice_frame_of(*d2, RIGHT))
			&& on_slice(d, slice_frame_of(*d2, TL))
			&& on_slice(f, slice_frame_of(*d2, BL)),
		"leaving maximize put the window back on its reserved slice");
	// Minimize: the slice frees up but is remembered.
	Point mini = deco_point(*c, DecoPart::minimize);
	click(mini.x, mini.y);
	return after(400);
}

Result minimized_then_restored() {
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d"), *f = titled("f");
	check(c && c->minimized && !c->tiled && c->seated_on(d2->id())
			&& slice_of(c) == RIGHT,
		"minimize freed the slice but kept it remembered");
	check(on_slice(d, slice_frame_of(*d2, TL))
			&& on_slice(f, slice_frame_of(*d2, BL)),
		"minimize moved no other window either");
	chord(KEY_M, mods::super_shift); // restore it
	return after(500);
}

Result restored_took_its_slice_back() {
	Desktop *d2 = desk(2);
	View *c = titled("c"), *d = titled("d"), *f = titled("f");
	check(!c->minimized && c->tiled && on_slice(c, slice_frame_of(*d2, RIGHT))
			&& on_slice(d, slice_frame_of(*d2, TL))
			&& on_slice(f, slice_frame_of(*d2, BL)),
		"the restored window took its own slice back, still free");
	// Nothing lies right of Desktop 2: the swipe events from the device
	// leave the view where it is.
	device_swipe(3, 900, 0);
	return after(600);
}

Result swipe_toward_nothing_then_the_free_view() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Output &o = output();
	Desktop *d2 = desk(2);
	check(o.desktop_showing() == d2 && !o.camera.panning()
			&& server->hint.text() == "No desktop that way",
		"a 3-finger swipe toward no desktop stays on Desktop 2 and says so");
	// The free view beside it is a pan away.
	for (int i = 0; i < 3; i++) {
		chord(KEY_RIGHT, mods::super_alt);
	}
	check(o.desktop_showing() == nullptr, "Super+Alt+Right three times panned onto a free view");
	chord(KEY_RIGHT, mods::super_ctrl_shift); // move the focused window here
	return after(600);
}

Result moved_out_then_maximize() {
	View *c = titled("c");
	check(c && !c->tiled && !c->board_seat.has_value()
			&& server->canvas.desktop_of(*c) == nullptr,
		"moving out freed the window's slice and left the board");
	chord(KEY_UP, mods::super); // a floating window still maximizes
	return after(500);
}

Result floating_maximized_then_back() {
	Output &o = output();
	View *c = titled("c");
	Rect fr = c->frame_rect(), vp = o.viewport();
	check(c->mode == WindowMode::maximized
			&& std::fabs(fr.x - vp.x) < 1 && std::fabs(fr.y - vp.y) < 1
			&& std::fabs(fr.w - vp.w) < 1 && std::fabs(fr.h - vp.h) < 1,
		"Super+Up still toggles maximize on a floating desktop");
	shot("floating-maximized");
	chord(KEY_UP, mods::super); // and back
	return after(500);
}

Result floating_restored_then_pinned() {
	View *c = titled("c");
	check(c && c->mode != WindowMode::maximized, "Super+Up restored the floating window");
	// Pin this free view as a desktop with tiling on: c joins it.
	chord(KEY_T, mods::super);
	Desktop *d3 = desk(3);
	check(d3 && d3->tiling() && output().desktop_showing() == d3,
		"Super+T pinned the free view as a desktop with tiling on");
	check(server->hint.text() == "Tiling on for Desktop 3",
		"the hint names the new tiled desktop");
	return after(400);
}

Result window_joined_the_new_board() {
	Desktop *d3 = desk(3);
	View *c = titled("c");
	check(c && c->seated_on(d3->id()) && on_slice(c, slice_frame_of(*d3, WHOLE)),
		"the window standing there joined the new desktop's board");
	// Back to Desktop 1 (via 2): its tiling survived, boxes unmoved.
	chord(KEY_LEFT, mods::super_ctrl);
	return after(800);
}

Result halfway_back_on_desktop_2() {
	if (auto wait = wait_for(output().desktop_showing() == desk(2))) {
		return *wait;
	}
	chord(KEY_LEFT, mods::super_ctrl);
	return after(800);
}

Result back_on_desktop_1() {
	Output &o = output();
	Desktop *d1 = desk(1);
	if (auto wait = wait_for(o.desktop_showing() == d1)) {
		return *wait;
	}
	auto [left, right] = desktop_1_pair();
	check(d1 && o.desktop_showing() == d1 && d1->tiling(),
		"back on Desktop 1, tiling still on");
	check(on_slice(left, slice_frame_of(*d1, LEFT))
			&& on_slice(right, slice_frame_of(*d1, RIGHT)),
		"and its two windows kept their slices all along");
	// `t` in the overview switches tiling off: boxes stay, boards drop.
	chord(KEY_TAB, mods::super);
	return after(700);
}

Result overview_targets_desktop_1() {
	Output &o = output();
	Desktop *d1 = desk(1);
	if (auto wait = wait_for(o.overview.active())) {
		return *wait;
	}
	check(o.overview.active() && o.overview.target().desktop == d1->id(),
		"the overview targets Desktop 1");
	tap(KEY_T);
	return after(500);
}

Result t_switched_tiling_off() {
	Output &o = output();
	Desktop *d1 = desk(1);
	View *a = titled("a");
	auto [left, right] = desktop_1_pair();
	Rect fl = left->frame_rect(), fr = right->frame_rect();
	check(d1 && !d1->tiling() && !a->tiled && !second->tiled
			&& !a->board_seat.has_value() && !second->board_seat.has_value(),
		"`t` in the overview switched tiling off, boards dropped");
	check(on_slice(left, slice_frame_of(*d1, LEFT))
			&& on_slice(right, slice_frame_of(*d1, RIGHT)),
		"switching off left the windows where the board had them");
	check(fl.x == left->frame_rect().x && fr.x == right->frame_rect().x,
		"not a pixel moved when the board went away");
	shot("tiled-off-kept");
	// The tab's Tile switch brings it back: re-placed as they stood.
	std::optional<TabLayout> layout = overview_tab_layout(o, *d1);
	click(layout->tile_button.x + layout->tile_button.width / 2.0,
		layout->tile_button.y + layout->tile_button.height / 2.0);
	return after(500);
}

Result tile_switch_brought_tiling_back() {
	Desktop *d1 = desk(1);
	View *a = titled("a");
	auto [left, right] = desktop_1_pair();
	check(d1 && d1->tiling() && a->tiled && second->tiled
			&& on_slice(left, slice_frame_of(*d1, LEFT))
			&& on_slice(right, slice_frame_of(*d1, RIGHT)),
		"the tab's Tile switch laid the standing windows back on the same slices");
	// And off again, for the combine below.
	tap(KEY_T);
	return after(500);
}

// Combine the tiled Desktop 2 into the untiled Desktop 1.
Result combine_into_untiled() {
	Desktop *d1 = desk(1), *d2 = desk(2);
	View *d = titled("d"), *f = titled("f");
	Rect d0 = d->frame_rect(), f0 = f->frame_rect();
	check(d1 && d2 && !d1->tiling() && d2->tiling(),
		"Desktop 1 untiled, Desktop 2 still tiled, before the combine");
	int dx = d1->frame().x - d2->frame().x;
	int dy = d1->frame().y - d2->frame().y;
	server->canvas.combine(d1->id(), d2->id(), d2->frame());
	check(desk(2) == nullptr, "the tiled desktop combined into the untiled one");
	check(d->frame_rect().x == d0.x + dx && d->frame_rect().y == d0.y + dy
			&& f->frame_rect().x == f0.x + dx && f->frame_rect().y == f0.y + dy,
		"its windows moved with the translation, keeping their layout");
	check(!d->tiled && !f->tiled && server->canvas.desktop_of(*d) == d1,
		"no longer tiled, they belong to the target desktop");
	shot("combined-untiled");
	return after(400);
}

// The overview's right clicks: on the empty canvas, a new desktop.
Result right_click_on_empty_canvas() {
	Output &o = output();
	check(o.overview.active(), "the overview is still open for the right clicks");
	Desktop *d1 = desk(1);
	Point c{d1->frame().x + d1->frame().width / 2.0,
		d1->frame().y + d1->frame().height * 1.9};
	check(server->canvas.desktops.at(c.x, c.y) == nullptr
			&& !server->view_at(c.x, c.y, o.camera.drawn().zoom).has_value(),
		"the spot below Desktop 1 is empty canvas");
	std::size_t before = server->canvas.desktops.size();
	Point p = o.canvas_to_local(c.x, c.y);
	click(p.x, p.y, BTN_RIGHT);
	Desktop *made = server->canvas.desktops.at(c.x, c.y);
	check(made != nullptr && server->canvas.desktops.size() == before + 1,
		"a right click on the empty canvas made a desktop there");
	right_made = made != nullptr ? std::optional(made->id()) : std::nullopt;
	return after(400);
}

// Inside a desktop, clear of windows: that desktop goes.
Result right_click_inside_a_desktop() {
	Desktop *made = server->canvas.desktops.find(right_made);
	std::size_t before = server->canvas.desktops.size();
	if (made != nullptr) {
		Point p = output().canvas_to_local(made->centre().x, made->centre().y);
		click(p.x, p.y, BTN_RIGHT);
	}
	check(made != nullptr && server->canvas.desktops.find(right_made) == nullptr
			&& server->canvas.desktops.size() == before - 1,
		"a right click inside a desktop, clear of windows, removed it");
	return after(400);
}

// On a window: nothing, the window's desktop stays.
Result right_click_on_a_window() {
	Desktop *d1 = desk(1);
	View *d = titled("d");
	std::size_t before = server->canvas.desktops.size();
	Point p = output().canvas_to_local(d->frame_rect().cx(), d->frame_rect().cy());
	click(p.x, p.y, BTN_RIGHT);
	check(desk(1) == d1 && server->canvas.desktops.size() == before,
		"a right click on a window inside Desktop 1 left the desktop alone");
	return after(400);
}

// On a desktop's tab: that desktop goes, its windows stay.
Result right_click_on_a_tab() {
	Desktop *d1 = desk(1);
	DesktopId id = d1->id();
	View *d = titled("d"), *f = titled("f");
	Rect d0 = d->frame_rect(), f0 = f->frame_rect();
	std::optional<TabLayout> tab = overview_tab_layout(output(), *d1);
	if (tab.has_value()) {
		click(tab->tab.x + 30, tab->tab.y + tab->tab.height / 2.0, BTN_RIGHT);
	}
	check(tab.has_value() && server->canvas.desktops.find(id) == nullptr,
		"a right click on Desktop 1's tab removed it");
	check(d->frame_rect().x == d0.x && d->frame_rect().y == d0.y
			&& f->frame_rect().x == f0.x && f->frame_rect().y == f0.y
			&& server->canvas.desktop_of(*d) == nullptr,
		"its windows stayed where they were, on no desktop");
	shot("right-click-removed");
	return after(400);
}

// canvas.conf: written with every default, chords from it.
Result canvas_conf_defaults() {
	tap(KEY_ESC);
	check(!output().overview.active(), "Escape left the overview");
	std::optional<std::string> text = read_file(canvas_conf);
	check(text.has_value() && *text == settings::template_text(),
		"the first run wrote canvas.conf with every default");
	chord(KEY_SPACE, mods::super);
	check(server->launcher.is_open(), "Super+Space opened the launcher");
	chord(KEY_SPACE, mods::super);
	check(!server->launcher.is_open(), "its own chord closed it");
	tap(KEY_LEFTMETA);
	check(!server->launcher.is_open(),
		"a lone Super tap runs nothing by default");
	return after(300);
}

// A hand edit saves over the file; the session applies it.
Result hand_edit_saved() {
	std::optional<std::string> text = read_file(canvas_conf);
	std::string from = "launcher = Super+Space";
	std::size_t at = text.has_value() ? text->find(from) : std::string::npos;
	if (at != std::string::npos) {
		text->replace(at, from.size(), "launcher = Super+K");
		save_conf(*text);
	}
	return after(300);
}

// Super+K now opens it, without any restart.
Result super_k_opens_the_launcher() {
	bool applied = holds_chord("launcher", {settings::mod_logo, XKB_KEY_k});
	if (auto wait = wait_for(applied)) {
		return *wait;
	}
	check(applied, "saving canvas.conf applied to the running session");
	chord(KEY_K, mods::super);
	check(server->launcher.is_open(), "Super+K, from the file, opened the launcher");
	chord(KEY_K, mods::super);
	check(!server->launcher.is_open(), "and closed it again");
	chord(KEY_SPACE, mods::super);
	check(!server->launcher.is_open(),
		"Super+Space, replaced in the file, opens nothing");
	return after(300);
}

// A hand-written file with comments, for the edits below.
Result hand_written_file() {
	save_conf("# my wayland-canvas\n[shortcuts]\n# the launcher\n"
		"launcher = Super+K\n\n[commands]\n# Super+B runs a command\n");
	return after(300);
}

Result settings_set_takes_a_chord() {
	if (auto wait = wait_for(holds_chord("launcher", {settings::mod_logo, XKB_KEY_k}))) {
		return *wait;
	}
	// settings.set takes Super+K from the launcher for the terminal.
	ask_socket("{\"id\":1,\"method\":\"settings.set\",\"params\":"
		"{\"action\":\"terminal\",\"keys\":[\"Super+K\"]}}\n", "set-terminal.out", 3);
	return after(400);
}

// One line rewritten, every other byte of the user's file kept.
Result one_line_rewritten() {
	std::string out = control_answer("set-terminal.out");
	check(out.find("\"moved\":[{\"scope\":\"global\","
			"\"action\":\"launcher\",\"keys\":[\"Super+K\"]}") != std::string::npos,
		"settings.set reported the chord taken from the launcher");
	std::string text = read_text(canvas_conf);
	check(text.find("# my wayland-canvas\n[shortcuts]\n# the launcher\n"
			"terminal = Super+K\n") == 0,
		"only the launcher's line was rewritten, comments kept");
	check(text.find("\n[commands]\n# Super+B runs a command\n")
			!= std::string::npos,
		"the [commands] section and its comment stayed");
	check(holds_chord("terminal", {settings::mod_logo, XKB_KEY_k}),
		"the edit applied at once");
	return after(300);
}

// A [commands] chord runs its command.
Result commands_entry_set() {
	ask_socket(std::format("{{\"id\":2,\"method\":\"settings.set\",\"params\":"
			"{{\"commands\":[{{\"keys\":\"Super+B\","
			"\"command\":\"touch {}\"}}]}}}}\n", in_scratch("command-marker")),
		"set-commands.out", 3);
	return after(400);
}

Result commands_entry_written() {
	std::string out = control_answer("set-commands.out");
	check(out.find("\"commands\":[{\"keys\":\"Super+B\"") != std::string::npos,
		"settings.set wrote the [commands] entry");
	chord(KEY_B, mods::super);
	return after(300);
}

Result commands_chord_ran() {
	if (auto wait = wait_for(file_exists(in_scratch("command-marker")))) {
		return *wait;
	}
	check(file_exists(in_scratch("command-marker")), "a [commands] chord ran its command");
	return after(300);
}

// A bad line: ignored with its number, hinted on every screen.
Result a_bad_line() {
	if (std::optional<std::string> text = read_file(canvas_conf)) {
		std::string in = "[shortcuts]\n";
		std::size_t at = text->find(in);
		if (at != std::string::npos) {
			text->insert(at + in.size(), "lancher = Super+P\n");
		}
		save_conf(*text);
	}
	return after(300);
}

Result the_bad_line_is_listed() {
	if (auto wait = wait_for(!server->conf.errors().empty())) {
		return *wait;
	}
	check(!server->conf.errors().empty()
			&& server->conf.errors().front().text == "unknown action 'lancher'",
		"the ignored line is listed with its reason");
	check(server->hint.text().starts_with("canvas.conf line ")
			&& server->hint.text().find("unknown action 'lancher'")
				!= std::string::npos,
		std::format("the first error shows as a hint ({})",
			server->hint.text()));
	return after(300);
}

// The gesture choices from the file reach the gesture engine.
Result a_swipe_preset_in_the_file() {
	if (std::optional<std::string> text = read_file(canvas_conf)) {
		save_conf(*text + "\n[gestures]\nswipes = four-finger-hops\n");
	}
	return after(300);
}

Result the_preset_reached_the_engine() {
	bool applied = server->gestures.options().swipes == settings::SwipePreset::four_finger_hops;
	if (auto wait = wait_for(applied)) {
		return *wait;
	}
	check(applied, "the file's swipe preset reached the gesture engine");
	return after(300);
}

// A subscriber sees the settings change as an event.
Result a_subscriber() {
	ask_socket("{\"id\":1,\"method\":\"subscribe\"}\n"
		"{\"id\":2,\"method\":\"settings.set\",\"params\":"
		"{\"pinchScale\":false}}\n", "settings-event.out", 4);
	return after(600);
}

Result the_subscriber_heard_it() {
	std::string out = control_answer("settings-event.out");
	check(out.find("\"event\":\"settings\"") != std::string::npos
			&& out.find("\"pinchScale\":false") != std::string::npos,
		"a subscriber received the settings event");
	return after(300);
}

// A recorded chord: every key belongs to the capture.
Result capture_a_chord() {
	// The terminal spawned by an early step reads into this marker:
	// a fresh one makes "ran nothing" observable.
	unlink(in_scratch("terminal-marker").c_str());
	ask_socket("{\"id\":7,\"method\":\"shortcut.capture\"}\n", "capture-chord.out", 24);
	return after(400);
}

// Super+K (the terminal's chord now) recorded: run by nothing.
Result the_recorded_chord() {
	chord(KEY_K, mods::super);
	return after(400);
}

Result recorded_chord_ran_nothing() {
	std::string out = control_answer("capture-chord.out");
	if (auto wait = wait_for(!out.empty())) {
		return *wait;
	}
	check(out.find("\"keys\":\"Super+K\"") != std::string::npos,
		"the capture reported the chord pressed");
	check(!file_exists(in_scratch("terminal-marker")),
		"the recorded chord ran nothing");
	// The same chord, unrecorded, is the terminal again.
	chord(KEY_K, mods::super);
	return after(300);
}

bool terminal_got(char c) {
	return read_text(in_scratch("terminal-marker")).find(c) != std::string::npos;
}

Result unrecorded_chord_ran_the_terminal() {
	if (auto wait = wait_for(file_exists(in_scratch("terminal-marker")))) {
		return *wait;
	}
	check(file_exists(in_scratch("terminal-marker")),
		"the same chord, unrecorded, ran the terminal");
	// A plain key reaches the terminal's client: foot reads into the
	// marker. Pressed until it lands, while the new terminal takes the
	// focus.
	tap(KEY_X);
	// The Super tap, given to the launcher through the socket.
	ask_socket("{\"id\":8,\"method\":\"settings.set\",\"params\":"
		"{\"action\":\"launcher\",\"keys\":[\"Super+Space\",\"Super\"]}}\n", "set-tap.out", 3);
	return after(400);
}

Result a_plain_key_reached_the_terminal() {
	if (!terminal_got('x')) {
		// The terminal may still be taking the focus, and its line
		// discipline holds the key until Enter: press x, then Enter.
		tap(KEY_X);
		tap(KEY_ENTER);
		if (auto wait = wait_for(terminal_got('x'))) {
			return *wait;
		}
	}
	if (auto wait = wait_for(holds_chord("launcher", {settings::mod_logo, 0}))) {
		return *wait;
	}
	check(terminal_got('x'), "a plain key reached the terminal's client");
	// pan-back holds the double tap by default, so the single tap runs
	// once the double-tap interval has passed without a second.
	tap(KEY_LEFTMETA);
	check(!server->launcher.is_open(),
		"the Super tap waits out the double-tap interval before it runs");
	return after(config::double_tap_ms + 200);
}

Result the_tap_opened_the_launcher() {
	check(server->launcher.is_open(),
		"the Super tap, given to the launcher, opened it after the interval");
	tap(KEY_LEFTMETA);
	return after(config::double_tap_ms + 200);
}

// Two taps within the interval: the double tap, not two singles.
Result the_second_tap_closed_it() {
	check(!server->launcher.is_open(), "a second tap later closed it again");
	shot("tap-launcher");
	tap(KEY_LEFTMETA);
	tap(KEY_LEFTMETA);
	return after(config::double_tap_ms + 200);
}

Result a_double_tap_ran_its_action() {
	check(!server->launcher.is_open(),
		"a double tap ran the double tap's action, neither single tap");
	// The settings record a double tap by tapping twice.
	ask_socket("{\"id\":12,\"method\":\"shortcut.capture\"}\n", "capture-double.out", 24);
	return after(400);
}

Result tapping_twice_for_the_capture() {
	tap(KEY_LEFTMETA);
	tap(KEY_LEFTMETA);
	return after(400);
}

Result the_capture_recorded_two_taps() {
	std::string out = control_answer("capture-double.out");
	if (auto wait = wait_for(!out.empty())) {
		return *wait;
	}
	check(out.find("\"keys\":\"Super Super\"") != std::string::npos,
		std::format("the capture recorded two taps as the double tap ({})", out));
	ask_socket("{\"id\":13,\"method\":\"shortcut.capture\"}\n", "capture-single.out", 24);
	return after(400);
}

Result one_tap_for_the_capture() {
	tap(KEY_LEFTMETA);
	return after(config::double_tap_ms + 300);
}

Result the_capture_recorded_one_tap() {
	std::string out = control_answer("capture-single.out");
	if (auto wait = wait_for(!out.empty())) {
		return *wait;
	}
	check(out.find("\"keys\":\"Super\"") != std::string::npos,
		std::format("one tap, the interval passed, was recorded as the tap ({})", out));
	return after(300);
}

// shortcut.cancel: a capture pending on one connection.
Result a_capture_pending() {
	// The terminal the chord spawns after the cancel recreates the
	// marker: a fresh one makes "it ran" observable.
	unlink(in_scratch("terminal-marker").c_str());
	ask_socket("{\"id\":9,\"method\":\"shortcut.capture\"}\n", "cancel-pending.out", 12);
	return after(1000);
}

// Still unanswered; the cancel comes from a second connection.
Result still_unanswered_then_cancelled() {
	check(control_answer("cancel-pending.out").empty(),
		"a shortcut.capture stays unanswered until it is finished or cancelled");
	ask_socket("{\"id\":10,\"method\":\"shortcut.cancel\"}\n", "cancel-answer.out", 3);
	return after(1000);
}

Result the_cancel_was_answered() {
	std::string pending = control_answer("cancel-pending.out");
	std::string answer = control_answer("cancel-answer.out");
	if (auto wait = wait_for(!pending.empty() && !answer.empty())) {
		return *wait;
	}
	check(pending.find("{\"id\":9,\"result\":{\"keys\":null}}") != std::string::npos,
		"the pending capture was answered {\"keys\": null}");
	check(answer.find("{\"id\":10,\"result\":true}") != std::string::npos,
		"shortcut.cancel answered true");
	// The capture is over: the chord the file binds runs its action.
	chord(KEY_K, mods::super);
	return after(300);
}

Result after_the_cancel_the_chord_ran() {
	if (auto wait = wait_for(file_exists(in_scratch("terminal-marker")))) {
		return *wait;
	}
	check(file_exists(in_scratch("terminal-marker")),
		"after the cancel, a bound chord ran its action (Super+K opened the terminal)");
	return after(300);
}

Result after_the_cancel_a_plain_key() {
	if (!terminal_got('z')) {
		// As for the first plain key: the terminal may still be taking
		// the focus, and its line discipline holds the key until Enter.
		tap(KEY_Z);
		tap(KEY_ENTER);
		if (auto wait = wait_for(terminal_got('z'))) {
			return *wait;
		}
	}
	check(terminal_got('z'), "after the cancel, a plain key reached the focused client");
	// With nothing to cancel the method is still just true.
	ask_socket("{\"id\":11,\"method\":\"shortcut.cancel\"}\n", "cancel-idle.out", 3);
	return after(1000);
}

Result cancel_with_nothing_pending() {
	std::string idle = control_answer("cancel-idle.out");
	if (auto wait = wait_for(!idle.empty())) {
		return *wait;
	}
	check(idle.find("{\"id\":11,\"result\":true}") != std::string::npos,
		"shortcut.cancel with no capture under way answered true");
	return after(300);
}

// A button held on a window keeps the pointer on it past its edge, as
// Wayland's implicit grab does (docs/intents/input/keyboard-and-pointer.md).
Result a_button_held_on_a_window() {
	View *v = server->views.empty() ? nullptr : server->views.front();
	check(v != nullptr, "a window to hold a button on");
	if (v == nullptr) {
		return after(100);
	}
	held_view = v;
	wlr_box content = v->content_box();
	Point inside = output().canvas_to_local(
		content.x + content.width / 2.0, content.y + content.height / 2.0);
	move_to(inside.x, inside.y);
	button(BTN_LEFT, true);
	return after(200);
}

Result the_press_travels_off_the_window() {
	Output &o = output();
	wlr_box frame = held_view->frame_box();
	Point right = o.canvas_to_local(frame.x + frame.width + 40, frame.y + frame.height / 2.0);
	Point left = o.canvas_to_local(frame.x - 40, frame.y + frame.height / 2.0);
	Point outside = right.x < o.width - 10 ? right : left;
	for (int i = 1; i <= 10; i++) {
		move_to(outside.x, outside.y - 10 + i); // the press travels off the window
	}
	check(server->seat->pointer_state.focused_surface == held_view->surface(),
		"a button held on a window keeps the pointer on it past its edge");
	button(BTN_LEFT, false);
	return after(200);
}

Result the_release_gave_the_pointer_away() {
	check(server->seat->pointer_state.focused_surface != held_view->surface(),
		"its release gives the pointer to what is under it");
	return after(300);
}

// Super+C and Super+V send the app with the keyboard the clipboard chords
// (docs/intents/input/keyboard-and-pointer.md); an X11 client through
// Xwayland proves them, the same as a wayland client takes them.
Result the_x11_client() {
	check(std::getenv("DISPLAY") != nullptr,
		"the session names an X display for X11 clients");
	check(x11.start_client("CANVAS_X11_CLIENT", {"--size=400x300"}),
		"the scenario's X11 client started");
	return after(1500);
}

// The client's window is up; a click gives it the keyboard.
Result the_x11_window_is_clicked() {
	if (auto wait = wait_for(x11_view() != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	View *v = x11_view();
	check(v != nullptr, "the X11 window mapped as a view");
	if (v != nullptr) {
		wlr_box frame = v->frame_box();
		Point inside = output().canvas_to_local(frame.x + frame.width / 2.0,
			frame.y + config::titlebar_height + 20.0);
		click(inside.x, inside.y);
	}
	return after(400);
}

// The shortcuts: each sends the client the app's own chord.
Result the_clipboard_chords() {
	View *v = x11_view();
	check(v != nullptr && server->focused_view == v,
		"the X11 window holds the keyboard");
	chord(KEY_C, mods::super);
	chord(KEY_V, mods::super);
	return after(500);
}

Result the_client_saw_the_chords() {
	std::string_view said = x11.said();
	if (auto wait = wait_for(said.find("KEY 118 4") != std::string_view::npos
			&& said.find("KEY 118 1") != std::string_view::npos)) {
		return *wait;
	}
	check(said.find("KEY 118 4") != std::string_view::npos,
		"Super+C sent the X11 client Ctrl+Insert (keycode 118, state 4)");
	check(said.find("KEY 118 1") != std::string_view::npos,
		"Super+V sent the X11 client Shift+Insert (keycode 118, state 1)");
	x11.kill();
	return finish();
}

} // namespace

int main() {
	if (!begin("input", {})) {
		return 2;
	}
	// The terminal action runs this run's script: foot reading stdin into
	// the marker, so a chord that spawns it, and keys that reach it, are
	// observable.
	std::string terminal_sh = in_scratch("terminal.sh");
	write_script(terminal_sh, std::format(
		"#!/bin/sh\nexec foot sh -c 'cat > {}/terminal-marker'\n", scratch()));
	setenv("TERMINAL", terminal_sh.c_str(), 1);
	// canvas.conf lives in this run's scratch (begin() points XDG_CONFIG_HOME
	// there), never in the user's real ~/.config; the scratch starts empty, so
	// the file is written fresh, as the first run does.
	canvas_conf = in_scratch("xdg-config/wayland-canvas/canvas.conf");
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	return run(options, {
		{"devices and window a", devices_and_window_a},
		{"window b", window_b},
		{"two windows", two_windows},
		{"a title bar drag", title_bar_drag},
		{"double-click on the title", double_click_on_the_title},
		{"the restore button", the_restore_button},
		{"a corner resize", corner_resize},
		{"the corner resize landed", the_corner_resize_landed},
		{"hover minimize", hover_minimize},
		{"minimize and restore", minimize_and_restore},
		{"hover close", hover_close},
		{"the close button", close_button},
		{"closed, then Super+Return", closed_then_super_return},
		{"the terminal launched", terminal_launched},
		{"the overview opened by Super+Tab", overview_opened_by_super_tab},
		{"Escape went back", escape_went_back},
		{"pin desktop 2", pin_desktop_2},
		{"jumped back, then a swipe right", jumped_back_then_a_swipe_right},
		{"hopped, then a swipe up", hopped_then_a_swipe_up},
		{"the overview opened by a swipe", overview_opened_by_a_swipe},
		{"a right click makes desktop 3", right_click_makes_desktop_3},
		{"a tab drag combines", tab_drag_combines},
		{"a frame drag", frame_drag},
		{"the tile switch", tile_switch},
		{"tiling took the halves", tiling_took_the_halves},
		{"leave the overview", leave_the_overview},
		{"left the overview, then window c", left_the_overview_then_window_c},
		{"window c mapped", window_c_mapped},
		{"the first window fills the desktop", first_window_fills_the_desktop},
		{"Super+Left took the left half", super_left_took_the_left_half},
		{"left then down", left_then_down},
		{"Super+Up widened to the left half", up_widened_to_the_left_half},
		{"Super+Right widened to the whole board", right_widened_to_the_whole_board},
		{"Super+Down took the bottom half", down_took_the_bottom_half},
		{"both orders agree", both_orders_agree},
		{"window d took the top half", window_d_took_the_top_half},
		{"c focused, then grown over d", c_focused_then_grown_over_d},
		{"growing pushed d aside", growing_pushed_d_aside},
		{"window e halved a slice", window_e_halved_a_slice},
		{"e closed, then window f", e_closed_and_f_opens},
		{"window f took the freed slice", window_f_took_the_freed_slice},
		{"swapped, then maximize", swapped_then_maximize},
		{"maximized, then restore", maximized_then_restore},
		{"restored, then minimize", restored_then_minimize},
		{"minimized, then restored", minimized_then_restored},
		{"restored took its slice back", restored_took_its_slice_back},
		{"a swipe toward nothing, then the free view", swipe_toward_nothing_then_the_free_view},
		{"moved out, then maximize", moved_out_then_maximize},
		{"floating maximized, then back", floating_maximized_then_back},
		{"floating restored, then pinned", floating_restored_then_pinned},
		{"the window joined the new board", window_joined_the_new_board},
		{"halfway back on desktop 2", halfway_back_on_desktop_2},
		{"back on desktop 1", back_on_desktop_1},
		{"the overview targets desktop 1", overview_targets_desktop_1},
		{"t switched tiling off", t_switched_tiling_off},
		{"the tile switch brought tiling back", tile_switch_brought_tiling_back},
		{"combine into the untiled desktop", combine_into_untiled},
		{"a right click on the empty canvas", right_click_on_empty_canvas},
		{"a right click inside a desktop", right_click_inside_a_desktop},
		{"a right click on a window", right_click_on_a_window},
		{"a right click on a tab", right_click_on_a_tab},
		{"canvas.conf defaults", canvas_conf_defaults},
		{"a hand edit saved", hand_edit_saved},
		{"Super+K opens the launcher", super_k_opens_the_launcher},
		{"a hand-written file", hand_written_file},
		{"settings.set takes a chord", settings_set_takes_a_chord},
		{"one line rewritten", one_line_rewritten},
		{"a commands entry set", commands_entry_set},
		{"the commands entry written", commands_entry_written},
		{"the commands chord ran", commands_chord_ran},
		{"a bad line", a_bad_line},
		{"the bad line is listed", the_bad_line_is_listed},
		{"a swipe preset in the file", a_swipe_preset_in_the_file},
		{"the preset reached the engine", the_preset_reached_the_engine},
		{"a subscriber", a_subscriber},
		{"the subscriber heard it", the_subscriber_heard_it},
		{"capture a chord", capture_a_chord},
		{"the recorded chord", the_recorded_chord},
		{"the recorded chord ran nothing", recorded_chord_ran_nothing},
		{"the unrecorded chord ran the terminal", unrecorded_chord_ran_the_terminal},
		{"a plain key reached the terminal", a_plain_key_reached_the_terminal},
		{"the tap opened the launcher", the_tap_opened_the_launcher},
		{"the second tap closed it", the_second_tap_closed_it},
		{"a double tap ran its action", a_double_tap_ran_its_action},
		{"tapping twice for the capture", tapping_twice_for_the_capture},
		{"the capture recorded two taps", the_capture_recorded_two_taps},
		{"one tap for the capture", one_tap_for_the_capture},
		{"the capture recorded one tap", the_capture_recorded_one_tap},
		{"a capture pending", a_capture_pending},
		{"still unanswered, then cancelled", still_unanswered_then_cancelled},
		{"the cancel was answered", the_cancel_was_answered},
		{"after the cancel the chord ran", after_the_cancel_the_chord_ran},
		{"after the cancel a plain key", after_the_cancel_a_plain_key},
		{"a cancel with nothing pending", cancel_with_nothing_pending},
		{"a button held on a window", a_button_held_on_a_window},
		{"the press travels off the window", the_press_travels_off_the_window},
		{"the release gave the pointer away", the_release_gave_the_pointer_away},
		{"the X11 client", the_x11_client},
		{"the X11 window is clicked", the_x11_window_is_clicked},
		{"the clipboard chords", the_clipboard_chords},
		{"the client saw the chords", the_client_saw_the_chords},
	});
}
