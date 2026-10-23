// End-to-end touch and pen scenario (docs/intents/input/touch-and-pen.md):
// a fake touchscreen and a fake pen tablet announced on the backend, the
// tests' own client reporting what reaches it (tests/clients/touch-client.c),
// and, where this machine has Xwayland, the tests' X11 client. A tap and a
// drag reach a window as touch; one finger drags a window by its title bar;
// a finger held still on the overview's empty canvas makes a desktop there,
// as a right click does; two fingers on the canvas look away and pinch a
// desktop's scale; three fingers hop, and not while the capture picker is
// open. A finger on a window as the session
// locks is cancelled for it, and locked, touch reaches the locker only - as
// a click, the locker taking no touch. A client without touch support gets
// a tap as a click. The pen reaches a tablet client with pressure and tilt,
// a client without tablet support as the pointer, and drags a title bar;
// a tap and the pen's tip reach an X11 window as its press. Writes only
// under $CANVAS_TMP; canvas.conf lives in its own scratch config dir.
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include "core/config.h"
#include "core/server.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "platform/output.h"
#include "windows/view.h"

using namespace harness;

namespace {

Child app; // the touch and tablet client
Child plain; // a client with neither
Child x11; // the tests' X11 client
Child locker;

// State carried between steps.
Camera home{};
wlr_box frame_before{};
std::size_t desktops_before = 0, said_before = 0, locker_before = 0;
std::optional<DesktopId> shown; // the desktop on screen when a pinch began

constexpr int mapping_budget_ms = 20'000;

// Whether the X11 client said "BUTTON <button> <x>,<y>" since `from`, within
// a pixel of `want`: X takes a position as a whole pixel, and a touch goes
// through Xwayland's 0..65535 device range and back, which can come out a
// pixel short.
bool said_button_near(Child &c, int button, Point want, std::size_t from) {
	std::string_view all = c.said();
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

void super_key(uint32_t code) {
	chord(code, mods::super);
}

Desktop *desk(int number) {
	return server->canvas.desktops.by_number(number);
}

Result devices_and_client() {
	attach_keyboard();
	attach_pointer();
	attach_touchscreen();
	attach_tablet();
	check(app.start_client("CANVAS_TOUCH_CLIENT", {"--size=400x300", "--title=touch-app"}),
		"the touch client started");
	return after(1000);
}

Result client_mapped() {
	if (auto wait = wait_for(titled("touch-app") != nullptr && settled()
			&& app.said_line("MAPPED 400x300"), {.budget_ms = mapping_budget_ms})) {
		return *wait;
	}
	check(titled("touch-app") != nullptr, "the touch client's window mapped as a view");
	check((server->seat->capabilities & WL_SEAT_CAPABILITY_TOUCH) != 0,
		"with a touchscreen connected the seat offers touch");
	home = output().camera.current();
	return after(300);
}

Result a_tap() {
	View *view = titled("touch-app");
	if (view == nullptr) {
		return after(300);
	}
	said_before = app.said().size();
	Point p = on_screen(*view, 100, 80);
	finger_down(1, p.x, p.y);
	finger_up(1);
	return after(400);
}

Result tap_arrived_then_a_drag() {
	View *view = titled("touch-app");
	check(app.said_line("TOUCH DOWN 1 100,80", said_before)
			&& app.said_line("TOUCH UP 1", said_before),
		std::format("a tap reached the window as touch, where it landed ({})",
			app.since(said_before)));
	check(view != nullptr && server->focused_view == view, "and focused it, as a click does");
	// A drag: the finger stays the window's, at its own point, past its
	// right edge too.
	said_before = app.said().size();
	if (view != nullptr) {
		Point a = on_screen(*view, 50, 50);
		finger_down(2, a.x, a.y);
		for (int s = 1; s <= 5; s++) {
			finger_move(2, a.x + 80 * s, a.y + 8 * s);
		}
		finger_up(2);
	}
	return after(400);
}

Result drag_arrived_then_a_title_bar_drag() {
	check(app.said_line("TOUCH MOTION 2 370,82", said_before)
			&& app.said_line("TOUCH MOTION 2 450,90", said_before),
		std::format("a drag's motion reached the window in its own coordinates, "
			"past its right edge too ({})", app.since(said_before)));
	// One finger on the title bar: the window moves with it.
	View *view = titled("touch-app");
	if (view == nullptr) {
		return after(300);
	}
	frame_before = view->frame_box();
	const Point from[] = {title_point(*view, 60)};
	fingers_drag(from, 200, 100);
	return after(500);
}

Result title_bar_drag_moved_it_then_the_overview() {
	View *view = titled("touch-app");
	wlr_box now = view != nullptr ? view->frame_box() : wlr_box{};
	check(view != nullptr && now.x - frame_before.x == 200 && now.y - frame_before.y == 100,
		std::format("one finger dragged the window by its title bar ({},{})",
			now.x - frame_before.x, now.y - frame_before.y));
	output().overview.enter();
	return after(800);
}

Result a_finger_held_on_the_canvas() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	desktops_before = std::ranges::distance(server->canvas.desktops.all());
	finger_down(3, 40, screen_h / 2.0);
	return after(config::touch_hold_ms + 300);
}

Result the_hold_made_a_desktop() {
	finger_up(3);
	std::size_t now = std::ranges::distance(server->canvas.desktops.all());
	check(now == desktops_before + 1, std::format(
		"a finger held on the overview's empty canvas made a desktop there, as a "
		"right click does ({} -> {})", desktops_before, now));
	output().overview.exit();
	return after(800);
}

// Two fingers on the canvas look away, the content following them.
Result two_fingers_look_away() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	home = output().camera.current();
	said_before = app.said().size();
	const Point from[] = {{100, 620}, {220, 620}};
	fingers_drag(from, 150, -100);
	return after(400);
}

Result looked_away_then_a_pinch() {
	Camera now = output().camera.current();
	check(std::abs(now.cx - (home.cx - 150)) < 1 && std::abs(now.cy - (home.cy + 100)) < 1,
		std::format("two fingers looked away, the content following them "
			"(camera moved {:.0f},{:.0f})", now.cx - home.cx, now.cy - home.cy));
	check(app.said().find("TOUCH DOWN", said_before) == std::string_view::npos,
		std::format("and no client saw them ({})", app.since(said_before)));
	output().camera.jump(home);
	return after(400);
}

// Two fingers spreading pinch the shown desktop's scale, as Super does.
Result two_fingers_spread() {
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
	return after(1000);
}

Result the_pinch_scaled_the_desktop() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	Desktop *pinched = server->canvas.desktops.find(shown);
	check(pinched != nullptr && pinched->scale() > 1.5, std::format(
		"two fingers spreading scaled the shown desktop ({:.2f})",
		pinched != nullptr ? pinched->scale() : 0.0));
	if (pinched != nullptr) {
		server->canvas.settle_scale(pinched->id(), 1.0);
	}
	return after(800);
}

// Desktop 3 to the right of desktop 1, then back on desktop 1.
Result desktop_3() {
	super_key(KEY_3);
	return after(900);
}

Result desktop_1() {
	super_key(KEY_1);
	return after(900);
}

// Three fingers swiping right hop right, onto desktop 3.
Result three_fingers_swipe_right() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(output().desktop_showing() == desk(1), "desktop 1 is shown before the swipe");
	const Point from[] = {{300, 620}, {360, 620}, {420, 620}};
	fingers_drag(from, 240, 0);
	return after(900);
}

Result they_hopped_right() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(desk(3) != nullptr && output().desktop_showing() == desk(3),
		"three fingers swiping right hopped right, onto desktop 3");
	super_key(KEY_1);
	return after(900);
}

// Four fingers swiping up open the overview.
Result four_fingers_swipe_up() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	const Point from[] = {{300, 620}, {360, 620}, {420, 620}, {480, 620}};
	fingers_drag(from, 0, -320);
	return after(900);
}

Result the_overview_opened() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(output().overview.active(), "four fingers swiping up opened the overview");
	output().overview.exit();
	return after(800);
}

// With the capture picker open, three fingers swiping do nothing.
Result the_picker_opens() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(output().desktop_showing() == desk(1), "desktop 1 is shown before the picker opens");
	chord(KEY_4, mods::super_shift);
	return after(500);
}

Result three_fingers_with_the_picker_open() {
	check(server->capture_ui.owns_keyboard(), "Super+Shift+4 opened the capture picker");
	const Point from[] = {{300, 620}, {360, 620}, {420, 620}};
	fingers_drag(from, 240, 0);
	return after(900);
}

// Nor do four fingers up; Escape closes the picker.
Result four_fingers_with_the_picker_open() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(output().desktop_showing() == desk(1),
		"three fingers swiping with the capture picker open did not hop");
	check(server->capture_ui.owns_keyboard(), "and the picker is still open");
	const Point from[] = {{300, 620}, {360, 620}, {420, 620}, {480, 620}};
	fingers_drag(from, 0, -320);
	return after(900);
}

Result escape_closes_the_picker() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(!output().overview.active(),
		"four fingers swiping up with the capture picker open did not open the overview");
	check(server->capture_ui.owns_keyboard(), "and the picker is still open");
	tap(KEY_ESC);
	return after(500);
}

// The picker closed: three fingers hop again.
Result the_picker_closed() {
	check(!server->capture_ui.owns_keyboard(), "Escape closed the capture picker");
	const Point from[] = {{300, 620}, {360, 620}, {420, 620}};
	fingers_drag(from, 240, 0);
	return after(900);
}

Result they_hop_again() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	check(desk(3) != nullptr && output().desktop_showing() == desk(3),
		"with the picker closed, three fingers swiping right hopped onto desktop 3");
	super_key(KEY_1);
	return after(900);
}

// A finger on the window as the session locks is cancelled for it.
Result a_finger_as_the_session_locks() {
	if (auto wait = wait_for(settled())) {
		return *wait;
	}
	View *view = titled("touch-app");
	said_before = app.said().size();
	if (view != nullptr) {
		Point p = on_screen(*view, 200, 150);
		finger_down(31, p.x, p.y);
	}
	check(locker.start_client("CANVAS_LOCK_CLIENT"), "the scenarios' locker started");
	return after(800);
}

// Locked, touch reaches the locker only.
Result locked_touch_goes_to_the_locker() {
	if (auto wait = wait_for(server->lock.locked() && locker.said_line("LOCKED"))) {
		return *wait;
	}
	check(server->lock.locked(), "the session is locked");
	check(app.said_line("TOUCH CANCEL", said_before), std::format(
		"a finger on the window as the session locked was cancelled for it ({})",
		app.since(said_before)));
	View *view = titled("touch-app");
	said_before = app.said().size();
	locker_before = locker.said().size();
	if (view != nullptr) {
		Point p = on_screen(*view, 200, 150);
		finger_move(31, p.x + 30, p.y + 20); // the held finger goes on: to nobody
		finger_up(31);
		frame_before = view->frame_box();
		finger_down(30, p.x, p.y);
		finger_up(30);
		const Point from[] = {title_point(*view, 60)};
		fingers_drag(from, 120, 60);
	}
	return after(500);
}

Result locked_touch_arrived_then_unlock() {
	View *view = titled("touch-app");
	check(app.said().find("TOUCH", said_before) == std::string_view::npos,
		std::format("locked, the window got no touch ({})", app.since(said_before)));
	check(locker.said_line("BUTTON 272", locker_before), std::format(
		"a tap reached the locker, which takes no touch, as a click ({})",
		locker.since(locker_before)));
	check(view != nullptr && view->frame_box().x == frame_before.x
			&& view->frame_box().y == frame_before.y,
		"and a title-bar drag moved nothing");
	locker.signal(SIGUSR1);
	return after(800);
}

// The pen over the tablet client: in range, pressure and tilt, the tip.
Result unlocked_then_the_pen() {
	if (auto wait = wait_for(!server->lock.locked())) {
		return *wait;
	}
	check(!server->lock.locked(), "the locker unlocked the session");
	locker.kill();
	View *view = titled("touch-app");
	said_before = app.said().size();
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
	return after(500);
}

Result the_pen_arrived_then_a_plain_client() {
	std::string got = app.since(said_before);
	check(app.said_line("TABLET IN", said_before)
			&& app.said_line("TABLET MOTION 120,90", said_before),
		std::format("the pen came in range of the tablet client where it was ({})", got));
	check(app.said_line("TABLET PRESSURE 32767", said_before)
			|| app.said_line("TABLET PRESSURE 32768", said_before),
		"with its pressure");
	check(app.said_line("TABLET TILT 10,-5", said_before), "and its tilt");
	check(app.said_line("TABLET DOWN", said_before)
			&& app.said_line("TABLET MOTION 160,120", said_before)
			&& app.said_line("TABLET UP", said_before)
			&& app.said_line("TABLET OUT", said_before),
		"the tip went down, moved and lifted, and the pen left range");
	check(plain.start_client("CANVAS_TOUCH_CLIENT",
			{"--size=300x200", "--title=touch-plain", "--no-tablet", "--no-touch"}),
		"a client without touch or tablet support started");
	return after(1000);
}

Result the_plain_client_mapped() {
	if (auto wait = wait_for(titled("touch-plain") != nullptr && settled()
			&& plain.said_line("MAPPED 300x200"), {.budget_ms = mapping_budget_ms})) {
		return *wait;
	}
	View *view = titled("touch-plain");
	check(view != nullptr, "it mapped as a view");
	said_before = plain.said().size();
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
	return after(500);
}

Result the_plain_client_saw_clicks() {
	check(plain.said_line("POINTER BUTTON 272 pressed 40,30", said_before)
			&& plain.said_line("POINTER BUTTON 272 released 40,30", said_before),
		std::format("to a client without touch support a tap is a click where it "
			"landed ({})", plain.since(said_before)));
	check(plain.said_line("POINTER BUTTON 272 pressed 60,60", said_before)
			&& plain.said_line("POINTER BUTTON 272 released 60,60", said_before),
		"and without tablet support the pen's tip is the left button");
	check(plain.said_line("POINTER BUTTON 273 pressed 60,60", said_before)
			&& plain.said_line("POINTER BUTTON 273 released 60,60", said_before),
		"and its barrel button the right");
	// The tip on a title bar drags the window, as the mouse does.
	View *view = titled("touch-plain");
	if (view == nullptr) {
		return after(300);
	}
	frame_before = view->frame_box();
	Point p = title_point(*view, 60);
	pen_proximity(p.x, p.y, true);
	pen_tip(p.x, p.y, true);
	for (int s = 1; s <= 10; s++) {
		pen_move(p.x - 15 * s, p.y + 5 * s);
	}
	pen_tip(p.x - 150, p.y + 50, false);
	pen_proximity(p.x - 150, p.y + 50, false);
	return after(500);
}

Result the_pen_dragged_the_title_bar() {
	View *view = titled("touch-plain");
	wlr_box now = view != nullptr ? view->frame_box() : wlr_box{};
	check(view != nullptr && now.x - frame_before.x == -150 && now.y - frame_before.y == 50,
		std::format("the pen's tip dragged a window by its title bar ({},{})",
			now.x - frame_before.x, now.y - frame_before.y));
	if (!x11.start_client("CANVAS_X11_CLIENT", {"--size=300x200", "--title=x11-touch"})) {
		return go_to("the pen reached the X11 window", 300); // no X11 client on this machine
	}
	return after(1500);
}

Result the_x11_client_mapped() {
	if (auto wait = wait_for(titled("x11-touch") != nullptr && settled(),
			{.budget_ms = mapping_budget_ms})) {
		return *wait;
	}
	View *view = titled("x11-touch");
	check(view != nullptr, "the X11 client's window mapped as a view");
	said_before = x11.said().size();
	if (view != nullptr) {
		Point p = on_screen(*view, 80, 60);
		finger_down(40, p.x, p.y);
		finger_up(40);
	}
	return after(600);
}

Result the_tap_reached_the_x11_window() {
	check(said_button_near(x11, 1, {80, 60}, said_before),
		std::format("a tap reached the X11 window as its press ({})", x11.since(said_before)));
	said_before = x11.said().size();
	// The tip lifts in a later step: Xwayland reads the tip once per
	// tablet frame, and a down and an up in one frame cancel out.
	View *view = titled("x11-touch");
	if (view != nullptr) {
		Point p = on_screen(*view, 120, 90);
		pen_proximity(p.x, p.y, true);
		pen_tip(p.x, p.y, true);
	}
	return after(200);
}

Result the_pen_lifts() {
	View *view = titled("x11-touch");
	if (view != nullptr) {
		Point p = on_screen(*view, 120, 90);
		pen_tip(p.x, p.y, false);
		pen_proximity(p.x, p.y, false);
	}
	return after(600);
}

Result the_pen_reached_the_x11_window() {
	if (x11.pid() > 0) {
		check(said_button_near(x11, 1, {120, 90}, said_before),
			std::format("the pen's tip reached the X11 window as its press ({})",
				x11.since(said_before)));
	}
	x11.kill();
	plain.kill();
	app.kill();
	return after(300);
}

// A touchscreen naming an output acts on that one.
Result a_second_output() {
	std::free(touchscreen().output_name);
	touchscreen().output_name = strdup("HEADLESS-2");
	wlr_output *second = add_output(screen_w, screen_h);
	check(second != nullptr && std::string_view{second->name} == "HEADLESS-2",
		"a second output was added");
	return after(800);
}

Result a_tap_on_the_second_output() {
	// A quarter across: on the second output only if the touchscreen is
	// mapped to it, not to the first or to the whole layout.
	finger_down(50, screen_w / 4.0, screen_h / 2.0);
	finger_up(50);
	Output *under = server->output_at(server->cursor->x, server->cursor->y);
	check(under != nullptr && std::string_view{under->wlr->name} == "HEADLESS-2",
		std::format("a touchscreen naming the second output acts on it (the tap "
			"put the pointer on {})", under != nullptr ? under->wlr->name : "nothing"));
	return after(300);
}

} // namespace

int main() {
	if (!begin("touch", {"WAYLAND_CANVAS_BIN", "CANVAS_TOUCH_CLIENT", "CANVAS_LOCK_CLIENT"})) {
		return 2;
	}
	// A stub systemctl first on PATH: a run that is not a login starts no
	// session services, and the machine's own user manager is never spoken to.
	std::string stubs = scratch_dir("bin");
	write_script(stubs + "/systemctl", "#!/usr/bin/env bash\nexit 0\n");
	put_first_on_path(stubs);
	Options options;
	options.start_bar = false; // the suites never spawn the shell
	return run(options, {
		{"devices and the touch client", devices_and_client},
		{"client mapped", client_mapped},
		{"a tap", a_tap},
		{"the tap arrived, then a drag", tap_arrived_then_a_drag},
		{"the drag arrived, then a title-bar drag", drag_arrived_then_a_title_bar_drag},
		{"the title-bar drag moved the window", title_bar_drag_moved_it_then_the_overview},
		{"a finger held on the canvas", a_finger_held_on_the_canvas},
		{"the hold made a desktop", the_hold_made_a_desktop},
		{"two fingers look away", two_fingers_look_away},
		{"they looked away, then a pinch", looked_away_then_a_pinch},
		{"two fingers spread", two_fingers_spread},
		{"the pinch scaled the desktop", the_pinch_scaled_the_desktop},
		{"desktop 3", desktop_3},
		{"desktop 1", desktop_1},
		{"three fingers swipe right", three_fingers_swipe_right},
		{"they hopped right", they_hopped_right},
		{"four fingers swipe up", four_fingers_swipe_up},
		{"the overview opened", the_overview_opened},
		{"the picker opens", the_picker_opens},
		{"three fingers with the picker open", three_fingers_with_the_picker_open},
		{"four fingers with the picker open", four_fingers_with_the_picker_open},
		{"escape closes the picker", escape_closes_the_picker},
		{"the picker closed", the_picker_closed},
		{"they hop again", they_hop_again},
		{"a finger as the session locks", a_finger_as_the_session_locks},
		{"locked touch goes to the locker", locked_touch_goes_to_the_locker},
		{"locked touch arrived, then unlock", locked_touch_arrived_then_unlock},
		{"unlocked, then the pen", unlocked_then_the_pen},
		{"the pen arrived, then a plain client", the_pen_arrived_then_a_plain_client},
		{"the plain client mapped", the_plain_client_mapped},
		{"the plain client saw clicks", the_plain_client_saw_clicks},
		{"the pen dragged the title bar", the_pen_dragged_the_title_bar},
		{"the X11 client mapped", the_x11_client_mapped},
		{"the tap reached the X11 window", the_tap_reached_the_x11_window},
		{"the pen lifts", the_pen_lifts},
		{"the pen reached the X11 window", the_pen_reached_the_x11_window},
		{"a second output", a_second_output},
		{"a tap on the second output", a_tap_on_the_second_output},
	});
}
