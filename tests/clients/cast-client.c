// A window-cast client for the scenarios: names the window titled argv[1]
// through ext-foreign-toplevel-list, makes an ext-image-capture source of it
// and copies its frames one after another, as fast as the compositor
// answers, until the session stops. Prints a line at a time:
//
//   CAST READY <w> <h>        the first frame arrived
//   CAST STOPPED <n>          the compositor ended the session after n frames
//   CAST FAILED <reason>      a frame failed
//
// SIGTERM ends it.
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

static struct wl_shm *shm;
static struct ext_foreign_toplevel_list_v1 *list;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_manager;
static struct ext_image_copy_capture_manager_v1 *copy_manager;
static struct ext_image_copy_capture_session_v1 *session;
static struct ext_image_copy_capture_frame_v1 *frame;
static struct wl_buffer *buffer;
static const char *wanted_title;
static uint32_t width, height, format;
static uint32_t frames;
static int session_done;
static int stopped;
static volatile sig_atomic_t quit_now;
static int started;

static void say(const char *text, ...) {
	va_list args;
	va_start(args, text);
	vprintf(text, args);
	va_end(args);
	fflush(stdout);
}

static int make_buffer(void) {
	size_t stride = (size_t)width * 4;
	size_t size = stride * height;
	int fd = memfd_create("cast-client", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		return -1;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	if (buffer != NULL) {
		wl_buffer_destroy(buffer);
	}
	buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height,
		(int32_t)stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer != NULL ? 0 : -1;
}

static void request_frame(void);

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *f,
	uint32_t transform) {}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *f,
	int32_t x, int32_t y, int32_t w, int32_t h) {}

static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *f,
	uint32_t hi, uint32_t lo, uint32_t nsec) {}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
	frames++;
	if (frames == 1) {
		say("CAST READY %u %u\n", width, height);
	}
	ext_image_copy_capture_frame_v1_destroy(f);
	frame = NULL;
	if (!stopped) {
		request_frame();
	}
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f,
	uint32_t reason) {
	say("CAST FAILED %u\n", reason);
	ext_image_copy_capture_frame_v1_destroy(f);
	frame = NULL;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

static void request_frame(void) {
	if (frame != NULL || buffer == NULL) {
		return;
	}
	frame = ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)width,
		(int32_t)height);
	ext_image_copy_capture_frame_v1_capture(frame);
}

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *s,
	uint32_t w, uint32_t h) {
	width = w;
	height = h;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s,
	uint32_t fmt) {
	if (format == 0 || fmt == WL_SHM_FORMAT_ARGB8888) {
		format = fmt;
	}
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s,
	struct wl_array *device) {}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s,
	uint32_t fmt, struct wl_array *modifiers) {}

static void session_done_event(void *data, struct ext_image_copy_capture_session_v1 *s) {
	session_done = 1;
	if (make_buffer() == 0) {
		request_frame();
	}
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s) {
	stopped = 1;
	say("CAST STOPPED %u\n", frames);
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done_event,
	.stopped = session_stopped,
};

struct handle {
	char *title;
};

static void handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h) {}

static void handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
	struct handle *self = data;
	if (started || self->title == NULL || strcmp(self->title, wanted_title) != 0) {
		return;
	}
	started = 1;
	struct ext_image_capture_source_v1 *source =
		ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
			source_manager, h);
	session = ext_image_copy_capture_manager_v1_create_session(copy_manager, source, 0);
	ext_image_capture_source_v1_destroy(source);
	ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
}

static void handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *h,
	const char *title) {
	struct handle *self = data;
	free(self->title);
	self->title = strdup(title);
}

static void handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h,
	const char *app_id) {}

static void handle_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *h,
	const char *identifier) {}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
	.closed = handle_closed,
	.done = handle_done,
	.title = handle_title,
	.app_id = handle_app_id,
	.identifier = handle_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *l,
	struct ext_foreign_toplevel_handle_v1 *h) {
	struct handle *self = calloc(1, sizeof *self);
	ext_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, self);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *l) {}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version) {
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
		list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(list, &list_listener, NULL);
	} else if (strcmp(interface,
			ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
		source_manager = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
		copy_manager = wl_registry_bind(registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void handle_sigterm(int signal) {
	quit_now = 1;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: cast-client <window title>\n");
		return 2;
	}
	wanted_title = argv[1];
	struct sigaction action = {.sa_handler = handle_sigterm};
	sigaction(SIGTERM, &action, NULL);
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cast-client: no wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (shm == NULL || list == NULL || source_manager == NULL || copy_manager == NULL) {
		fprintf(stderr, "cast-client: the compositor lacks a protocol\n");
		return 1;
	}
	while (!quit_now && !stopped) {
		wl_display_flush(display);
		if (wl_display_prepare_read(display) != 0) {
			wl_display_dispatch_pending(display);
			continue;
		}
		struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
		int ready = poll(&pfd, 1, 100);
		if (ready > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_read_events(display) != 0
					|| wl_display_dispatch_pending(display) < 0) {
				return 0;
			}
		} else {
			wl_display_cancel_read(display);
		}
	}
	return 0;
}
