// A layer-shell client for the scenarios: one surface on the top layer in the
// namespace argv[1], a solid colour at the size the compositor configures
// (200x100 until it names one), so a plugin's surface on the canvas can be
// mapped, placed and pressed without the shell. Prints CONFIGURED <w> <h>,
// MAPPED and CLOSED. SIGTERM ends it.
#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "client-common.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

enum {
	default_width = 200,
	default_height = 100,
};

static const uint32_t colour = 0xff3366cc;

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_surface *surface;
static volatile sig_atomic_t quit_now;
static int closed;
static int mapped;

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
		uint32_t serial, uint32_t width, uint32_t height) {
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	int w = width != 0 ? (int)width : default_width;
	int h = height != 0 ? (int)height : default_height;
	say("CONFIGURED %d %d\n", w, h);
	if (draw_solid(shm, surface, w, h, colour) == 0 && !mapped) {
		mapped = 1;
		say("MAPPED\n");
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	closed = 1;
	say("CLOSED\n");
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
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
		fprintf(stderr, "usage: layer-client <namespace>\n");
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "layer-client: no Wayland display\n");
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || layer_shell == NULL) {
		fprintf(stderr, "layer-client: the compositor lacks wl_compositor, wl_shm or layer shell\n");
		return 2;
	}
	signal(SIGTERM, handle_sigterm);
	surface = wl_compositor_create_surface(compositor);
	struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, argv[1]);
	zwlr_layer_surface_v1_add_listener(layer, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer, default_width, default_height);
	wl_surface_commit(surface);
	wl_display_flush(display);

	struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
	while (!quit_now && !closed) {
		wl_display_flush(display);
		if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_dispatch(display) == -1) {
				return 0;
			}
		} else {
			wl_display_dispatch_pending(display);
		}
	}
	return 0;
}
