// End-to-end device-level input scenario: drives the compositor through fake
// wlr_pointer / wlr_keyboard devices announced via backend new_input, i.e.
// the same path libinput takes. Writes only under $CANVAS_TMP (meson passes
// the repo's tmp/).
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/util/log.h>
#include "config.h"
#include "server.h"

static struct cv_server server;
static struct wl_event_source *timer;
static int step, failures, retries;
static uint32_t t = 50000;
static pid_t grim_pid = -1;
static struct wlr_pointer ptr;
static struct wlr_keyboard kbd;
static const struct wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
static const struct wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
static struct wlr_box saved;
static int saved_x;
static struct cv_view *closed_view; /* compared by address only, after it is freed */
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

static struct cv_view *titled(const char *title) {
	struct cv_view *v;
	wl_list_for_each(v, &server.views, link) {
		if (v->mapped && v->toplevel->title && strcmp(v->toplevel->title, title) == 0) {
			return v;
		}
	}
	return NULL;
}

static int mapped(void) {
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

/* Pointer at output-local logical (x, y) on the single 1280x720 output. */
static void move_to(double x, double y) {
	t += 8;
	struct wlr_pointer_motion_absolute_event ev = {
		.pointer = &ptr, .time_msec = t, .x = x / 1280.0, .y = y / 720.0};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

static void button(uint32_t code, bool pressed) {
	t += 8;
	struct wlr_pointer_button_event ev = {.pointer = &ptr, .time_msec = t, .button = code,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&ptr.events.button, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

static void click(double x, double y, uint32_t code) {
	move_to(x, y);
	button(code, true);
	button(code, false);
}

static void drag(double x0, double y0, double x1, double y1) {
	move_to(x0, y0);
	button(BTN_LEFT, true);
	for (int i = 1; i <= 10; i++) {
		move_to(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
	}
	button(BTN_LEFT, false);
}

static void key(uint32_t code, bool pressed) {
	t += 8;
	struct wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

/* Press mods..., tap k, release mods. */
static void chord(uint32_t k, int nmods, const uint32_t *mods) {
	for (int i = 0; i < nmods; i++) key(mods[i], true);
	key(k, true);
	key(k, false);
	for (int i = nmods - 1; i >= 0; i--) key(mods[i], false);
}

static void swipe(uint32_t fingers, double dx, double dy) {
	t += 8;
	struct wlr_pointer_swipe_begin_event b = {.pointer = &ptr, .time_msec = t, .fingers = fingers};
	wl_signal_emit_mutable(&ptr.events.swipe_begin, &b);
	for (int i = 0; i < 20; i++) {
		t += 8;
		struct wlr_pointer_swipe_update_event u = {.pointer = &ptr, .time_msec = t,
			.fingers = fingers, .dx = dx / 20, .dy = dy / 20};
		wl_signal_emit_mutable(&ptr.events.swipe_update, &u);
	}
	t += 8;
	struct wlr_pointer_swipe_end_event e = {.pointer = &ptr, .time_msec = t};
	wl_signal_emit_mutable(&ptr.events.swipe_end, &e);
}

static void local(struct cv_output *o, double cx, double cy, double *x, double *y) {
	output_canvas_to_local(o, cx, cy, x, y);
}

static const uint32_t SUPER[] = {KEY_LEFTMETA};
static const uint32_t SUPER_SHIFT[] = {KEY_LEFTMETA, KEY_LEFTSHIFT};
static const uint32_t SUPER_CTRL[] = {KEY_LEFTMETA, KEY_LEFTCTRL};
static const uint32_t SUPER_ALT[] = {KEY_LEFTMETA, KEY_LEFTALT};

static int run_step(void) {
	struct cv_output *o = out();
	struct cv_view *a = titled("a"), *b = titled("b");
	double x, y;
	switch (step) {
	case 0:
		ptr = (struct wlr_pointer){0};
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server.backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server.backend->events.new_input, &kbd.base);
		server_spawn("foot -T a sh -c 'echo a; exec sleep 600'");
		return 400;
	case 1:
		server_spawn("foot -T b sh -c 'echo b; exec sleep 600'");
		return 300;
	case 2:
		if (mapped() < 2 && retries++ < 100) return -100;
		CHECK(a && b && server.focused_view == b, "two windows, newest focused");
		return 100;
	case 3: { // title bar drag moves the window
		struct wlr_box f = view_frame_box(b);
		saved_x = b->x;
		int saved_y = b->y;
		drag(f.x + 80, f.y + 16, f.x + 180, f.y + 66);
		CHECK(b->x == saved_x + 100 && b->y == saved_y + 50, "title bar drag moved the window by (100,50) -> (%d,%d)",
			b->x - saved_x, b->y - saved_y);
		return 200;
	}
	case 4: { // double-click title toggles maximize
		struct wlr_box f = view_frame_box(b);
		move_to(f.x + 80, f.y + 16);
		click(f.x + 80, f.y + 16, BTN_LEFT);
		click(f.x + 80, f.y + 16, BTN_LEFT);
		CHECK(b->maximized, "double-click on the title maximized");
		return 400;
	}
	case 5: { // the maximize button now shows restore; click it
		struct wlr_box f = view_frame_box(b);
		shot("maximized");
		click(f.x + f.width - BUTTON_WIDTH * 1.5, f.y + 16, BTN_LEFT);
		CHECK(!b->maximized, "restore button restored the window");
		return 400;
	}
	case 6: { // resize from the top-left corner: right/bottom edges must stay put
		saved = view_content_box(b);
		struct wlr_box f = view_frame_box(b);
		drag(f.x - 3, f.y - 3, f.x - 83, f.y - 53);
		return 400;
	}
	case 7: {
		struct wlr_box c = view_content_box(b);
		// foot snaps to whole character cells, so allow up to one cell short.
		CHECK(c.width >= saved.width + 60 && c.height >= saved.height + 30 &&
			c.x + c.width == saved.x + saved.width && c.y + c.height == saved.y + saved.height,
			"top-left corner drag resized %dx%d -> %dx%d with the bottom-right corner fixed",
			saved.width, saved.height, c.width, c.height);
		return 100;
	}
	case 8: { // minimize button
		struct wlr_box f = view_frame_box(b);
		move_to(f.x + f.width - BUTTON_WIDTH * 2.5, f.y + 16);
		CHECK(b->hover == CV_DECO_MINIMIZE, "hovering the minimize button highlights it");
		shot("hover-minimize");
		return 300;
	}
	case 9: {
		struct wlr_box f = view_frame_box(b);
		click(f.x + f.width - BUTTON_WIDTH * 2.5, f.y + 16, BTN_LEFT);
		CHECK(b->minimized && server.focused_view == a, "minimize button hid it, focus moved to a");
		chord(KEY_M, 2, SUPER_SHIFT);
		CHECK(!b->minimized && server.focused_view == b, "Super+Shift+M restored it");
		return 200;
	}
	case 10: { // close button
		struct wlr_box f = view_frame_box(b);
		move_to(f.x + f.width - BUTTON_WIDTH * 0.5, f.y + 16);
		shot("hover-close");
		return 300;
	}
	case 11: {
		struct wlr_box f = view_frame_box(b);
		click(f.x + 60, f.y + 16, BTN_LEFT); // a title click: b is now the double-click candidate
		closed_view = b;
		click(f.x + f.width - BUTTON_WIDTH * 0.5, f.y + 16, BTN_LEFT);
		return 600;
	}
	case 12:
		CHECK(mapped() == 1 && titled("b") == NULL, "close button closed the window");
		CHECK(server.last_title_view != closed_view,
			"a closed window is no longer the double-click candidate");
		chord(KEY_ENTER, 1, SUPER);
		return 300;
	case 13:
		if (mapped() < 2 && retries++ < 100) return -100;
		CHECK(mapped() == 2, "Super+Return launched a terminal");
		chord(KEY_TAB, 1, SUPER);
		return 500;
	case 14:
		CHECK(o->mode == CV_MODE_OVERVIEW, "Super+Tab opened the overview");
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		return 500;
	case 15:
		CHECK(o->mode == CV_MODE_NORMAL && o->cam.zoom == 1, "Escape went back");
		for (int i = 0; i < 3; i++) chord(KEY_RIGHT, 2, SUPER_ALT);
		return 600;
	case 16:
		chord(KEY_D, 2, SUPER_CTRL);
		CHECK(wl_list_length(&server.desktops) == 2, "Super+Alt+Right x3 then Super+Ctrl+D pinned Desktop 2");
		chord(KEY_LEFT, 2, SUPER_CTRL);
		return 600;
	case 17: {
		struct cv_desktop *d1 = desktop_by_id(&server, 1);
		CHECK(d1 && desktop_showing(o) == d1, "Super+Ctrl+Left jumped back to Desktop 1");
		move_to(640, 360);
		double cx0 = o->cam.cx;
		swipe(3, 300, 0); // fingers right: canvas follows, camera moves left
		CHECK(o->cam.cx < cx0 - 250, "3-finger swipe events from the device panned (%.0f)", o->cam.cx - cx0);
		return 1200;
	}
	case 18:
		swipe(4, 0, -300);
		return 600;
	case 19:
		CHECK(o->mode == CV_MODE_OVERVIEW, "4-finger swipe events opened the overview");
		shot("overview");
		return 300;
	case 20: { // right-click on empty canvas below the desktops: new desktop
		struct cv_desktop *d1 = desktop_by_id(&server, 1);
		local(o, d1->box.x + d1->box.width / 2.0, d1->box.y + d1->box.height * 1.9, &x, &y);
		click(x, y, BTN_RIGHT);
		CHECK(wl_list_length(&server.desktops) == 3, "right-click on empty canvas added Desktop 3");
		return 400;
	}
	case 21: { // drag Desktop 3 by its tab onto Desktop 2: combine
		shot("three-desktops");
		struct cv_desktop *d2 = desktop_by_id(&server, 2), *d3 = desktop_by_id(&server, 3);
		struct wlr_box tab, tile, abandon;
		overview_tab_layout(o, d3, &tab, &tile, &abandon);
		double tx, ty;
		local(o, d2->box.x, d2->box.y, &tx, &ty);
		double sx, sy;
		local(o, d3->box.x, d3->box.y, &sx, &sy);
		drag(tab.x + 30, tab.y + 14, tab.x + 30 + (tx - sx), tab.y + 14 + (ty - sy));
		CHECK(wl_list_length(&server.desktops) == 2 && desktop_by_id(&server, 3) == NULL,
			"dropping a frame onto another combined them");
		return 400;
	}
	case 22: { // drag Desktop 2's frame over the windows of Desktop 1 (frames move, windows don't)
		struct cv_desktop *d2 = desktop_by_id(&server, 2);
		struct wlr_box tab, tile, abandon;
		overview_tab_layout(o, d2, &tab, &tile, &abandon);
		int ax = a->x;
		struct wlr_box before = d2->box;
		drag(tab.x + 30, tab.y + 14, tab.x + 30, tab.y + 14 + 40);
		CHECK(d2->box.y > before.y && a->x == ax, "dragging a frame moves only the frame");
		return 300;
	}
	case 23: { // click the Tile button on Desktop 1's tab
		struct cv_desktop *d1 = desktop_by_id(&server, 1);
		struct wlr_box tab, tile, abandon;
		overview_tab_layout(o, d1, &tab, &tile, &abandon);
		click(tile.x + tile.width / 2.0, tile.y + tile.height / 2.0, BTN_LEFT);
		return 500; // clients commit the new sizes asynchronously
	}
	case 24: {
		struct rect ra = view_frame_rect(a);
		struct cv_view *other = NULL, *v;
		wl_list_for_each(v, &server.views, link) if (v->mapped && v != a) other = v;
		CHECK(other && !rect_intersects(ra, view_frame_rect(other)), "tab Tile button tiled Desktop 1");
		shot("tiled");
		return 300;
	}
	case 25: // click empty canvas far away: zoom into that free area
		click(40, 300, BTN_LEFT);
		return 700;
	case 26:
		CHECK(o->mode == CV_MODE_NORMAL && o->target_desktop == NULL && desktop_showing(o) == NULL,
			"click on empty canvas landed on a free area");
		return 0;
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

int main(void) {
	wlr_log_init(WLR_ERROR, NULL);
	const char *root = getenv("CANVAS_TMP");
	if (root == NULL || !*root) {
		root = "tmp"; // meson passes the repo's tmp/ via env
	}
	snprintf(shot_dir, sizeof(shot_dir), "%s/scenarios/input", root);
	mkdir_p(shot_dir);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	setenv("TERMINAL", "foot", 1);
	struct cv_options options = {0};
	if (!server_init(&server, &options) || !server_start(&server)) {
		return 2;
	}
	timer = wl_event_loop_add_timer(server.loop, handle_timer, NULL);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server.display);
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server_finish(&server);
	return failures != 0;
}
