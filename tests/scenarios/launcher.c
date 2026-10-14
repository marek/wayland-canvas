// End-to-end launcher scenario through fake keyboard/pointer devices, plus a
// custom desktop entry installed into $XDG_DATA_HOME before server_init.
// Writes only under $CANVAS_TMP (meson passes the repo's tmp/).
#include <errno.h>
#include <linux/input-event-codes.h>
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
static int step, failures, retries, views_before;
static uint32_t t = 90000;
static pid_t grim_pid = -1;
static struct wlr_pointer ptr;
static struct wlr_keyboard kbd;
static const struct wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
static const struct wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
static char shot_dir[512];
static char xdg_dir[512];
static char marker_path[640];

#define CHECK(cond, ...) do { \
	printf("%s: ", (cond) ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
	fflush(stdout); if (!(cond)) failures++; } while (0)

static void mkdir_p(const char *path) {
	char buf[640];
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

static int mapped(void) {
	int n = 0;
	struct cv_view *v;
	wl_list_for_each(v, &server.views, link) {
		n += v->mapped;
	}
	return n;
}

static void shot(const char *name) {
	char path[700];
	snprintf(path, sizeof(path), "%s/%s.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path, (char *)NULL);
		_exit(127);
	}
}

static void key(uint32_t code, bool pressed) {
	t += 8;
	struct wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

static void tap(uint32_t code) {
	key(code, true);
	key(code, false);
}

static void type(const char *s) {
	static const struct { char c; uint32_t code; } map[] = {
		{'a', KEY_A}, {'b', KEY_B}, {'c', KEY_C}, {'d', KEY_D}, {'e', KEY_E}, {'f', KEY_F},
		{'g', KEY_G}, {'h', KEY_H}, {'i', KEY_I}, {'j', KEY_J}, {'k', KEY_K}, {'l', KEY_L},
		{'m', KEY_M}, {'n', KEY_N}, {'o', KEY_O}, {'p', KEY_P}, {'q', KEY_Q}, {'r', KEY_R},
		{'s', KEY_S}, {'t', KEY_T}, {'u', KEY_U}, {'v', KEY_V}, {'w', KEY_W}, {'x', KEY_X},
		{'y', KEY_Y}, {'z', KEY_Z}, {' ', KEY_SPACE},
	};
	for (; *s; s++) {
		for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
			if (map[i].c == *s) {
				tap(map[i].code);
			}
		}
	}
}

static void click(double x, double y) {
	t += 8;
	struct wlr_pointer_motion_absolute_event m = {
		.pointer = &ptr, .time_msec = t, .x = x / 1280.0, .y = y / 720.0};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &m);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
	for (int pressed = 1; pressed >= 0; pressed--) {
		t += 8;
		struct wlr_pointer_button_event b = {.pointer = &ptr, .time_msec = t, .button = BTN_LEFT,
			.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
		wl_signal_emit_mutable(&ptr.events.button, &b);
		wl_signal_emit_mutable(&ptr.events.frame, &ptr);
	}
}

static int run_step(void) {
	switch (step) {
	case 0:
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server.backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server.backend->events.new_input, &kbd.base);
		return 300;
	case 1:
		tap(KEY_LEFTMETA);
		CHECK(launcher_is_open(&server), "tapping the Windows key opened the launcher");
		shot("open");
		return 500;
	case 2: {
		struct cv_output *o = output_under_cursor(&server);
		struct wlr_box box;
		CHECK(launcher_texture(&server, o, &box) != NULL && box.width > 0, "launcher panel rendered (%dx%d)",
			box.width, box.height);
		type("foot");
		shot("filtered");
		return 500;
	}
	case 3:
		views_before = mapped();
		tap(KEY_ENTER);
		CHECK(!launcher_is_open(&server), "Enter launched the selection and closed the launcher");
		return 300;
	case 4:
		if (mapped() <= views_before && retries++ < 100) return -100;
		CHECK(mapped() == views_before + 1, "the launched app (foot) mapped a window");
		shot("launched");
		return 400;
	case 5: // logo used as a modifier must not open the launcher
		key(KEY_LEFTMETA, true);
		tap(KEY_T);
		key(KEY_LEFTMETA, false);
		CHECK(!launcher_is_open(&server), "Super+T did not open the launcher on release");
		return 100;
	case 6:
		tap(KEY_LEFTMETA);
		tap(KEY_ESC);
		CHECK(!launcher_is_open(&server), "Escape closed it");
		key(KEY_LEFTMETA, true);
		tap(KEY_SPACE);
		key(KEY_LEFTMETA, false);
		CHECK(launcher_is_open(&server), "Super+Space opened it");
		type("zzqqx");
		shot("no-match");
		return 500;
	case 7:
		click(20, 20);
		CHECK(!launcher_is_open(&server), "a click outside closed it");
		tap(KEY_LEFTMETA);
		type("fo");
		return 300;
	case 8: {
		// Click the first result row: launches it.
		struct cv_output *o = output_under_cursor(&server);
		struct wlr_box box;
		launcher_texture(&server, o, &box);
		views_before = mapped();
		click(box.x + box.width / 2.0, box.y + 12 + LAUNCHER_SEARCH_HEIGHT + 8 + LAUNCHER_ROW_HEIGHT / 2.0);
		CHECK(!launcher_is_open(&server), "clicking a result launched it and closed the launcher");
		return 300;
	}
	case 9:
		if (mapped() <= views_before && retries++ < 100) return -100;
		CHECK(mapped() == views_before + 1, "clicked app mapped a window");
		return 300;
	case 10: // the custom desktop entry from $XDG_DATA_HOME: find it by name
		tap(KEY_LEFTMETA);
		CHECK(launcher_is_open(&server), "launcher reopened for the custom entry");
		type("zydeco");
		shot("custom-entry");
		return 300;
	case 11:
		views_before = mapped();
		tap(KEY_ENTER);
		CHECK(!launcher_is_open(&server), "Enter launched the custom entry and closed the launcher");
		return 400;
	case 12:
		if (access(marker_path, F_OK) != 0 && retries++ < 100) return -100;
		CHECK(access(marker_path, F_OK) == 0, "the custom entry's Exec touched the marker file");
		if (mapped() <= views_before && retries++ < 200) return -100;
		CHECK(mapped() == views_before + 1, "the custom entry opened foot (window mapped)");
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
	t += 1000;
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

/* Installs a spec-valid custom entry into $XDG_DATA_HOME/applications; its
 * Exec uses double quotes, touches the marker file and opens foot. */
static void install_custom_entry(void) {
	char apps[640], desktop[768];
	snprintf(apps, sizeof(apps), "%s/applications", xdg_dir);
	mkdir_p(apps);
	unlink(marker_path);
	snprintf(desktop, sizeof(desktop), "%s/scenario-custom.desktop", apps);
	FILE *f = fopen(desktop, "w");
	if (f == NULL) {
		fprintf(stderr, "fopen %s: %s\n", desktop, strerror(errno));
		exit(2);
	}
	fprintf(f, "[Desktop Entry]\n");
	fprintf(f, "Type=Application\n");
	fprintf(f, "Name=Zydeco Marker\n");
	fprintf(f, "Exec=sh -c \"touch %s; exec foot -T scenariocustom\"\n", marker_path);
	fclose(f);
}

int main(void) {
	wlr_log_init(WLR_ERROR, NULL);
	const char *root = getenv("CANVAS_TMP");
	if (root == NULL || !*root) {
		root = "tmp"; // meson passes the repo's tmp/ via env
	}
	snprintf(shot_dir, sizeof(shot_dir), "%s/scenarios/launcher", root);
	snprintf(xdg_dir, sizeof(xdg_dir), "%s/scenarios/launcher/xdg", root);
	snprintf(marker_path, sizeof(marker_path), "%s/scenarios/launcher/custom-marker", root);
	mkdir_p(shot_dir);
	install_custom_entry();
	// Before server_init: apps_load() scans $XDG_DATA_HOME/applications first.
	setenv("XDG_DATA_HOME", xdg_dir, 1);
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
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server_finish(&server);
	return failures != 0;
}
