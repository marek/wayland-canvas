// End-to-end capture scenario: the key chords (whole screen, region picker,
// window picking with Space, Esc, the Print family, Ctrl for the
// clipboard), the after-shot thumbnail (shown, opened by a click through an
// xdg-open stub, dismissed by a swipe), the capture toolbar (modes and
// options, the timer), the `capture.pick` IPC with its deferred answers,
// the `wayland-canvas msg capture.pick` chooser bridge, and recordings
// (the compositor's own offscreen frames piped into ffmpeg; started,
// stopped by Alt+Print and by `capture.stop`, left as playable files,
// proven by ffprobe). Shots are compared against grim pixel for pixel.
// Writes only under $CANVAS_TMP: the XDG pictures and videos dirs point
// into it, and xdg-open is a stub.
#include <algorithm>
#include <cairo.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <ranges>
#include <signal.h>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "core/config.h"
#include "core/server.h"
#include "core/wlr.h"
#include "platform/output.h"
#include "windows/view.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool acted = false; // a step's interaction has happened (see handle_timer)
std::string term_geometry; // the window crop the key-leak check compares
bool finished = false; // the run reached its end; see main()
uint32_t t = 90000;
pid_t grim_pid = -1;
pid_t sub_pid = -1; // the events subscriber
std::string shot_dir;
std::string pictures_dir; // the scratch XDG pictures dir
std::string videos_dir; // the scratch XDG videos dir
std::string events_path;
wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
wlr_keyboard kbd{};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};

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
		std::fprintf(stderr, "cannot write %s\n", path.c_str());
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

bool file_exists(const std::string &path) {
	struct stat st{};
	return stat(path.c_str(), &st) == 0;
}

// The shot files that carry `prefix` in `dir`, newest first.
std::vector<std::string> files_with(const std::string &dir, std::string_view prefix) {
	std::vector<std::string> out;
	DIR *d = opendir(dir.c_str());
	if (d == nullptr) {
		return out;
	}
	while (dirent *e = readdir(d)) {
		if (e->d_name[0] != '.' && std::string_view{e->d_name}.starts_with(prefix)) {
			out.push_back(dir + "/" + e->d_name);
		}
	}
	closedir(d);
	auto mtime = [](const std::string &path) {
		struct stat st{};
		return stat(path.c_str(), &st) == 0 ? st.st_mtim.tv_sec * 1000000000ll
			+ st.st_mtim.tv_nsec : -1;
	};
	std::ranges::sort(out, [&](const std::string &a, const std::string &b) {
		return mtime(a) > mtime(b);
	});
	return out;
}

// ------------------------------------------------------------- pictures

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

// A grim crop of a region, in grim's own "x,y WxH" geometry format.
void shot_region(const char *name, const std::string &geometry) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", "-g", geometry.c_str(), path.c_str(),
			static_cast<char *>(nullptr));
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

using CairoSurfacePtr = std::unique_ptr<cairo_surface_t,
	void (*)(cairo_surface_t *)>;

// Loads a PNG as ARGB32 (grim and the shots may write other formats).
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

// Loads the newest shot file under the pictures dir; null when there is
// none (or it is unreadable).
cairo_surface_t *load_newest_shot(std::string *path_out = nullptr) {
	std::vector<std::string> files = files_with(pictures_dir + "/Screenshots",
		"Screenshot");
	if (files.empty()) {
		return nullptr;
	}
	std::string path = files.front();
	std::string name = "newest-shot";
	std::string dest = std::format("{}/{}.png", shot_dir, name);
	FILE *from = std::fopen(path.c_str(), "r");
	FILE *to = std::fopen(dest.c_str(), "w");
	if (from == nullptr || to == nullptr) {
		if (from != nullptr) {
			std::fclose(from);
		}
		if (to != nullptr) {
			std::fclose(to);
		}
		return nullptr;
	}
	char buf[4096];
	std::size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, from)) > 0) {
		std::fwrite(buf, 1, n, to);
	}
	std::fclose(from);
	std::fclose(to);
	if (path_out != nullptr) {
		*path_out = path;
	}
	return load_png(name.c_str());
}

// How many pixels of two same-sized ARGB32 images differ by more than `tol`
// in any RGB channel, not counting pixels inside `skip` (the cursor's box).
long pixels_differing(cairo_surface_t *a, cairo_surface_t *b,
		const std::vector<wlr_box> &skip = {}, int tol = 2) {
	if (a == nullptr || b == nullptr) {
		return -1;
	}
	int w = cairo_image_surface_get_width(a), h = cairo_image_surface_get_height(a);
	if (w != cairo_image_surface_get_width(b) || h != cairo_image_surface_get_height(b)) {
		return -1;
	}
	cairo_surface_flush(a);
	cairo_surface_flush(b);
	const unsigned char *da = cairo_image_surface_get_data(a);
	const unsigned char *db = cairo_image_surface_get_data(b);
	int sa = cairo_image_surface_get_stride(a), sb = cairo_image_surface_get_stride(b);
	long bad = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
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

// The box around the pointer's on-screen position, translated into the
// reference image's coordinates (a crop's origin for grim -g shots).
wlr_box cursor_box(double px, double py, int ref_x = 0, int ref_y = 0) {
	return {static_cast<int>(px) - ref_x - 24, static_cast<int>(py) - ref_y - 24,
		64, 64};
}

// A shot (an offscreen capture, cursorless) against a grim reference: they
// match, at the usual 0.1 % budget, outside `skip` (the cursor's box in the
// reference, none when the pointer was parked outside it). `pw`/`ph` are
// the sizes the shot must have.
bool shot_matches(cairo_surface_t *shot_surf, const char *ref,
		const std::vector<wlr_box> &skip, int pw, int ph) {
	if (shot_surf == nullptr) {
		return false;
	}
	if (cairo_image_surface_get_width(shot_surf) != pw
			|| cairo_image_surface_get_height(shot_surf) != ph) {
		return false;
	}
	cairo_surface_t *reference = load_png(ref);
	if (reference == nullptr) {
		return false;
	}
	long bad = pixels_differing(shot_surf, reference, skip);
	long total = static_cast<long>(pw) * ph;
	cairo_surface_destroy(reference);
	return bad >= 0 && bad * 1000 <= total;
}

// ------------------------------------------------------------- clients

// Runs `cmd` through the compositor's own spawn (never blocking this
// process) with stdout, stderr and the exit code captured under `name`.
void run_shell(const std::string &cmd, const char *name) {
	std::string full = std::format("{} > {}/{}.out 2> {}/{}.err; echo $? > {}/{}.code",
		cmd, shot_dir, name, shot_dir, name, shot_dir, name);
	spawn(full.c_str());
}

// The real `wayland-canvas msg` client. Params, when given, are
// single-quoted for the shell.
void run_client(std::string_view method, std::string_view params, const char *name) {
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	std::string cmd = std::format("\"{}\" msg {}", bin, method);
	if (!params.empty()) {
		cmd += std::format(" '{}'", params);
	}
	run_shell(cmd, name);
}

// The chooser bridge: `msg capture.pick` fed a chooser list on stdin.
void run_bridge(const char *list, const char *name) {
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	run_shell(std::format("\"{}\" msg capture.pick < {}/{}", bin, shot_dir, list), name);
}

bool client_done(const char *name) {
	return file_exists(std::format("{}/{}.code", shot_dir, name));
}

std::string client_out(const char *name) {
	return read_file(std::format("{}/{}.out", shot_dir, name));
}

std::string client_err(const char *name) {
	return read_file(std::format("{}/{}.err", shot_dir, name));
}

int client_code(const char *name) {
	return std::atoi(read_file(std::format("{}/{}.code", shot_dir, name)).c_str());
}

// Runs a command that needs no compositor interaction to completion.
int run_sync(const std::string &cmd, const char *out) {
	pid_t pid = fork();
	if (pid == 0) {
		FILE *f = std::fopen((shot_dir + "/" + out).c_str(), "w");
		if (f == nullptr || dup2(fileno(f), 1) < 0) {
			_exit(126);
		}
		execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ------------------------------------------------------ json members

std::optional<long long> member_int(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return std::nullopt;
	}
	JsonNode *node = json_object_get_member(object, name);
	return json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_INT64
		? std::optional{json_node_get_int(node)} : std::nullopt;
}

std::string member_string(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return {};
	}
	JsonNode *node = json_object_get_member(object, name);
	return json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? std::string{json_node_get_string(node)} : std::string{};
}

// A parsed result object of one client's answer: fills `*parser` (to unref
// when done) and `*result`. False when the answer is not an object.
bool client_result(const char *name, JsonParser **parser, JsonObject **result) {
	*parser = nullptr;
	*result = nullptr;
	std::string text = client_out(name);
	if (text.empty()) {
		return false;
	}
	JsonParser *p = json_parser_new();
	if (!json_parser_load_from_data(p, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		g_object_unref(p);
		return false;
	}
	JsonNode *root = json_parser_get_root(p);
	*result = root != nullptr && json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
	if (*result == nullptr) {
		g_object_unref(p);
		return false;
	}
	*parser = p;
	return true;
}

// The `recording` member of a `msg state` answer: "" when absent, "none"
// when null, else the recording's path.
std::string state_recording(const char *name) {
	std::string text = client_out(name);
	JsonParser *parser = json_parser_new();
	std::string out;
	if (json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		JsonNode *root = json_parser_get_root(parser);
		JsonObject *object = root != nullptr
				&& json_node_get_node_type(root) == JSON_NODE_OBJECT
			? json_node_get_object(root) : nullptr;
		if (object != nullptr && json_object_has_member(object, "recording")) {
			JsonNode *node = json_object_get_member(object, "recording");
			if (json_node_get_node_type(node) == JSON_NODE_NULL) {
				out = "none";
			} else if (json_node_get_node_type(node) == JSON_NODE_OBJECT) {
				out = member_string(json_node_get_object(node), "path");
			}
		}
	}
	g_object_unref(parser);
	return out;
}

// ------------------------------------------------------ input fakes

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

void click(double x, double y) {
	move_to(x, y);
	button(BTN_LEFT, true);
	button(BTN_LEFT, false);
}

void drag(double x0, double y0, double x1, double y1) {
	move_to(x0, y0);
	button(BTN_LEFT, true);
	for (int i = 1; i <= 10; i++) {
		move_to(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
	}
	button(BTN_LEFT, false);
}

void key(uint32_t code, bool pressed) {
	t += 8;
	wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

// Press mods..., tap k, release mods.
void chord(uint32_t k, std::span<const uint32_t> mods) {
	for (uint32_t m : mods) {
		key(m, true);
	}
	key(k, true);
	key(k, false);
	for (uint32_t m : std::views::reverse(mods)) {
		key(m, false);
	}
}

constexpr uint32_t SHIFT[] = {KEY_LEFTSHIFT};
constexpr uint32_t SUPER_SHIFT[] = {KEY_LEFTMETA, KEY_LEFTSHIFT};
constexpr uint32_t SUPER_CTRL_SHIFT[] = {KEY_LEFTMETA, KEY_LEFTCTRL,
	KEY_LEFTSHIFT};
constexpr uint32_t ALT[] = {KEY_LEFTALT};

// ------------------------------------------------------ window helpers

View *cap_window() {
	for (View *v : server->views) {
		if (v->mapped) {
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

// The middle of a toolbar button, mirroring CaptureUi::toolbar_layout().
double toolbar_button_x(int index, bool option) {
	Output *output = server->outputs.front().get();
	int fixed = 2 * config::capture_toolbar_pad + 5 * config::capture_toolbar_gap
		+ config::capture_toolbar_group_gap;
	int avail = output->width - 2 * config::capture_thumbnail_margin - fixed;
	int bw = (avail - 5 * config::capture_toolbar_gap) / 6;
	if (bw > config::capture_toolbar_button_w) {
		bw = config::capture_toolbar_button_w;
	}
	int inner = 6 * bw + 5 * config::capture_toolbar_gap
		+ config::capture_toolbar_group_gap;
	int box_x = (output->width - (inner + 2 * config::capture_toolbar_pad)) / 2;
	if (option) {
		int pill = (inner - 2 * config::capture_toolbar_gap) / 3;
		return box_x + config::capture_toolbar_pad
			+ index * (pill + config::capture_toolbar_gap) + pill / 2.0;
	}
	return box_x + config::capture_toolbar_pad + index * (bw + config::capture_toolbar_gap)
		+ (index >= 3 ? config::capture_toolbar_group_gap : 0) + bw / 2.0;
}

// The middle of the modes row (an option row is above it).
double toolbar_modes_y() {
	Output *output = server->outputs.front().get();
	int box_y = output->height - config::capture_toolbar_margin
		- (2 * config::capture_toolbar_pad + config::capture_toolbar_options_h
			+ config::capture_toolbar_gap + config::capture_toolbar_button_h
			+ config::capture_toolbar_gap + config::capture_toolbar_footer_h);
	return box_y + config::capture_toolbar_pad + config::capture_toolbar_options_h
		+ config::capture_toolbar_gap + config::capture_toolbar_button_h / 2.0;
}

double toolbar_options_y() {
	Output *output = server->outputs.front().get();
	int box_y = output->height - config::capture_toolbar_margin
		- (2 * config::capture_toolbar_pad + config::capture_toolbar_options_h
			+ config::capture_toolbar_gap + config::capture_toolbar_button_h
			+ config::capture_toolbar_gap + config::capture_toolbar_footer_h);
	return box_y + config::capture_toolbar_pad
		+ config::capture_toolbar_options_h / 2.0;
}

// The thumbnail's on-screen box (the newest shot is 1280x720, fit into the
// 240x140 slot), for the appear/dismiss pixel checks.
wlr_box thumb_box() {
	Output *output = server->outputs.front().get();
	double ar = 1280.0 / 720.0;
	double w = std::fmin(config::capture_thumbnail_max_w,
		config::capture_thumbnail_max_h * ar);
	double h = w / ar;
	return {static_cast<int>(std::lround(output->width
			- config::capture_thumbnail_margin - w)),
		static_cast<int>(std::lround(output->height
			- config::capture_thumbnail_margin - h)),
		static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))};
}

// Pixels of two grims that differ inside `box`.
long grim_differs_inside(const char *a, const char *b, const wlr_box &box) {
	cairo_surface_t *sa = load_png(a);
	cairo_surface_t *sb = load_png(b);
	if (sa == nullptr || sb == nullptr) {
		if (sa != nullptr) {
			cairo_surface_destroy(sa);
		}
		if (sb != nullptr) {
			cairo_surface_destroy(sb);
		}
		return -1;
	}
	// Count only the box: everything outside it is skipped through four
	// rectangles.
	wlr_box wide = {0, 0, cairo_image_surface_get_width(sa),
		cairo_image_surface_get_height(sa)};
	wlr_box left = wide, right = wide, top = wide, bottom = wide;
	left.width = box.x;
	right.x = box.x + box.width;
	right.width = wide.width - right.x;
	top.height = box.y;
	bottom.y = box.y + box.height;
	bottom.height = wide.height - bottom.y;
	long inside = pixels_differing(sa, sb, {left, right, top, bottom});
	cairo_surface_destroy(sa);
	cairo_surface_destroy(sb);
	return inside >= 0 ? inside : -1;
}

// ffprobe's csv rows ("W,H" and the duration) joined by the tr in
// run_sync: the size matches and the recording runs at least `min_s`.
bool probe_matches(const std::string &probe, int want_w, int want_h, double min_s) {
	int w = 0, h = 0;
	double dur = 0;
	if (std::sscanf(probe.c_str(), "%d,%d %lf", &w, &h, &dur) != 3) {
		return false;
	}
	return w == want_w && h == want_h && dur >= min_s;
}


// --------------------------------------------------------------- steps

int shots_saved() {
	return static_cast<int>(files_with(pictures_dir + "/Screenshots",
		"Screenshot").size());
}

int run_step() {
	switch (step) {
	case 0: { // the fake input devices, and one foot window to frame
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		spawn("foot -T cap sh -c 'echo capture scenario; exec sleep 600'");
		// The events subscriber, running for the whole run.
		events_path = shot_dir + "/events.out";
		sub_pid = fork();
		if (sub_pid == 0) {
			sigset_t empty;
			sigemptyset(&empty);
			sigprocmask(SIG_SETMASK, &empty, nullptr);
			FILE *f = std::fopen(events_path.c_str(), "w");
			if (f == nullptr || dup2(fileno(f), 1) < 0) {
				_exit(126);
			}
			execlp(std::getenv("WAYLAND_CANVAS_BIN"), "wayland-canvas", "msg",
				"subscribe", static_cast<char *>(nullptr));
			_exit(127);
		}
		return 500;
	}
	case 1: { // the window mapped and the camera settled
		View *view = cap_window();
		if ((view == nullptr || !output_settled()) && retries++ < 100) {
			return -100;
		}
		check(view != nullptr, "one foot window mapped");
		if (view != nullptr) {
			server->focus_view(view);
		}
		return 2500; // any startup hint clears before the reference shot
	}
	case 2: { // the reference: the whole output, cursor parked top-left
		move_to(40, 40);
		shot("ref-full");
		return 400;
	}
	case 3: { // Super+Shift+3: the whole screen, saved and thumbnailed
		if (grim_running()) {
			return -100;
		}
		chord(KEY_3, SUPER_SHIFT);
		int saved = shots_saved();
		check(saved == 1, std::format("Super+Shift+3 saved one shot ({})", saved));
		cairo_surface_t *shot_surf = load_newest_shot();
		check(shot_matches(shot_surf, "ref-full", {cursor_box(40, 40)}, 1280, 720),
			"the saved shot matches grim of the whole screen");
		if (shot_surf != nullptr) {
			cairo_surface_destroy(shot_surf);
		}
		return 2300; // the save hint clears; the thumbnail (5 s) is still up
	}
	case 4: { // the thumbnail is on screen, bottom-right
		shot("with-thumb");
		return 400;
	}
	case 5: {
		if (grim_running()) {
			return -100;
		}
		wlr_box box = thumb_box();
		long changed = grim_differs_inside("with-thumb", "ref-full", box);
		check(changed > 500,
			std::format("the shot shows as a bottom-right thumbnail "
				"({} of {} px changed)", changed,
				static_cast<long>(box.width) * box.height));
		return 200;
	}
	case 6: { // a swipe (a drag) on the thumbnail dismisses it
		wlr_box box = thumb_box();
		drag(box.x + box.width / 2.0, box.y + box.height / 2.0,
			box.x + box.width / 2.0 - 160, box.y + box.height / 2.0);
		return 400;
	}
	case 7: {
		shot("thumb-gone");
		return 400;
	}
	case 8: {
		if (grim_running()) {
			return -100;
		}
		wlr_box box = thumb_box();
		long changed = grim_differs_inside("thumb-gone", "with-thumb", box);
		check(changed > 500,
			std::format("a swipe dismissed the thumbnail ({} of {} px changed)",
				changed, static_cast<long>(box.width) * box.height));
		return 300;
	}
	case 9: { // Ctrl+Super+Shift+3: the clipboard instead of a file
		chord(KEY_3, SUPER_CTRL_SHIFT);
		check(shots_saved() == 1, "the Ctrl chord saved no file");
		run_shell(std::format("wl-paste --no-newline | tee {}/pasted-1.png",
			shot_dir), "paste-1");
		return 400;
	}
	case 10: {
		if (!client_done("paste-1") && retries++ < 50) {
			return -100;
		}
		check(client_code("paste-1") == 0, "wl-paste read the clipboard");
		// pasted-1.png lands in shot_dir; compare as pictures.
		cairo_surface_t *pasted = load_png("pasted-1");
		bool matches = false;
		if (pasted != nullptr) {
			cairo_surface_t *ref = load_png("ref-full");
			matches = pixels_differing(pasted, ref, {cursor_box(40, 40)}) >= 0
				&& pixels_differing(pasted, ref, {cursor_box(40, 40)}) * 1000
					<= 1280 * 720;
			cairo_surface_destroy(ref);
			cairo_surface_destroy(pasted);
		}
		check(matches, "the clipboard holds the shot as PNG, matching grim");
		return 1200; // a later shot keeps its own second
	}
	case 11: { // Super+Shift+4: drag a region
		move_to(40, 40); // the cursor parks outside the crop
		shot_region("ref-region", "300,200 400x300");
		return 400;
	}
	case 12: {
		if (grim_running()) {
			return -100;
		}
		chord(KEY_4, SUPER_SHIFT);
		check(server->capture_ui.picking(), "Super+Shift+4 opened the picker");
		shot("picker-region");
		return 400;
	}
	case 13: {
		if (grim_running()) {
			return -100;
		}
		drag(300, 200, 700, 500);
		check(shots_saved() == 2, "the drag saved a second shot");
		cairo_surface_t *shot_surf = load_newest_shot();
		check(shot_matches(shot_surf, "ref-region", {}, 400, 300),
			"the region shot matches grim's crop of the same area");
		if (shot_surf != nullptr) {
			cairo_surface_destroy(shot_surf);
		}
		return 2300; // let the hint clear while the thumbnail is still up
	}
	case 14: { // dismiss that thumbnail, then reference the window alone
		wlr_box box = thumb_box();
		drag(box.x + box.width / 2.0, box.y + box.height / 2.0,
			box.x + box.width / 2.0 - 160, box.y + box.height / 2.0);
		return 400;
	}
	case 15: {
		if (grim_running()) {
			return -100;
		}
		View *view = cap_window();
		if (view == nullptr) {
			check(false, "the window is still mapped");
			return 200;
		}
		wlr_box f = view_on_screen(*view);
		move_to(f.x + 60.0, f.y + 14.0); // the pointer parks on the title bar
		shot_region("ref-window",
			std::format("{},{} {}x{}", f.x, f.y, f.width, f.height));
		return 400;
	}
	case 16: { // a click on the window captures the window alone
		if (grim_running()) {
			return -100;
		}
		View *view = cap_window();
		wlr_box f = view != nullptr ? view_on_screen(*view) : wlr_box{};
		chord(KEY_4, SUPER_SHIFT);
		check(server->capture_ui.picking(), "the picker opened again");
		click(f.x + 60.0, f.y + 14.0);
		check(shots_saved() == 3, "the click saved a window shot");
		cairo_surface_t *shot_surf = load_newest_shot();
		check(shot_surf != nullptr && view != nullptr
				&& shot_matches(shot_surf, "ref-window",
					{cursor_box(f.x + 60, f.y + 14, f.x, f.y)}, f.width, f.height),
			"the window shot matches grim's crop of its frame, alone");
		if (shot_surf != nullptr) {
			cairo_surface_destroy(shot_surf);
		}
		return 2300;
	}
	case 17: { // dismiss, then Space: window picking on purpose
		wlr_box box = thumb_box();
		drag(box.x + box.width / 2.0, box.y + box.height / 2.0,
			box.x + box.width / 2.0 - 160, box.y + box.height / 2.0);
		return 400;
	}
	case 18: {
		if (grim_running()) {
			return -100;
		}
		View *view = cap_window();
		wlr_box f = view != nullptr ? view_on_screen(*view) : wlr_box{};
		chord(KEY_4, SUPER_SHIFT);
		chord(KEY_SPACE, {});
		click(80, 650); // empty canvas: nothing is picked
		check(server->capture_ui.picking(),
			"Space switched to window picking; empty canvas picks nothing");
		check(shots_saved() == 3, "no shot from a click on the empty canvas");
		click(f.x + 60.0, f.y + 14.0);
		check(shots_saved() == 4, "clicking the window in Space mode shot it");
		return 2300;
	}
	case 19: { // Esc cancels
		wlr_box box = thumb_box();
		drag(box.x + box.width / 2.0, box.y + box.height / 2.0,
			box.x + box.width / 2.0 - 160, box.y + box.height / 2.0);
		return 400;
	}
	case 20: {
		if (grim_running()) {
			return -100;
		}
		chord(KEY_4, SUPER_SHIFT);
		check(server->capture_ui.picking(), "the picker is up for Esc");
		chord(KEY_ESC, {});
		check(!server->capture_ui.picking(), "Esc cancelled the picker");
		check(shots_saved() == 4, "no shot from a cancelled pick");
		return 300;
	}
	case 21: { // Print opens the same picker; Shift+Print takes the screen
		chord(KEY_SYSRQ, {});
		check(server->capture_ui.picking(), "Print opened the picker");
		chord(KEY_ESC, {});
		check(!server->capture_ui.picking(), "Esc closed it again");
		return 1200;
	}
	case 22: {
		move_to(40, 40);
		chord(KEY_SYSRQ, SHIFT);
		check(shots_saved() == 5, "Shift+Print saved a whole-screen shot");
		cairo_surface_t *shot_surf = load_newest_shot();
		check(shot_matches(shot_surf, "ref-full", {cursor_box(40, 40)}, 1280, 720),
			"the Shift+Print shot matches grim of the whole screen");
		if (shot_surf != nullptr) {
			cairo_surface_destroy(shot_surf);
		}
		return 2300;
	}
	case 23: { // a click opens the shot: the xdg-open stub logs its path
		wlr_box box = thumb_box();
		click(box.x + box.width / 2.0, box.y + box.height / 2.0);
		return 600;
	}
	case 24: {
		std::string path;
		load_newest_shot(&path);
		std::string opened = read_file(shot_dir + "/xdg-open.args");
		check(opened == path,
			std::format("a click on the thumbnail opened the shot ({})", opened));
		// clean up: dismiss this thumbnail too
		wlr_box box = thumb_box();
		drag(box.x + box.width / 2.0, box.y + box.height / 2.0,
			box.x + box.width / 2.0 - 160, box.y + box.height / 2.0);
		return 400;
	}
	case 25: { // the toolbar: Super+Shift+5
		if (grim_running()) {
			return -100;
		}
		chord(KEY_5, SUPER_SHIFT);
		check(server->capture_ui.owns_keyboard(), "the toolbar owns the keyboard");
		shot("toolbar");
		return 400;
	}
	case 26: { // destination to the clipboard, then a screen shot from it
		if (grim_running()) {
			return -100;
		}
		click(toolbar_button_x(0, true), toolbar_options_y()); // clipboard on
		click(toolbar_button_x(0, false), toolbar_modes_y()); // shot the screen
		check(shots_saved() == 5, "the toolbar's clipboard option saved no file");
		run_shell(std::format("wl-paste --no-newline | tee {}/pasted-2.png",
			shot_dir), "paste-2");
		return 400;
	}
	case 27: {
		if (!client_done("paste-2") && retries++ < 50) {
			return -100;
		}
		cairo_surface_t *pasted = load_png("pasted-2");
		long bad = -1;
		if (pasted != nullptr) {
			cairo_surface_t *ref = load_png("ref-full");
			bad = pixels_differing(pasted, ref, {cursor_box(40, 40)});
			cairo_surface_destroy(ref);
			cairo_surface_destroy(pasted);
		}
		check(bad >= 0 && bad * 1000 <= 1280 * 720,
			"the toolbar's clipboard shot matches grim");
		return 200;
	}
	case 28: { // the timer: 5 s, then a screen shot that waits for it
		chord(KEY_5, SUPER_SHIFT);
		click(toolbar_button_x(0, true), toolbar_options_y()); // clipboard off
		click(toolbar_button_x(1, true), toolbar_options_y()); // timer 5 s
		click(toolbar_button_x(0, false), toolbar_modes_y()); // shot the screen
		check(server->capture_ui.owns_keyboard(), "the countdown owns the keyboard");
		shot("countdown");
		return 2000; // inside the countdown: no file yet
	}
	case 29: {
		if (grim_running()) {
			return -100;
		}
		check(shots_saved() == 5, "the countdown had not saved anything yet");
		return 4200; // past the 5 s
	}
	case 30: {
		check(shots_saved() == 6, "the shot landed after the countdown");
		return 600; // its thumbnail will be dismissed with the next picker
	}
	case 31: { // capture.pick, region: the answer is deferred until the drag
		wlr_box box = thumb_box();
		drag(box.x + box.width / 2.0, box.y + box.height / 2.0,
			box.x + box.width / 2.0 - 160, box.y + box.height / 2.0);
		run_client("capture.pick", R"({"what":"region"})", "pick-region");
		return 400;
	}
	case 32: {
		if (!acted) { // the picker wait and the drag happen once, not per retry
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100; // the request has not arrived yet
			}
			check(server->capture_ui.picking(), "capture.pick opened the picker");
			drag(400, 150, 600, 350);
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!client_done("pick-region") && retries++ < 30) {
			return -100;
		}
		JsonParser *parser = nullptr;
		JsonObject *result = nullptr;
		client_result("pick-region", &parser, &result);
		bool ok = result != nullptr && member_string(result, "output") != ""
			&& member_int(result, "x") == 400 && member_int(result, "y") == 150
			&& member_int(result, "w") == 200 && member_int(result, "h") == 200;
		check(ok, std::format("capture.pick answered the dragged region ({})",
			client_out("pick-region")));
		if (parser != nullptr) {
			g_object_unref(parser);
		}
		return 300;
	}
	case 33: { // capture.pick, window: the window's id and identifier
		run_client("capture.pick", R"({"what":"window"})", "pick-window");
		return 400;
	}
	case 34: {
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			View *first = cap_window();
			wlr_box f = first != nullptr ? view_on_screen(*first) : wlr_box{};
			click(f.x + 60.0, f.y + 14.0);
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		View *view = cap_window();
		if (!client_done("pick-window") && retries++ < 30) {
			return -100;
		}
		JsonParser *parser = nullptr;
		JsonObject *result = nullptr;
		client_result("pick-window", &parser, &result);
		bool ok = result != nullptr && view != nullptr
			&& member_int(result, "window") == static_cast<long long>(view->session_id)
			&& !member_string(result, "identifier").empty();
		check(ok, std::format("capture.pick answered the picked window by id "
			"and identifier ({})", client_out("pick-window")));
		if (parser != nullptr) {
			g_object_unref(parser);
		}
		return 300;
	}
	case 35: { // capture.pick, output and any
		run_client("capture.pick", R"({"what":"output"})", "pick-output");
		return 400;
	}
	case 36: {
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			click(80, 650);
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!client_done("pick-output") && retries++ < 30) {
			return -100;
		}
		std::string out = client_out("pick-output");
		check(out.find("\"output\"") != std::string::npos,
			std::format("capture.pick answered the output ({})", out));
		run_client("capture.pick", R"({"what":"any"})", "pick-any");
		return 400;
	}
	case 37: { // any: the empty canvas picks the output, a window the window
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			click(80, 650);
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!client_done("pick-any") && retries++ < 30) {
			return -100;
		}
		std::string out = client_out("pick-any");
		check(out.find("\"output\"") != std::string::npos,
			std::format("capture.pick in any mode picked the output on the empty "
				"canvas ({})", out));
		run_client("capture.pick", R"({"what":"any"})", "pick-any2");
		return 400;
	}
	case 38: {
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			View *view = cap_window();
			wlr_box f = view != nullptr ? view_on_screen(*view) : wlr_box{};
			click(f.x + 60.0, f.y + 14.0);
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!client_done("pick-any2") && retries++ < 30) {
			return -100;
		}
		std::string out = client_out("pick-any2");
		check(out.find("\"window\"") != std::string::npos,
			std::format("capture.pick in any mode picked the window ({})", out));
		return 300;
	}
	case 39: { // only one pick at a time
		chord(KEY_4, SUPER_SHIFT);
		run_client("capture.pick", R"({"what":"region"})", "pick-busy");
		return 400;
	}
	case 40: {
		if (!client_done("pick-busy") && retries++ < 30) {
			return -100;
		}
		check(client_code("pick-busy") == 1
				&& client_err("pick-busy").find("already running") != std::string::npos,
			"a second pick is answered with an error");
		chord(KEY_ESC, {}); // close the key-opened picker
		check(!server->capture_ui.picking(), "Esc closed the key-opened picker");
		return 300;
	}
	case 41: { // a cancel is an error answer
		run_client("capture.pick", R"({"what":"region"})", "pick-cancel");
		return 400;
	}
	case 42: {
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			chord(KEY_ESC, {});
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!client_done("pick-cancel") && retries++ < 30) {
			return -100;
		}
		check(client_code("pick-cancel") == 1
				&& client_err("pick-cancel").find("cancelled") != std::string::npos,
			"cancelling the pick answers with an error");
		return 300;
	}
	case 43: { // params are checked
		run_client("capture.pick", R"({"what":"nope"})", "pick-bad");
		return 400;
	}
	case 44: {
		if (!client_done("pick-bad") && retries++ < 30) {
			return -100;
		}
		check(client_code("pick-bad") == 1,
			"a bad `what` is answered with the usage error");
		return 300;
	}
	case 45: { // the chooser bridge: both monitors and windows on stdin
		std::string name = server->outputs.front()->wlr->name;
		JsonParser *parser = nullptr;
		JsonObject *result = nullptr;
		client_result("pick-window", &parser, &result);
		std::string identifier = result != nullptr
			? member_string(result, "identifier") : "";
		if (parser != nullptr) {
			g_object_unref(parser);
		}
		if (identifier.empty()) {
			check(false, "the window pick had an identifier for the bridge list");
			return 200;
		}
		write_file(shot_dir + "/chooser.txt", std::format(
			"Monitor: {} the headless output\nWindow: cap ({})\n", name, identifier));
		run_bridge("chooser.txt", "bridge-output");
		return 400;
	}
	case 46: { // the bridge asks `any`; a click on the canvas picks the monitor
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			click(80, 650);
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!client_done("bridge-output") && retries++ < 30) {
			return -100;
		}
		std::string name = server->outputs.front()->wlr->name;
		check(client_out("bridge-output")
				== std::format("Monitor: {} the headless output", name)
			&& client_code("bridge-output") == 0,
			"the bridge printed the chosen monitor's line, byte-exactly");
		run_bridge("chooser.txt", "bridge-window");
		return 400;
	}
	case 47: { // the same list, a window chosen
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			View *view = cap_window();
			wlr_box f = view != nullptr ? view_on_screen(*view) : wlr_box{};
			click(f.x + 60.0, f.y + 14.0);
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!client_done("bridge-window") && retries++ < 30) {
			return -100;
		}
		JsonParser *parser = nullptr;
		JsonObject *result = nullptr;
		client_result("pick-window", &parser, &result);
		std::string identifier = result != nullptr
			? member_string(result, "identifier") : "";
		if (parser != nullptr) {
			g_object_unref(parser);
		}
		check(client_out("bridge-window")
				== std::format("Window: cap ({})", identifier)
			&& client_code("bridge-window") == 0,
			"the bridge printed the chosen window's line, byte-exactly");
		run_bridge("chooser.txt", "bridge-cancel");
		return 400;
	}
	case 48: { // a cancelled bridge prints nothing and exits 0
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			chord(KEY_ESC, {});
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!client_done("bridge-cancel") && retries++ < 30) {
			return -100;
		}
		check(client_out("bridge-cancel").empty() && client_code("bridge-cancel") == 0,
			"a cancelled chooser bridge prints nothing, exiting 0");
		return 300;
	}
	case 49: { // a recording from the toolbar, whole screen
		chord(KEY_5, SUPER_SHIFT);
		// the timer option still reads 5 s from the countdown test: cycle
		// it back to none (5 -> 10 -> none)
		click(toolbar_button_x(1, true), toolbar_options_y());
		click(toolbar_button_x(1, true), toolbar_options_y());
		click(toolbar_button_x(3, false), toolbar_modes_y()); // record the screen
		return 600;
	}
	case 50: {
		if (!server->capture_ui.recording() && retries++ < 30) {
			return -100;
		}
		if (acted) {
			return -100; // the state client is on its way
		}
		std::string path = server->capture_ui.recording_path();
		check(server->capture_ui.recording()
				&& path.starts_with(videos_dir)
				&& path.find("Screen Recording") != std::string::npos,
			std::format("the recording started into the videos dir ({})", path));
		check(read_file(events_path).find("capture.recording") != std::string::npos,
			"the capture.recording event told the bar");
		run_client("state", "", "state-recording");
		acted = true;
		return 400;
	}
	case 51: {
		if (!client_done("state-recording") && retries++ < 30) {
			return -100;
		}
		std::string rec = state_recording("state-recording");
		check(!rec.empty() && rec != "none",
			std::format("the state reports the recording ({})", rec));
		shot("recording-hint");
		return 2500; // some footage
	}
	case 52: { // Alt+Print stops it
		if (grim_running()) {
			return -100;
		}
		chord(KEY_SYSRQ, ALT);
		return 400;
	}
	case 53: {
		if (server->capture_ui.recording() && retries++ < 30) {
			return -100;
		}
		if (acted) {
			return -100; // the state client is on its way
		}
		check(!server->capture_ui.recording(), "Alt+Print stopped the recording");
		run_client("state", "", "state-stopped");
		acted = true;
		return 400;
	}
	case 54: {
		if (!client_done("state-stopped") && retries++ < 30) {
			return -100;
		}
		check(state_recording("state-stopped") == "none",
			"the state reports no recording any more");
		std::vector<std::string> files = files_with(videos_dir, "Screen Recording");
		check(files.size() == 1 && !files.empty(),
			"one recording file was left behind");
		if (!files.empty()) {
			int code = run_sync(std::format(
				"ffprobe -v error -select_streams v:0 "
					"-show_entries stream=width,height -show_entries format=duration "
					"-of csv=p=0 \"{}\" | tr '\\n' ' '",
				files.front()), "ffprobe-screen.txt");
			std::string probe = read_file(shot_dir + "/ffprobe-screen.txt");
			check(code == 0 && probe_matches(probe, 1280, 720, 0.5),
				std::format("ffprobe reads the recording as a 1280x720 video ({})",
					probe));
		}
		return 300;
	}
	case 55: { // capture.stop on nothing is an error
		run_client("capture.stop", "", "stop-idle");
		return 400;
	}
	case 56: {
		if (!client_done("stop-idle") && retries++ < 30) {
			return -100;
		}
		check(client_code("stop-idle") == 1
				&& client_err("stop-idle").find("not recording") != std::string::npos,
			"capture.stop with nothing running is an error");
		return 200;
	}
	case 57: { // a region recording, stopped through the IPC
		chord(KEY_5, SUPER_SHIFT);
		click(toolbar_button_x(5, false), toolbar_modes_y()); // record a region
		return 600;
	}
	case 58: {
		if (!acted) {
			if (!server->capture_ui.picking() && retries++ < 30) {
				return -100;
			}
			drag(150, 100, 550, 400);
			acted = true;
			retries = 0; // a fresh budget for the answer's wait
		}
		if (!server->capture_ui.recording() && retries++ < 30) {
			return -100;
		}
		check(server->capture_ui.recording(), "the region recording started");
		return 2200; // some footage
	}
	case 59: {
		run_client("capture.stop", "", "stop-region");
		return 400;
	}
	case 60: {
		if ((!client_done("stop-region") || server->capture_ui.recording())
				&& retries++ < 30) {
			return -100;
		}
		check(client_code("stop-region") == 0, "capture.stop answered true");
		check(!server->capture_ui.recording(), "the region recording stopped");
		std::vector<std::string> files = files_with(videos_dir, "Screen Recording");
		check(files.size() == 2, "two recordings, one per run");
		if (files.size() == 2) {
			run_sync(std::format(
				"ffprobe -v error -select_streams v:0 "
					"-show_entries stream=width,height -show_entries format=duration "
					"-of csv=p=0 \"{}\" | tr '\\n' ' '",
				files.front()), "ffprobe-region.txt");
			std::string probe = read_file(shot_dir + "/ffprobe-region.txt");
			check(probe_matches(probe, 400, 300, 0.5),
				std::format("ffprobe reads the region recording as 400x300 ({})",
					probe));
		}
		return 300;
	}
	case 61: { // keys pressed while the picker is up never reach a client
		move_to(40, 40); // the cursor stays out of the compared crop
		View *view = cap_window();
		wlr_box f = view != nullptr ? view_on_screen(*view) : wlr_box{};
		check(f.width > 0, "the window is still there to type at");
		term_geometry = std::format("{},{} {}x{}", f.x, f.y, f.width, f.height);
		shot_region("term-before", term_geometry);
		return 400;
	}
	case 62: {
		if (grim_running()) {
			return -100;
		}
		// Everything the picker consumes: open, Space, letters, Esc, twice.
		chord(KEY_4, SUPER_SHIFT);
		chord(KEY_SPACE, {});
		chord(KEY_A, {});
		chord(KEY_ESC, {});
		chord(KEY_PRINT, {});
		chord(KEY_ESC, {});
		shot_region("term-after", term_geometry);
		return 400;
	}
	case 63: {
		if (grim_running()) {
			return -100;
		}
		cairo_surface_t *before = load_png("term-before");
		cairo_surface_t *after = load_png("term-after");
		long changed = pixels_differing(before, after);
		long total = before != nullptr && after != nullptr
			? static_cast<long>(cairo_image_surface_get_width(before))
				* cairo_image_surface_get_height(before)
			: 0;
		// The caret may blink; nothing else may change: no key reached foot.
		check(changed >= 0 && total > 0 && changed * 1000 <= total,
			std::format("no key the picker saw reached the window ({}/{} px)",
				changed, total));
		if (before != nullptr) {
			cairo_surface_destroy(before);
		}
		if (after != nullptr) {
			cairo_surface_destroy(after);
		}
		return 300;
	}
	case 64: { // the end: nothing recording, the event closed it
		check(!server->capture_ui.recording(), "no recording is left running");
		check(read_file(events_path).find("\"active\":false") != std::string::npos,
			"the capture.recording event closed the recording");
		finished = true;
		run_client("quit", "", "quit");
		return 300;
	}
	default:
		std::println("{}: {} failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
		return -1; // `msg quit` already terminated the display
	}
}

int handle_timer() {
	if (grim_pid > 0) {
		if (waitpid(grim_pid, nullptr, WNOHANG) == 0) {
			wl_event_source_timer_update(timer, 30);
			return 0;
		}
		grim_pid = -1;
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
	acted = false; // the next step's interactions start fresh
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

} // namespace

int main() {
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr) {
		std::fprintf(stderr, "capture: $WAYLAND_CANVAS_BIN is not set (meson passes it)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/capture", root);
	// A fresh world every run: the shots and recordings of a previous run
	// must never count as this run's.
	std::error_code ec;
	std::filesystem::remove_all(shot_dir, ec);
	mkdir_p(shot_dir);
	// Nothing touches the real Pictures, Videos or clipboard: both user
	// dirs point into the scratch tree, and xdg-open is a stub that logs
	// what it was asked to open. glib caches the user dirs, so this has to
	// happen before the server exists.
	std::string conf = std::format("{}/config", shot_dir);
	std::string bin = std::format("{}/bin", shot_dir);
	pictures_dir = std::format("{}/pictures", shot_dir);
	videos_dir = std::format("{}/videos", shot_dir);
	mkdir_p(conf);
	mkdir_p(bin);
	mkdir_p(pictures_dir);
	mkdir_p(videos_dir);
	write_file(std::format("{}/user-dirs.dirs", conf), std::format(
		"XDG_PICTURES_DIR=\"{}\"\nXDG_VIDEOS_DIR=\"{}\"\n", pictures_dir, videos_dir));
	write_file(std::format("{}/xdg-open", bin), std::format(
		"#!/bin/sh\nprintf '%s\\n' \"$1\" > {}/xdg-open.args\n", shot_dir));
	chmod((bin + "/xdg-open").c_str(), 0755);
	setenv("XDG_CONFIG_HOME", conf.c_str(), 1);
	std::string path = std::format("{}:{}", bin, std::getenv("PATH") != nullptr
		? std::getenv("PATH") : "");
	setenv("PATH", path.c_str(), 1);

	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *data) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display); // `msg quit` ends it
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	wl_event_source_remove(timer);
	if (sub_pid > 0) {
		kill(sub_pid, SIGINT);
		waitpid(sub_pid, nullptr, 0);
	}
	wlr_pointer_finish(&ptr);
	server.reset(); // releases the control socket and the clients
	return failures != 0;
}
