// End-to-end scenario for the capture core and its protocols: the offscreen
// captures (an output area, a single window) proven pixel-by-pixel against
// grim, a window shot that leaves out what overlaps it, ext-image-copy-
// capture with real clients, screen sharing through xdg-desktop-portal-
// wlr on a private D-Bus session, and a window cast of an X11 window that
// closes while it is copied. Writes only under $CANVAS_TMP (meson passes
// the repo's tmp/); meson passes the compositor binary as $WAYLAND_CANVAS_BIN.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>
#include "core/process.h"
#include "core/server.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/control.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "platform/output.h"
#include "render/capture.h"
#include "windows/view.h"

using namespace harness;

namespace {

// A pixel differs when a colour channel is further off than this.
constexpr int tolerance = 2;
// The area of the region captures, in the output's logical pixels.
constexpr wlr_box region{180, 140, 420, 260};
// How near the screen's edge a title-bar drag may go.
constexpr int edge_allowance = 10;
constexpr uint32_t x11_colour = 0xc04040; // x11-client's default window colour

wlr_box over_apart{}; // where "over" sat when the reference captures were taken

std::string portal_dir; // the portal's scratch world, under the scratch directory
std::string portal_runtime; // the session's short-lived runtime dir in /tmp
Child portal_bus; // the dbus-daemon this run owns
Child portal_session; // the session's shell: the portal's backend and the ScreenCast client
Child x11; // the X11 client whose window the cast phase copies
Child x11_cast; // the cast client of that window
bool x11_cast_started = false;

// Ends the portal phase: the session's group, then the bus's (SIGKILL: test
// processes with nothing to save, and a TERM may leave the bus waiting on a
// client), the document portal's FUSE mount (its own unmount runs detached
// after the kill), and the runtime dir. From the phase's last step, and from
// the run's teardown: a timeout's SIGTERM only ends the loop.
void stop_portal() {
	portal_session.stop();
	portal_bus.kill();
	if (portal_runtime.empty()) {
		return;
	}
	if (std::system(std::format("fusermount3 -uz {0}/doc 2>/dev/null"
			" || fusermount -uz {0}/doc 2>/dev/null; rm -rf {0}", portal_runtime).c_str())
			!= 0) {
		std::println("note: the scratch portal runtime {} was not removed", portal_runtime);
	}
	portal_runtime.clear();
}

// A title-bar drag of `view` by logical (dx, dy), ten moves like a hand per
// grab. The pointer cannot leave the output, so a move longer than the edge
// allowance is several grabs, re-aimed at the title bar each time.
void drag_view(const View &view, double dx, double dy) {
	for (int grab = 0; (std::fabs(dx) > 1.0 || std::fabs(dy) > 1.0) && grab < 6; grab++) {
		// Grabbed 60 px into the title bar, so a window pushed toward an edge
		// keeps a reachable grab point as long as possible.
		Point grip = title_point(view, 60);
		double step_x = clampd(dx, edge_allowance - grip.x, screen_w - edge_allowance - grip.x);
		double step_y = clampd(dy, edge_allowance - grip.y, screen_h - edge_allowance - grip.y);
		if (step_x == 0.0 && step_y == 0.0) {
			return; // the title bar is pinned against an edge already
		}
		drag(grip.x, grip.y, grip.x + step_x, grip.y + step_y);
		dx -= step_x;
		dy -= step_y;
	}
}

// How many pixels of `image` differ from the shot `name`; -1 when the shot is
// missing or their sizes differ.
long differing_from_shot(const Image &image, std::string_view name,
		const ImageCompare &how = {.tolerance = tolerance}) {
	std::optional<Image> reference = read_shot(name);
	return reference.has_value() ? image.differing(*reference, how) : -1;
}

long output_pixels() {
	return output().width * static_cast<long>(output().height);
}

int mapped_views() {
	return static_cast<int>(std::ranges::count_if(server->views,
		[](const View *view) { return view->mapped; }));
}

// The ScreenCast client the portal phase runs: CreateSession, SelectSources
// (a monitor, then a window), Start, and OpenPipeWireRemote, on the private
// session bus. It prints one "portal-<kind> node=<id>" line per source.
constexpr std::string_view portal_client_script = R"(import os
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

wlr_box overlap_on_screen() {
	wlr_box under = frame_on_screen(*titled("under"));
	wlr_box over = frame_on_screen(*titled("over"));
	wlr_box overlap{};
	wlr_box_intersection(&overlap, &under, &over);
	return overlap;
}

// What a private portal session needs and this machine lacks, one name after
// another; empty when it has everything.
std::string portal_pieces_missing() {
	std::string absent;
	for (const char *program : {"dbus-run-session", "pipewire"}) {
		if (std::system(std::format("command -v {} >/dev/null 2>&1", program).c_str()) != 0) {
			absent += std::format(" {}", program);
		}
	}
	// The portal and its backends install outside $PATH, and the system
	// python carries gi (a virtualenv's does not).
	for (std::string_view path : {"/usr/lib/xdg-desktop-portal",
			"/usr/lib/xdg-desktop-portal-wlr"}) {
		if (std::system(std::format("[ -x {} ]", path).c_str()) != 0) {
			absent += std::format(" {}", path.substr(path.rfind('/') + 1));
		}
	}
	if (std::system("/usr/bin/python3 -c 'import gi' >/dev/null 2>&1") != 0) {
		absent += " python3-gi";
	}
	return absent;
}

// While the portal session runs, its chooser is the compositor's own picker:
// answer each one by clicking the source the client asked for (the mode file
// it wrote before its request).
void answer_the_chooser() {
	if (!server->capture_ui.picking()) {
		return;
	}
	View *wanted = read_text(portal_dir + "/chooser-mode").contains("window")
		? titled("under") : nullptr;
	if (wanted != nullptr) {
		Point title = title_point(*wanted, 60);
		click(title.x, title.y);
		return;
	}
	// A click no window covers answers with the output itself.
	for (Point spot : {Point{40, 40}, Point{80, 650}, Point{640, 700}}) {
		bool covered = std::ranges::any_of(server->views, [&](const View *view) {
			if (!view->mapped || view->minimized) {
				return false;
			}
			wlr_box frame = frame_on_screen(*view);
			return spot.x >= frame.x && spot.x < frame.x + frame.width
				&& spot.y >= frame.y && spot.y < frame.y + frame.height;
		});
		if (!covered) {
			click(spot.x, spot.y);
			return;
		}
	}
}

Result foot_windows_open() {
	attach_pointer();
	spawn("foot -T under sh -c 'echo window under; exec sleep 600'");
	spawn("foot -T over sh -c 'echo window over; exec sleep 600'");
	return after(500);
}

Result foot_windows_mapped() {
	if (auto wait = wait_for(titled("under") != nullptr && titled("over") != nullptr
			&& settled())) {
		return *wait;
	}
	View *under = titled("under"), *over = titled("over");
	int count = mapped_views();
	check(under != nullptr && over != nullptr && count == 2,
		std::format("two foot windows mapped ({})", count));
	check(under != nullptr && over != nullptr && under->deco.ssd && over->deco.ssd,
		"both windows negotiated server-side title bars");
	if (under == nullptr || over == nullptr) {
		return finish();
	}
	return after(400);
}

// Both map centred, so they start overlapped: "over" is pulled clear.
Result over_is_pulled_clear() {
	wlr_box under = frame_on_screen(*titled("under")), over = frame_on_screen(*titled("over"));
	// Parked just right of "under", a 40 px gap between the frames.
	drag_view(*titled("over"), (under.x + under.width + 40.0) - over.x, 0);
	return after(500);
}

Result windows_are_apart() {
	if (auto wait = wait_for(settled(), {.budget_ms = 5000})) {
		return *wait;
	}
	wlr_box apart = overlap_on_screen();
	check(wlr_box_empty(&apart), "the windows are apart on screen");
	shot("ref-full");
	return after(400);
}

// The offscreen whole-output capture against grim's screenshot.
Result whole_output_matches_grim() {
	CairoSurfacePtr cap = capture_output(output(), {0, 0, output().width, output().height});
	check(cap != nullptr, "capture_output returns the whole output");
	if (cap != nullptr) {
		Image image = Image::of(cap.get());
		image.save("cap-full");
		long bad = differing_from_shot(image, "ref-full");
		check(bad >= 0 && bad * 1000 <= output_pixels(),
			std::format("the whole-output capture matches grim pixel for pixel "
				"({} of {} px differ)", bad, output_pixels()));
	}
	shot("ref-region", std::format("{},{} {}x{}", region.x, region.y, region.width,
		region.height));
	return after(400);
}

// A region capture against grim's crop of the same area, and the reference
// window shot, taken with nothing over the window.
Result region_matches_grim() {
	long region_pixels = region.width * static_cast<long>(region.height);
	CairoSurfacePtr cap = capture_output(output(), region);
	check(cap != nullptr, "capture_output returns the region");
	if (cap != nullptr) {
		Image image = Image::of(cap.get());
		image.save("cap-region");
		check(image.width() == region.width && image.height() == region.height,
			"the region capture is the area at the output's scale");
		long bad = differing_from_shot(image, "ref-region");
		check(bad >= 0 && bad * 1000 <= region_pixels,
			std::format("the region capture matches grim -g pixel for pixel "
				"({} of {} px differ)", bad, region_pixels));
	}
	View *under = titled("under");
	CairoSurfacePtr window = capture_view(*under, output().wlr->scale);
	check(window != nullptr, "capture_view returns the window");
	if (window != nullptr) {
		Image image = Image::of(window.get());
		image.save("under-apart");
		wlr_box frame = under->frame_box();
		check(image.width() == frame.width && image.height() == frame.height,
			"the window capture is the frame at the output's scale");
	}
	return after(300);
}

// "Over" is dropped back over most of "under".
Result over_covers_under() {
	wlr_box under = frame_on_screen(*titled("under")), over = frame_on_screen(*titled("over"));
	over_apart = over; // where "over" sat in the reference captures
	drag_view(*titled("over"), (under.x + under.width * 0.15) - over.x,
		(under.y + under.height * 0.15) - over.y);
	return after(500);
}

Result overlap_leaves_no_trace() {
	if (auto wait = wait_for(settled(), {.budget_ms = 5000})) {
		return *wait;
	}
	View *under = titled("under");
	wlr_box under_box = frame_on_screen(*under), over_box = frame_on_screen(*titled("over"));
	wlr_box overlap = overlap_on_screen();
	check(!wlr_box_empty(&overlap) && overlap.width > 100 && overlap.height > 100,
		std::format("the windows overlap on screen ({}x{} px)", overlap.width, overlap.height));
	// The window alone: whatever overlaps it on screen leaves no trace.
	CairoSurfacePtr window = capture_view(*under, output().wlr->scale);
	check(window != nullptr, "capture_view returns the overlapped window");
	if (window != nullptr) {
		Image image = Image::of(window.get());
		image.save("under-overlapped");
		long bad = differing_from_shot(image, "under-apart");
		long total = under_box.width * static_cast<long>(under_box.height);
		check(bad == 0, std::format("the window shot leaves out what overlaps it "
			"({} of {} px differ)", bad, total));
	}
	// And the overlap really is on screen: the whole-output capture now
	// differs inside it, and outside the moved window nowhere - counting
	// neither where "over" sits now nor the area it vacated.
	CairoSurfacePtr full = capture_output(output(), {0, 0, output().width, output().height});
	if (full != nullptr) {
		Image image = Image::of(full.get());
		image.save("cap-overlapped");
		const wlr_box moved[] = {over_box, over_apart};
		long inside = differing_from_shot(image, "cap-full",
			{.within = overlap, .tolerance = tolerance});
		long outside = differing_from_shot(image, "cap-full",
			{.skip = moved, .tolerance = tolerance});
		check(inside > 1000, std::format("the overlapping window shows in the output capture "
			"({} px changed in the overlap)", inside));
		check(outside == 0,
			std::format("nothing changed outside the moved window ({} px)", outside));
	}
	return after(300);
}

// The protocols: grim now travels ext-image-copy-capture.
Result protocol_shot() {
	shot("ref-ext-full");
	return after(400);
}

// The whole output again, this time through the capture protocols, and one
// window by its ext-foreign-toplevel identifier.
Result protocol_screenshot_matches() {
	CairoSurfacePtr cap = capture_output(output(), {0, 0, output().width, output().height});
	long bad = cap != nullptr ? differing_from_shot(Image::of(cap.get()), "ref-ext-full") : -1;
	check(bad >= 0 && bad * 1000 <= output_pixels(),
		std::format("grim's protocol screenshot matches the offscreen capture "
			"({} of {} px differ)", bad, output_pixels()));
	const char *identifier = titled("under")->ext_toplevel_identifier();
	check(identifier != nullptr && *identifier != '\0',
		"the window carries an ext-foreign-toplevel identifier");
	if (identifier != nullptr) {
		shot_toplevel("ref-ext-window", identifier);
	}
	return after(400);
}

// The window by identifier, against the offscreen window capture - still
// taken while "over" covers most of it.
Result window_by_identifier_matches() {
	CairoSurfacePtr cap = capture_view(*titled("under"), output().wlr->scale);
	long bad = -1, total = 0;
	if (cap != nullptr) {
		Image image = Image::of(cap.get());
		bad = differing_from_shot(image, "ref-ext-window");
		total = image.width() * static_cast<long>(image.height());
	}
	check(bad == 0, std::format("grim -T <identifier> captures the window alone "
		"({} of {} px differ)", bad, total));
	return after(300);
}

// The portal: sharing through xdg-desktop-portal-wlr. Only with everything a
// private portal session needs; a run without it skips this phase (the
// capture proofs above still ran).
Result portal_starts() {
	std::string absent = portal_pieces_missing();
	if (!absent.empty()) {
		std::println("SKIP: portal phase, not installed:{}", absent);
		return go_to("x11 client starts", 600);
	}
	// The scratch world: a bus and XDG dirs of its own, so the machine's
	// portal, PipeWire and config are never touched.
	portal_dir = scratch() + "/portal";
	scratch_dir("portal/share");
	scratch_dir("portal/config/xdg-desktop-portal");
	scratch_dir("portal/config/xdg-desktop-portal-wlr");
	// PipeWire's socket must fit a unix socket name (107 chars), and the
	// worktree's tmp is deeper than that: only the runtime dir rides in
	// /tmp, under a name of this run's own, and is removed afterwards.
	std::string runtime = "/tmp/canvas-sharing-runtime-XXXXXX";
	if (mkdtemp(runtime.data()) == nullptr) {
		check(false, "the portal's runtime directory was made");
		return go_to("x11 client starts", 600);
	}
	portal_runtime = runtime;
	write_file(portal_dir + "/config/xdg-desktop-portal/portals.conf",
		"[preferred]\ndefault=gtk\n"
		"org.freedesktop.impl.portal.ScreenCast=wlr\n"
		"org.freedesktop.impl.portal.Screenshot=wlr\n");
	// The chooser xdpw runs is the compositor's own picker, through the msg
	// client: xdpw offers its monitor and window lines on stdin,
	// `capture.pick` opens the picker, and the scenario (the next step)
	// clicks the source the client asked for by the mode file it wrote
	// first. The config is the one the package ships, byte for byte, under
	// the name xdpw reads for this desktop; the build's binary is first on
	// the session's PATH, as an installed one would be.
	const char *source = std::getenv("CANVAS_SOURCE");
	std::string shipped = read_text(std::format("{}/packaging/xdg-desktop-portal-wlr.conf",
		source != nullptr && *source != '\0' ? source : "."));
	check(shipped.contains("chooser_cmd = wayland-canvas msg capture.pick"),
		"the shipped xdpw config names the compositor's picker as the chooser");
	write_file(portal_dir + "/config/xdg-desktop-portal-wlr/wayland-canvas", shipped);
	std::string bin = std::getenv("WAYLAND_CANVAS_BIN");
	std::string bin_dir = bin.substr(0, bin.rfind('/'));
	write_file(portal_dir + "/client.py", portal_client_script);
	// The compositor's socket as an absolute path, for the session's portal
	// and backend to find this compositor and nothing else.
	std::string wayland = std::format("{}/{}", std::getenv("XDG_RUNTIME_DIR"),
		std::getenv("WAYLAND_DISPLAY"));
	std::string bus = portal_runtime + "/bus";
	// What both the bus and the session know, handed over through env(1) so
	// the compositor's own environment is never changed. gvfs stays off the
	// bus; the document portal still mounts FUSE under the runtime dir, which
	// stop_portal() unmounts.
	std::vector<std::string> world{
		std::format("XDG_RUNTIME_DIR={}", portal_runtime),
		std::format("XDG_CONFIG_HOME={}/config", portal_dir),
		std::format("XDG_DATA_HOME={}/share", portal_dir),
		"XDG_CURRENT_DESKTOP=wayland-canvas:wlroots",
		std::format("WAYLAND_DISPLAY={}", wayland),
		std::format("DBUS_SESSION_BUS_ADDRESS=unix:path={}", bus),
		"GIO_USE_VFS=local",
		"GVFS_DISABLE_FUSE=1",
	};
	// The bus of its own: started here, killed here, and dead even if this
	// run itself is killed. The bus and everything it activates die together.
	std::vector<std::string> bus_argv{"env"};
	bus_argv.insert(bus_argv.end(), world.begin(), world.end());
	bus_argv.insert(bus_argv.end(), {"/usr/bin/dbus-daemon", "--session", "--nofork",
		"--nopidfile", std::format("--address=unix:path={}", bus)});
	check(portal_bus.start(bus_argv, {.own_group = true, .log = portal_dir + "/bus.log"}),
		"the portal's own bus started");
	// A service the session bus starts knows only what the session hands
	// over: never the control socket's own variable. The shell gets a TERM
	// when this run dies, so its trap reaps pipewire and xdpw; the session
	// dies as a group, leaving no one behind.
	std::vector<std::string> session_argv{"env", "-u", "WAYLAND_CANVAS_SOCKET"};
	session_argv.insert(session_argv.end(), world.begin(), world.end());
	session_argv.insert(session_argv.end(), {
		std::format("PATH={}:{}", bin_dir, std::getenv("PATH")),
		std::format("CHOOSER_MODE_FILE={}/chooser-mode", portal_dir),
		std::format("PORTAL_DIR={}", portal_dir),
		"/bin/sh", "-c",
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
		"echo $? >\"$PORTAL_DIR/client.code\""});
	check(portal_session.start(session_argv, {.own_group = true,
			.log = portal_dir + "/session.log", .death_signal = SIGTERM}),
		"the portal session started");
	return after(300);
}

// The whole session gets a minute.
Result portal_shares_a_monitor_and_a_window() {
	answer_the_chooser();
	if (auto wait = wait_for(!portal_session.running(), {.poll_ms = 500, .budget_ms = 60'000})) {
		return *wait;
	}
	stop_portal();
	// What the ScreenCast client saw, one line per source kind.
	std::string out = read_text(portal_dir + "/client.out");
	auto node_of = [&out](std::string_view kind) {
		std::string needle = std::format("portal-{} node=", kind);
		std::size_t at = out.find(needle);
		return at == std::string::npos ? 0 : std::atoi(out.c_str() + at + needle.size());
	};
	int monitor_node = node_of("monitor");
	int window_node = node_of("window");
	int code = std::atoi(read_text(portal_dir + "/client.code").c_str());
	check(code == 0 && monitor_node > 0,
		std::format("the portal shares a monitor: the ScreenCast client "
			"got a PipeWire stream (node {}, exit {})", monitor_node, code));
	check(code == 0 && window_node > 0,
		std::format("the portal shares a window: the ScreenCast client "
			"got a PipeWire stream (node {}, exit {})", window_node, code));
	return after(300);
}

// A window cast of an X11 window: the cast client copies the frames of the
// X11 client's window through the toplevel source.
Result x11_client_starts() {
	const char *client = std::getenv("CANVAS_X11_CLIENT");
	if (client == nullptr || *client == '\0') {
		std::println("SKIP: the X11 window cast, no X11 client built");
		return go_to("quit", 100);
	}
	check(x11.start_client("CANVAS_X11_CLIENT", {"--title=x11-cast"}),
		"the X11 client started");
	return after(1500);
}

Result x11_window_is_cast() {
	if (auto wait = wait_for(titled("x11-cast") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	View *window = titled("x11-cast");
	check(window != nullptr && window->as_x11() != nullptr,
		"the X11 client's window mapped as an X11 view");
	if (window == nullptr) {
		return go_to("x11 cast ends with the window", 100);
	}
	x11_cast_started = x11_cast.start_client("CANVAS_CAST_CLIENT",
		{"x11-cast", std::format("{:06x}", x11_colour)});
	check(x11_cast_started, "the cast client of the X11 window started");
	return after(800);
}

Result x11_cast_shows_the_client() {
	if (auto wait = wait_for(x11_cast.count("CAST SHARE 3 ") > 0)) {
		return *wait;
	}
	std::string_view said = x11_cast.said();
	View *window = titled("x11-cast");
	wlr_box frame = window != nullptr ? window->frame_box() : wlr_box{};
	int w = 0, h = 0;
	if (std::size_t ready = said.find("CAST READY "); ready != std::string_view::npos) {
		std::sscanf(said.data() + ready, "CAST READY %d %d", &w, &h);
	}
	check(w > 0 && w == frame.width && h == frame.height,
		std::format("the X11 window's cast frames are the window's size "
			"({}x{}, frame {}x{})", w, h, frame.width, frame.height));
	for (int n = 1; n <= 3; n++) {
		long hits = 0, total = 0;
		if (std::size_t at = said.find(std::format("CAST SHARE {} ", n));
				at != std::string_view::npos) {
			std::sscanf(said.data() + at, "CAST SHARE %*d %ld %ld", &hits, &total);
		}
		check(total > 0 && hits * 10 > total * 7,
			std::format("frame {} of the X11 window's cast shows the client's "
				"colour ({} of {} px)", n, hits, total));
	}
	// The window goes while the cast runs: the session ends with it.
	x11.kill();
	return after(300);
}

Result x11_cast_ends_with_the_window() {
	x11.kill(); // the window never mapped: nothing is left running
	if (auto wait = wait_for(!x11_cast_started || x11_cast.count("CAST STOPPED") > 0)) {
		return *wait;
	}
	if (x11_cast_started) {
		check(x11_cast.count("CAST STOPPED") > 0,
			"the compositor ended the X11 window's cast with the window");
		x11_cast.ask_to_stop();
	}
	run_msg("state", "", "x11-state");
	return after(600);
}

Result compositor_still_answers() {
	if (auto wait = wait_for(command_done("x11-state"), {.budget_ms = 5000})) {
		return *wait;
	}
	check(command_code("x11-state") == 0 && command_out("x11-state").contains("\"windows\""),
		"the compositor still answers `msg state` after the X11 window's cast");
	return after(300);
}

Result quit() {
	return quit_by_msg();
}

} // namespace

int main() {
	if (!begin("sharing", {"WAYLAND_CANVAS_BIN"})) {
		return 2;
	}
	// foot reads its config under $XDG_CONFIG_HOME: a scratch one keeps the
	// terminal's cursor steady, so a window's pixels do not change between
	// the captures that must compare equal.
	write_file(scratch_dir("xdg-config/foot") + "/foot.ini", "[cursor]\nblink=false\nstyle=block\n");
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	return run(options, {
		{"foot windows open", foot_windows_open},
		{"foot windows mapped", foot_windows_mapped},
		{"over is pulled clear", over_is_pulled_clear},
		{"windows are apart", windows_are_apart},
		{"whole output matches grim", whole_output_matches_grim},
		{"region matches grim", region_matches_grim},
		{"over covers under", over_covers_under},
		{"overlap leaves no trace", overlap_leaves_no_trace},
		{"protocol shot", protocol_shot},
		{"protocol screenshot matches", protocol_screenshot_matches},
		{"window by identifier matches", window_by_identifier_matches},
		{"portal starts", portal_starts},
		{"portal shares a monitor and a window", portal_shares_a_monitor_and_a_window},
		{"x11 client starts", x11_client_starts},
		{"x11 window is cast", x11_window_is_cast},
		{"x11 cast shows the client", x11_cast_shows_the_client},
		{"x11 cast ends with the window", x11_cast_ends_with_the_window},
		{"compositor still answers", compositor_still_answers},
		{"quit", quit},
	}, stop_portal);
}
