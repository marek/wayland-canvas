// A touch, pointer and tablet-tool client for the scenarios: maps one
// window of one solid colour and reports on stdout, a line at a time,
// every touch, pointer and tablet-tool event that reaches it, so a
// scenario can assert what the compositor forwarded:
//
//   MAPPED <w>x<h>   the first configured buffer was committed
//   TOUCH DOWN <id> <x>,<y>
//   TOUCH MOTION <id> <x>,<y>
//   TOUCH UP <id>
//   TOUCH CANCEL
//   POINTER ENTER <x>,<y>
//   POINTER LEAVE
//   POINTER BUTTON <button> <pressed|released> <x>,<y>   at the last position
//   TABLET IN
//   TABLET MOTION <x>,<y>
//   TABLET PRESSURE <0..65535>     only when it changes
//   TABLET TILT <x>,<y>            degrees, only when it changes
//   TABLET DOWN
//   TABLET UP
//   TABLET BUTTON <button> <pressed|released>
//   TABLET OUT
//
// Coordinates are surface-local, rounded to the nearest integer.
// --size=WxH (400x300), --colour=0xRRGGBB (0x2060a0), --title=<text>;
// --no-tablet leaves the tablet manager unbound, so the pen must arrive
// as pointer events instead; --no-touch never takes a wl_touch, so a
// finger must arrive as the pointer. The window asks for the compositor's
// title bar (xdg-decoration, server side), so a scenario can drag it by
// that. The touch capability can appear after startup (a touchscreen
// connected later), so the wl_touch is taken when the capability shows up
// and released when it goes.
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
#include "xdg-shell-client-protocol.h"
#include "tablet-v2-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zwp_tablet_manager_v2 *tablet_manager;
static struct zxdg_decoration_manager_v1 *decoration_manager;
static struct zwp_tablet_seat_v2 *tablet_seat;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct wl_buffer *buffer;
static struct wl_touch *touch;
static struct wl_pointer *pointer;
static int pointer_x, pointer_y;
static int width = 400, height = 300;
static uint32_t fill = 0xff2060a0;
static const char *title = "touch-client";
static int want_tablet = 1;
static int want_touch = 1;
static int mapped;
static volatile sig_atomic_t exit_now;

// Per-tool memory of the last reported pressure and tilt, so repeats
// can be told from changes.
struct tool {
	uint32_t pressure;
	int have_pressure;
	double tilt_x, tilt_y;
	int have_tilt;
};

static int iround(double value) {
	return (int)(value < 0 ? value - 0.5 : value + 0.5);
}

// One shm buffer of a solid colour, made once: the window keeps its
// requested size through every configure.
static struct wl_buffer *make_buffer(void) {
	return solid_buffer(shm, width, height, fill);
}

static void surface_configure(void *data, struct xdg_surface *xdg,
		uint32_t serial) {
	xdg_surface_ack_configure(xdg, serial);
	if (buffer == NULL) {
		buffer = make_buffer();
		if (buffer == NULL) {
			return;
		}
	}
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	if (!mapped) {
		mapped = 1;
		say("MAPPED %dx%d\n", width, height);
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = surface_configure,
};

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	exit(0);
}

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t w, int32_t h, struct wl_array *states) {}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wb, uint32_t serial) {
	xdg_wm_base_pong(wb, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void touch_down(void *data, struct wl_touch *t, uint32_t serial,
		uint32_t time, struct wl_surface *s, int32_t id, wl_fixed_t x,
		wl_fixed_t y) {
	say("TOUCH DOWN %d %d,%d\n", id, iround(wl_fixed_to_double(x)),
		iround(wl_fixed_to_double(y)));
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial,
		uint32_t time, int32_t id) {
	say("TOUCH UP %d\n", id);
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time,
		int32_t id, wl_fixed_t x, wl_fixed_t y) {
	say("TOUCH MOTION %d %d,%d\n", id, iround(wl_fixed_to_double(x)),
		iround(wl_fixed_to_double(y)));
}

static void touch_cancel(void *data, struct wl_touch *t) {
	say("TOUCH CANCEL\n");
}

static void touch_frame(void *data, struct wl_touch *t) {}
static void touch_shape(void *data, struct wl_touch *t, int32_t id,
		wl_fixed_t major, wl_fixed_t minor) {}
static void touch_orientation(void *data, struct wl_touch *t, int32_t id,
		wl_fixed_t orientation) {}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
	.shape = touch_shape,
	.orientation = touch_orientation,
};

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	pointer_x = iround(wl_fixed_to_double(x));
	pointer_y = iround(wl_fixed_to_double(y));
	say("POINTER ENTER %d,%d\n", pointer_x, pointer_y);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s) {
	say("POINTER LEAVE\n");
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	pointer_x = iround(wl_fixed_to_double(x));
	pointer_y = iround(wl_fixed_to_double(y));
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	say("POINTER BUTTON %u %s %d,%d\n", button,
		state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released",
		pointer_x, pointer_y);
}

static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
		uint32_t axis, wl_fixed_t value) {}
static void pointer_axis_source(void *data, struct wl_pointer *p,
		uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
		uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p,
		uint32_t axis, int32_t discrete) {}
static void pointer_axis_value120(void *data, struct wl_pointer *p,
		uint32_t axis, int32_t value120) {}
static void pointer_axis_relative_direction(void *data, struct wl_pointer *p,
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

static void tool_proximity_in(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t serial, struct zwp_tablet_v2 *tablet,
		struct wl_surface *s) {
	say("TABLET IN\n");
}

static void tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *wl) {
	say("TABLET OUT\n");
}

static void tool_down(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t serial) {
	say("TABLET DOWN\n");
}

static void tool_up(void *data, struct zwp_tablet_tool_v2 *wl) {
	say("TABLET UP\n");
}

static void tool_motion(void *data, struct zwp_tablet_tool_v2 *wl,
		wl_fixed_t x, wl_fixed_t y) {
	say("TABLET MOTION %d,%d\n", iround(wl_fixed_to_double(x)),
		iround(wl_fixed_to_double(y)));
}

static void tool_pressure(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t pressure) {
	struct tool *tool = data;
	if (tool->have_pressure && tool->pressure == pressure) {
		return;
	}
	tool->have_pressure = 1;
	tool->pressure = pressure;
	say("TABLET PRESSURE %u\n", pressure);
}

static void tool_tilt(void *data, struct zwp_tablet_tool_v2 *wl,
		wl_fixed_t tilt_x, wl_fixed_t tilt_y) {
	struct tool *tool = data;
	double x = wl_fixed_to_double(tilt_x);
	double y = wl_fixed_to_double(tilt_y);
	if (tool->have_tilt && tool->tilt_x == x && tool->tilt_y == y) {
		return;
	}
	tool->have_tilt = 1;
	tool->tilt_x = x;
	tool->tilt_y = y;
	say("TABLET TILT %d,%d\n", iround(x), iround(y));
}

static void tool_button(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t serial, uint32_t button, uint32_t state) {
	say("TABLET BUTTON %u %s\n", button,
		state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED ?
			"pressed" : "released");
}

static void tool_removed(void *data, struct zwp_tablet_tool_v2 *wl) {
	zwp_tablet_tool_v2_destroy(wl);
	free(data);
}

static void tool_type(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t tool_type) {}
static void tool_hardware_serial(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t hi, uint32_t lo) {}
static void tool_hardware_id_wacom(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t hi, uint32_t lo) {}
static void tool_capability(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t capability) {}
static void tool_done(void *data, struct zwp_tablet_tool_v2 *wl) {}
static void tool_distance(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t distance) {}
static void tool_rotation(void *data, struct zwp_tablet_tool_v2 *wl,
		wl_fixed_t degrees) {}
static void tool_slider(void *data, struct zwp_tablet_tool_v2 *wl,
		int32_t position) {}
static void tool_wheel(void *data, struct zwp_tablet_tool_v2 *wl,
		wl_fixed_t degrees, int32_t clicks) {}
static void tool_frame(void *data, struct zwp_tablet_tool_v2 *wl,
		uint32_t time) {}

static const struct zwp_tablet_tool_v2_listener tablet_tool_listener = {
	.type = tool_type,
	.hardware_serial = tool_hardware_serial,
	.hardware_id_wacom = tool_hardware_id_wacom,
	.capability = tool_capability,
	.done = tool_done,
	.removed = tool_removed,
	.proximity_in = tool_proximity_in,
	.proximity_out = tool_proximity_out,
	.down = tool_down,
	.up = tool_up,
	.motion = tool_motion,
	.pressure = tool_pressure,
	.distance = tool_distance,
	.tilt = tool_tilt,
	.rotation = tool_rotation,
	.slider = tool_slider,
	.wheel = tool_wheel,
	.button = tool_button,
	.frame = tool_frame,
};

static void tablet_seat_tool_added(void *data, struct zwp_tablet_seat_v2 *ts,
		struct zwp_tablet_tool_v2 *wl) {
	// Every tool gets its own pressure and tilt memory.
	struct tool *tool = calloc(1, sizeof *tool);
	zwp_tablet_tool_v2_add_listener(wl, &tablet_tool_listener, tool);
}

static void tablet_seat_tablet_added(void *data, struct zwp_tablet_seat_v2 *ts,
		struct zwp_tablet_v2 *tablet) {}
static void tablet_seat_pad_added(void *data, struct zwp_tablet_seat_v2 *ts,
		struct zwp_tablet_pad_v2 *pad) {}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
	.tablet_added = tablet_seat_tablet_added,
	.tool_added = tablet_seat_tool_added,
	.pad_added = tablet_seat_pad_added,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps) {
	// The touch capability can show up or go away at any time, a
	// touchscreen being connected or unplugged.
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && want_touch) {
		if (touch == NULL) {
			touch = wl_seat_get_touch(s);
			wl_touch_add_listener(touch, &touch_listener, NULL);
		}
	} else if (touch != NULL) {
		wl_touch_release(touch);
		touch = NULL;
	}
	if (caps & WL_SEAT_CAPABILITY_POINTER) {
		if (pointer == NULL) {
			pointer = wl_seat_get_pointer(s);
			wl_pointer_add_listener(pointer, &pointer_listener, NULL);
		}
	} else if (pointer != NULL) {
		wl_pointer_release(pointer);
		pointer = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (want_tablet &&
			strcmp(interface, zwp_tablet_manager_v2_interface.name) == 0) {
		tablet_manager = wl_registry_bind(registry, name,
			&zwp_tablet_manager_v2_interface, 1);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		decoration_manager = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void handle_sigterm(int signal) {
	exit_now = 1;
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--size=", 7) == 0) {
			sscanf(argv[i] + 7, "%dx%d", &width, &height);
		} else if (strncmp(argv[i], "--colour=", 9) == 0) {
			fill = (uint32_t)strtoul(argv[i] + 9, NULL, 16) | 0xff000000;
		} else if (strncmp(argv[i], "--title=", 8) == 0) {
			title = argv[i] + 8;
		} else if (strcmp(argv[i], "--no-tablet") == 0) {
			want_tablet = 0;
		} else if (strcmp(argv[i], "--no-touch") == 0) {
			want_touch = 0;
		}
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "touch-client: no Wayland display\n");
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display); // the seat's capabilities
	if (compositor == NULL || shm == NULL || wm_base == NULL || seat == NULL) {
		fprintf(stderr, "touch-client: the compositor offers no seat or shell\n");
		return 2;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, title);
	if (decoration_manager != NULL) {
		struct zxdg_toplevel_decoration_v1 *decoration =
			zxdg_decoration_manager_v1_get_toplevel_decoration(decoration_manager,
				toplevel);
		zxdg_toplevel_decoration_v1_set_mode(decoration,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(surface);
	if (tablet_manager != NULL) {
		tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(tablet_manager,
			seat);
		zwp_tablet_seat_v2_add_listener(tablet_seat, &tablet_seat_listener,
			NULL);
	}
	wl_display_flush(display);

	// A poll of its own, so SIGTERM interrupting the wait is seen at
	// once instead of ending the loop as a dispatch error.
	signal(SIGTERM, handle_sigterm);
	struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
	while (!exit_now) {
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
