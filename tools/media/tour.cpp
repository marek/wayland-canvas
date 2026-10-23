// The README's media (docs/media/): the compositor on the headless backend at
// a laptop panel's size and scale, real apps arranged the way the README says
// people work — code and writing on tiled desktops, design and review spread
// over a desktop at 50 % like papers on a table — its stills taken and its
// walkthrough recorded, a caption per step. tools/media.py builds the world it
// runs in and turns what it writes into docs/media/; nothing of the user's own
// profiles, accounts or buses is read. Runs on the scenarios' harness.
#include <algorithm>
#include <cairo.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "canvas/overview_chrome.h"
#include "core/config.h"
#include "core/server.h"
#include "core/process.h"
#include "core/wlr.h"
#include "harness/bus.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "harness/steps.h"
#include "platform/output.h"
#include "render/capture.h"
#include "windows/view.h"

using namespace harness;

namespace {

// A laptop panel: 3200x2000 pixels at scale 2.
constexpr int panel_w = 1600, panel_h = 1000;
constexpr double panel_scale = 2;
constexpr int record_fps = 30;
// The walkthrough is encoded at the panel's logical size.
constexpr int video_w = panel_w, video_h = panel_h;
// What the tour leaves in its out/ folder besides the stills; tools/media.py
// learns these names, the size above and the stills taken from manifest.json.
constexpr const char *walkthrough_file = "walkthrough-raw.mp4";
constexpr const char *captions_file = "captions.tsv";

// Desktop 1 (code) is the compositor's own, at the output's box. Writing
// sits beside it; the table, at half scale, under both.
constexpr wlr_box writing_frame{1700, 0, panel_w, panel_h};
constexpr double table_scale = 0.5;
constexpr wlr_box table_frame{50, 1150, 3200, 2000};

// Where a window's frame goes on the canvas, and its content size.
struct Spot {
	int x, y, w, h;
};

// The table: references on the left, the drawing in the middle where a
// zoom to 100 % lands, the web pages under review on the right, the music
// player in a corner.
constexpr Spot reference_spots[] = {
	{170, 1270, 380, 500},
	{580, 1270, 380, 500},
	{170, 1840, 380, 500},
	{580, 1840, 380, 500},
};
constexpr Spot sketch_spot{1010, 1690, 1280, 860};
constexpr Spot review_spots[] = {
	{2340, 1270, 800, 680},
	{2340, 2020, 800, 680},
};
constexpr Spot music_spot{170, 2440, 400, 400};

// A sticky note on the table, as the notes plugin keeps it: canvas box,
// colour, and the HTML of its text.
struct Note {
	int x, y, w, h;
	const char *color;
	const char *html;
};
constexpr Note table_notes[] = {
	{1010, 1260, 380, 200, "#ffd866",
		"<p><b>References</b></p><p>Haeckel plates: the orchid and hummingbird palette</p>"},
	{1450, 1260, 380, 200, "#a6e3a1",
		"<p><b>Sketch v2</b></p><p>Thicker contour, fewer hatch lines on the armour</p>"},
	{1890, 1260, 380, 200, "#7dcfff",
		"<p><b>Review</b></p><p>Front page or feed: one big story, or a dense list?</p>"},
	{620, 2490, 440, 150, "#f4b8e4", "<p>❤️ Murder Mittens is the best</p>"},
};

// A web page by its name in tools/media/pages.tsv, and a word of its title
// that tells its window from the browser's others.
struct Page {
	const char *name;
	const char *title;
};
constexpr Page code_page{"code", "Wayland"};
constexpr Page writing_page{"writing", "Protocol Specification"};
constexpr Page review_pages[] = {{"news", "Star"}, {"forum", "programming"}};
constexpr const char *reference_files[] = {"orchids.jpg", "hummingbirds.jpg",
	"anemones.jpg", "mosses.jpg"};
constexpr const char *sketch_file = "rhinoceros.jpg";
constexpr const char *music_file = "gymnopedie.mp3";

// The editor files: copies of the repo's own, so typing changes no source.
constexpr const char *code_file = "src/canvas/camera.cpp";
constexpr int code_line = 17;
constexpr const char *writing_file = "docs/intents/overview.md";
constexpr int writing_line = 20;

// How long an app may take to show its first window.
constexpr int app_wait_ms = 60000;
constexpr int poll_ms = 100;
constexpr Wait app_wait{.poll_ms = poll_ms, .budget_ms = app_wait_ms};

bool stills_only = false;
bool walkthrough_taken = false;
std::string assets, source, out_dir, tools_dir;
std::vector<std::string> stills_taken;

Child standins;
std::deque<Child> apps;

// A file, with the folders it lives in made.
void put(const std::string &path, std::string_view text) {
	mkdir_p(std::filesystem::path(path).parent_path().string());
	write_file(path, text);
}

std::string env_or(const char *name, const std::string &fallback) {
	const char *value = std::getenv(name);
	return value != nullptr && *value != '\0' ? value : fallback;
}

// Ends the run from outside the steps (a timer's callback).
void fail(std::string_view what) {
	check(false, what);
	wl_display_terminate(server->display);
}

// Ends the run from a step.
Result abandon(std::string_view what) {
	check(false, what);
	return finish();
}

// An app on the canvas: its own process group, so the whole of it — a
// browser's helpers included — goes when the tour ends.
void launch(const std::string &cmd) {
	bool started = apps.emplace_back().start({"/bin/sh", "-c", cmd},
		{.own_group = true, .log = scratch() + "/apps.log"});
	if (!started) {
		fail(std::format("cannot start {}", cmd));
	}
}

std::string shell_word(std::string_view text) {
	std::string out = "'";
	for (char c : text) {
		out += c == '\'' ? std::string{"'\\''"} : std::string(1, c);
	}
	return out + "'";
}

// The bar's stand-in services (tests/scenarios/bar-standins.py): Wi-Fi,
// Bluetooth, a battery and power profiles, so the bar shows what a laptop's
// does. Without them the bar shows only the widgets that need no service.
void start_standins() {
	const char *py = std::getenv("CANVAS_BAR_STANDIN_PY");
	const char *script = std::getenv("CANVAS_BAR_STANDINS");
	if (py == nullptr || *py == '\0' || script == nullptr || *script == '\0') {
		std::println(stderr, "media: no bar stand-ins; the bar shows fewer widgets");
		return;
	}
	// The script logs under $CANVAS_TMP/scenarios/bar, which the bar scenario
	// makes: the tour gives it a folder of its own.
	std::string logs = scratch_dir("standins");
	mkdir_p(logs + "/scenarios/bar");
	if (!standins.start({"env", "CANVAS_TMP=" + logs, py, script})
			|| !standins.prints("READY\n", 30'000)) {
		std::println(stderr, "media: the bar stand-ins did not become ready");
		standins.stop();
	}
}

// The weather widget asks wttr.in through curl: answered from the bar
// scenario's canned response, so no location is looked up.
void start_curl_standin() {
	const char *script = std::getenv("CANVAS_BAR_STANDINS");
	if (script == nullptr || *script == '\0') {
		return;
	}
	std::string fixture = (std::filesystem::path(script).parent_path()
		/ "bar-standins/wttr-j1.json").string();
	std::string dir = scratch_dir("bin");
	write_script(dir + "/curl", "#!/bin/sh\n"
		"# The tour's curl: wttr.in from a canned response, anything else real.\n"
		"for arg in \"$@\"; do\n"
		"\tcase \"$arg\" in\n"
		"\t*wttr.in*) exec cat " + shell_word(fixture) + " ;;\n"
		"\tesac\n"
		"done\n"
		"PATH=${PATH#*:} exec curl \"$@\"\n");
	put_first_on_path(dir);
}

std::string note_file(const Note &note) {
	return std::format("<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
		"<meta name=\"canvas-note\" content=\"x={} y={} width={} height={}\">\n"
		"<style>\nbody {{ background: {}; color: #1e1e1e; font-family: \"Noto Sans\";"
		" font-size: 26px; }}\np {{ margin-top: 0; margin-bottom: 0; white-space: pre-wrap; }}\n"
		"</style>\n</head>\n<body>\n{}\n</body>\n</html>\n",
		note.x, note.y, note.w, note.h, note.color, note.html);
}

void copy_into_checkout(const char *file) {
	std::optional<std::string> text = read_file(source + "/" + file);
	bool copied = text.has_value();
	if (copied) {
		put(std::format("{}/checkout/{}", scratch(), file), *text);
	}
	check(copied, std::format("{} copied for the editors", file));
}

// Scratch HOME and XDG dirs, the compositor's and the shell's settings,
// the notes on the table, and the files the editors open.
void build_world() {
	scratch_env("HOME", "home");
	std::string config = scratch_env("XDG_CONFIG_HOME", "home/.config");
	scratch_env("XDG_DATA_HOME", "home/.local/share");
	std::string state = scratch_env("XDG_STATE_HOME", "home/.local/state");
	scratch_env("XDG_CACHE_HOME", "home/.cache");
	setenv("GSETTINGS_BACKEND", "keyfile", 1);
	setenv("XKB_DEFAULT_LAYOUT", "us", 1);
	setenv("TERMINAL", "foot", 1);
	setenv("MEDIA_SOURCE", source.c_str(), 1);
	setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
	// The tour is its own session: nothing may reach the user's compositor.
	for (const char *name : {"WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY",
			"WAYLAND_CANVAS_SOCKET"}) {
		unsetenv(name);
	}
	// One icon theme for the compositor's launcher (gsettings), the apps
	// (GTK) and the shell (Quickshell).
	std::string icons = env_or("MEDIA_ICON_THEME", "Adwaita");
	put(config + "/glib-2.0/settings/keyfile", std::format(
		"[org/gnome/desktop/interface]\nicon-theme='{}'\n", icons));
	put(config + "/gtk-3.0/settings.ini", std::format(
		"[Settings]\ngtk-icon-theme-name={}\n", icons));
	setenv("QS_ICON_THEME", icons.c_str(), 1);
	// imv's own title names the file by its whole path
	put(config + "/imv/config",
		"[options]\ntitle_text = $(basename \"$imv_current_file\")\n");

	put(config + "/wayland-canvas/canvas.conf", "[session]\nlock-idle = off\n");
	// The tray stand-in's item has no icon, and the agents panel reads the
	// user's own accounts: both stay out of the bar.
	put(config + "/wayland-canvas/shell.json",
		"{\n\t\"plugins\": {\n\t\t\"canvas.agents\": {\"enabled\": false}\n\t},\n"
		"\t\"bar\": {\"hidden\": [\"canvas.tray/tray\"]}\n}\n");
	for (std::size_t i = 0; i < std::size(table_notes); i++) {
		put(std::format("{}/wayland-canvas/plugins/canvas.notes/{}.html", state, i + 1),
			note_file(table_notes[i]));
	}
	copy_into_checkout(code_file);
	copy_into_checkout(writing_file);
}

std::string foot(std::string_view dir, std::string_view command) {
	return std::format("foot -c {} -D {} {}", shell_word(tools_dir + "/foot.ini"),
		shell_word(dir), command);
}

std::string editor(std::string_view file, int line) {
	return foot(scratch() + "/checkout", std::format("nvim -u {} -i NONE -n +{} {}",
		shell_word(tools_dir + "/init.lua"), line, shell_word(file)));
}

// The URL a page was recorded at ($MEDIA_PAGES: `name<TAB>url` lines), or
// empty when it was not.
std::string page_url(std::string_view name) {
	std::ifstream index(env_or("MEDIA_PAGES", ""));
	std::string line;
	while (std::getline(index, line)) {
		std::size_t tab = line.find('\t');
		if (tab != std::string::npos && line.substr(0, tab) == name) {
			return line.substr(tab + 1);
		}
	}
	return {};
}

// Chrome in its scratch profile; $MEDIA_CHROME_FLAGS, one flag a line,
// points it at the replayed pages.
std::string chrome(std::string_view url) {
	std::string flags;
	std::istringstream extra(env_or("MEDIA_CHROME_FLAGS", ""));
	for (std::string flag; std::getline(extra, flag);) {
		if (!flag.empty()) {
			flags += " " + shell_word(flag);
		}
	}
	return std::format("google-chrome-stable --user-data-dir={} --ozone-platform=wayland"
		" --no-first-run --no-default-browser-check --disable-sync --password-store=basic"
		" --disable-search-engine-choice-screen --hide-crash-restore-bubble"
		" --disable-features=PrivacySandboxSettings4,ChromeWhatsNewUI{}"
		" --new-window {}", shell_word(scratch() + "/chrome"), flags, shell_word(url));
}

// A hand on the touchpad, the pointer or the keys over time: one at a
// time, eased, through the devices' own events.
struct Motion {
	enum class Kind { swipe, pinch, drag, typing };
	Kind kind = Kind::swipe;
	int fingers = 0;
	double dx = 0, dy = 0; // a swipe's travel, or the pointer's for a drag
	double scale = 1; // the scale a pinch ends at
	double ox = 0, oy = 0; // where a drag starts, output-local
	std::string text; // what is typed
	double ms = 0;
	double start = 0;
	double sent_x = 0, sent_y = 0;
	std::size_t typed = 0;
};
std::optional<Motion> motion;
wl_event_source *motion_timer = nullptr;
constexpr int motion_tick_ms = 8;

double ease(double p) {
	return p * p * (3 - 2 * p);
}

void start_motion(Motion m) {
	m.start = now_ms();
	switch (m.kind) {
	case Motion::Kind::swipe:
		touchpad_swipe_begin(m.fingers);
		break;
	case Motion::Kind::pinch:
		touchpad_pinch_begin(m.fingers);
		break;
	case Motion::Kind::drag:
		move_to(m.ox, m.oy);
		button(BTN_LEFT, true);
		break;
	case Motion::Kind::typing:
		break;
	}
	motion = std::move(m);
	wl_event_source_timer_update(motion_timer, motion_tick_ms);
}

int tick_motion() {
	if (!motion) {
		return 0;
	}
	Motion &m = *motion;
	double p = m.ms > 0 ? std::clamp((now_ms() - m.start) / m.ms, 0.0, 1.0) : 1.0;
	double e = ease(p);
	switch (m.kind) {
	case Motion::Kind::swipe: {
		double x = m.dx * e, y = m.dy * e;
		touchpad_swipe_update(m.fingers, x - m.sent_x, y - m.sent_y);
		m.sent_x = x;
		m.sent_y = y;
		if (p >= 1) {
			touchpad_swipe_end();
		}
		break;
	}
	case Motion::Kind::pinch:
		touchpad_pinch_update(m.fingers, 1 + (m.scale - 1) * e);
		if (p >= 1) {
			touchpad_pinch_end();
		}
		break;
	case Motion::Kind::drag:
		move_to(m.ox + m.dx * e, m.oy + m.dy * e);
		if (p >= 1) {
			button(BTN_LEFT, false);
		}
		break;
	case Motion::Kind::typing: {
		auto due = static_cast<std::size_t>(p * static_cast<double>(m.text.size()));
		while (m.typed < due) {
			type_char(m.text[m.typed++]);
		}
		break;
	}
	}
	if (p >= 1) {
		motion.reset();
		return 0;
	}
	wl_event_source_timer_update(motion_timer, motion_tick_ms);
	return 0;
}

void swipe_over(int fingers, double dx, double dy, double ms) {
	start_motion(Motion{.kind = Motion::Kind::swipe, .fingers = fingers, .dx = dx, .dy = dy,
		.ms = ms});
}

void pinch_over(int fingers, double scale, double ms) {
	start_motion(Motion{.kind = Motion::Kind::pinch, .fingers = fingers, .scale = scale,
		.ms = ms});
}

void drag_over(double ox, double oy, double dx, double dy, double ms) {
	start_motion(Motion{.kind = Motion::Kind::drag, .dx = dx, .dy = dy, .ox = ox, .oy = oy,
		.ms = ms});
}

// Typed at a person's pace.
void type_over(std::string_view text, double ms_per_key = 70) {
	start_motion(Motion{.kind = Motion::Kind::typing, .text = std::string{text},
		.ms = ms_per_key * static_cast<double>(text.size())});
}

std::vector<View *> claimed;
// The table's windows and their spots: placed again once their clients
// have settled on a size of their own.
std::vector<std::pair<View *, Spot>> placed;

bool holds(const char *text, std::string_view part) {
	return part.empty() || (text != nullptr && std::string_view{text}.find(part) != std::string_view::npos);
}

// A window nobody has claimed yet whose app id and title hold the parts:
// the one the last launch opened. A browser opens every window from its
// first process, so the title tells its windows apart.
View *fresh_view(std::string_view app_id, std::string_view title) {
	for (View *view : server->views) {
		if (view->mapped && view->kind() == ViewKind::normal
				&& std::ranges::find(claimed, view) == claimed.end()
				&& holds(view->shell_app_id(), app_id) && holds(view->shell_title(), title)) {
			return view;
		}
	}
	return nullptr;
}

// The window's frame at the spot's corner, its content at the spot's size.
void place(View &view, const Spot &spot) {
	if (view.mode == WindowMode::maximized) {
		view.set_maximized(false);
	}
	view.configure_size(spot.w, spot.h);
	wlr_box content = view.content_box(), frame = view.frame_box();
	view.move(spot.x + content.x - frame.x, spot.y + content.y - frame.y);
}

void place_on_table(View &view, const Spot &spot) {
	place(view, spot);
	placed.emplace_back(&view, spot);
}

void look_at(const Desktop &desktop) {
	output().camera.jump(camera_for_box(desktop.frame(), desktop.scale()));
}

Desktop *desktop_numbered(int number) {
	return server->canvas.desktops.by_number(number);
}

// Output-local centre of a canvas box under the current camera.
Point on_screen(wlr_box box) {
	return output().canvas_to_local(box.x + box.width / 2.0, box.y + box.height / 2.0);
}

std::string still_file(std::string_view name) {
	return std::format("{}.png", name);
}

Result take_still(const char *name) {
	Output &o = output();
	CairoSurfacePtr shot = capture_output(o, wlr_box{0, 0, o.width, o.height});
	if (shot == nullptr) {
		return abandon(std::format("the {} still could not be captured", name));
	}
	std::string path = std::format("{}/{}", out_dir, still_file(name));
	if (cairo_surface_write_to_png(shot.get(), path.c_str()) != CAIRO_STATUS_SUCCESS) {
		return abandon(std::format("cannot write {}", path));
	}
	stills_taken.emplace_back(name);
	std::println("media: still {}", path);
	return after(300);
}

// An arrow where the pointer is: offscreen captures leave the cursor out.
void draw_cursor(cairo_surface_t *frame) {
	cairo_t *cr = cairo_create(frame);
	cairo_translate(cr, server->cursor->x * panel_scale, server->cursor->y * panel_scale);
	cairo_scale(cr, panel_scale, panel_scale);
	cairo_move_to(cr, 0, 0);
	cairo_line_to(cr, 0, 17);
	cairo_line_to(cr, 4.2, 13);
	cairo_line_to(cr, 7.2, 19.6);
	cairo_line_to(cr, 9.8, 18.4);
	cairo_line_to(cr, 6.9, 12);
	cairo_line_to(cr, 12, 12);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill_preserve(cr);
	cairo_set_line_width(cr, 1.1);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_stroke(cr);
	cairo_destroy(cr);
	cairo_surface_mark_dirty(frame);
}

// The walkthrough: offscreen captures piped to ffmpeg at a steady rate.
// A capture slower than a frame is written again for every frame it
// covered, so the video keeps the wall clock the animations run on.
struct Recording {
	pid_t pid = -1;
	int fd = -1;
	double start = 0;
	long frames = 0;
	int width = 0, height = 0;
	wl_event_source *timer = nullptr;
};
Recording rec;

struct Caption {
	double from, to;
	std::string text;
};
std::vector<Caption> captions;

double rec_ms() {
	return now_ms() - rec.start;
}

void caption(std::string_view text) {
	if (!captions.empty() && captions.back().to < 0) {
		captions.back().to = rec_ms();
	}
	captions.push_back({rec_ms(), -1, std::string{text}});
}

void end_caption() {
	if (!captions.empty() && captions.back().to < 0) {
		captions.back().to = rec_ms();
	}
}

bool write_frame(cairo_surface_t *frame) {
	const unsigned char *data = cairo_image_surface_get_data(frame);
	int stride = cairo_image_surface_get_stride(frame);
	for (int y = 0; y < rec.height; y++) {
		const unsigned char *row = data + static_cast<std::size_t>(y) * stride;
		std::size_t left = static_cast<std::size_t>(rec.width) * 4;
		while (left > 0) {
			ssize_t n = write(rec.fd, row, left);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				return false;
			}
			row += n;
			left -= static_cast<std::size_t>(n);
		}
	}
	return true;
}

int pump_frames() {
	if (rec.fd < 0) {
		return 0;
	}
	long due = static_cast<long>(rec_ms() * record_fps / 1000.0) + 1;
	if (due > rec.frames) {
		Output &o = output();
		CairoSurfacePtr frame = capture_output(o, wlr_box{0, 0, o.width, o.height});
		if (frame == nullptr || cairo_image_surface_get_width(frame.get()) != rec.width
				|| cairo_image_surface_get_height(frame.get()) != rec.height) {
			fail("a walkthrough frame could not be captured");
			return 0;
		}
		cairo_surface_flush(frame.get());
		draw_cursor(frame.get());
		for (; rec.frames < due; rec.frames++) {
			if (!write_frame(frame.get())) {
				fail("the walkthrough encoder stopped");
				return 0;
			}
		}
	}
	double next = rec.start + rec.frames * 1000.0 / record_fps;
	wl_event_source_timer_update(rec.timer, std::max(1, static_cast<int>(next - now_ms())));
	return 0;
}

bool start_recording() {
	Output &o = output();
	rec.width = static_cast<int>(std::lround(o.width * panel_scale));
	rec.height = static_cast<int>(std::lround(o.height * panel_scale));
	std::string path = std::format("{}/{}", out_dir, walkthrough_file);
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(fds[0], STDIN_FILENO);
		close_range(STDERR_FILENO + 1, ~0U, 0);
		std::string size = std::format("{}x{}", rec.width, rec.height);
		std::string fps = std::format("{}", record_fps);
		std::string scale = std::format("scale={}:{}:flags=lanczos", video_w, video_h);
		execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
			"-f", "rawvideo", "-pix_fmt", "bgra", "-s", size.c_str(), "-framerate", fps.c_str(),
			"-i", "pipe:0", "-vf", scale.c_str(), "-c:v", "libx264", "-preset", "veryfast",
			"-crf", "14", "-pix_fmt", "yuv420p", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[0]);
	if (pid < 0) {
		close(fds[1]);
		return false;
	}
	rec.pid = pid;
	rec.fd = fds[1];
	rec.start = now_ms();
	rec.frames = 0;
	captions.clear();
	rec.timer = wl_event_loop_add_timer(server->loop, [](void *) { return pump_frames(); },
		nullptr);
	wl_event_source_timer_update(rec.timer, 1);
	return true;
}

// False when the encoder failed.
bool stop_recording() {
	if (rec.fd < 0) {
		return true;
	}
	end_caption();
	wl_event_source_remove(rec.timer);
	rec.timer = nullptr;
	close(rec.fd);
	rec.fd = -1;
	int status = 0;
	waitpid(rec.pid, &status, 0);
	rec.pid = -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return false;
	}
	std::string text;
	for (const Caption &c : captions) {
		text += std::format("{:.0f}\t{:.0f}\t{}\n", c.from, c.to, c.text);
	}
	write_file(std::format("{}/{}", out_dir, captions_file), text);
	walkthrough_taken = true;
	std::println("media: walkthrough {}/{}, {} frames", out_dir, walkthrough_file, rec.frames);
	return true;
}

// What tools/media.py encodes: the size of the video, the stills taken and,
// unless the run was stills-only, the walkthrough and its captions.
void write_manifest() {
	std::string stills_json;
	for (const std::string &name : stills_taken) {
		stills_json += std::format("{}{{\"name\": \"{}\", \"file\": \"{}\"}}",
			stills_json.empty() ? "" : ", ", name, still_file(name));
	}
	std::string walkthrough_json;
	if (walkthrough_taken) {
		walkthrough_json = std::format(",\n  \"walkthrough\": {{\"video\": \"{}\", \"captions\": \"{}\"}}",
			walkthrough_file, captions_file);
	}
	write_file(out_dir + "/manifest.json", std::format(
		"{{\n  \"video\": {{\"width\": {}, \"height\": {}}},\n  \"stills\": [{}]{}\n}}\n",
		video_w, video_h, stills_json, walkthrough_json));
}

// What the tour hung on the compositor's loop, taken down while the
// compositor is still there.
void release() {
	if (rec.fd >= 0) {
		close(rec.fd);
		waitpid(rec.pid, nullptr, 0);
	}
	if (rec.timer != nullptr) {
		wl_event_source_remove(rec.timer);
	}
	if (motion_timer != nullptr) {
		wl_event_source_remove(motion_timer);
	}
}

std::vector<Step> steps;

// A step is announced once, on its first visit.
void add(const char *name, std::function<Result()> run) {
	steps.push_back({name, [name, run = std::move(run), announced = false]() mutable {
		if (!announced) {
			announced = true;
			std::println("media: {}", name);
		}
		return run();
	}});
}

// Waits for the window the last launch opens — the first unclaimed one
// whose app id and title hold the parts — then hands it to `use`.
Result await_window(std::string_view app_id, std::string_view title,
		const std::function<void(View &)> &use) {
	View *view = fresh_view(app_id, title);
	if (auto wait = wait_for(view != nullptr, app_wait)) {
		return *wait;
	}
	if (view == nullptr) {
		return abandon(std::format("no {} window titled with '{}' opened", app_id, title));
	}
	claimed.push_back(view);
	std::println("media: window {} \"{}\"", view->shell_app_id(), view->shell_title());
	use(*view);
	return after(600);
}

// A web page's window, once the page has named it.
Result await_page(const Page &page, const std::function<void(View &)> &use) {
	return await_window("chrome", page.title, use);
}

Result open_page(const Page &page) {
	std::string url = page_url(page.name);
	if (url.empty()) {
		return abandon(std::format("the {} page was never recorded", page.name));
	}
	launch(chrome(url));
	return after(0);
}

View *editor_view = nullptr, *tests_view = nullptr, *research_view = nullptr,
	*sketch_view = nullptr;

void stage() {
	add("output", [] {
		if (add_output(static_cast<int>(panel_w * panel_scale),
				static_cast<int>(panel_h * panel_scale)) == nullptr) {
			return abandon("no headless output");
		}
		return after(0);
	});
	add("devices", [] {
		attach_pointer();
		attach_keyboard();
		use_real_time();
		motion_timer = wl_event_loop_add_timer(server->loop,
			[](void *) { return tick_motion(); }, nullptr);
		move_to(panel_w - 40, panel_h - 40);
		return after(200);
	});
	add("shell", [] {
		if (auto wait = wait_for(layer_named("wayland-canvas-bar") != nullptr, app_wait)) {
			return *wait;
		}
		if (layer_named("wayland-canvas-bar") == nullptr) {
			return abandon("the shell's bar never appeared");
		}
		return after(1500);
	});
	add("desktops", [] {
		Desktop *code = desktop_numbered(1);
		if (code == nullptr) {
			return abandon("the compositor made no first desktop");
		}
		Desktop &writing = server->canvas.desktops.create(writing_frame);
		server->canvas.desktops.create(table_frame, table_scale);
		server->canvas.toggle_tiling(code->id());
		server->canvas.toggle_tiling(writing.id());
		look_at(*code);
		return after(300);
	});

	// Code: the editor on the left, the docs and the tests on the right.
	add("code: editor", [] {
		launch(editor(code_file, code_line));
		return after(0);
	});
	add("code: editor window", [] {
		return await_window("foot", "", [](View &view) { editor_view = &view; });
	});
	add("code: docs", [] { return open_page(code_page); });
	// The second window halves the board; stepped up, it leaves the
	// quarter under it for the tests.
	add("code: docs window", [] {
		return await_page(code_page, [](View &view) { server->canvas.step(view, 1, -1); });
	});
	add("code: tests", [] {
		launch(foot(source, std::format("bash --rcfile {} -i", shell_word(tools_dir + "/bashrc"))));
		return after(0);
	});
	add("code: tests window", [] {
		return await_window("foot", "", [](View &view) { tests_view = &view; });
	});
	add("code: run the tests", [] {
		server->focus.focus_view(tests_view);
		type_text("meson test -C build --suite unit --no-rebuild\n");
		return after(400);
	});

	// Writing: the draft and the reading beside it.
	add("writing: look", [] {
		look_at(*desktop_numbered(2));
		return after(300);
	});
	add("writing: editor", [] {
		launch(editor(writing_file, writing_line));
		return after(0);
	});
	add("writing: editor window", [] {
		return await_window("foot", "", [](View &) {});
	});
	add("writing: research", [] { return open_page(writing_page); });
	add("writing: research window", [] {
		return await_page(writing_page, [](View &view) { research_view = &view; });
	});

	// The table.
	add("table: look", [] {
		look_at(*desktop_numbered(3));
		return after(300);
	});
	for (std::size_t i = 0; i < std::size(reference_files); i++) {
		add("table: reference", [i] {
			// from the assets folder, so the title names the file and no path
			launch(std::format("cd {} && exec imv -b 1b1d23 {}", shell_word(assets),
				shell_word(reference_files[i])));
			return after(0);
		});
		add("table: reference window", [i] {
			return await_window("imv", "", [i](View &view) {
				place_on_table(view, reference_spots[i]);
			});
		});
	}
	add("table: sketch", [] {
		launch(std::format("krita --nosplash {}", shell_word(assets + "/" + sketch_file)));
		return after(0);
	});
	add("table: sketch window", [] {
		return await_window("krita", "", [](View &view) {
			sketch_view = &view;
			place_on_table(view, sketch_spot);
		});
	});
	for (std::size_t i = 0; i < std::size(review_pages); i++) {
		add("table: review", [i] { return open_page(review_pages[i]); });
		add("table: review window", [i] {
			return await_page(review_pages[i], [i](View &view) {
				place_on_table(view, review_spots[i]);
			});
		});
	}
	add("table: music", [] {
		launch(std::format("mpv --ao=null --force-window=immediate --keep-open=always"
			" --loop-file=inf --osc=yes --script-opts=osc-visibility=always"
			" --title='Gymnopédie No. 1 — Erik Satie' {}", shell_word(assets + "/" + music_file)));
		return after(0);
	});
	add("table: music window", [] {
		return await_window("mpv", "", [](View &view) { place_on_table(view, music_spot); });
	});
	// Pages load, Krita builds its dockers, the tests finish.
	add("settle", [] { return after(12000); });
	// A client may settle on a size of its own after the first configure.
	add("re-place", [] {
		for (auto &[view, spot] : placed) {
			place(*view, spot);
		}
		return after(2000);
	});
}

void stills() {
	add("still: tiling", [] {
		look_at(*desktop_numbered(1));
		server->focus.focus_view(editor_view);
		return after(1500);
	});
	add("still: tiling shot", [] { return take_still("tiling"); });
	add("still: writing", [] {
		look_at(*desktop_numbered(2));
		return after(1500);
	});
	add("still: writing shot", [] { return take_still("writing"); });
	add("still: table", [] {
		look_at(*desktop_numbered(3));
		return after(1500);
	});
	add("still: table shot", [] { return take_still("table"); });
	add("still: overview", [] {
		look_at(*desktop_numbered(1));
		return after(600);
	});
	add("still: overview open", [] {
		chord(KEY_TAB, mods::super);
		return after(2000);
	});
	add("still: hero shot", [] { return take_still("hero"); });
	add("still: overview close", [] {
		tap(KEY_ESC);
		return after(1500);
	});
}

// Waits out the motion under way, then `ms` more.
Result after_motion(int ms) {
	return motion ? again(motion_tick_ms) : after(ms);
}

// How far the walkthrough drags the table's frame in the overview, in
// output-local px, and where the drag took hold of its tab.
constexpr double frame_drag_px = 150;
Point drag_from;

void walkthrough() {
	add("walk: start", [] {
		look_at(*desktop_numbered(1));
		server->focus.focus_view(editor_view);
		Point centre = on_screen(editor_view->content_box());
		move_to(centre.x + 120, centre.y + 160);
		return after(1200);
	});
	add("walk: record", [] {
		if (!start_recording()) {
			return abandon("the walkthrough encoder could not start");
		}
		return after(1200);
	});
	add("walk: code", [] {
		caption("A tiled desktop");
		type_over("O// drawn on the pixel grid, so windows stay sharp at 1:1");
		return after(0);
	});
	add("walk: code typed", [] {
		if (motion) {
			return again(motion_tick_ms);
		}
		tap(KEY_ESC);
		return after(1400);
	});
	add("walk: hop", [] {
		caption("Three-finger swipe to the next desktop");
		swipe_over(3, 160, 0, 320);
		return after(0);
	});
	add("walk: hop done", [] { return after_motion(1800); });
	add("walk: slices", [] {
		caption("Super+arrows move the window between tiles");
		server->focus.focus_view(research_view);
		chord(KEY_DOWN, mods::super);
		return after(1500);
	});
	add("walk: slices back", [] {
		chord(KEY_UP, mods::super);
		return after(1800);
	});
	add("walk: overview", [] {
		caption("Four-finger swipe up opens the overview");
		swipe_over(4, 0, -320, 650);
		return after(0);
	});
	add("walk: overview done", [] { return after_motion(2600); });
	// The table is not tiled, so its frame moves alone: a tiled desktop's
	// frame carries its board along.
	add("walk: frame", [] {
		caption("Dragging a desktop's frame leaves its windows in place");
		std::optional<TabLayout> tab = overview_tab_layout(output(),
			*desktop_numbered(3));
		if (!tab) {
			return abandon("the table's tab is not on the screen");
		}
		drag_from = {tab->tab.x + 40.0, tab->tab.y + tab->tab.height / 2.0};
		drag_over(drag_from.x, drag_from.y, frame_drag_px, 0, 1100);
		return after(0);
	});
	add("walk: frame moved", [] { return after_motion(1200); });
	add("walk: frame back", [] {
		drag_over(drag_from.x + frame_drag_px, drag_from.y, -frame_drag_px, 0, 1100);
		return after(0);
	});
	add("walk: frame placed", [] { return after_motion(1400); });
	add("walk: table", [] {
		caption("Three-finger swipe down picks the desktop below, four fingers down enter it");
		swipe_over(3, 0, 160, 320);
		return after(0);
	});
	add("walk: table target", [] { return after_motion(900); });
	add("walk: table in", [] {
		swipe_over(4, 0, 320, 650);
		return after(0);
	});
	add("walk: table shown", [] { return after_motion(3000); });
	add("walk: lean in", [] {
		caption("Super is the modifier for every action. Super + pinch zooms");
		Point centre = on_screen(sketch_view->content_box());
		move_to(centre.x, centre.y);
		key(KEY_LEFTMETA, true);
		pinch_over(2, 1 / table_scale, 1100);
		return after(0);
	});
	add("walk: leaned in", [] {
		if (motion) {
			return again(motion_tick_ms);
		}
		key(KEY_LEFTMETA, false);
		return after(2600);
	});
	add("walk: step back", [] {
		key(KEY_LEFTMETA, true);
		pinch_over(2, table_scale, 1100);
		return after(0);
	});
	add("walk: stepped back", [] {
		if (motion) {
			return again(motion_tick_ms);
		}
		key(KEY_LEFTMETA, false);
		return after(2000);
	});
	add("walk: launcher", [] {
		caption("Super+Space opens the launcher");
		chord(KEY_SPACE, mods::super);
		return after(900);
	});
	add("walk: launcher typed", [] {
		type_over("krita", 140);
		return after(0);
	});
	add("walk: launcher shown", [] { return after_motion(1600); });
	add("walk: launcher closed", [] {
		tap(KEY_ESC);
		return after(1000);
	});
	add("walk: home overview", [] {
		caption("Super+Tab, then click a desktop to go there");
		chord(KEY_TAB, mods::super);
		return after(2600);
	});
	add("walk: home", [] {
		Desktop *code = desktop_numbered(1);
		Point centre = on_screen(code->frame());
		move_to(centre.x, centre.y);
		return after(700);
	});
	add("walk: home click", [] {
		button(BTN_LEFT, true);
		button(BTN_LEFT, false);
		return after(2600);
	});
	add("walk: stop", [] {
		if (!stop_recording()) {
			return abandon("the walkthrough encoder failed");
		}
		return after(300);
	});
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	wlr_log_init(WLR_ERROR, nullptr);
	std::string root = tmp_root();
	std::string work = env_or("MEDIA_WORK", root + "/media/run");
	assets = env_or("MEDIA_ASSETS", root + "/media/assets");
	source = env_or("MEDIA_SOURCE", std::filesystem::current_path().string());
	tools_dir = source + "/tools/media";
	stills_only = env_or("MEDIA_ONLY", "") == "stills";
	// The run's folder is the tour's own: one without the tour's mark is
	// never emptied.
	std::string mark = work + "/.media-tour";
	std::error_code ec;
	if (std::filesystem::exists(work, ec) && !std::filesystem::is_empty(work, ec)
			&& !file_exists(mark)) {
		std::println(stderr, "media: {} is not a tour's folder; not emptying it", work);
		return 2;
	}
	use_scratch(work);
	write_file(mark, "");
	out_dir = scratch_dir("out");
	use_headless_backend();
	// The output is added by the first step, at the panel's size.
	setenv("WLR_HEADLESS_OUTPUTS", "0", 1);
	build_world();

	// a unix socket path is capped near 108 bytes: the shortest scratch path
	if (!start_private_buses(scratch_dir("bus"))) {
		std::println(stderr, "media: the private buses did not start");
		return 2;
	}
	start_standins();
	start_curl_standin();

	stage();
	stills();
	if (!stills_only) {
		walkthrough();
	}
	add("manifest", [] {
		write_manifest();
		return finish();
	});

	Options options;
	options.scale = panel_scale;
	int status = run(options, std::move(steps), release);
	if (status != 0) {
		std::println(stderr, "media: the tour did not finish");
	}
	return status;
}
