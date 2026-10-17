// A locker for the scenarios: takes ext-session-lock-v1, fills every output
// with one colour and reports what it is told on stdout, a line at a time:
//
//   LOCKED            the compositor confirmed the session is locked
//   DRAWN <n>         n lock surfaces have been drawn
//   KEY <keycode>     a key arrived on a lock surface
//   BUTTON <button>   a pointer button arrived on one
//   FINISHED          the compositor refused or ended this lock
//
// SIGUSR1 unlocks and exits; --inhibit-idle also holds an idle inhibitor,
// so the idle lock can be proven to stand down while one is alive.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-session-lock-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct ext_session_lock_manager_v1 *lock_manager;
static struct zwp_idle_inhibit_manager_v1 *inhibit_manager;
static struct ext_session_lock_v1 *lock;
static struct wl_list outputs;
static volatile sig_atomic_t unlock_now;
static int drawn;
static int inhibit_idle;
static uint32_t fill = 0xff3050a0;

struct output {
	struct wl_list link;
	struct wl_output *wl;
	struct ext_session_lock_surface_v1 *lock_surface;
	struct wl_surface *surface;
	struct zwp_idle_inhibitor_v1 *inhibitor;
	int width, height;
};

static void say(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	fflush(stdout);
}

// One shm buffer of a solid colour.
static struct wl_buffer *solid_buffer(int width, int height) {
	int stride = width * 4;
	int size = stride * height;
	int fd = memfd_create("lock-client", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		return NULL;
	}
	uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	for (int i = 0; i < width * height; i++) {
		data[i] = fill;
	}
	munmap(data, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void surface_configure(void *data,
		struct ext_session_lock_surface_v1 *lock_surface, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct output *output = data;
	ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
	output->width = (int)width;
	output->height = (int)height;
	struct wl_buffer *buffer = solid_buffer(output->width, output->height);
	if (buffer == NULL) {
		return;
	}
	wl_surface_attach(output->surface, buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, output->width, output->height);
	wl_surface_commit(output->surface);
	drawn++;
	say("DRAWN %d\n", drawn);
}

static const struct ext_session_lock_surface_v1_listener surface_listener = {
	.configure = surface_configure,
};

static void lock_locked(void *data, struct ext_session_lock_v1 *l) {
	say("LOCKED\n");
}

static void lock_finished(void *data, struct ext_session_lock_v1 *l) {
	say("FINISHED\n");
	exit(0);
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		say("KEY %u\n", key);
	}
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
		int32_t fd, uint32_t size) {
	close(fd);
}
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	struct wl_surface *surface) {}
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {}
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate,
	int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		say("BUTTON %u\n", button);
	}
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
	struct wl_surface *surface) {}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
	wl_fixed_t x, wl_fixed_t y) {}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
	uint32_t axis, wl_fixed_t value) {}
static void pointer_frame(void *data, struct wl_pointer *pointer) {}
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
	uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
	int32_t discrete) {}
static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis,
	int32_t value120) {}
static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer,
	uint32_t axis, uint32_t direction) {}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps) {
	if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		struct wl_keyboard *keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
	if (caps & WL_SEAT_CAPABILITY_POINTER) {
		struct wl_pointer *pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
		lock_manager = wl_registry_bind(registry, name,
			&ext_session_lock_manager_v1_interface, 1);
	} else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
		inhibit_manager = wl_registry_bind(registry, name,
			&zwp_idle_inhibit_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *output = calloc(1, sizeof *output);
		output->wl = wl_registry_bind(registry, name, &wl_output_interface, 3);
		wl_list_insert(&outputs, &output->link);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void handle_sigusr1(int signal) {
	unlock_now = 1;
}

int main(int argc, char **argv) {
	wl_list_init(&outputs);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--inhibit-idle") == 0) {
			inhibit_idle = 1;
		} else if (strncmp(argv[i], "--fill=", 7) == 0) {
			fill = (uint32_t)strtoul(argv[i] + 7, NULL, 16);
		}
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "lock-client: no Wayland display\n");
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display); // the seat's capabilities
	if (compositor == NULL || shm == NULL || lock_manager == NULL) {
		fprintf(stderr, "lock-client: the compositor offers no session lock\n");
		return 2;
	}

	signal(SIGUSR1, handle_sigusr1);
	lock = ext_session_lock_manager_v1_lock(lock_manager);
	ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
	struct output *output;
	wl_list_for_each(output, &outputs, link) {
		output->surface = wl_compositor_create_surface(compositor);
		output->lock_surface = ext_session_lock_v1_get_lock_surface(lock,
			output->surface, output->wl);
		ext_session_lock_surface_v1_add_listener(output->lock_surface,
			&surface_listener, output);
		if (inhibit_idle && inhibit_manager != NULL) {
			output->inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
				inhibit_manager, output->surface);
		}
	}
	wl_display_flush(display);

	// A poll of its own, so SIGUSR1 interrupting the wait is seen at once
	// instead of ending the loop as a dispatch error.
	struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
	while (!unlock_now) {
		wl_display_flush(display);
		if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_dispatch(display) == -1) {
				return 0;
			}
		} else {
			wl_display_dispatch_pending(display);
		}
	}
	ext_session_lock_v1_unlock_and_destroy(lock);
	wl_display_flush(display);
	wl_display_roundtrip(display);
	say("UNLOCKED\n");
	return 0;
}
