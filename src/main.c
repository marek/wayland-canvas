#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "server.h"

static const char usage_text[] =
	"usage: wayland-canvas [-s <startup command>] [-S <scale>] [-d] [-h]\n"
	"\n"
	"An infinite-canvas Wayland compositor: virtual desktops are dashed\n"
	"frames you drag around freely on an unbounded zoomable canvas.\n"
	"\n"
	"Options:\n"
	"  -s <command>  run <command> at startup (through /bin/sh -c)\n"
	"  -S <scale>    fixed output scale (default: picked per output;\n"
	"                can also be set via $WAYLAND_CANVAS_SCALE)\n"
	"  -d            enable debug logging\n"
	"  -h            show this help and exit\n"
	"\n"
	"Touchpad gestures:\n"
	"  3-finger swipe       pan the canvas anywhere; momentum on release,\n"
	"                       snapping onto a nearby desktop\n"
	"  4-finger swipe up    zoom out into the overview of all desktops\n"
	"  In the overview:\n"
	"    3-finger swipe     hop to the nearest desktop in that direction\n"
	"    3-finger pinch out tile that desktop's windows into a grid\n"
	"    3-finger pinch in  combine it with its nearest neighbour desktop\n"
	"    4-finger swipe down  zoom back into the highlighted target\n"
	"    4-finger swipe up    zoom out further\n"
	"    4-finger swipe sideways  swipe a desktop away (abandon it)\n"
	"    2-finger pinch     zoom the overview around the cursor\n"
	"\n"
	"See the keyboard bindings in src/config.h and src/input.c.\n";

int main(int argc, char *argv[]) {
	struct cv_options options = {0};
	bool debug = false;
	double scale = 0;
	bool have_scale = false;

	// The environment variable is the default; -S overrides it.
	const char *scale_env = getenv("WAYLAND_CANVAS_SCALE");
	if (scale_env != NULL) {
		scale = strtod(scale_env, NULL);
		have_scale = scale > 0;
	}

	int opt;
	while ((opt = getopt(argc, argv, "s:S:dh")) != -1) {
		switch (opt) {
		case 's':
			options.startup_cmd = optarg;
			break;
		case 'S':
			scale = strtod(optarg, NULL);
			have_scale = scale > 0;
			break;
		case 'd':
			debug = true;
			break;
		case 'h':
			fputs(usage_text, stdout);
			return 0;
		default:
			fputs(usage_text, stderr);
			return 1;
		}
	}
	if (have_scale) {
		options.scale = scale;
	}

	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, NULL);

	struct cv_server server = {0};
	if (!server_init(&server, &options)) {
		wlr_log(WLR_ERROR, "failed to initialize the server");
		return 1;
	}
	if (!server_start(&server)) {
		server_finish(&server);
		return 1;
	}

	wl_display_run(server.display);
	server_finish(&server);
	return 0;
}
