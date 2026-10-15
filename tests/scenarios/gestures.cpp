// End-to-end gesture scenario: the real compositor in-process on the headless
// backend, real foot clients, synthetic touchpad gestures through the Gesture
// entry points, screenshots. Writes only under $CANVAS_TMP (meson passes the
// repo's tmp/).
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "config.h"
#include "output.h"
#include "render.h"
#include "server.h"
#include "wlr.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
uint32_t t_ms = 10000;
pid_t grim_pid = -1;
std::optional<DesktopId> d1, d2, d3;
Camera cam0{};
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
	server->gesture.swipe_begin(t_ms, fingers);
	for (int i = 0; i < steps; i++) {
		t_ms += 8;
		server->gesture.swipe_update(t_ms, dx / steps, dy / steps);
	}
	t_ms += 8;
	server->gesture.swipe_end(t_ms, false);
}

void pinch(uint32_t fingers, double scale, int steps) {
	server->gesture.pinch_begin(t_ms, fingers);
	for (int i = 1; i <= steps; i++) {
		t_ms += 8;
		server->gesture.pinch_update(t_ms, 0, 0, 1 + (scale - 1) * i / steps, 0);
	}
	t_ms += 8;
	server->gesture.pinch_end(t_ms, false);
}

bool inside(const Rect &r, const wlr_box &b) {
	return r.x >= b.x - 0.5 && r.y >= b.y - 0.5 && r.x + r.w <= b.x + b.width + 0.5 &&
		r.y + r.h <= b.y + b.height + 0.5;
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
		cam0 = o->cam;
		swipe(3, -600, 0, 30); // fingers left: canvas follows, camera moves right
		return 1600;
	case 5:
		check(o->mode == Mode::normal && o->cam.zoom == 1, "still normal mode after pan");
		check(o->cam.cx > cam0.cx + 550,
			std::format("3-finger swipe panned + coasted (dx={:.0f})", o->cam.cx - cam0.cx));
		check(!o->coasting, "momentum settled");
		shot("03-panned-free-area");
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
		check(o->mode == Mode::overview, "4-finger swipe up opened the overview");
		check(o->cam.zoom < 0.5, std::format("overview is zoomed out (zoom {:.3f})", o->cam.zoom));
		check(o->target_desktop == d2, "overview target starts at the current desktop");
		shot("04-overview");
		return 400;
	case 9:
		swipe(3, -80, 0, 8); // 3 fingers left: jump to the desktop on the left
		return 500;
	case 10:
		check(o->target_desktop == d1, "3-finger left jumped to Desktop 1");
		shot("05-hop-left");
		return 400;
	case 11:
		swipe(3, 0, -80, 8); // up: no desktop that way -> free spot one screen up
		return 400;
	case 12:
		check(!o->target_desktop.has_value() && o->target.y == desk(d1)->frame().y - o->height,
			"no desktop above: target moved one screen up to a free spot");
		shot("06-free-spot-target");
		return 600;
	case 13:
		swipe(3, 0, 80, 8); // back down onto Desktop 1
		check(o->target_desktop == d1, "3-finger down returned to Desktop 1");
		pinch(3, 1.5, 10); // spread: tile Desktop 1
		return 500;
	case 14: {
		Rect a = one->frame_rect(), b = two->frame_rect();
		check(inside(a, desk(d1)->frame()) && inside(b, desk(d1)->frame()),
			"tiled windows stay inside Desktop 1");
		check(!a.intersects(b), "tiled windows do not overlap");
		check(std::fabs(a.w - b.w) < 2 && std::fabs(a.y - b.y) < 2, "two windows tiled side by side");
		shot("07-tiled");
		return 400;
	}
	case 15: {
		int before_x = three->x - desk(d2)->frame().x;
		pinch(3, 0.6, 10); // gather: combine nearest desktop (2) into Desktop 1
		check(server->canvas.desktops.size() == 1,
			"pinch-in combined Desktop 2 into Desktop 1");
		check(server->canvas.desktop_of(*three) == desk(d1)
			&& three->x - desk(d1)->frame().x == before_x,
			"window three moved into Desktop 1 keeping its relative position");
		d2 = std::nullopt;
		return 500;
	}
	case 16: {
		shot("08-combined");
		const wlr_box &frame = desk(d1)->frame();
		o->overview_new_desktop(frame.x + frame.width * 1.8, frame.y + frame.height / 2.0);
		d3 = o->target_desktop;
		check(d3.has_value() && server->canvas.desktops.size() == 2,
			"new desktop created in the overview");
		return 400;
	}
	case 17:
		shot("09-new-desktop");
		swipe(4, 320, 0, 20); // 4 fingers sideways: abandon the target desktop
		return 400;
	case 18: {
		check(server->canvas.desktops.size() == 1
			&& server->canvas.desktops.find(o->target_desktop) == nullptr,
			"4-finger sideways swipe abandoned the desktop");
		check(mapped_views() == 3, "abandoning kept every window");
		// Abandoning (by key) the desktop a swipe is sliding away must end that swipe cleanly.
		const wlr_box &frame = desk(d1)->frame();
		o->overview_new_desktop(frame.x + frame.width * 1.8, frame.y + frame.height / 2.0);
		server->gesture.swipe_begin(t_ms += 8, 4);
		for (int i = 0; i < 6; i++) {
			server->gesture.swipe_update(t_ms += 8, 20, 0);
		}
		check(server->gesture.kind == GestureKind::abandon,
			"4-finger sideways swipe is sliding a desktop away");
		o->overview_abandon(o->target_desktop);
		for (int i = 0; i < 6; i++) {
			server->gesture.swipe_update(t_ms += 8, 20, 0);
		}
		server->gesture.swipe_end(t_ms += 8, false);
		check(server->canvas.desktops.size() == 1 && server->gesture.kind == GestureKind::none,
			"abandoning the swiped desktop mid-swipe ends the swipe cleanly");
		o->overview_set_target(desk(d1)->frame(), d1, true);
		return 400;
	}
	case 19:
		swipe(4, 0, 300, 20); // 4 fingers down: zoom into the target
		return 700;
	case 20: {
		check(o->mode == Mode::normal && o->cam.zoom == 1, "4-finger down returned to normal mode");
		Camera want = camera_for_box(desk(d1)->frame());
		check(std::fabs(o->cam.cx - want.cx) < 1 && std::fabs(o->cam.cy - want.cy) < 1,
			"landed on Desktop 1");
		shot("10-desktop1");
		return 400;
	}
	case 21:
		one->set_minimized(true);
		check(one->minimized && server->focused_view != one,
			"minimize hides the window and moves focus");
		o->overview_enter();
		return 600;
	case 22:
		shot("11-overview-shelf");
		return 400;
	case 23:
		one->activate();
		check(!one->minimized, "restore from minimized");
		o->overview_target_point(desk(d1)->frame().x - 3000, desk(d1)->frame().y + 2000);
		o->overview_exit();
		return 700;
	case 24: {
		check(o->mode == Mode::normal && !o->target_desktop.has_value() &&
			std::fabs(o->cam.cx - (desk(d1)->frame().x - 3000)) < 1,
			"clicking empty overview canvas lands on a free area");
		shot("12-free-area");
		return 400;
	}
	case 25:
		o->goto_direction(1, 0); // Super+Ctrl+Right: nearest desktop to the right
		return 600;
	case 26: {
		Camera want = camera_for_box(desk(d1)->frame());
		check(std::fabs(o->cam.cx - want.cx) < 1 && std::fabs(o->cam.cy - want.cy) < 1,
			"keyboard jump to the desktop in that direction");
		return 0;
	}
	default:
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
	mkdir_p(shot_dir);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *data) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display);
	wl_event_source_remove(timer);
	server.reset();
	return failures != 0;
}
