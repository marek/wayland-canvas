// The README's media (docs/media/): the compositor on the headless backend
// at a laptop panel's size and scale, real apps arranged the way the README
// says people work — code and writing on tiled desktops, design and review
// spread over a desktop at 50 % like papers on a table — its stills taken
// and its walkthrough recorded, a caption per step. tools/media.sh builds
// the world it runs in (the downloaded assets, the apps' configs) and turns
// what it writes into docs/media/. Every app runs with a scratch HOME and
// scratch XDG dirs, and the shell on private buses with the bar's stand-in
// services: nothing of the user's own profiles, accounts or buses is read.
#include <algorithm>
#include <cairo.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <ranges>
#include <signal.h>
#include <string>
#include <string_view>
#include <sstream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "core/config.h"
#include "core/server.h"
#include "core/wlr.h"
#include "platform/output.h"
#include "render/capture.h"
#include "render/render.h"
#include "windows/view.h"

namespace {

// ------------------------------------------------------------- the scenes

// A laptop panel: 3200x2000 pixels at scale 2.
constexpr int screen_w = 1600, screen_h = 1000;
constexpr double screen_scale = 2;
constexpr int record_fps = 30;
// The walkthrough is encoded at the panel's logical size.
constexpr int video_w = screen_w, video_h = screen_h;
// What the tour leaves in its out/ folder besides the stills; tools/media.py
// learns these names, the size above and the stills taken from manifest.json.
constexpr const char *walkthrough_file = "walkthrough-raw.mp4";
constexpr const char *captions_file = "captions.tsv";

// Desktop 1 (code) is the compositor's own, at the output's box. Writing
// sits beside it; the table, at half scale, under both.
constexpr wlr_box writing_frame{1700, 0, screen_w, screen_h};
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

// ------------------------------------------------------------- the world

std::optional<Server> server;
wl_event_source *timer = nullptr;
bool failed = false;
bool stills_only = false, devices = false;
bool walkthrough_taken = false;
std::string work, assets, source, out_dir, tools_dir;
std::vector<std::string> stills_taken;
wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "media-pointer"};
wlr_keyboard kbd{};
const wlr_keyboard_impl kbd_impl = {.name = "media-keyboard"};

struct PrivateBus {
	pid_t pid = -1;
	std::string address;
};
PrivateBus private_system, private_session;
pid_t standins_pid = -1;
std::vector<pid_t> apps; // each the leader of its own process group

void mkdir_p(const std::string &path) {
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
}

void write_file(const std::string &path, std::string_view text) {
	mkdir_p(std::filesystem::path(path).parent_path());
	FILE *f = std::fopen(path.c_str(), "w");
	if (f == nullptr) {
		std::println(stderr, "media: cannot write {}", path);
		return;
	}
	std::fwrite(text.data(), 1, text.size(), f);
	std::fclose(f);
}

void copy_file(const std::string &from, const std::string &to) {
	mkdir_p(std::filesystem::path(to).parent_path());
	std::error_code ec;
	std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		std::println(stderr, "media: cannot copy {}: {}", from, ec.message());
	}
}

std::string env_or(const char *name, const std::string &fallback) {
	const char *value = std::getenv(name);
	return value != nullptr && *value != '\0' ? value : fallback;
}

void fail(std::string_view what) {
	std::println(stderr, "media: {}", what);
	failed = true;
	wl_display_terminate(server->display);
}

// ------------------------------------------------------------ processes

struct Spawned {
	pid_t pid = -1;
	int out = -1;
};

// A helper of the world (a bus, the stand-ins): its stdout is read, and it
// dies with the tour.
Spawned spawn_helper(const char *const argv[]) {
	int fds[2];
	if (pipe(fds) != 0) {
		return {};
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		execvp(argv[0], const_cast<char *const *>(argv));
		_exit(127);
	}
	close(fds[1]);
	return pid > 0 ? Spawned{pid, fds[0]} : Spawned{};
}

// Reads the helper's output up to `marker` within `seconds`.
bool prints(Spawned &child, std::string_view marker, int seconds) {
	std::string seen;
	double until = now_ms() + seconds * 1000.0;
	fcntl(child.out, F_SETFL, O_NONBLOCK);
	while (now_ms() < until) {
		char buf[256];
		ssize_t n = read(child.out, buf, sizeof buf);
		if (n > 0) {
			seen.append(buf, static_cast<std::size_t>(n));
			if (seen.find(marker) != std::string::npos) {
				return true;
			}
			continue;
		}
		if (n == 0) {
			return false;
		}
		usleep(20000);
	}
	return false;
}

void reap(pid_t pid) {
	if (pid <= 0) {
		return;
	}
	kill(pid, SIGTERM);
	for (int i = 0; i < 200; i++) {
		if (waitpid(pid, nullptr, WNOHANG) == pid) {
			return;
		}
		usleep(10000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, nullptr, 0);
}

// An app on the canvas: its own process group, so the whole of it — a
// browser's helpers included — goes when the tour ends.
pid_t launch(const std::string &cmd) {
	std::string log = work + "/apps.log";
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		int null = open("/dev/null", O_RDONLY);
		int logfd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
		}
		if (logfd >= 0) {
			dup2(logfd, STDOUT_FILENO);
			dup2(logfd, STDERR_FILENO);
		}
		close_range(STDERR_FILENO + 1, ~0U, 0);
		execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	if (pid > 0) {
		apps.push_back(pid);
	}
	return pid;
}

void stop_apps() {
	for (pid_t pid : apps) {
		kill(-pid, SIGTERM);
	}
	for (int i = 0; i < 300 && !apps.empty(); i++) {
		std::erase_if(apps, [](pid_t pid) { return waitpid(pid, nullptr, WNOHANG) == pid; });
		usleep(10000);
	}
	for (pid_t pid : apps) {
		kill(-pid, SIGKILL);
		waitpid(pid, nullptr, 0);
	}
	apps.clear();
}

std::string shell_word(std::string_view text) {
	std::string out = "'";
	for (char c : text) {
		out += c == '\'' ? std::string{"'\\''"} : std::string(1, c);
	}
	return out + "'";
}

// A dbus-daemon of the tour's own: everything on it is allowed, and no
// machine service is reachable through it.
bool start_private_bus(PrivateBus &bus, const char *type, const char *name) {
	// a unix socket path is capped near 108 bytes: the shortest scratch path
	std::string dir = work + "/bus";
	mkdir_p(dir);
	std::string socket = std::format("{}/{}", dir, name);
	std::string conf = socket + ".conf";
	unlink(socket.c_str());
	write_file(conf, std::format(
		"<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
		" \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
		"<busconfig>\n"
		"  <type>{}</type>\n"
		"  <listen>unix:path={}</listen>\n"
		"  <auth>EXTERNAL</auth>\n"
		"  <policy context=\"default\">\n"
		"    <allow own=\"*\"/>\n"
		"    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
		"    <allow eavesdrop=\"true\"/>\n"
		"  </policy>\n"
		"</busconfig>\n", type, socket));
	const char *const argv[] = {"dbus-daemon", "--config-file", conf.c_str(),
		"--print-address=1", static_cast<const char *>(nullptr)};
	Spawned child = spawn_helper(argv);
	if (child.pid <= 0) {
		return false;
	}
	if (!prints(child, "unix:", 10)) {
		reap(child.pid);
		return false;
	}
	close(child.out);
	bus.pid = child.pid;
	bus.address = "unix:path=" + socket;
	return true;
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
	const char *const argv[] = {py, script, static_cast<const char *>(nullptr)};
	Spawned child = spawn_helper(argv);
	if (child.pid <= 0 || !prints(child, "READY\n", 30)) {
		std::println(stderr, "media: the bar stand-ins did not become ready");
		reap(child.pid);
		return;
	}
	close(child.out);
	standins_pid = child.pid;
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
	std::string dir = work + "/bin";
	write_file(dir + "/curl", "#!/bin/sh\n"
		"# The tour's curl: wttr.in from a canned response, anything else real.\n"
		"for arg in \"$@\"; do\n"
		"\tcase \"$arg\" in\n"
		"\t*wttr.in*) exec cat " + shell_word(fixture) + " ;;\n"
		"\tesac\n"
		"done\n"
		"PATH=${PATH#*:} exec curl \"$@\"\n");
	chmod((dir + "/curl").c_str(), 0755);
	setenv("PATH", std::format("{}:{}", dir, env_or("PATH", "/usr/bin:/bin")).c_str(), 1);
}

std::string note_file(const Note &note) {
	return std::format("<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
		"<meta name=\"canvas-note\" content=\"x={} y={} width={} height={}\">\n"
		"<style>\nbody {{ background: {}; color: #1e1e1e; font-family: \"Noto Sans\";"
		" font-size: 26px; }}\np {{ margin-top: 0; margin-bottom: 0; white-space: pre-wrap; }}\n"
		"</style>\n</head>\n<body>\n{}\n</body>\n</html>\n",
		note.x, note.y, note.w, note.h, note.color, note.html);
}

// Scratch HOME and XDG dirs, the compositor's and the shell's settings,
// the notes on the table, and the files the editors open.
void build_world() {
	std::string home = work + "/home";
	std::string config = home + "/.config", data = home + "/.local/share",
		state = home + "/.local/state", cache = home + "/.cache";
	for (const std::string &dir : {config, data, state, cache}) {
		mkdir_p(dir);
	}
	setenv("HOME", home.c_str(), 1);
	setenv("XDG_CONFIG_HOME", config.c_str(), 1);
	setenv("XDG_DATA_HOME", data.c_str(), 1);
	setenv("XDG_STATE_HOME", state.c_str(), 1);
	setenv("XDG_CACHE_HOME", cache.c_str(), 1);
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
	write_file(config + "/glib-2.0/settings/keyfile", std::format(
		"[org/gnome/desktop/interface]\nicon-theme='{}'\n", icons));
	write_file(config + "/gtk-3.0/settings.ini", std::format(
		"[Settings]\ngtk-icon-theme-name={}\n", icons));
	setenv("QS_ICON_THEME", icons.c_str(), 1);
	// imv's own title names the file by its whole path
	write_file(config + "/imv/config",
		"[options]\ntitle_text = $(basename \"$imv_current_file\")\n");

	write_file(config + "/wayland-canvas/canvas.conf", "[session]\nlock-idle = off\n");
	// The tray stand-in's item has no icon, and the agents panel reads the
	// user's own accounts: both stay out of the bar.
	write_file(config + "/wayland-canvas/shell.json",
		"{\n\t\"plugins\": {\n\t\t\"canvas.agents\": {\"enabled\": false}\n\t},\n"
		"\t\"bar\": {\"hidden\": [\"canvas.tray/tray\"]}\n}\n");
	for (std::size_t i = 0; i < std::size(table_notes); i++) {
		write_file(std::format("{}/wayland-canvas/plugins/canvas.notes/{}.html", state, i + 1),
			note_file(table_notes[i]));
	}
	copy_file(source + "/" + code_file, work + "/checkout/" + code_file);
	copy_file(source + "/" + writing_file, work + "/checkout/" + writing_file);
}

std::string foot(std::string_view dir, std::string_view command) {
	return std::format("foot -c {} -D {} {}", shell_word(tools_dir + "/foot.ini"),
		shell_word(dir), command);
}

std::string editor(std::string_view file, int line) {
	return foot(work + "/checkout", std::format("nvim -u {} -i NONE -n +{} {}",
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
		" --new-window {}", shell_word(work + "/chrome"), flags, shell_word(url));
}

// ------------------------------------------------------------------ input

Output *out() {
	return server->outputs.empty() ? nullptr : server->outputs.front().get();
}

uint32_t stamp() {
	return static_cast<uint32_t>(now_ms());
}

void key(uint32_t code, bool pressed) {
	wlr_keyboard_key_event ev = {.time_msec = stamp(), .keycode = code,
		.update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

void chord(std::initializer_list<uint32_t> mods, uint32_t code) {
	for (uint32_t mod : mods) {
		key(mod, true);
	}
	key(code, true);
	key(code, false);
	for (uint32_t mod : std::views::reverse(mods)) {
		key(mod, false);
	}
}

struct KeyStroke {
	uint32_t code;
	bool shift;
};

// The key a US layout types `c` with.
std::optional<KeyStroke> stroke_for(char c) {
	static constexpr uint32_t letters[] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F,
		KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q,
		KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
	static constexpr uint32_t digits[] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5,
		KEY_6, KEY_7, KEY_8, KEY_9};
	if (c >= 'a' && c <= 'z') {
		return KeyStroke{letters[c - 'a'], false};
	}
	if (c >= 'A' && c <= 'Z') {
		return KeyStroke{letters[c - 'A'], true};
	}
	if (c >= '0' && c <= '9') {
		return KeyStroke{digits[c - '0'], false};
	}
	struct Punct {
		char c;
		uint32_t code;
		bool shift;
	};
	static constexpr Punct punct[] = {
		{' ', KEY_SPACE, false}, {'\n', KEY_ENTER, false}, {'-', KEY_MINUS, false},
		{'_', KEY_MINUS, true}, {'=', KEY_EQUAL, false}, {'+', KEY_EQUAL, true},
		{'/', KEY_SLASH, false}, {'?', KEY_SLASH, true}, {'.', KEY_DOT, false},
		{'>', KEY_DOT, true}, {',', KEY_COMMA, false}, {'<', KEY_COMMA, true},
		{';', KEY_SEMICOLON, false}, {':', KEY_SEMICOLON, true},
		{'\'', KEY_APOSTROPHE, false}, {'"', KEY_APOSTROPHE, true},
		{'(', KEY_9, true}, {')', KEY_0, true}, {'!', KEY_1, true}, {'%', KEY_5, true},
		{'*', KEY_8, true}, {'&', KEY_7, true}, {'#', KEY_3, true}, {'@', KEY_2, true},
		{'[', KEY_LEFTBRACE, false}, {']', KEY_RIGHTBRACE, false},
		{'{', KEY_LEFTBRACE, true}, {'}', KEY_RIGHTBRACE, true},
	};
	for (const Punct &p : punct) {
		if (p.c == c) {
			return KeyStroke{p.code, p.shift};
		}
	}
	return std::nullopt;
}

void type_char(char c) {
	std::optional<KeyStroke> stroke = stroke_for(c);
	if (!stroke) {
		return;
	}
	if (stroke->shift) {
		key(KEY_LEFTSHIFT, true);
	}
	key(stroke->code, true);
	key(stroke->code, false);
	if (stroke->shift) {
		key(KEY_LEFTSHIFT, false);
	}
}

void type_now(std::string_view text) {
	for (char c : text) {
		type_char(c);
	}
}

// The pointer at an output-local logical point.
void move_to(double x, double y) {
	wlr_pointer_motion_absolute_event ev = {.pointer = &ptr, .time_msec = stamp(),
		.x = x / screen_w, .y = y / screen_h};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

void button(uint32_t code, bool pressed) {
	wlr_pointer_button_event ev = {.pointer = &ptr, .time_msec = stamp(), .button = code,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&ptr.events.button, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

// A hand on the touchpad, the pointer or the keys over time: one at a
// time, eased, through the devices' own events.
struct Motion {
	enum class Kind { swipe, pinch, drag, typing };
	Kind kind = Kind::swipe;
	uint32_t fingers = 0;
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

void begin(Motion m) {
	m.start = now_ms();
	switch (m.kind) {
	case Motion::Kind::swipe: {
		wlr_pointer_swipe_begin_event ev = {.pointer = &ptr, .time_msec = stamp(),
			.fingers = m.fingers};
		wl_signal_emit_mutable(&ptr.events.swipe_begin, &ev);
		break;
	}
	case Motion::Kind::pinch: {
		wlr_pointer_pinch_begin_event ev = {.pointer = &ptr, .time_msec = stamp(),
			.fingers = m.fingers};
		wl_signal_emit_mutable(&ptr.events.pinch_begin, &ev);
		break;
	}
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
		wlr_pointer_swipe_update_event ev = {.pointer = &ptr, .time_msec = stamp(),
			.fingers = m.fingers, .dx = x - m.sent_x, .dy = y - m.sent_y};
		wl_signal_emit_mutable(&ptr.events.swipe_update, &ev);
		m.sent_x = x;
		m.sent_y = y;
		if (p >= 1) {
			wlr_pointer_swipe_end_event end = {.pointer = &ptr, .time_msec = stamp(),
				.cancelled = false};
			wl_signal_emit_mutable(&ptr.events.swipe_end, &end);
		}
		break;
	}
	case Motion::Kind::pinch: {
		wlr_pointer_pinch_update_event ev = {.pointer = &ptr, .time_msec = stamp(),
			.fingers = m.fingers, .dx = 0, .dy = 0, .scale = 1 + (m.scale - 1) * e,
			.rotation = 0};
		wl_signal_emit_mutable(&ptr.events.pinch_update, &ev);
		if (p >= 1) {
			wlr_pointer_pinch_end_event end = {.pointer = &ptr, .time_msec = stamp(),
				.cancelled = false};
			wl_signal_emit_mutable(&ptr.events.pinch_end, &end);
		}
		break;
	}
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

void swipe(uint32_t fingers, double dx, double dy, double ms) {
	begin(Motion{.kind = Motion::Kind::swipe, .fingers = fingers, .dx = dx, .dy = dy, .ms = ms});
}

void pinch(uint32_t fingers, double scale, double ms) {
	begin(Motion{.kind = Motion::Kind::pinch, .fingers = fingers, .scale = scale, .ms = ms});
}

void drag(double ox, double oy, double dx, double dy, double ms) {
	begin(Motion{.kind = Motion::Kind::drag, .dx = dx, .dy = dy, .ox = ox, .oy = oy, .ms = ms});
}

// Typed at a person's pace.
void type(std::string_view text, double ms_per_key = 70) {
	begin(Motion{.kind = Motion::Kind::typing, .text = std::string{text},
		.ms = ms_per_key * static_cast<double>(text.size())});
}

// ------------------------------------------------------------------ views

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
	out()->camera.jump(camera_for_box(desktop.frame(), desktop.scale()));
}

Desktop *desktop_numbered(int number) {
	return server->canvas.desktops.by_number(number);
}

// Output-local centre of a canvas box under the current camera.
Point on_screen(wlr_box box) {
	return out()->canvas_to_local(box.x + box.width / 2.0, box.y + box.height / 2.0);
}

const LayerSurface *layer_named(const char *ns) {
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (const auto &layer : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : layer) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strcmp(l->wlr->namespace_, ns) == 0) {
					return l.get();
				}
			}
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------- capture

std::string still_file(std::string_view name) {
	return std::format("{}.png", name);
}

bool still(const char *name) {
	Output *o = out();
	CairoSurfacePtr shot = capture_output(*o, wlr_box{0, 0, o->width, o->height});
	if (shot == nullptr) {
		fail(std::format("the {} still could not be captured", name));
		return false;
	}
	std::string path = std::format("{}/{}", out_dir, still_file(name));
	if (cairo_surface_write_to_png(shot.get(), path.c_str()) != CAIRO_STATUS_SUCCESS) {
		fail(std::format("cannot write {}", path));
		return false;
	}
	stills_taken.emplace_back(name);
	std::println("media: still {}", path);
	return true;
}

// An arrow where the pointer is: offscreen captures leave the cursor out.
void draw_cursor(cairo_surface_t *frame) {
	cairo_t *cr = cairo_create(frame);
	cairo_translate(cr, server->cursor->x * screen_scale, server->cursor->y * screen_scale);
	cairo_scale(cr, screen_scale, screen_scale);
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
		Output *o = out();
		CairoSurfacePtr frame = capture_output(*o, wlr_box{0, 0, o->width, o->height});
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
	Output *o = out();
	rec.width = static_cast<int>(std::lround(o->width * screen_scale));
	rec.height = static_cast<int>(std::lround(o->height * screen_scale));
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

void stop_recording() {
	if (rec.fd < 0) {
		return;
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
		fail("the walkthrough encoder failed");
		return;
	}
	std::string text;
	for (const Caption &c : captions) {
		text += std::format("{:.0f}\t{:.0f}\t{}\n", c.from, c.to, c.text);
	}
	write_file(std::format("{}/{}", out_dir, captions_file), text);
	walkthrough_taken = true;
	std::println("media: walkthrough {}/{}, {} frames", out_dir, walkthrough_file, rec.frames);
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

// ----------------------------------------------------------------- script

// A step returns how long to wait before the next one, in ms; `again(ms)`
// runs the same step again after that wait, for something still on its way.
struct Step {
	const char *name;
	std::function<int()> run;
};
std::vector<Step> steps;
std::size_t step = 0;
double step_started = 0;

constexpr int again(int ms) {
	return -ms;
}

// Waits for the window the last launch opens — the first unclaimed one
// whose app id and title hold the parts — then hands it to `use`.
int await_window(std::string_view app_id, std::string_view title,
		const std::function<void(View &)> &use) {
	View *view = fresh_view(app_id, title);
	if (view == nullptr) {
		if (now_ms() - step_started > app_wait_ms) {
			fail(std::format("no {} window titled with '{}' opened", app_id, title));
			return 0;
		}
		return again(poll_ms);
	}
	claimed.push_back(view);
	std::println("media: window {} \"{}\"", view->shell_app_id(), view->shell_title());
	use(*view);
	return 600;
}

// A web page's window, once the page has named it.
int await_page(const Page &page, const std::function<void(View &)> &use) {
	return await_window("chrome", page.title, use);
}

void open_page(const Page &page) {
	std::string url = page_url(page.name);
	if (url.empty()) {
		fail(std::format("the {} page was never recorded", page.name));
		return;
	}
	launch(chrome(url));
}

View *editor_view = nullptr, *tests_view = nullptr, *research_view = nullptr,
	*sketch_view = nullptr;

void add(const char *name, std::function<int()> run) {
	steps.push_back({name, std::move(run)});
}

void stage() {
	add("devices", [] {
		wlr_pointer_init(&ptr, &ptr_impl, "media-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "media-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		devices = true;
		move_to(screen_w - 40, screen_h - 40);
		return 200;
	});
	add("shell", [] {
		if (layer_named("wayland-canvas-bar") == nullptr) {
			if (now_ms() - step_started > app_wait_ms) {
				fail("the shell's bar never appeared");
				return 0;
			}
			return again(poll_ms);
		}
		return 1500;
	});
	add("desktops", [] {
		Desktop *code = desktop_numbered(1);
		if (code == nullptr) {
			fail("the compositor made no first desktop");
			return 0;
		}
		Desktop &writing = server->canvas.desktops.create(writing_frame);
		server->canvas.desktops.create(table_frame, table_scale);
		server->canvas.toggle_tiling(code->id());
		server->canvas.toggle_tiling(writing.id());
		look_at(*code);
		return 300;
	});

	// Code: the editor on the left, the docs and the tests on the right.
	add("code: editor", [] {
		launch(editor(code_file, code_line));
		return 0;
	});
	add("code: editor window", [] {
		return await_window("foot", "", [](View &view) { editor_view = &view; });
	});
	add("code: docs", [] {
		open_page(code_page);
		return 0;
	});
	// The second window halves the board; stepped up, it leaves the
	// quarter under it for the tests.
	add("code: docs window", [] {
		return await_page(code_page, [](View &view) { server->canvas.step(view, 1, -1); });
	});
	add("code: tests", [] {
		launch(foot(source, std::format("bash --rcfile {} -i", shell_word(tools_dir + "/bashrc"))));
		return 0;
	});
	add("code: tests window", [] {
		return await_window("foot", "", [](View &view) { tests_view = &view; });
	});
	add("code: run the tests", [] {
		server->focus_view(tests_view);
		type_now("meson test -C build --suite unit --no-rebuild\n");
		return 400;
	});

	// Writing: the draft and the reading beside it.
	add("writing: look", [] {
		look_at(*desktop_numbered(2));
		return 300;
	});
	add("writing: editor", [] {
		launch(editor(writing_file, writing_line));
		return 0;
	});
	add("writing: editor window", [] {
		return await_window("foot", "", [](View &) {});
	});
	add("writing: research", [] {
		open_page(writing_page);
		return 0;
	});
	add("writing: research window", [] {
		return await_page(writing_page, [](View &view) { research_view = &view; });
	});

	// The table.
	add("table: look", [] {
		look_at(*desktop_numbered(3));
		return 300;
	});
	for (std::size_t i = 0; i < std::size(reference_files); i++) {
		add("table: reference", [i] {
			// from the assets folder, so the title names the file and no path
			launch(std::format("cd {} && exec imv -b 1b1d23 {}", shell_word(assets),
				shell_word(reference_files[i])));
			return 0;
		});
		add("table: reference window", [i] {
			return await_window("imv", "", [i](View &view) {
				place_on_table(view, reference_spots[i]);
			});
		});
	}
	add("table: sketch", [] {
		launch(std::format("krita --nosplash {}", shell_word(assets + "/" + sketch_file)));
		return 0;
	});
	add("table: sketch window", [] {
		return await_window("krita", "", [](View &view) {
			sketch_view = &view;
			place_on_table(view, sketch_spot);
		});
	});
	for (std::size_t i = 0; i < std::size(review_pages); i++) {
		add("table: review", [i] {
			open_page(review_pages[i]);
			return 0;
		});
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
		return 0;
	});
	add("table: music window", [] {
		return await_window("mpv", "", [](View &view) { place_on_table(view, music_spot); });
	});
	// Pages load, Krita builds its dockers, the tests finish.
	add("settle", [] { return 12000; });
	// A client may settle on a size of its own after the first configure.
	add("re-place", [] {
		for (auto &[view, spot] : placed) {
			place(*view, spot);
		}
		return 2000;
	});
}

void stills() {
	add("still: tiling", [] {
		look_at(*desktop_numbered(1));
		server->focus_view(editor_view);
		return 1500;
	});
	add("still: tiling shot", [] { return still("tiling") ? 300 : 0; });
	add("still: writing", [] {
		look_at(*desktop_numbered(2));
		return 1500;
	});
	add("still: writing shot", [] { return still("writing") ? 300 : 0; });
	add("still: table", [] {
		look_at(*desktop_numbered(3));
		return 1500;
	});
	add("still: table shot", [] { return still("table") ? 300 : 0; });
	add("still: overview", [] {
		look_at(*desktop_numbered(1));
		return 600;
	});
	add("still: overview open", [] {
		chord({KEY_LEFTMETA}, KEY_TAB);
		return 2000;
	});
	add("still: hero shot", [] { return still("hero") ? 300 : 0; });
	add("still: overview close", [] {
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		return 1500;
	});
}

// Waits out the motion under way, then `ms` more.
int after_motion(int ms) {
	return motion ? again(motion_tick_ms) : ms;
}

// How far the walkthrough drags the table's frame in the overview, in
// output-local px, and where the drag took hold of its tab.
constexpr double frame_drag_px = 150;
Point drag_from;

void walkthrough() {
	add("walk: start", [] {
		look_at(*desktop_numbered(1));
		server->focus_view(editor_view);
		Point centre = on_screen(editor_view->content_box());
		move_to(centre.x + 120, centre.y + 160);
		return 1200;
	});
	add("walk: record", [] {
		if (!start_recording()) {
			fail("the walkthrough encoder could not start");
			return 0;
		}
		return 1200;
	});
	add("walk: code", [] {
		caption("A tiled desktop");
		type("O// drawn on the pixel grid, so windows stay sharp at 1:1");
		return 0;
	});
	add("walk: code typed", [] {
		if (motion) {
			return again(motion_tick_ms);
		}
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		return 1400;
	});
	add("walk: hop", [] {
		caption("Three-finger swipe to the next desktop");
		swipe(3, 160, 0, 320);
		return 0;
	});
	add("walk: hop done", [] { return after_motion(1800); });
	add("walk: slices", [] {
		caption("Super+arrows move the window between tiles");
		server->focus_view(research_view);
		chord({KEY_LEFTMETA}, KEY_DOWN);
		return 1500;
	});
	add("walk: slices back", [] {
		chord({KEY_LEFTMETA}, KEY_UP);
		return 1800;
	});
	add("walk: overview", [] {
		caption("Four-finger swipe up opens the overview");
		swipe(4, 0, -320, 650);
		return 0;
	});
	add("walk: overview done", [] { return after_motion(2600); });
	// The table is not tiled, so its frame moves alone: a tiled desktop's
	// frame carries its board along.
	add("walk: frame", [] {
		caption("Dragging a desktop's frame leaves its windows in place");
		std::optional<TabLayout> tab = server->painter.overview_tab_layout(*out(),
			*desktop_numbered(3));
		if (!tab) {
			fail("the table's tab is not on the screen");
			return 0;
		}
		drag_from = {tab->tab.x + 40.0, tab->tab.y + tab->tab.height / 2.0};
		drag(drag_from.x, drag_from.y, frame_drag_px, 0, 1100);
		return 0;
	});
	add("walk: frame moved", [] { return after_motion(1200); });
	add("walk: frame back", [] {
		drag(drag_from.x + frame_drag_px, drag_from.y, -frame_drag_px, 0, 1100);
		return 0;
	});
	add("walk: frame placed", [] { return after_motion(1400); });
	add("walk: table", [] {
		caption("Three-finger swipe down picks the desktop below, four fingers down enter it");
		swipe(3, 0, 160, 320);
		return 0;
	});
	add("walk: table target", [] { return after_motion(900); });
	add("walk: table in", [] {
		swipe(4, 0, 320, 650);
		return 0;
	});
	add("walk: table shown", [] { return after_motion(3000); });
	add("walk: lean in", [] {
		caption("Super is the modifier for every action. Super + pinch zooms");
		Point centre = on_screen(sketch_view->content_box());
		move_to(centre.x, centre.y);
		key(KEY_LEFTMETA, true);
		pinch(2, 1 / table_scale, 1100);
		return 0;
	});
	add("walk: leaned in", [] {
		if (motion) {
			return again(motion_tick_ms);
		}
		key(KEY_LEFTMETA, false);
		return 2600;
	});
	add("walk: step back", [] {
		key(KEY_LEFTMETA, true);
		pinch(2, table_scale, 1100);
		return 0;
	});
	add("walk: stepped back", [] {
		if (motion) {
			return again(motion_tick_ms);
		}
		key(KEY_LEFTMETA, false);
		return 2000;
	});
	add("walk: launcher", [] {
		caption("Super+Space opens the launcher");
		chord({KEY_LEFTMETA}, KEY_SPACE);
		return 900;
	});
	add("walk: launcher typed", [] {
		type("krita", 140);
		return 0;
	});
	add("walk: launcher shown", [] { return after_motion(1600); });
	add("walk: launcher closed", [] {
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		return 1000;
	});
	add("walk: home overview", [] {
		caption("Super+Tab, then click a desktop to go there");
		chord({KEY_LEFTMETA}, KEY_TAB);
		return 2600;
	});
	add("walk: home", [] {
		Desktop *code = desktop_numbered(1);
		Point centre = on_screen(code->frame());
		move_to(centre.x, centre.y);
		return 700;
	});
	add("walk: home click", [] {
		button(BTN_LEFT, true);
		button(BTN_LEFT, false);
		return 2600;
	});
	add("walk: stop", [] {
		stop_recording();
		return 300;
	});
}

int handle_timer() {
	if (failed) {
		return 0;
	}
	if (step >= steps.size()) {
		wl_display_terminate(server->display);
		return 0;
	}
	if (step_started == 0) {
		step_started = now_ms();
		std::println("media: {}", steps[step].name);
	}
	int delay = steps[step].run();
	if (failed) {
		return 0;
	}
	if (delay < 0) {
		wl_event_source_timer_update(timer, -delay);
		return 0;
	}
	step++;
	step_started = 0;
	wl_event_source_timer_update(timer, std::max(1, delay));
	return 0;
}

wlr_backend *headless_backend() {
	wlr_backend *found = nullptr;
	wlr_multi_for_each_backend(server->backend,
		[](wlr_backend *backend, void *data) {
			if (wlr_backend_is_headless(backend)) {
				*static_cast<wlr_backend **>(data) = backend;
			}
		}, &found);
	return found;
}

void stop_world() {
	stop_apps();
	reap(standins_pid);
	reap(private_session.pid);
	reap(private_system.pid);
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	wlr_log_init(WLR_ERROR, nullptr);
	std::string root = env_or("CANVAS_TMP", "tmp");
	work = env_or("MEDIA_WORK", root + "/media/run");
	assets = env_or("MEDIA_ASSETS", root + "/media/assets");
	source = env_or("MEDIA_SOURCE", std::filesystem::current_path().string());
	tools_dir = source + "/tools/media";
	out_dir = work + "/out";
	stills_only = env_or("MEDIA_ONLY", "") == "stills";
	// The run's folder is the tour's own: one without the tour's mark is
	// never emptied.
	std::string mark = work + "/.media-tour";
	std::error_code ec;
	if (std::filesystem::exists(work, ec) && !std::filesystem::is_empty(work, ec)
			&& !std::filesystem::exists(mark, ec)) {
		std::println(stderr, "media: {} is not a tour's folder; not emptying it", work);
		return 2;
	}
	std::filesystem::remove_all(work, ec);
	mkdir_p(out_dir);
	write_file(mark, "");
	build_world();

	if (!start_private_bus(private_system, "system", "y")
			|| !start_private_bus(private_session, "session", "n")) {
		std::println(stderr, "media: the private buses did not start");
		stop_world();
		return 2;
	}
	setenv("DBUS_SYSTEM_BUS_ADDRESS", private_system.address.c_str(), 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", private_session.address.c_str(), 1);
	start_standins();
	start_curl_standin();

	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	setenv("WLR_HEADLESS_OUTPUTS", "0", 1);
	server.emplace();
	Options options;
	options.scale = screen_scale;
	if (!server->init(options) || !server->start()) {
		server.reset();
		stop_world();
		return 2;
	}
	wlr_backend *headless = headless_backend();
	if (headless == nullptr || wlr_headless_add_output(headless,
			static_cast<unsigned>(screen_w * screen_scale),
			static_cast<unsigned>(screen_h * screen_scale)) == nullptr) {
		std::println(stderr, "media: no headless output");
		server.reset();
		stop_world();
		return 2;
	}

	stage();
	stills();
	if (!stills_only) {
		walkthrough();
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *) { return handle_timer(); },
		nullptr);
	motion_timer = wl_event_loop_add_timer(server->loop, [](void *) { return tick_motion(); },
		nullptr);
	wl_event_source_timer_update(timer, 300);
	wl_display_run(server->display);

	bool done = !failed && step >= steps.size();
	if (done) {
		write_manifest();
	}
	if (rec.fd >= 0) {
		close(rec.fd);
		waitpid(rec.pid, nullptr, 0);
	}
	if (rec.timer != nullptr) {
		wl_event_source_remove(rec.timer);
	}
	wl_event_source_remove(motion_timer);
	wl_event_source_remove(timer);
	stop_apps();
	if (devices) {
		wlr_pointer_finish(&ptr);
		wlr_keyboard_finish(&kbd);
	}
	server.reset();
	stop_world();
	if (!done) {
		std::println(stderr, "media: the tour stopped at step {}", step < steps.size()
			? steps[step].name : "end");
	}
	return done ? 0 : 1;
}
