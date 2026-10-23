// A taskbar for the scenarios: lists every window through
// zwlr_foreign_toplevel_manager_v1 and asks for changes to them, so what the
// compositor publishes of a window, and how it answers a taskbar's requests,
// is read from what a client received. Prints a line at a time:
//
//   TITLE <title>              a title event
//   STATE <title> <app_id> <s> a window's state after a done event; <s> is
//                              the set of maximized, minimized, activated and
//                              fullscreen, comma-separated, or "-"
//   OUTPUT <enter|leave> <title>
//   CLOSED <title>
//
// Requests arrive as lines "<action> <title>" on the FIFO named by argv[1]:
// maximize, unmaximize, minimize, unminimize, fullscreen, unfullscreen,
// activate and close. Titles hold no spaces.
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "client-common.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

static struct zwlr_foreign_toplevel_manager_v1 *manager;
static struct wl_seat *seat;
static struct wl_list toplevels;

struct toplevel {
	struct wl_list link;
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *title, *app_id;
	uint32_t state; // bit n set: state value n
};

static void set_text(char **field, const char *text) {
	free(*field);
	*field = strdup(text);
}

static void handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *title) {
	struct toplevel *toplevel = data;
	set_text(&toplevel->title, title);
	say("TITLE %s\n", title);
}

static void handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *app_id) {
	set_text(&((struct toplevel *)data)->app_id, app_id);
}

static void handle_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct wl_output *output) {
	say("OUTPUT enter %s\n", ((struct toplevel *)data)->title);
}

static void handle_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct wl_output *output) {
	say("OUTPUT leave %s\n", ((struct toplevel *)data)->title);
}

static void handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct wl_array *state) {
	struct toplevel *toplevel = data;
	toplevel->state = 0;
	uint32_t *value;
	wl_array_for_each(value, state) {
		toplevel->state |= 1u << *value;
	}
}

static void handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
	struct toplevel *toplevel = data;
	static const struct {
		uint32_t value;
		const char *name;
	} names[] = {
		{ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED, "maximized"},
		{ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED, "minimized"},
		{ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED, "activated"},
		{ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN, "fullscreen"},
	};
	char states[64] = "";
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (toplevel->state & (1u << names[i].value)) {
			strcat(states, states[0] != '\0' ? "," : "");
			strcat(states, names[i].name);
		}
	}
	say("STATE %s %s %s\n", toplevel->title != NULL ? toplevel->title : "?",
		toplevel->app_id != NULL ? toplevel->app_id : "?",
		states[0] != '\0' ? states : "-");
}

static void handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
	struct toplevel *toplevel = data;
	say("CLOSED %s\n", toplevel->title != NULL ? toplevel->title : "?");
	wl_list_remove(&toplevel->link);
	zwlr_foreign_toplevel_handle_v1_destroy(handle);
	free(toplevel->title);
	free(toplevel->app_id);
	free(toplevel);
}

static void handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct zwlr_foreign_toplevel_handle_v1 *parent) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
	.title = handle_title,
	.app_id = handle_app_id,
	.output_enter = handle_output_enter,
	.output_leave = handle_output_leave,
	.state = handle_state,
	.done = handle_done,
	.closed = handle_closed,
	.parent = handle_parent,
};

static void manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
		struct zwlr_foreign_toplevel_handle_v1 *handle) {
	struct toplevel *toplevel = calloc(1, sizeof *toplevel);
	toplevel->handle = handle;
	wl_list_insert(&toplevels, &toplevel->link);
	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, toplevel);
}

static void manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
	.toplevel = manager_toplevel,
	.finished = manager_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name,
			&zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(manager, &manager_listener, NULL);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		// Bound, so the compositor can tell this client which output a window is on.
		wl_registry_bind(registry, name, &wl_output_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && seat == NULL) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void request(const char *action, const char *title) {
	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link) {
		if (toplevel->title == NULL || strcmp(toplevel->title, title) != 0) {
			continue;
		}
		struct zwlr_foreign_toplevel_handle_v1 *handle = toplevel->handle;
		if (strcmp(action, "maximize") == 0) {
			zwlr_foreign_toplevel_handle_v1_set_maximized(handle);
		} else if (strcmp(action, "unmaximize") == 0) {
			zwlr_foreign_toplevel_handle_v1_unset_maximized(handle);
		} else if (strcmp(action, "minimize") == 0) {
			zwlr_foreign_toplevel_handle_v1_set_minimized(handle);
		} else if (strcmp(action, "unminimize") == 0) {
			zwlr_foreign_toplevel_handle_v1_unset_minimized(handle);
		} else if (strcmp(action, "fullscreen") == 0) {
			zwlr_foreign_toplevel_handle_v1_set_fullscreen(handle, NULL);
		} else if (strcmp(action, "unfullscreen") == 0) {
			zwlr_foreign_toplevel_handle_v1_unset_fullscreen(handle);
		} else if (strcmp(action, "activate") == 0) {
			zwlr_foreign_toplevel_handle_v1_activate(handle, seat);
		} else if (strcmp(action, "close") == 0) {
			zwlr_foreign_toplevel_handle_v1_close(handle);
		}
		return;
	}
}

// Runs the complete lines of what the FIFO has delivered.
static void read_requests(int fd) {
	static char pending[256];
	static size_t used;
	ssize_t got = read(fd, pending + used, sizeof pending - used - 1);
	if (got <= 0) {
		return;
	}
	used += (size_t)got;
	pending[used] = '\0';
	char *line = pending, *end;
	while ((end = strchr(line, '\n')) != NULL) {
		*end = '\0';
		char *space = strchr(line, ' ');
		if (space != NULL) {
			*space = '\0';
			request(line, space + 1);
		}
		line = end + 1;
	}
	used -= (size_t)(line - pending);
	memmove(pending, line, used);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: foreign-client <fifo>\n");
		return 2;
	}
	wl_list_init(&toplevels);
	int requests = open(argv[1], O_RDWR | O_NONBLOCK);
	if (requests < 0) {
		fprintf(stderr, "foreign-client: cannot open %s\n", argv[1]);
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "foreign-client: no Wayland display\n");
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (manager == NULL || seat == NULL) {
		fprintf(stderr, "foreign-client: the compositor offers no foreign-toplevel management\n");
		return 2;
	}
	say("READY\n");

	struct pollfd fds[2] = {
		{.fd = wl_display_get_fd(display), .events = POLLIN},
		{.fd = requests, .events = POLLIN},
	};
	for (;;) {
		wl_display_flush(display);
		if (poll(fds, 2, 100) > 0) {
			if ((fds[1].revents & POLLIN) != 0) {
				read_requests(requests);
			}
			if ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(display) == -1) {
				return 0;
			}
		}
		wl_display_dispatch_pending(display);
	}
}
