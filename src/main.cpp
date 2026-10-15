#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "platform/ipc.h"
#include "core/server.h"

namespace {

constexpr char usage_text[] =
	"usage: wayland-canvas [-s <startup command>] [-S <scale>] [-d] [-B] [-h]\n"
	"       wayland-canvas msg <command>\n"
	"\n"
	"An infinite-canvas Wayland compositor: virtual desktops are dashed\n"
	"frames you drag around freely on an unbounded zoomable canvas.\n"
	"\n"
	"Options:\n"
	"  -s <command>  run <command> at startup (through /bin/sh -c)\n"
	"  -S <scale>    fixed output scale (default: picked per output;\n"
	"                can also be set via $WAYLAND_CANVAS_SCALE)\n"
	"  -d            enable debug logging\n"
	"  -B            start without the top bar\n"
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
	"Control commands, over $WAYLAND_CANVAS_SOCKET (wayland-canvas msg):\n"
	"  launcher      toggle the app launcher on the output under the cursor\n"
	"  overview      toggle the overview there\n"
	"  quit          end the session\n"
	"\n"
	"See the keyboard bindings in src/core/config.h and src/input/input.cpp.\n";

} // namespace

int main(int argc, char *argv[]) {
	Options options;
	bool debug = false;
	double scale = 0;
	bool have_scale = false;

	// The one non-compositor mode: send a control command to a running
	// compositor through $WAYLAND_CANVAS_SOCKET.
	if (argc > 1 && strcmp(argv[1], "msg") == 0) {
		if (argc != 3 || *argv[2] == '\0') {
			std::fputs("usage: wayland-canvas msg <command>\n", stderr);
			return 2;
		}
		std::optional<std::string> response = send_ipc_message(argv[2]);
		if (!response) {
			return 1;
		}
		std::printf("%s\n", response->c_str());
		return response->rfind("error", 0) == 0 ? 1 : 0;
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
