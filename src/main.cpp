#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "platform/ipc.h"
#include "core/server.h"
#include "core/settings.h"

namespace {

constexpr char usage_head[] =
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
	"  3-finger swipe       hop to the nearest desktop in that direction;\n"
	"                       never into empty canvas\n"
	"  4-finger swipe up    zoom out into the overview of all desktops\n"
	"  Super + 2-finger pinch  scale the desktop shown (the picture only:\n"
	"                       windows keep their size); Super+0 back to 100 %\n"
	"  Super + 2-finger scroll  look around, over windows too, as does\n"
	"                       Super + drag on empty canvas; double Super\n"
	"                       goes back to the view you left\n"
	"  In the overview:\n"
	"    3-finger swipe     hop to the nearest desktop in that direction\n"
	"    Super+arrows step the focused window's slice on a tiled desktop\n"
		"    3-finger pinch out tile that desktop's windows into slices\n"
	"    3-finger pinch in  combine it with its nearest neighbour desktop\n"
	"    4-finger swipe down  zoom back into the highlighted target\n"
	"    4-finger swipe up    zoom out further\n"
	"    4-finger swipe sideways  swipe a desktop away (abandon it)\n"
	"    2-finger pinch     zoom the overview around the cursor\n"
	"  canvas.conf's [gestures] and [session] sections hold these settings:\n";

constexpr char usage_tail[] =
	"\n"
	"Screen capture:\n"
	"  Super+Shift+3      screenshot of the whole screen\n"
	"  Super+Shift+4      pick: drag a region, click a window, Space window-\n"
	"                     only, Esc cancel (Print does the same)\n"
	"  Super+Shift+5      the capture toolbar: shots and recordings, options\n"
	"  Ctrl + a shot key  copy to the clipboard instead of saving\n"
	"  Shift+Print        screenshot of the whole screen\n"
	"  Alt+Print          stop a recording, else the toolbar's recordings\n"
	"\n"
	"Control socket, over $WAYLAND_CANVAS_SOCKET (wayland-canvas msg):\n"
	"  state              outputs, desktops, the theme mode, the recording\n"
	"  subscribe          print every event, one per line, until interrupted\n"
	"  launcher.toggle    toggle the app launcher (\"output\" param optional)\n"
	"  overview.toggle    toggle the overview there (\"output\" param optional)\n"
	"  theme.set          {\"mode\": \"light\"} or {\"mode\": \"dark\"}\n"
	"  surface.place      put a plugin's surface on the plane: {\"surface\":\n"
	"                     \"<name>\", \"x\", \"y\", \"width\", \"height\"}\n"
	"  surface.unplace    give it back to the screen: {\"surface\": \"<name>\"}\n"
	"  surface.move       with a button held on it, the compositor drags it;\n"
	"                     the answer is where it was dropped\n"
	"  surface.resize     likewise by its edges: {\"edges\": [\"right\", \"bottom\"]}\n"
	"  window.focus       go to a window by its id from the state's window\n"
	"                     list: {\"window\": 3}\n"
	"  action.run         {\"name\": \"<plugin action>\", \"args\": {...}}\n"
	"  capture.pick       run the capture picker: {\"what\": \"region\" | \"window\"\n"
	"                     | \"output\" | \"any\"}; the answer comes when the user\n"
	"                     has picked. Bare `msg capture.pick` reads the portal's\n"
	"                     chooser list on stdin and prints the chosen line\n"
	"  capture.stop       stop a running recording\n"
	"  settings.get       the shortcuts, the commands and every setting, with\n"
	"                     the file, its ignored lines and what each setting takes\n"
	"  settings.set       set one action's keys (\"keys\": null for the\n"
	"                     defaults, [] for none), the commands or one setting\n"
	"                     (a param named as in settings.get's gestures and\n"
	"                     session); the answer says where a taken chord came from\n"
	"  shortcut.capture   record a shortcut: the answer comes when the chord\n"
	"                     is pressed (Esc alone cancels)\n"
	"  shortcut.cancel    cancel a recording under way\n"
	"  quit               end the session\n"
	"\n"
	"Every keyboard shortcut and every setting live in\n"
	"~/.config/wayland-canvas/canvas.conf, written with every default on the\n"
	"first run and re-read whenever it is saved.\n";

// The whole help: the settings in the middle are the settings table's.
std::string usage_text() {
	return std::string{usage_head} + settings::fields_help() + usage_tail;
}

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
			std::fputs(usage_text().c_str(), stdout);
			return 0;
		default:
			std::fputs(usage_text().c_str(), stderr);
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
