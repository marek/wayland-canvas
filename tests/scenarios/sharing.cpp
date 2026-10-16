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
		if (v->mapped && v->wlr->title != nullptr && title == v->wlr->title) {
			return v;
		}
	}
	return nullptr;
}

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
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	server.reset(); // releases the control socket
	return failures != 0;
}
