// A pointer-gestures client for the scenarios: a small solid square on the
// overlay layer in the output's top-left corner, so a pointer put there hands
// it the focus without a window changing any desktop. Prints MAPPED, POINTER
// ENTER, POINTER LEAVE and every swipe and pinch the compositor forwards
// (SWIPE|PINCH BEGIN <fingers>, UPDATE <dx> <dy> [<scale> <rotation>], END
// <cancelled>); deltas and rotation carry one decimal, a scale two.
// --size=<px> sets the side (8). SIGTERM ends it.
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
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static const uint32_t colour = 0xffcc3366;

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwp_pointer_gestures_v1 *gestures;
static struct wl_surface *surface;
static struct wl_pointer *pointer;
static int side = 8;
static int mapped;
static int closed;
static volatile sig_atomic_t quit_now;

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
		uint32_t serial, uint32_t width, uint32_t height) {
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (draw_solid(shm, surface, side, side, colour) == 0 && !mapped) {
		mapped = 1;
		say("MAPPED\n");
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	closed = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *entered, wl_fixed_t x, wl_fixed_t y) {
	say("POINTER ENTER\n");
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *left) {
	say("POINTER LEAVE\n");
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t x, wl_fixed_t y) {}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state) {}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value) {}

// A version-1 wl_pointer sends only these five events.
static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void swipe_begin(void *data, struct zwp_pointer_gesture_swipe_v1 *swipe,
		uint32_t serial, uint32_t time, struct wl_surface *on, uint32_t fingers) {
	say("SWIPE BEGIN %u\n", fingers);
}

static void swipe_update(void *data, struct zwp_pointer_gesture_swipe_v1 *swipe,
		uint32_t time, wl_fixed_t dx, wl_fixed_t dy) {
	say("SWIPE UPDATE %.1f %.1f\n", wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}

static void swipe_end(void *data, struct zwp_pointer_gesture_swipe_v1 *swipe,
		uint32_t serial, uint32_t time, int32_t cancelled) {
	say("SWIPE END %d\n", cancelled);
}

static const struct zwp_pointer_gesture_swipe_v1_listener swipe_listener = {
	.begin = swipe_begin,
	.update = swipe_update,
	.end = swipe_end,
};

static void pinch_begin(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch,
		uint32_t serial, uint32_t time, struct wl_surface *on, uint32_t fingers) {
	say("PINCH BEGIN %u\n", fingers);
}

static void pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch,
		uint32_t time, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t scale,
		wl_fixed_t rotation) {
	say("PINCH UPDATE %.1f %.1f %.2f %.1f\n", wl_fixed_to_double(dx),
		wl_fixed_to_double(dy), wl_fixed_to_double(scale),
		wl_fixed_to_double(rotation));
}

static void pinch_end(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch,
		uint32_t serial, uint32_t time, int32_t cancelled) {
	say("PINCH END %d\n", cancelled);
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
	.begin = pinch_begin,
	.update = pinch_update,
	.end = pinch_end,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t capabilities) {
	if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) || pointer != NULL) {
		return;
	}
	pointer = wl_seat_get_pointer(s);
	wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	struct zwp_pointer_gesture_swipe_v1 *swipe =
		zwp_pointer_gestures_v1_get_swipe_gesture(gestures, pointer);
	zwp_pointer_gesture_swipe_v1_add_listener(swipe, &swipe_listener, NULL);
	struct zwp_pointer_gesture_pinch_v1 *pinch =
		zwp_pointer_gestures_v1_get_pinch_gesture(gestures, pointer);
	zwp_pointer_gesture_pinch_v1_add_listener(pinch, &pinch_listener, NULL);
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
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 2);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zwp_pointer_gestures_v1_interface.name) == 0) {
		gestures = wl_registry_bind(registry, name,
			&zwp_pointer_gestures_v1_interface, 1);
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
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--size=", 7) == 0) {
			side = atoi(argv[i] + 7);
		} else {
			fprintf(stderr, "usage: gesture-client [--size=<px>]\n");
			return 2;
		}
	}
	if (side <= 0) {
		fprintf(stderr, "gesture-client: --size must be positive\n");
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "gesture-client: no Wayland display\n");
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || seat == NULL || layer_shell == NULL
			|| gestures == NULL) {
		fprintf(stderr, "gesture-client: the compositor lacks wl_compositor, "
			"wl_shm, wl_seat, layer shell or pointer gestures\n");
		return 2;
	}
	signal(SIGTERM, handle_sigterm);
	wl_seat_add_listener(seat, &seat_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		"gesture-reporter");
	zwlr_layer_surface_v1_add_listener(layer, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_size(layer, (uint32_t)side, (uint32_t)side);
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
