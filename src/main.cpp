#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "platform/ipc.h"
#include "core/server.h"

namespace {

constexpr char usage_text[] =
	"usage: wayland-canvas [-s <startup command>] [-S <scale>] [-d] [-B] [-h]\n"
	"       wayland-canvas msg <method> [<params as JSON>]\n"
	"\n"
	"An infinite-canvas Wayland compositor: virtual desktops are dashed\n"
	"frames you drag around freely on an unbounded zoomable canvas.\n"
	"\n"
	"Options:\n"
	"  -s <command>  run <command> at startup (through /bin/sh -c)\n"
	"  -S <scale>    fixed output scale (default: picked per output;\n"
	"                can also be set via $WAYLAND_CANVAS_SCALE)\n"
	"  -d            enable debug logging\n"
	"  -B            start without the shell (bar, toolbar, notes)\n"
	"  -h            show this help and exit\n"
	"\n"
	"Touchpad gestures:\n"
	"  3-finger swipe       hop to the nearest desktop in that direction,\n"
	"                       else one screen that way (as in the overview)\n"
	"  4-finger swipe up    zoom out into the overview of all desktops\n"
	"  Super + 2-finger pinch  scale the desktop shown (the picture only:\n"
	"                       windows keep their size); Super+0 back to 100 %\n"
	"  In the overview:\n"
	"    3-finger swipe     hop to the nearest desktop in that direction\n"
	"    3-finger pinch out tile that desktop's windows into a grid\n"
	"    3-finger pinch in  combine it with its nearest neighbour desktop\n"
	"    4-finger swipe down  zoom back into the highlighted target\n"
	"    4-finger swipe up    zoom out further\n"
	"    4-finger swipe sideways  swipe a desktop away (abandon it)\n"
	"    2-finger pinch     zoom the overview around the cursor\n"
	"\n"
	"Control socket, over $WAYLAND_CANVAS_SOCKET (wayland-canvas msg):\n"
	"  state              outputs, desktops and the theme mode\n"
	"  subscribe          print every event, one per line, until interrupted\n"
	"  launcher.toggle    toggle the app launcher (\"output\" param optional)\n"
	"  overview.toggle    toggle the overview there (\"output\" param optional)\n"
	"  theme.set          {\"mode\": \"light\"} or {\"mode\": \"dark\"}\n"
	"  item.set           put or update a canvas item (id, color, text, size, place)\n"
	"  item.remove        remove a canvas item by id\n"
	"  item.activate      report an item as double-clicked, by id\n"
	"  action.run         {\"name\": \"<plugin action>\", \"args\": {...}}\n"
	"  quit               end the session\n"
	"\n"
	"See the keyboard bindings in src/input/bindings.cpp.\n";

} // namespace

int main(int argc, char *argv[]) {
	Options options;
	bool debug = false;
	double scale = 0;
	bool have_scale = false;

	// The one non-compositor mode: speak the control socket's JSON-lines
	// protocol to a running compositor.
	if (argc > 1 && strcmp(argv[1], "msg") == 0) {
		return msg_command(argc - 1, argv + 1);
	}

	// The environment variable is the default; -S overrides it.
	const char *scale_env = getenv("WAYLAND_CANVAS_SCALE");
	if (scale_env != nullptr) {
		scale = strtod(scale_env, nullptr);
		have_scale = scale > 0;
	}

	int opt;
	while ((opt = getopt(argc, argv, "s:S:dBh")) != -1) {
		switch (opt) {
		case 's':
			options.startup_cmd = optarg;
			break;
		case 'S':
			scale = strtod(optarg, nullptr);
			have_scale = scale > 0;
			break;
		case 'd':
			debug = true;
			break;
		case 'B':
			options.start_bar = false;
			break;
		case 'h':
			std::fputs(usage_text, stdout);
			return 0;
		default:
			std::fputs(usage_text, stderr);
			return 1;
		}
	}
	if (have_scale) {
		options.scale = scale;
	}

	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, nullptr);

	Server server;
	if (!server.init(options)) {
		wlr_log(WLR_ERROR, "failed to initialize the server");
		return 1;
	}
	if (!server.start()) {
		return 1;
	}

	wl_display_run(server.display);
	return 0;
}
