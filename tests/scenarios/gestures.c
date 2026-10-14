// End-to-end gesture scenario: the real compositor in-process on the headless
// backend, real foot clients, synthetic touchpad gestures through gesture_*()
// screenshots. Writes only under $CANVAS_TMP (meson passes the repo's tmp/).
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "config.h"
#include "server.h"

static struct cv_server server;
static struct wl_event_source *timer;
static int step, failures, retries;
static uint32_t t_ms = 10000;
static pid_t grim_pid = -1;
static struct cv_desktop *d1, *d2, *d3;
static struct camera cam0;
static char shot_dir[512];

#define CHECK(cond, ...) do { \
	printf("%s: ", (cond) ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
	fflush(stdout); if (!(cond)) failures++; } while (0)

static void mkdir_p(const char *path) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(buf, 0755);
			*p = '/';
		}
	}
	if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "mkdir %s: %s\n", buf, strerror(errno));
	}
}

static struct cv_output *out(void) {
	struct cv_output *o;
	return wl_container_of(server.outputs.next, o, link);
}

static struct cv_view *view_titled(const char *title) {
	struct cv_view *v;
	wl_list_for_each(v, &server.views, link) {
		if (v->mapped && v->toplevel->title && strcmp(v->toplevel->title, title) == 0) {
			return v;
		}
	}
	return NULL;
}

static int mapped_views(void) {
	int n = 0;
	struct cv_view *v;
	wl_list_for_each(v, &server.views, link) {
		n += v->mapped;
	}
	return n;
}

static void shot(const char *name) {
	char path[600];
	snprintf(path, sizeof(path), "%s/%s.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path, (char *)NULL);
		_exit(127);
	}
}

static void swipe(uint32_t fingers, double dx, double dy, int steps) {
	gesture_swipe_begin(&server, t_ms, fingers);
	for (int i = 0; i < steps; i++) {
		t_ms += 8;
		gesture_swipe_update(&server, t_ms, dx / steps, dy / steps);
	}
	t_ms += 8;
	gesture_swipe_end(&server, t_ms, false);
}

static void pinch(uint32_t fingers, double scale, int steps) {
	gesture_pinch_begin(&server, t_ms, fingers);
	for (int i = 1; i <= steps; i++) {
		t_ms += 8;
		gesture_pinch_update(&server, t_ms, 0, 0, 1 + (scale - 1) * i / steps, 0);
	}
	t_ms += 8;
	gesture_pinch_end(&server, t_ms, false);
}

static bool inside(struct rect r, struct wlr_box b) {
	return r.x >= b.x - 0.5 && r.y >= b.y - 0.5 && r.x + r.w <= b.x + b.width + 0.5 &&
		r.y + r.h <= b.y + b.height + 0.5;
}

static int run_step(void) {
	struct cv_output *o = out();
	struct cv_view *one = view_titled("one"), *two = view_titled("two"), *three = view_titled("three");
	switch (step) {
	case 0:
		server_spawn("foot -T one sh -c 'echo window one; exec sleep 600'");
		server_spawn("foot -T two sh -c 'echo window two; exec sleep 600'");
		server_spawn("foot -T three sh -c 'echo window three; exec sleep 600'");
		return 300;
	case 1:
		if (mapped_views() < 3 && retries++ < 100) {
			return -100; // retry
		}
		CHECK(mapped_views() == 3, "three foot windows mapped (%d)", mapped_views());
		CHECK(wl_list_length(&server.desktops) == 1, "Desktop 1 exists at start");
		d1 = wl_container_of(server.desktops.next, d1, link);
		CHECK(three && one && two && three->ssd, "foot negotiated server-side decorations");
		shot("01-three-windows");
		return 400;
	case 2: {
		// Title bar hit-testing at the close/maximize/minimize buttons.
		struct wlr_box f = view_frame_box(three);
		uint32_t edges = 0;
		CHECK(deco_hit(three, f.x + f.width - 10, f.y + 10, 1, &edges) == CV_DECO_CLOSE, "close button hit");
		CHECK(deco_hit(three, f.x + f.width - 50, f.y + 10, 1, &edges) == CV_DECO_MAXIMIZE, "maximize button hit");
		CHECK(deco_hit(three, f.x + f.width - 100, f.y + 10, 1, &edges) == CV_DECO_MINIMIZE, "minimize button hit");
		CHECK(deco_hit(three, f.x + 60, f.y + 10, 1, &edges) == CV_DECO_TITLE, "title hit");
		CHECK(deco_hit(three, f.x + f.width + 3, f.y + f.height + 3, 1, &edges) == CV_DECO_RESIZE &&
			edges == (WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM), "bottom-right resize corner");
		view_set_maximized(three, true);
		return 500;
	}
	case 3: {
		struct rect fr = view_frame_rect(three);
		struct rect vp = output_viewport(o);
		CHECK(fabs(fr.x - vp.x) < 1 && fabs(fr.w - vp.w) < 1 && fabs(fr.y - vp.y) < 1 &&
			fabs(fr.h - vp.h) < 1, "maximized frame fills the screen (%.0f,%.0f %.0fx%.0f)",
			fr.x, fr.y, fr.w, fr.h);
		shot("02-maximized");
		return 400;
	}
	case 4:
		view_set_maximized(three, false);
		cam0 = o->cam;
		swipe(3, -600, 0, 30); // fingers left: canvas follows, camera moves right
		return 1600;
	case 5:
		CHECK(o->mode == CV_MODE_NORMAL && o->cam.zoom == 1, "still normal mode after pan");
		CHECK(o->cam.cx > cam0.cx + 550, "3-finger swipe panned + coasted (dx=%.0f)", o->cam.cx - cam0.cx);
		CHECK(!o->coasting, "momentum settled");
		shot("03-panned-free-area");
		return 400;
	case 6:
		canvas_pin_desktop(o);
		CHECK(wl_list_length(&server.desktops) == 2, "pinned the free area as Desktop 2");
		d2 = desktop_showing(o);
		CHECK(d2 && d2 != d1 && d2->id == 2, "Desktop 2 is what we're looking at");
		view_move(three, d2->box.x + 200, d2->box.y + 150);
		CHECK(desktop_of_view(&server, three) == d2, "window three now belongs to Desktop 2");
		return 300;
	case 7:
		swipe(4, 0, -300, 20); // 4 fingers up: overview
		return 700;
	case 8:
		CHECK(o->mode == CV_MODE_OVERVIEW, "4-finger swipe up opened the overview");
		CHECK(o->cam.zoom < 0.5, "overview is zoomed out (zoom %.3f)", o->cam.zoom);
		CHECK(o->target_desktop == d2, "overview target starts at the current desktop");
		shot("04-overview");
		return 400;
	case 9:
		swipe(3, -80, 0, 8); // 3 fingers left: jump to the desktop on the left
		return 500;
	case 10:
		CHECK(o->target_desktop == d1, "3-finger left jumped to Desktop 1");
		shot("05-hop-left");
		return 400;
	case 11:
		swipe(3, 0, -80, 8); // up: no desktop that way -> free spot one screen up
		return 400;
	case 12:
		CHECK(o->target_desktop == NULL && o->target.y == d1->box.y - o->height,
			"no desktop above: target moved one screen up to a free spot");
		shot("06-free-spot-target");
		return 600;
	case 13:
		swipe(3, 0, 80, 8); // back down onto Desktop 1
		CHECK(o->target_desktop == d1, "3-finger down returned to Desktop 1");
		pinch(3, 1.5, 10); // spread: tile Desktop 1
		return 500;
	case 14: {
		struct rect a = view_frame_rect(one), b = view_frame_rect(two);
		CHECK(inside(a, d1->box) && inside(b, d1->box), "tiled windows stay inside Desktop 1");
		CHECK(!rect_intersects(a, b), "tiled windows do not overlap");
		CHECK(fabs(a.w - b.w) < 2 && fabs(a.y - b.y) < 2, "two windows tiled side by side");
		shot("07-tiled");
		return 400;
	}
	case 15: {
		int before_x = three->x - d2->box.x;
		pinch(3, 0.6, 10); // gather: combine nearest desktop (2) into Desktop 1
		CHECK(wl_list_length(&server.desktops) == 1, "pinch-in combined Desktop 2 into Desktop 1");
		CHECK(desktop_of_view(&server, three) == d1 && three->x - d1->box.x == before_x,
			"window three moved into Desktop 1 keeping its relative position");
		d2 = NULL;
		return 500;
	}
	case 16:
		shot("08-combined");
		overview_new_desktop(o, d1->box.x + d1->box.width * 1.8, d1->box.y + d1->box.height / 2.0);
		d3 = o->target_desktop;
		CHECK(d3 && wl_list_length(&server.desktops) == 2, "new desktop created in the overview");
		return 400;
	case 17:
		shot("09-new-desktop");
		swipe(4, 320, 0, 20); // 4 fingers sideways: abandon the target desktop
		return 400;
	case 18:
		CHECK(wl_list_length(&server.desktops) == 1 && o->target_desktop == NULL,
			"4-finger sideways swipe abandoned the desktop");
		CHECK(mapped_views() == 3, "abandoning kept every window");
		// Abandoning (by key) the desktop a swipe is sliding away must end that swipe cleanly.
		overview_new_desktop(o, d1->box.x + d1->box.width * 1.8, d1->box.y + d1->box.height / 2.0);
		gesture_swipe_begin(&server, t_ms += 8, 4);
		for (int i = 0; i < 6; i++) {
			gesture_swipe_update(&server, t_ms += 8, 20, 0);
		}
		CHECK(server.gesture.kind == CV_GESTURE_ABANDON, "4-finger sideways swipe is sliding a desktop away");
		overview_abandon(o, o->target_desktop);
		for (int i = 0; i < 6; i++) {
			gesture_swipe_update(&server, t_ms += 8, 20, 0);
		}
		gesture_swipe_end(&server, t_ms += 8, false);
		CHECK(wl_list_length(&server.desktops) == 1 && server.gesture.kind == CV_GESTURE_NONE,
			"abandoning the swiped desktop mid-swipe ends the swipe cleanly");
		overview_set_target(o, d1->box, d1, true);
		return 400;
	case 19:
		swipe(4, 0, 300, 20); // 4 fingers down: zoom into the target
		return 700;
	case 20: {
		CHECK(o->mode == CV_MODE_NORMAL && o->cam.zoom == 1, "4-finger down returned to normal mode");
		struct camera want = camera_for_box(d1->box);
		CHECK(fabs(o->cam.cx - want.cx) < 1 && fabs(o->cam.cy - want.cy) < 1, "landed on Desktop 1");
		shot("10-desktop1");
		return 400;
	}
	case 21:
		view_set_minimized(one, true);
		CHECK(one->minimized && server.focused_view != one, "minimize hides the window and moves focus");
		overview_enter(o);
		return 600;
	case 22:
		shot("11-overview-shelf");
		return 400;
	case 23:
		view_activate(one);
		CHECK(!one->minimized, "restore from minimized");
		overview_target_point(o, d1->box.x - 3000, d1->box.y + 2000);
		overview_exit(o);
		return 700;
	case 24: {
		CHECK(o->mode == CV_MODE_NORMAL && o->target_desktop == NULL &&
			fabs(o->cam.cx - (d1->box.x - 3000)) < 1, "clicking empty overview canvas lands on a free area");
		shot("12-free-area");
		return 400;
	}
	case 25:
		canvas_goto_direction(o, 1, 0); // Super+Ctrl+Right: nearest desktop to the right
		return 600;
	case 26: {
		struct camera want = camera_for_box(d1->box);
		CHECK(fabs(o->cam.cx - want.cx) < 1 && fabs(o->cam.cy - want.cy) < 1,
			"keyboard jump to the desktop in that direction");
		return 0;
	}
	default:
		printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
		wl_display_terminate(server.display);
		return -1;
	}
}

static int handle_timer(void *data) {
	if (grim_pid > 0) {
		if (waitpid(grim_pid, NULL, WNOHANG) == 0) {
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

int main(void) {
	wlr_log_init(WLR_ERROR, NULL);
	const char *root = getenv("CANVAS_TMP");
	if (root == NULL || !*root) {
		root = "tmp"; // meson passes the repo's tmp/ via env
	}
	snprintf(shot_dir, sizeof(shot_dir), "%s/scenarios/gestures", root);
	mkdir_p(shot_dir);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	struct cv_options options = {0};
	if (!server_init(&server, &options) || !server_start(&server)) {
		return 2;
	}
	timer = wl_event_loop_add_timer(server.loop, handle_timer, NULL);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server.display);
	wl_event_source_remove(timer);
	server_finish(&server);
	return failures != 0;
}
