// End-to-end scenario for the capture core and its protocols: the offscreen
// captures (an output area, a single window) proven pixel-by-pixel against
// grim, a window shot that leaves out what overlaps it, ext-image-copy-
// capture with real clients, and screen sharing through xdg-desktop-portal-
// wlr on a private D-Bus session. Writes only under $CANVAS_TMP (meson passes
// the repo's tmp/); meson passes the compositor binary as $WAYLAND_CANVAS_BIN.
#include <cairo.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/prctl.h>
#include <format>
#include <optional>
#include <print>
#include <source_location>
#include <span>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include "platform/output.h"
#include "render/capture.h"
#include "core/server.h"
#include "core/wlr.h"
#include "windows/view.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false; // the run reached its end; see main()
uint32_t t = 90000;
pid_t grim_pid = -1;
std::string shot_dir;
wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
wlr_box over_apart{}; // where "over" sat when the reference captures were taken
bool portal_skip = false; // the portal pieces are not on this machine
pid_t portal_pid = -1; // the portal session's shell, its process group's leader
std::string portal_runtime; // the session's short-lived runtime dir in /tmp
pid_t portal_bus_pid = -1; // the dbus-daemon this run owns, killed by pid
std::string portal_dir; // its scratch world, under the shot directory

// Prints PASS/FAIL with the note; a failure counts and names its source line.
void check(bool cond, std::string_view note,
	std::source_location where = std::source_location::current()) {
	std::println("{}: {}", cond ? "PASS" : "FAIL", note);
	std::fflush(stdout);
	if (!cond) {
		failures++;
		std::println(stderr, "  at {}:{} in {}", where.file_name(), where.line(),
			where.function_name());
	}
}

// Ends the portal phase: the session's group, then the bus's (SIGKILL: test
// processes with nothing to save, and a TERM may leave the bus waiting on a
// client), the document portal's FUSE mount (its own unmount runs detached
// after the kill), and the runtime dir. From the phase's last step, and from
// main() when the run ends early: a timeout's SIGTERM only ends the loop.
void stop_portal() {
	int status = 0;
	if (portal_pid > 0) {
		kill(-portal_pid, SIGTERM); // ask first: the script's trap reaps
		usleep(100000);
		kill(-portal_pid, SIGKILL); // then the group, whatever is left
		waitpid(portal_pid, &status, 0);
		portal_pid = -1;
	}
	if (portal_bus_pid > 0) {
		kill(-portal_bus_pid, SIGKILL);
		waitpid(portal_bus_pid, &status, 0);
		portal_bus_pid = -1;
	}
	if (!portal_runtime.empty()) {
		if (std::system(std::format("fusermount3 -uz {0}/doc 2>/dev/null"
				" || fusermount -uz {0}/doc 2>/dev/null; rm -rf {0}", portal_runtime).c_str())
				!= 0) {
			std::println("note: the scratch portal runtime {} was not removed", portal_runtime);
		}
		portal_runtime.clear();
	}
}

void mkdir_p(const std::string &path) {
	for (std::size_t i = 1; i < path.size(); i++) {
		if (path[i] == '/') {
			mkdir(path.substr(0, i).c_str(), 0755);
		}
	}
	if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
		std::fprintf(stderr, "mkdir %s: %s\n", path.c_str(), std::strerror(errno));
	}
}

void write_file(const std::string &path, std::string_view text) {
	FILE *f = std::fopen(path.c_str(), "w");
	if (f == nullptr) {
		std::fprintf(stderr, "fopen %s: %s\n", path.c_str(), std::strerror(errno));
		return;
	}
	std::fwrite(text.data(), 1, text.size(), f);
	std::fclose(f);
}

std::string read_file(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		return "";
	}
	std::string out;
	char buf[256];
	std::size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.append(buf, n);
	}
	std::fclose(f);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}


// Runs the real client through /bin/sh with stdout, stderr and the exit code
// captured under `name` (.out, .err, .code). Params, when given, are
// single-quoted for the shell.
void run_client(std::string_view method, std::string_view params, const char *name) {
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	std::string cmd = std::format("\"{}\" msg {}", bin, method);
	if (!params.empty()) {
		cmd += std::format(" '{}'", params);
	}
	cmd += std::format(" > {}/{}.out 2> {}/{}.err; echo $? > {}/{}.code",
		shot_dir, name, shot_dir, name, shot_dir, name);
	spawn(cmd.c_str());
}



// ------------------------------------------------------ screenshots

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

// A grim crop of a region, in grim's own "x,y WxH" geometry format.
void shot_region(const char *name, const char *geometry) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", "-g", geometry, path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

bool grim_running() {
	if (grim_pid <= 0) {
		return false;
	}
	if (waitpid(grim_pid, nullptr, WNOHANG) == 0) {
		return true;
	}
	grim_pid = -1;
	return false;
}

// Loads a grim PNG as ARGB32 (grim may write RGB24); null on failure.
cairo_surface_t *load_png(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	cairo_surface_t *raw = cairo_image_surface_create_from_png(path.c_str());
	if (cairo_surface_status(raw) != CAIRO_STATUS_SUCCESS) {
		std::fprintf(stderr, "cannot read %s\n", path.c_str());
		cairo_surface_destroy(raw);
		return nullptr;
	}
	if (cairo_image_surface_get_format(raw) == CAIRO_FORMAT_ARGB32) {
		return raw;
	}
	cairo_surface_t *argb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		cairo_image_surface_get_width(raw), cairo_image_surface_get_height(raw));
	cairo_t *cr = cairo_create(argb);
	cairo_set_source_surface(cr, raw, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(raw);
	return argb;
}

void save_png(const CairoSurfacePtr &surface, const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	cairo_surface_write_to_png(surface.get(), path.c_str());
}

// How many pixels in `region` (whole image when empty) of two same-sized
// ARGB32 images differ by more than `tol` in any RGB channel. Pixels inside
// any of `skip` are not counted.
long pixels_differing(cairo_surface_t *a, cairo_surface_t *b, wlr_box region,
		std::span<const wlr_box> skip = {}, int tol = 2) {
	if (a == nullptr || b == nullptr) {
		return -1;
	}
	int w = cairo_image_surface_get_width(a), h = cairo_image_surface_get_height(a);
	if (w != cairo_image_surface_get_width(b) || h != cairo_image_surface_get_height(b)) {
		return -1;
	}
	if (wlr_box_empty(&region)) {
		region = {0, 0, w, h};
	}
	region.x = std::clamp(region.x, 0, w);
	region.y = std::clamp(region.y, 0, h);
	region.width = std::clamp(region.width, 0, w - region.x);
	region.height = std::clamp(region.height, 0, h - region.y);
	cairo_surface_flush(a);
	cairo_surface_flush(b);
	const unsigned char *da = cairo_image_surface_get_data(a);
	const unsigned char *db = cairo_image_surface_get_data(b);
	int sa = cairo_image_surface_get_stride(a), sb = cairo_image_surface_get_stride(b);
	long bad = 0;
	for (int y = region.y; y < region.y + region.height; y++) {
		for (int x = region.x; x < region.x + region.width; x++) {
			bool skipped = false;
			for (const wlr_box &s : skip) {
				if (x >= s.x && y >= s.y && x < s.x + s.width && y < s.y + s.height) {
					skipped = true;
					break;
				}
			}
			if (skipped) {
				continue;
			}
			// Both layouts little-endian: B, G, R at bytes 0..2.
			const unsigned char *pa = da + y * sa + static_cast<std::size_t>(x) * 4;
			const unsigned char *pb = db + y * sb + static_cast<std::size_t>(x) * 4;
			if (std::abs(pa[0] - pb[0]) > tol || std::abs(pa[1] - pb[1]) > tol
					|| std::abs(pa[2] - pb[2]) > tol) {
				bad++;
			}
		}
	}
	return bad;
}

// ------------------------------------------------------ fake pointer

// Pointer at output-local logical (x, y) on the single 1280x720 output.
void move_to(double x, double y) {
	t += 8;
	wlr_pointer_motion_absolute_event ev = {
		.pointer = &ptr, .time_msec = t, .x = x / 1280.0, .y = y / 720.0};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

void button(uint32_t code, bool pressed) {
	t += 8;
	wlr_pointer_button_event ev = {.pointer = &ptr, .time_msec = t, .button = code,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			: WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&ptr.events.button, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

// A plain click at output-local (x, y).
void click(double x, double y) {
	move_to(x, y);
	button(BTN_LEFT, true);
	button(BTN_LEFT, false);
}

// A title-bar drag of `view` by logical (dx, dy), ten moves like a hand per
// grab. The pointer cannot leave the output, so a move longer than the edge
// allowance is several grabs, re-aimed at the title bar each time.
void drag_view(View &view, double dx, double dy) {
	for (int grab = 0; (std::fabs(dx) > 1.0 || std::fabs(dy) > 1.0) && grab < 6; grab++) {
		Output *output = server->outputs.front().get();
		Camera cam = output->camera.drawn();
		wlr_box frame = view.frame_box();
		Point tl = output->canvas_to_local(frame.x, frame.y);
		// Grabbed 60 px into the title bar, so a window pushed toward an edge
		// keeps a reachable grab point as long as possible.
		double x = tl.x + 60.0;
		double y = tl.y + view.deco.titlebar() * cam.zoom / 2.0;
		double step_x = clampd(dx, 10.0 - x, 1270.0 - x);
		double step_y = clampd(dy, 10.0 - y, 710.0 - y);
		if (step_x == 0.0 && step_y == 0.0) {
			return; // the title bar is pinned against an edge already
		}
		move_to(x, y);
		button(BTN_LEFT, true);
		for (int i = 1; i <= 10; i++) {
			move_to(x + step_x * i / 10.0, y + step_y * i / 10.0);
		}
		button(BTN_LEFT, false);
		dx -= step_x;
		dy -= step_y;
	}
}

// ------------------------------------------------------ views

View *view_titled(std::string_view title) {
	for (View *v : server->views) {
		if (v->mapped && v->shell_title() != nullptr && title == v->shell_title()) {
			return v;
		}
	}
	return nullptr;
}
// The ScreenCast client the portal phase runs: CreateSession, SelectSources
// (a monitor, then a window), Start, and OpenPipeWireRemote, on the private
// session bus. It prints one "portal-<kind> node=<id>" line per source.
constexpr std::string_view PORTAL_CLIENT = R"(import os
import sys

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

BUS = 'org.freedesktop.portal.Desktop'
OBJ = '/org/freedesktop/portal/desktop'
IFACE = 'org.freedesktop.portal.ScreenCast'
MODE_FILE = os.environ['CHOOSER_MODE_FILE']

conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
loop = GLib.MainLoop()
serial = [0]


def call(method, params, reply):
    return conn.call_sync(BUS, OBJ, IFACE, method, params,
        GLib.VariantType(reply), Gio.DBusCallFlags.NONE, -1, None)


def request(method, fmt, args, options):
    # A portal call: the reply is a request handle, the answer comes as the
    # Response signal on that handle's path.
    serial[0] += 1
    options = dict(options)
    token = 'req%d' % serial[0]
    options['handle_token'] = GLib.Variant('s', token)
    sender = conn.get_unique_name()[1:].replace('.', '_')
    path = '%s/request/%s/%s' % (OBJ, sender, token)
    box = []

    def on_response(c, s, p, i, m, response, user_data=None):
        if p == path and not box:
            box.append(response)
            # The subscription callback runs on GDBus's worker thread: quit
            # through the main context, or the loop sleeps on to its timeout.
            GLib.idle_add(loop.quit)

    def on_timeout():
        loop.quit()
        return False

    sid = conn.signal_subscribe(BUS, 'org.freedesktop.portal.Request', 'Response',
        path, None, Gio.DBusSignalFlags.NONE, on_response, None)
    tid = GLib.timeout_add(25000, on_timeout)
    values = tuple(args) + (options,)
    call(method, GLib.Variant(fmt, values), '(o)')
    loop.run()
    if box:
        GLib.Source.remove(tid) # the timeout source only lives while pending
    conn.signal_unsubscribe(sid)
    if not box:
        raise RuntimeError('no Response for %s' % method)
    code, results = box[0].unpack()
    if code != 0:
        raise RuntimeError('%s refused: %s' % (method, results))
    return results


def screencast(source_type, kind):
    # The chooser xdpw runs is the compositor's own capture.pick picker; the
    # mode file tells the scenario which offered source to click, so write
    # it before the request that makes xdpw ask.
    with open(MODE_FILE, 'w') as mode:
        mode.write(kind)
    results = request('CreateSession', '(a{sv})', [],
        {'session_handle_token': GLib.Variant('s', 'ses%d' % serial[0])})
    session = results['session_handle']
    request('SelectSources', '(oa{sv})', [session], {
        'types': GLib.Variant('u', source_type),
        'multiple': GLib.Variant('b', False),
        'cursor_mode': GLib.Variant('u', 1)})
    results = request('Start', '(osa{sv})', [session, ''], {})
    streams = results.get('streams', [])
    if len(streams) != 1:
        raise RuntimeError('%s: expected one stream, got %d' % (kind, len(streams)))
    node = streams[0][0]
    if not node or node <= 0:
        raise RuntimeError('%s: no PipeWire node in the stream' % kind)
    handle, fds = conn.call_with_unix_fd_list_sync(BUS, OBJ, IFACE,
        'OpenPipeWireRemote',
        GLib.Variant('(oa{sv})', (session,
            {'handle_token': GLib.Variant('s', 'pwr%d' % serial[0])})),
        GLib.VariantType('(h)'), Gio.DBusCallFlags.NONE, -1, None, None)
    os.close(fds.get(handle.unpack()[0]))
    print('portal-%s node=%d' % (kind, node))


try:
    screencast(1, 'monitor')
    screencast(2, 'window')
except Exception:
    import traceback
    traceback.print_exc()
    sys.exit(1)
)";

// The view's frame where the output shows it, output-local logical px.
wlr_box view_on_screen(View &view) {
	Output *output = server->outputs.front().get();
	Camera cam = output->camera.drawn();
	wlr_box frame = view.frame_box();
	Point tl = output->canvas_to_local(frame.x, frame.y);
	return wlr_box{
		.x = static_cast<int>(std::lround(tl.x)),
		.y = static_cast<int>(std::lround(tl.y)),
		.width = static_cast<int>(std::lround(frame.width * cam.zoom)),
		.height = static_cast<int>(std::lround(frame.height * cam.zoom)),
	};
}

bool output_settled() {
	Output *output = server->outputs.front().get();
	return output != nullptr && !output->camera.panning() && !output->camera.zooming();
}

// --------------------------------------------------------------- steps

int run_step() {
	switch (step) {
	case 0:
		spawn("foot -T under sh -c 'echo window under; exec sleep 600'");
		spawn("foot -T over sh -c 'echo window over; exec sleep 600'");
		return 500;
	case 1: {
		bool mapped = view_titled("under") != nullptr && view_titled("over") != nullptr;
		if ((!mapped || !output_settled()) && retries++ < 100) {
			return -100;
		}
		int count = 0;
		for (View *v : server->views) {
			count += v->mapped ? 1 : 0;
		}
		check(mapped && count == 2,
			std::format("two foot windows mapped ({})", count));
		check(view_titled("under")->deco.ssd && view_titled("over")->deco.ssd,
			"both windows negotiated server-side title bars");
		return 400;
	}
	case 2: { // both map centred, so they start overlapped: pull "over" clear
		View *under = view_titled("under");
		View *over = view_titled("over");
		wlr_box u = view_on_screen(*under), o = view_on_screen(*over);
		// Park "over" just right of "under", a 40 px gap between the frames.
		double dx = (u.x + u.width + 40.0) - o.x;
		drag_view(*over, dx, 0);
		return 500;
	}
	case 3: {
		if (!output_settled() && retries++ < 50) {
			return -100;
		}
		// Separated: the parking left no overlap.
		wlr_box u = view_on_screen(*view_titled("under"));
		wlr_box o = view_on_screen(*view_titled("over"));
		wlr_box overlap{};
		wlr_box_intersection(&overlap, &u, &o);
		check(wlr_box_empty(&overlap), "the windows are apart on screen");
		shot("ref-full");
		return 400;
	}
	case 4: {
		if (grim_running()) {
			return -100;
		}
		// The offscreen whole-output capture against grim's screenshot.
		Output *output = server->outputs.front().get();
		CairoSurfacePtr cap = capture_output(*output, {0, 0, output->width, output->height});
		check(cap != nullptr, "capture_output returns the whole output");
		if (cap != nullptr) {
			save_png(cap, "cap-full");
			cairo_surface_t *ref = load_png("ref-full");
			long bad = pixels_differing(cap.get(), ref, {});
			long total = output->width * static_cast<long>(output->height);
			check(bad >= 0 && bad * 1000 <= total,
				std::format("the whole-output capture matches grim pixel for pixel "
					"({} of {} px differ)", bad, total));
			cairo_surface_destroy(ref);
		}
		shot_region("ref-region", "180,140 420x260");
		return 400;
	}
	case 5: {
		if (grim_running()) {
			return -100;
		}
		// A region capture against grim's crop of the same area.
		Output *output = server->outputs.front().get();
		wlr_box area{180, 140, 420, 260};
		CairoSurfacePtr cap = capture_output(*output, area);
		check(cap != nullptr, "capture_output returns the region");
		if (cap != nullptr) {
			save_png(cap, "cap-region");
			check(cairo_image_surface_get_width(cap.get()) == 420
					&& cairo_image_surface_get_height(cap.get()) == 260,
				"the region capture is the area at the output's scale");
			cairo_surface_t *ref = load_png("ref-region");
			long bad = pixels_differing(cap.get(), ref, {});
			check(bad >= 0 && bad * 1000 <= 420L * 260,
				std::format("the region capture matches grim -g pixel for pixel "
					"({} of 109200 px differ)", bad));
			cairo_surface_destroy(ref);
		}
		// The reference window shot, taken with nothing over it.
		CairoSurfacePtr under = capture_view(*view_titled("under"), output->wlr->scale);
		check(under != nullptr, "capture_view returns the window");
		if (under != nullptr) {
			save_png(under, "under-apart");
			wlr_box frame = view_titled("under")->frame_box();
			check(cairo_image_surface_get_width(under.get()) == frame.width
					&& cairo_image_surface_get_height(under.get()) == frame.height,
				"the window capture is the frame at the output's scale");
		}
		return 300;
	}
	case 6: { // drop "over" back over most of "under"
		View *under = view_titled("under");
		View *over = view_titled("over");
		wlr_box u = view_on_screen(*under), o = view_on_screen(*over);
		over_apart = o; // where "over" sat in the reference captures
		double dx = (u.x + u.width * 0.15) - o.x;
		double dy = (u.y + u.height * 0.15) - o.y;
		drag_view(*over, dx, dy);
		return 500;
	}
	case 7: {
		if (!output_settled() && retries++ < 50) {
			return -100;
		}
		Output *output = server->outputs.front().get();
		View *under = view_titled("under");
		View *over = view_titled("over");
		wlr_box u = view_on_screen(*under), o = view_on_screen(*over);
		wlr_box overlap{};
		wlr_box_intersection(&overlap, &u, &o);
		check(!wlr_box_empty(&overlap) && overlap.width > 100 && overlap.height > 100,
			std::format("the windows overlap on screen ({}x{} px)",
				overlap.width, overlap.height));
		// The window alone: whatever overlaps it on screen leaves no trace.
		CairoSurfacePtr cap = capture_view(*under, output->wlr->scale);
		check(cap != nullptr, "capture_view returns the overlapped window");
		if (cap != nullptr) {
			save_png(cap, "under-overlapped");
			cairo_surface_t *ref = load_png("under-apart");
			long bad = pixels_differing(cap.get(), ref, {});
			long total = u.width * static_cast<long>(u.height);
			check(bad == 0,
				std::format("the window shot leaves out what overlaps it "
					"({} of {} px differ)", bad, total));
			cairo_surface_destroy(ref);
		}
		// And the overlap really is on screen: the whole-output capture now
		// differs inside it, and outside the moved window nowhere — counting
		// neither where "over" sits now nor the area it vacated.
		CairoSurfacePtr full = capture_output(*output, {0, 0, output->width, output->height});
		if (full != nullptr) {
			save_png(full, "cap-overlapped");
			cairo_surface_t *before = load_png("cap-full");
			const wlr_box moved[] = {o, over_apart};
			long inside = pixels_differing(full.get(), before, overlap, {}, 2);
			long outside = pixels_differing(full.get(), before, {}, moved, 2);
			check(inside > 1000,
				std::format("the overlapping window shows in the output capture "
					"({} px changed in the overlap)", inside));
			check(outside == 0,
				std::format("nothing changed outside the moved window ({} px)", outside));
			cairo_surface_destroy(before);
		}
		return 300;
	}
	case 8: { // the protocols: grim now travels ext-image-copy-capture
		shot("ref-ext-full");
		return 400;
	}
	case 9: {
		if (grim_running()) {
			return -100;
		}
		// The whole output again, this time through the capture protocols.
		Output *output = server->outputs.front().get();
		CairoSurfacePtr cap = capture_output(*output, {0, 0, output->width, output->height});
		cairo_surface_t *ref = load_png("ref-ext-full");
		long bad = cap != nullptr ? pixels_differing(cap.get(), ref, {}) : -1;
		long total = output->width * static_cast<long>(output->height);
		check(bad >= 0 && bad * 1000 <= total,
			std::format("grim's protocol screenshot matches the offscreen capture "
				"({} of {} px differ)", bad, total));
		cairo_surface_destroy(ref);
		// And one window by its ext-foreign-toplevel identifier.
		View *under = view_titled("under");
		const char *identifier = under->ext_toplevel_identifier();
		check(identifier != nullptr && *identifier != '\0',
			"the window carries an ext-foreign-toplevel identifier");
		if (identifier != nullptr) {
			std::string path = std::format("{}/ref-ext-window.png", shot_dir);
			std::string err = std::format("{}/ref-ext-window.err", shot_dir);
		grim_pid = fork();
		if (grim_pid == 0) {
			FILE *log = std::fopen(err.c_str(), "w");
			if (log != nullptr) {
				dup2(fileno(log), STDERR_FILENO);
			}
			execlp("grim", "grim", "-T", identifier, path.c_str(),
				static_cast<char *>(nullptr));
			_exit(127);
		}
	}
		return 400;
	}
	case 10: {
		if (grim_running()) {
			return -100;
		}
		// The window by identifier, against the offscreen window capture —
		// still taken while "over" covers most of it.
		Output *output = server->outputs.front().get();
		View *under = view_titled("under");
		CairoSurfacePtr cap = capture_view(*under, output->wlr->scale);
		cairo_surface_t *ref = load_png("ref-ext-window");
		long bad = cap != nullptr ? pixels_differing(cap.get(), ref, {}) : -1;
		long total = cap != nullptr
			? cairo_image_surface_get_width(cap.get())
				* static_cast<long>(cairo_image_surface_get_height(cap.get()))
			: 0;
		check(bad == 0,
			std::format("grim -T <identifier> captures the window alone "
				"({} of {} px differ)", bad, total));
		cairo_surface_destroy(ref);
		return 300;
	}
	case 11: { // the portal: sharing through xdg-desktop-portal-wlr
		// Only with everything a private portal session needs; a run without
		// them skips this phase (the capture proofs above still ran).
		const char *const needed[] = {"dbus-run-session", "pipewire"};
		std::string absent;
		for (const char *cmd : needed) {
			if (std::system(std::format("command -v {} >/dev/null 2>&1", cmd).c_str()) != 0) {
				absent += std::format(" {}", cmd);
			}
		}
		// The portal and its backends install outside $PATH, and the system
		// python carries gi (a virtualenv's does not).
		const char *const tucked[] = {"/usr/lib/xdg-desktop-portal",
			"/usr/lib/xdg-desktop-portal-wlr"};
		for (const char *path : tucked) {
			if (std::system(std::format("[ -x {} ]", path).c_str()) != 0) {
				absent += std::format(" {}", std::strrchr(path, '/') + 1);
			}
		}
		if (std::system("/usr/bin/python3 -c 'import gi' >/dev/null 2>&1") != 0) {
			absent += " python3-gi";
		}
		if (!absent.empty()) {
			std::println("SKIP: portal phase, not installed:{}", absent);
			portal_skip = true;
			return 300;
		}
		// The scratch world: a bus and XDG dirs of its own, so the machine's
		// portal, PipeWire and config are never touched.
		portal_dir = std::format("{}/portal", shot_dir);
		mkdir_p(std::format("{}/share", portal_dir));
		mkdir_p(std::format("{}/config/xdg-desktop-portal", portal_dir));
		mkdir_p(std::format("{}/config/xdg-desktop-portal-wlr", portal_dir));
		// PipeWire's socket must fit a unix socket name (107 chars), and the
		// worktree's tmp is deeper than that: only the runtime dir rides in
		// /tmp, under a name of this run's own, and is removed afterwards.
		std::string pattern = "/tmp/canvas-sharing-runtime-XXXXXX";
		portal_runtime = mkdtemp(pattern.data());
		write_file(std::format("{}/config/xdg-desktop-portal/portals.conf", portal_dir),
			"[preferred]\ndefault=gtk\n"
			"org.freedesktop.impl.portal.ScreenCast=wlr\n"
			"org.freedesktop.impl.portal.Screenshot=wlr\n");
		// The chooser xdpw runs is the compositor's own picker, through the
		// msg client: xdpw offers its monitor and window lines on stdin,
		// `capture.pick` opens the picker, and the scenario (case 12) clicks
		// the source the client asked for by the mode file it wrote first.
		// The config is the one the package ships, byte for byte, under the
		// name xdpw reads for this desktop; the build's binary is first on
		// the session's PATH, as an installed one would be.
		const char *source = std::getenv("CANVAS_SOURCE");
		std::string shipped = read_file(std::format("{}/packaging/xdg-desktop-portal-wlr.conf",
			source != nullptr && *source ? source : "."));
		check(shipped.find("chooser_cmd = wayland-canvas msg capture.pick") != std::string::npos,
			"the shipped xdpw config names the compositor's picker as the chooser");
		write_file(std::format("{}/config/xdg-desktop-portal-wlr/wayland-canvas", portal_dir),
			shipped);
		const char *canvas_bin = std::getenv("WAYLAND_CANVAS_BIN");
		std::string bin = canvas_bin != nullptr ? canvas_bin : "";
		std::string bin_dir = bin.substr(0, bin.rfind('/'));
		write_file(std::format("{}/client.py", portal_dir), PORTAL_CLIENT);
		// The compositor's socket as an absolute path, for the session's
		// portal and backend to find this compositor and nothing else.
		const char *runtime = std::getenv("XDG_RUNTIME_DIR");
		const char *display = std::getenv("WAYLAND_DISPLAY");
		std::string wayland = std::format("{}/{}", runtime, display);
		// The bus of its own: started here, killed here — and dead even if
		// this run itself is killed, by PDEATHSIG. Nothing relies on a
		// wrapper cleaning up after itself. gvfs stays off the bus; the
		// document portal still mounts FUSE under the runtime dir, which
		// stop_portal() unmounts.
		std::string bus = std::format("{}/bus", portal_runtime);
		portal_bus_pid = fork();
		if (portal_bus_pid == 0) {
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			setpgid(0, 0); // the bus and everything it activates die together
			// The compositor blocks the signals it watches; the bus and what it
			// starts must not inherit that mask.
			sigset_t unblocked;
			sigemptyset(&unblocked);
			sigprocmask(SIG_SETMASK, &unblocked, nullptr);
			setenv("XDG_RUNTIME_DIR", portal_runtime.c_str(), 1);
			setenv("XDG_CONFIG_HOME", std::format("{}/config", portal_dir).c_str(), 1);
			setenv("XDG_DATA_HOME", std::format("{}/share", portal_dir).c_str(), 1);
			setenv("XDG_CURRENT_DESKTOP", "wayland-canvas:wlroots", 1);
			setenv("WAYLAND_DISPLAY", wayland.c_str(), 1);
			setenv("DBUS_SESSION_BUS_ADDRESS", std::format("unix:path={}", bus).c_str(), 1);
			setenv("GIO_USE_VFS", "local", 1);
			setenv("GVFS_DISABLE_FUSE", "1", 1);
			execl("/usr/bin/dbus-daemon", "dbus-daemon", "--session", "--nofork",
				"--nopidfile", std::format("--address=unix:path={}", bus).c_str(),
				static_cast<char *>(nullptr));
			_exit(127);
		}
		portal_pid = fork();
		if (portal_pid == 0) {
			prctl(PR_SET_PDEATHSIG, SIGTERM);
			setpgid(0, 0); // the session dies as a group, leaving no one behind
			sigset_t unblocked; // a TERM must reach the script's trap
			sigemptyset(&unblocked);
			sigprocmask(SIG_SETMASK, &unblocked, nullptr);
			setenv("XDG_RUNTIME_DIR", portal_runtime.c_str(), 1);
			setenv("XDG_CONFIG_HOME", std::format("{}/config", portal_dir).c_str(), 1);
			setenv("XDG_DATA_HOME", std::format("{}/share", portal_dir).c_str(), 1);
			setenv("XDG_CURRENT_DESKTOP", "wayland-canvas:wlroots", 1);
			setenv("WAYLAND_DISPLAY", wayland.c_str(), 1);
			setenv("DBUS_SESSION_BUS_ADDRESS", std::format("unix:path={}", bus).c_str(), 1);
			// A service the session bus starts knows only what the session
			// hands over: never the control socket's own variable.
			unsetenv("WAYLAND_CANVAS_SOCKET");
			setenv("PATH", std::format("{}:{}", bin_dir, std::getenv("PATH")).c_str(), 1);
			setenv("GIO_USE_VFS", "local", 1);
			setenv("GVFS_DISABLE_FUSE", "1", 1);
			setenv("CHOOSER_MODE_FILE", std::format("{}/chooser-mode", portal_dir).c_str(), 1);
			setenv("PORTAL_DIR", portal_dir.c_str(), 1);
			execl("/bin/sh", "/bin/sh", "-c",
				"exec >\"$PORTAL_DIR/session.out\" 2>\"$PORTAL_DIR/session.err\";"
				"i=0; while [ $i -lt 100 ] && [ ! -S \"$XDG_RUNTIME_DIR/bus\" ]; do"
				" sleep 0.1; i=$((i+1)); done;"
				"pipewire >\"$PORTAL_DIR/pipewire.out\" 2>&1 & pw=$!;"
				"i=0; while [ $i -lt 100 ] && [ ! -S \"$XDG_RUNTIME_DIR/pipewire-0\" ]; do"
				" sleep 0.1; i=$((i+1)); done;"
				"/usr/lib/xdg-desktop-portal-wlr -l DEBUG"
				" >\"$PORTAL_DIR/xdpw.out\" 2>&1 & xw=$!;"
				"trap 'kill $pw $xw 2>/dev/null' TERM EXIT;"
				"sleep 1;"
				"/usr/bin/python3 \"$PORTAL_DIR/client.py\""
				" >\"$PORTAL_DIR/client.out\" 2>\"$PORTAL_DIR/client.err\";"
				"echo $? >\"$PORTAL_DIR/client.code\"", nullptr);
			_exit(127);
		}
		return 300;
	}
	case 12: {
		if (portal_skip || portal_pid <= 0) {
			return 300;
		}
		// While the portal session runs, its chooser is the compositor's own
		// picker: answer each one by clicking the source the client asked
		// for (the mode file it wrote before its request).
		if (server->capture_ui.picking()) {
			std::string mode = read_file(std::format("{}/chooser-mode", portal_dir));
			View *wanted = mode.find("window") != std::string::npos
				? view_titled("under") : nullptr;
			if (wanted != nullptr) {
				wlr_box f = view_on_screen(*wanted);
				click(f.x + 60.0, f.y + 14.0); // the window's title bar
			} else {
				// A click no window covers answers with the output itself.
				const double spots[][2] = {{40, 40}, {80, 650}, {640, 700}};
				for (auto &spot : spots) {
					bool covered = false;
					for (View *v : server->views) {
						if (v->mapped && !v->minimized) {
							wlr_box f = view_on_screen(*v);
							covered = covered || (spot[0] >= f.x && spot[0] < f.x + f.width
								&& spot[1] >= f.y && spot[1] < f.y + f.height);
						}
					}
					if (!covered) {
						click(spot[0], spot[1]);
						break;
					}
				}
			}
		}
		int status = 0;
		if (waitpid(portal_pid, &status, WNOHANG) == 0 && retries++ < 120) {
			return -500; // still sharing; the whole session gets a minute
		}
		stop_portal();
		// What the ScreenCast client saw, one line per source kind.
		std::string out = read_file(std::format("{}/client.out", portal_dir));
		auto node_of = [&out](std::string_view kind) {
			std::string needle = std::format("portal-{} node=", kind);
			std::size_t at = out.find(needle);
			return at == std::string::npos
				? 0 : std::atoi(out.c_str() + at + needle.size());
		};
		int monitor_node = node_of("monitor");
		int window_node = node_of("window");
		int code = std::atoi(read_file(std::format("{}/client.code", portal_dir)).c_str());
		check(code == 0 && monitor_node > 0,
			std::format("the portal shares a monitor: the ScreenCast client "
				"got a PipeWire stream (node {}, exit {})", monitor_node, code));
		check(code == 0 && window_node > 0,
			std::format("the portal shares a window: the ScreenCast client "
				"got a PipeWire stream (node {}, exit {})", window_node, code));
		// stop_portal() left nothing of this run in /tmp.
		return 300;
	}
	default:
		std::println("{}: {} failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
		finished = true;
		run_client("quit", "", "quit");
		return 300;
	}
}

int handle_timer() {
	if (grim_running()) {
		wl_event_source_timer_update(timer, 30);
		return 0;
	}
	t += 1000;
	int delay = run_step();
	if (delay < -1) {
		wl_event_source_timer_update(timer, -delay);
		return 0;
	}
	if (delay == -1) {
		return 0;
	}
	step++;
	retries = 0;
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

} // namespace

int main() {
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr) {
		std::fprintf(stderr, "sharing: $WAYLAND_CANVAS_BIN is not set (meson passes it)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/sharing", root);
	mkdir_p(shot_dir);
	// foot reads its config under $XDG_CONFIG_HOME: a scratch one keeps the
	// terminal's cursor steady, so a window's pixels do not change between
	// the captures that must compare equal.
	std::string config_home = std::format("{}/scenarios/sharing/config", root);
	mkdir_p(std::format("{}/foot", config_home));
	write_file(std::format("{}/foot/foot.ini", config_home),
		"[cursor]\nblink=false\nstyle=block\n");
	setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
	wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
	timer = wl_event_loop_add_timer(server->loop, [](void *data) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display); // `msg quit` ends it
	stop_portal(); // a run ended early still leaves no session behind
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	server.reset(); // releases the control socket
	return failures != 0;
}
