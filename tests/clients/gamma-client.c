// A gamma-control client for the scenarios: takes every output's gamma
// table through zwlr_gamma_control_v1 and sets a ramp of its own, so the
// compositor's own night light can be proven to step aside while it holds
// one and to come back when it lets go. Prints a line at a time:
//
//   SIZE <n>     an output's ramp is n entries long (0: it has none)
//   SET <n>      n outputs have been given a table
//   FAILED       the compositor refused one
//
// SIGUSR1 lets every table go and exits. --blue dims red instead of blue,
// so the frame's change is unmistakable.
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
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

static struct zwlr_gamma_control_manager_v1 *manager;
static struct wl_list outputs;
static volatile sig_atomic_t release_now;
static int set_count;
static double red_scale = 1.0, green_scale = 1.0, blue_scale = 0.25;

struct output {
	struct wl_list link;
	struct wl_output *wl;
	struct zwlr_gamma_control_v1 *control;
	uint32_t ramp_size;
	int written;
};

// One ramp per channel, each scaled by its own factor.
static void write_table(struct output *output) {
	if (output->ramp_size == 0 || output->written) {
		return;
	}
	size_t count = output->ramp_size;
	size_t size = count * 3 * sizeof(uint16_t);
	void *mapped;
	int fd = shm_file(size, &mapped);
	if (fd < 0) {
		return;
	}
	uint16_t *table = mapped;
	for (size_t i = 0; i < count; i++) {
		double value = (double)i / (double)(count - 1) * 65535.0;
		table[i] = (uint16_t)(value * red_scale);
		table[count + i] = (uint16_t)(value * green_scale);
		table[2 * count + i] = (uint16_t)(value * blue_scale);
	}
	munmap(table, size);
	zwlr_gamma_control_v1_set_gamma(output->control, fd);
	close(fd);
	output->written = 1;
	set_count++;
	say("SET %d\n", set_count);
}

static void gamma_size(void *data, struct zwlr_gamma_control_v1 *control,
		uint32_t size) {
	struct output *output = data;
	output->ramp_size = size;
	say("SIZE %u\n", size);
	write_table(output);
}

static void gamma_failed(void *data, struct zwlr_gamma_control_v1 *control) {
	say("FAILED\n");
}

static const struct zwlr_gamma_control_v1_listener gamma_listener = {
	.gamma_size = gamma_size,
	.failed = gamma_failed,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name,
			&zwlr_gamma_control_manager_v1_interface, 1);
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
	release_now = 1;
}

int main(int argc, char **argv) {
	wl_list_init(&outputs);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--red") == 0) {
			red_scale = 0.25;
			blue_scale = 1.0;
		}
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "gamma-client: no Wayland display\n");
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (manager == NULL) {
		fprintf(stderr, "gamma-client: the compositor offers no gamma control\n");
		return 2;
	}
	signal(SIGUSR1, handle_sigusr1);
	struct output *output;
	wl_list_for_each(output, &outputs, link) {
		output->control = zwlr_gamma_control_manager_v1_get_gamma_control(
			manager, output->wl);
		zwlr_gamma_control_v1_add_listener(output->control, &gamma_listener, output);
	}
	wl_display_flush(display);

	struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
	while (!release_now) {
		wl_display_flush(display);
		if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_dispatch(display) == -1) {
				return 0;
			}
		} else {
			wl_display_dispatch_pending(display);
		}
	}
	wl_list_for_each(output, &outputs, link) {
		if (output->control != NULL) {
			zwlr_gamma_control_v1_destroy(output->control);
		}
	}
	wl_display_flush(display);
	wl_display_roundtrip(display);
	say("RELEASED\n");
	return 0;
}
