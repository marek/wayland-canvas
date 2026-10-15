// End-to-end bar-panels scenario: the real compositor on the headless
// backend with the real bar client (qs — the suite is registered only when
// qs and the dbusmock stand-in environment are present), fake pointer and
// keyboard devices, grim screenshots. The bar never touches the machine:
// it starts on a private system and session D-Bus pair under tmp/ (a
// startup failure of either means no bar at all) carrying stand-in
// services — NetworkManager, BlueZ, UPower, power-profiles-daemon and one
// StatusNotifierItem with a menu (tests/scenarios/bar-standins.py) — so
// the scenario's clicks and typing can only ever act on those. Every
// indicator is clicked with the pointer and its panel must appear as a new
// surface of the bar client, then close again — by a click outside, by
// Escape and by a second click of its icon. The Wi-Fi password field must
// receive typing (never submitted), and only one panel may be open at a
// time. The same world then carries the canvas.notes plugin: a note
// created through the public action.run, its editor mapping over it with
// the keyboard, typing kept in the state and the plugin's storage, a
// pointer drag's new place, a shell reload that neither loses nor doubles
// the note (and puts it back once it was removed), the overview's
// double-click reaching the editor, a colour dot recolouring, the ×
// deleting, and a note closed empty being removed. Writes only under
// $CANVAS_TMP (meson passes the repo's tmp/).
#include <cairo.h>
#include <json-glib/json-glib.h>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <poll.h>
#include <print>
#include <source_location>
#include <string>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/wlr.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
uint32_t t = 90000;
pid_t grim_pid = -1;
wlr_pointer ptr{};
wlr_keyboard kbd{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
std::string shot_dir;
std::string user_plugin; // the canvas.hello plugin staged for step 11
std::string staged_plugin; // where its files wait until the drop-in
std::size_t base_runs = 0; // the bar's run count before the user plugin
int plugin_retries = 0; // the plugin phase's own retry counter (steps move)
double user_x1 = 0; // the user widget's right edge, before the QML edit
pid_t bar_pid = 0; // the shell client, for the idle spawn watch
int idle_ticks = 0; // samples left of the idle watch
bool idle_spawned = false; // any child of the shell seen while idle
constexpr int idle_sample_count = 1000; // 10 s of samples
constexpr int idle_sample_ms = 10;
constexpr double widen_px = 20; // how much the edited widget must grow

// ---- the notes phase, over the public control socket ----

// The plugin's settings defaults, kept in sync with
// plugins/canvas.notes/plugin.json (the swatch colours the test creates
// and looks for on screen).
constexpr const char *note_yellow = "#ffd866";
constexpr const char *note_green = "#a6e3a1";
constexpr const char *note_blue = "#7dcfff";
constexpr int note_size = 220;
constexpr int note_strip = 26; // canvas px, the darker strip
constexpr const char *note_editor_namespace = "wayland-canvas-note-editor";
constexpr int colour_tune = 8; // channel tolerance when matching a colour on a screenshot
constexpr int edge_tune = 4; // px tolerance where a box on a screenshot may sit
// The scratch shell.json the plugin phase wrote: rewritten as-is, an edit
// the shell sees, so it reloads.
constexpr std::string_view plugin_config =
	"{\n\t\"plugins\": {\n\t\t\"canvas.hello\": {\n\t\t\t\"enabled\": false\n"
	"\t\t}\n\t}\n}\n";

struct NoteEntry {
	std::string id, color, text;
	double x = 0, y = 0;
	long long width = 0, height = 0;
};

NoteEntry note; // the note under test
int note_retries = 0; // the notes phase's retry counter
LayerSurface *bar_before_reload = nullptr;
std::vector<pid_t> msg_children; // `wayland-canvas msg` runs, swept when done

// ---- isolation: private buses and the stand-in services on them ----

struct PrivateBus {
	pid_t pid = -1;
	std::string address; // what DBUS_*_BUS_ADDRESS becomes for the bar
};

PrivateBus system_bus, session_bus;
pid_t standins_pid = -1;

// The panel widths shell/Panel.qml commits, one distinct width per panel so
// a panel can be told apart by its surface. Keep in sync with the panels.
constexpr int network_panel = 340;
constexpr int weather_panel = 320;
constexpr int bluetooth_panel = 300;
constexpr int audio_panel = 280;
constexpr int power_panel = 260;
constexpr int brightness_panel = 240;
constexpr int clock_panel = 220;
constexpr int tray_panel = 200;
constexpr const char *bar_namespace = "wayland-canvas-bar";

// Output geometry the headless backend gives: 1280x720, one output.
constexpr int screen_w = 1280, screen_h = 720;

enum CloseMode { close_outside, close_escape, close_icon };

struct Indicator {
	const char *name;
	int width;
	double x = 0; // centre of the bar run that opens it
	int button = BTN_LEFT;
	CloseMode close_mode = close_outside;
	bool connect_flow = false; // the Wi-Fi flow, ending in Connect/Disconnect
	bool bt_flow = false; // the paired/found device actions
	bool profile_flow = false; // the power profile switch
};

std::vector<Indicator> indicators;
std::size_t current = 0;
int phase = 0;
wlr_box popup_box{}; // the open panel's card, from the walk's phase 1
wlr_box field_box{}; // where the Wi-Fi password field ended up
double clock_x = 0, network_x = 0; // where the walk found the clock, network

// Prints PASS/FAIL with the note; a failure counts and names its source line.
void check(bool cond, std::string_view note,
	std::source_location where = std::source_location::current()) {
	std::println("{}: {} ({}:{})", cond ? "PASS" : "FAIL", note,
		where.file_name(), where.line());
	if (!cond) {
		failures++;
	}
}

// Reads a small text file whole (the msg clients' captured output).
std::string read_file(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		return {};
	}
	std::string out;
	char buf[4096];
	std::size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.append(buf, n);
	}
	std::fclose(f);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
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

// Writes a small text file whole (the scratch plugin world's files).
void write_file(const std::string &path, std::string_view text) {
	FILE *f = std::fopen(path.c_str(), "w");
	if (f == nullptr) {
		std::fprintf(stderr, "cannot write %s\n", path.c_str());
		return;
	}
	std::fwrite(text.data(), 1, text.size(), f);
	std::fclose(f);
}

// ---- machine state: which indicators the bar shows ----

// The stand-ins decide almost all of it: their world always has a battery
// (UPower's discharging display device), a Wi-Fi device (NetworkManager)
// and a Bluetooth adapter (BlueZ), so those indicators are unconditional.
// The backlight is the one real-machine signal left — no stand-in can
// provide sysfs, and the bar's brightness widget checks for one with
// brightnessctl before it shows.

// Any entry under /sys/class/<name> besides the dot entries.
bool has_sysfs_class(const char *name) {
	std::string root = std::format("/sys/class/{}", name);
	DIR *d = opendir(root.c_str());
	if (d == nullptr) {
		return false;
	}
	bool found = false;
	for (dirent *e = readdir(d); e != nullptr; e = readdir(d)) {
		if (e->d_name[0] != '.') {
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}


// ---- bringing up the private buses and the stand-ins ----

// A forked child that keeps running: its stdout on a pipe, its death tied
// to the scenario process so nothing outlives a killed run.
struct Spawned {
	pid_t pid = -1;
	int out = -1; // its stdout, or -1 once drained
};

Spawned spawn(const char *const argv[]) {
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
		prctl(PR_SET_PDEATHSIG, SIGKILL); // a killed scenario must not leak buses
		execvp(argv[0], const_cast<char *const *>(argv));
		_exit(127);
	}
	close(fds[1]);
	return pid > 0 ? Spawned{pid, fds[0]} : Spawned{};
}

// The child's first stdout line, or empty when it ends without one.
std::string first_line(Spawned &child) {
	std::string line;
	char c = 0;
	while (read(child.out, &c, 1) == 1 && c != '\n') {
		line += c;
	}
	close(child.out);
	child.out = -1;
	return line;
}

// True when the child prints the line within the timeout; false when it
// exits or stalls first.
bool prints_line(Spawned &child, std::string_view want, int timeout_s) {
	std::string buffer;
	char chunk[256];
	for (int tick = 0; tick < timeout_s * 2; tick++) { // ticks of 500 ms
		pollfd p = {.fd = child.out, .events = POLLIN, .revents = 0};
		if (poll(&p, 1, 500) > 0 && (p.revents & (POLLIN | POLLHUP)) != 0) {
			ssize_t n = read(child.out, chunk, sizeof chunk);
			if (n <= 0) {
				break;
			}
			buffer.append(chunk, static_cast<std::size_t>(n));
			if (buffer.find(want) != std::string::npos) {
				return true;
			}
		}
		if (waitpid(child.pid, nullptr, WNOHANG) == child.pid) {
			break;
		}
	}
	return false;
}

// A dbus-daemon of our own on a private socket under tmp/: everything on
// it is allowed, it exists for this run only, and no machine service is
// reachable through it.
bool start_private_bus(PrivateBus &bus, const char *type, const char *name) {
	std::string dir = shot_dir + "/bus";
	mkdir_p(dir);
	std::string socket = std::format("{}/{}", dir, name);
	std::string conf = socket + ".conf";
	unlink(socket.c_str()); // a stale socket from a killed run
	std::string xml = std::format(
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
		"</busconfig>\n",
		type, socket);
	FILE *f = std::fopen(conf.c_str(), "w");
	if (f == nullptr) {
		std::fprintf(stderr, "cannot write %s\n", conf.c_str());
		return false;
	}
	std::fwrite(xml.data(), 1, xml.size(), f);
	std::fclose(f);
	const char *const argv[] = {"dbus-daemon", "--config-file", conf.c_str(), "--print-address=1",
		static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid <= 0) {
		return false;
	}
	// The printed address only says the daemon is serving; the address the
	// bar and the stand-ins get is the plain listen form — dbus-python
	// never finishes connecting to the guid-decorated one.
	std::string printed = first_line(child);
	if (!printed.starts_with("unix:")) {
		std::fprintf(stderr, "the private %s bus did not start: %s\n", type,
			printed.c_str());
		kill(child.pid, SIGKILL);
		waitpid(child.pid, nullptr, 0);
		return false;
	}
	bus.pid = child.pid;
	bus.address = "unix:path=" + socket;
	return true;
}

// The dbusmock stand-ins on the private buses (tests/scenarios/
// bar-standins.py). READY means their four system-bus names are up; the
// tray item registers with the bar's own watcher once the bar appears.
bool start_standins() {
	const char *py = std::getenv("CANVAS_BAR_STANDIN_PY");
	const char *script = std::getenv("CANVAS_BAR_STANDINS");
	if (py == nullptr || *py == '\0' || script == nullptr || *script == '\0') {
		std::fprintf(stderr, "CANVAS_BAR_STANDIN_PY or CANVAS_BAR_STANDINS is unset: "
			"the stand-in environment is missing\n");
		return false;
	}
	const char *const argv[] = {py, script, static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid <= 0) {
		return false;
	}
	if (!prints_line(child, "READY\n", 30)) {
		std::fprintf(stderr, "the stand-in services did not become ready\n");
		kill(child.pid, SIGTERM);
		waitpid(child.pid, nullptr, 0);
		return false;
	}
	close(child.out);
	child.out = -1;
	standins_pid = child.pid;
	return true;
}

// Waits for a child up to two seconds, then kills it.
void reap(pid_t pid) {
	if (pid <= 0) {
		return;
	}
	for (int i = 0; i < 200; i++) {
		if (waitpid(pid, nullptr, WNOHANG) == pid) {
			return;
		}
		timespec ts = {.tv_sec = 0, .tv_nsec = 10'000'000};
		nanosleep(&ts, nullptr);
	}
	kill(pid, SIGKILL);
	waitpid(pid, nullptr, 0);
}

// Everything the scenario started, in reverse order: the launcher (which
// terminates the dbusmock mocks with it), then the buses.
void stop_isolation() {
	reap(standins_pid);
	reap(system_bus.pid);
	reap(session_bus.pid);
}

// One variable of a live process's environment, as /proc tells it — the
// isolation proof straight from the bar's own process.
std::string proc_env_value(pid_t pid, const char *key) {
	if (pid <= 0) {
		return {};
	}
	FILE *f = std::fopen(std::format("/proc/{}/environ", pid).c_str(), "r");
	if (f == nullptr) {
		return {};
	}
	std::string blob;
	char buf[4096];
	std::size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) { // environ may exceed 8 KiB
		blob.append(buf, n);
	}
	std::fclose(f);
	std::string prefix = std::format("{}=", key);
	std::size_t start = 0;
	while (start < blob.size()) {
		std::size_t end = blob.find('\0', start);
		if (end == std::string::npos) {
			end = blob.size();
		}
		if (blob.compare(start, prefix.size(), prefix) == 0) {
			return blob.substr(start + prefix.size(), end - start - prefix.size());
		}
		start = end + 1;
	}
	return {};
}

// Whether a live process has any child right now, as /proc tells it —
// the idle watch: a shell that reloads by change notifications must
// spawn nothing while it just sits there.
bool proc_has_children(pid_t pid) {
	if (pid <= 0) {
		return false;
	}
	std::string tasks = std::format("/proc/{}/task", pid);
	DIR *dir = opendir(tasks.c_str());
	if (dir == nullptr) {
		return false;
	}
	bool any = false;
	struct dirent *e = nullptr;
	while (!any && (e = readdir(dir)) != nullptr) {
		if (e->d_name[0] == '.') {
			continue;
		}
		FILE *f = std::fopen((tasks + "/" + e->d_name + "/children").c_str(), "r");
		if (f == nullptr) {
			continue;
		}
		char buf[512];
		while (!any && std::fgets(buf, sizeof buf, f) != nullptr) {
			for (const char *p = buf; *p != '\0'; p++) {
				if (*p != ' ' && *p != '\n') {
					any = true;
					break;
				}
			}
		}
		std::fclose(f);
	}
	closedir(dir);
	return any;
}

// ---- input, through the same fake devices libinput events reach ----

void key(uint32_t code, bool pressed) {
	t += 8;
	wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

void tap(uint32_t code) {
	key(code, true);
	key(code, false);
}

// Lower-case letters, digits and the space, enough for a password or a
// note's text.
void type(std::string_view s) {
	struct KeyMap {
		char c;
		uint32_t code;
	};
	constexpr KeyMap map[] = {
		{' ', KEY_SPACE},
		{'a', KEY_A}, {'b', KEY_B}, {'c', KEY_C}, {'d', KEY_D}, {'e', KEY_E}, {'f', KEY_F},
		{'g', KEY_G}, {'h', KEY_H}, {'i', KEY_I}, {'j', KEY_J}, {'k', KEY_K}, {'l', KEY_L},
		{'m', KEY_M}, {'n', KEY_N}, {'o', KEY_O}, {'p', KEY_P}, {'q', KEY_Q}, {'r', KEY_R},
		{'s', KEY_S}, {'t', KEY_T}, {'u', KEY_U}, {'v', KEY_V}, {'w', KEY_W}, {'x', KEY_X},
		{'y', KEY_Y}, {'z', KEY_Z}, {'0', KEY_0}, {'1', KEY_1}, {'2', KEY_2}, {'3', KEY_3},
		{'4', KEY_4}, {'5', KEY_5}, {'6', KEY_6}, {'7', KEY_7}, {'8', KEY_8}, {'9', KEY_9},
	};
	for (char c : s) {
		for (const KeyMap &m : map) {
			if (m.c == c) {
				tap(m.code);
			}
		}
	}
}

void click(double x, double y, int button = BTN_LEFT) {
	t += 8;
	wlr_pointer_motion_absolute_event m = {
		.pointer = &ptr, .time_msec = t, .x = x / double(screen_w), .y = y / double(screen_h)};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &m);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
	for (int pressed = 1; pressed >= 0; pressed--) {
		t += 8;
		wlr_pointer_button_event b = {.pointer = &ptr, .time_msec = t, .button = uint32_t(button),
			.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
		wl_signal_emit_mutable(&ptr.events.button, &b);
		wl_signal_emit_mutable(&ptr.events.frame, &ptr);
	}
}

// Two clicks close together in device time: the compositor's double-click.
void double_click(double x, double y) {
	click(x, y);
	click(x, y);
}

// A drag with the fake pointer: move, press, ten moves, release. No click
// first — a click ahead of the press would read as one half of a
// double-click on the item.
void drag_note(double x0, double y0, double x1, double y1) {
	t += 8;
	wlr_pointer_motion_absolute_event start = {
		.pointer = &ptr, .time_msec = t,
		.x = x0 / double(screen_w), .y = y0 / double(screen_h)};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &start);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
	for (int pressed = 1; pressed >= 0; pressed--) {
		t += 8;
		wlr_pointer_button_event b = {.pointer = &ptr, .time_msec = t,
			.button = BTN_LEFT,
			.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
				: WL_POINTER_BUTTON_STATE_RELEASED};
		wl_signal_emit_mutable(&ptr.events.button, &b);
		wl_signal_emit_mutable(&ptr.events.frame, &ptr);
		if (pressed) {
			for (int i = 1; i <= 10; i++) {
				t += 8;
				double x = x0 + (x1 - x0) * i / 10, y = y0 + (y1 - y0) * i / 10;
				wlr_pointer_motion_absolute_event m = {
					.pointer = &ptr, .time_msec = t,
					.x = x / double(screen_w), .y = y / double(screen_h)};
				wl_signal_emit_mutable(&ptr.events.motion_absolute, &m);
				wl_signal_emit_mutable(&ptr.events.frame, &ptr);
			}
		}
	}
}

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

// ---- the bar client and its surfaces ----

LayerSurface *bar_layer() {
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strcmp(l->wlr->namespace_, bar_namespace) == 0) {
					return l.get();
				}
			}
		}
	}
	return nullptr;
}

wl_client *bar_client() {
	LayerSurface *l = bar_layer();
	return l == nullptr ? nullptr
		: wl_resource_get_client(l->wlr->surface->resource);
}

// The bar strip's height: panels hang directly under it.
int bar_h() {
	LayerSurface *l = bar_layer();
	return l != nullptr ? l->geo.height : 26;
}

// The bar client's mapped panel layer surface with this name (each panel
// names its surface; the shell and this suite agree on the names).
LayerSurface *panel_layer(const char *name) {
	std::string ns = std::format("wayland-canvas-panel-{}", name);
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& ns == l->wlr->namespace_) {
					return l.get();
				}
			}
		}
	}
	return nullptr;
}

// Mapped panels of any name: the one-at-a-time rule counts them.
int panel_layers() {
	int n = 0;
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strncmp(l->wlr->namespace_, "wayland-canvas-panel",
							20) == 0) {
					n++;
				}
			}
		}
	}
	return n;
}

// The card a panel draws, on screen: centred under the indicator and
// clamped exactly the way Panel.qml clamps it.
wlr_box card_box(double icon_x, int width) {
	double x = icon_x - width / 2.0;
	x = std::max(8.0, std::min(x, double(screen_w - width - 8)));
	return wlr_box{static_cast<int>(std::round(x)), bar_h(), width, 0};
}

// The lit text bands inside a card's column, top to bottom: one per row,
// header or field of the panel's content, so positions are read off the
// screenshot instead of assumed from the layout.
struct Band {
	int top, bottom;
	int lit = 0; // lit pixels: a row of text has hundreds, a dim header
		     // fragment dozens — the row filter below tells them apart
	double centre() const { return (top + bottom - 1) / 2.0; }
};

std::vector<Band> text_bands(cairo_surface_t *s, const wlr_box &card) {
	std::vector<Band> bands;
	if (s == nullptr) {
		return bands;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	int x0 = card.x + 8, x1 = card.x + card.width - 8;
	bool in_band = false;
	int band_top = 0, band_lit = 0;
	for (int y = card.y; y < screen_h; y++) {
		int lit = 0;
		for (int x = x0; x < x1; x++) {
			unsigned char *p = data + y * stride + x * 4;
			if (p[0] + p[1] + p[2] > 300) {
				lit++;
			}
		}
		if (lit > 0 && !in_band) {
			in_band = true;
			band_top = y;
			band_lit = lit;
		} else if (lit > 0) {
			band_lit += lit;
		} else if (in_band) {
			in_band = false;
			bands.push_back({band_top, y, band_lit});
		}
	}
	if (in_band) { // a band running to the scan's end
		bands.push_back({band_top, screen_h, band_lit});
	}
	return bands;
}

cairo_surface_t *load_png(const char *name); // defined with the screenshots
std::vector<Band> panel_bands(const char *name) {
	cairo_surface_t *panel = load_png(name);
	std::vector<Band> bands = panel != nullptr ? text_bands(panel, popup_box)
						   : std::vector<Band>();
	if (panel != nullptr) {
		cairo_surface_destroy(panel);
	}
	return bands;
}

// Only the rows of a named shot: dim section headers render as thin
// half-lit fragments that shift raw band indices, so the action flows
// click by row, counted from the bottom (the last row is stable).
std::vector<Band> panel_rows(const char *name) {
	std::vector<Band> rows;
	for (const Band &b : panel_bands(name)) {
		if (b.lit >= 60) {
			rows.push_back(b);
		}
	}
	return rows;
}

// ---- waiting for a panel to stop changing ----

// The panel's shape: each band's extent and a coarse lit count. The caret
// blinking in the password field lights a few pixels inside one band; the
// shape stays put, and a stable shape means the rows the flows aim at are
// where the screenshot says.
std::string band_shape(cairo_surface_t *panel) {
	std::string shape;
	for (const Band &b : text_bands(panel, popup_box)) {
		shape += std::format("{}:{}:{},", b.top, b.bottom, b.lit / 16);
	}
	return shape;
}

int settle_stage = 0; // 0: idle, 1: first shot taken, 2: sampling pairs
int settle_retries = 0;
std::string settle_bytes; // the previous shot's shape

// Shots `name` on each call and reports whether two consecutive shots
// showed the same shape. Call it on successive ticks (a tick is enough
// time for grim to finish: handle_timer waits for it first) and click
// only once it says yes — the shot on disk is then current, so the aim
// comes from it. Under load the bar renders late; clicking at a stale
// screenshot lands wrong or out of order, and a re-issued click can
// toggle a row twice, so the flows wait instead of retrying.
bool settle_advance(const char *name) {
	if (settle_stage == 0) {
		shot(name);
		settle_stage = 1;
		settle_retries = 0;
		return false;
	}
	cairo_surface_t *panel = load_png(name); // the previous call's shot
	std::string shape = panel != nullptr ? band_shape(panel) : std::string();
	if (panel != nullptr) {
		cairo_surface_destroy(panel);
	}
	bool same = shape == settle_bytes;
	settle_bytes = shape;
	if (settle_stage == 1) {
		settle_stage = 2;
		shot(name);
		return false;
	}
	if (!same && ++settle_retries < 25) { // a changing panel; keep watching
		shot(name);
		return false;
	}
	settle_stage = 0; // matched (or given up waiting): the shot is stable
	return true;
}
// ---- the stand-ins' own record: what the mocks received ----

constexpr const char *NM_NAME = "org.freedesktop.NetworkManager";
constexpr const char *NM_MANAGER = "/org/freedesktop/NetworkManager";
constexpr const char *NM_DEVICE = "/org/freedesktop/NetworkManager/Devices/wifi0";
constexpr const char *BT_NAME = "org.bluez";
constexpr const char *BT_HEADPHONES = "/org/bluez/hci0/dev_11_22_33_44_55_66";
constexpr const char *BT_SPEAKER = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";
constexpr const char *PPD_NAME = "org.freedesktop.UPower.PowerProfiles";
constexpr const char *PPD_PATH = "/org/freedesktop/UPower/PowerProfiles";
constexpr const char *PPD_IFACE = "org.freedesktop.UPower.PowerProfiles";

// All of a child's stdout, or empty; the child is reaped either way.
std::string child_output(Spawned &child, int timeout_s) {
	std::string out;
	char buf[4096];
	std::time_t deadline = std::time(nullptr) + timeout_s;
	while (std::time(nullptr) < deadline) {
		pollfd p{.fd = child.out, .events = POLLIN};
		if (poll(&p, 1, 200) <= 0 || (p.revents & (POLLIN | POLLHUP)) == 0) {
			continue;
		}
		ssize_t n = read(child.out, buf, sizeof buf);
		if (n <= 0) {
			break; // EOF: the child is done writing
		}
		out.append(buf, n);
	}
	kill(child.pid, SIGKILL); // a no-op on an already-exited child
	waitpid(child.pid, nullptr, 0);
	close(child.out);
	return out;
}

// busctl against the private system bus only — the address is always
// explicit, so nothing can ever reach the machine's bus.
std::string busctl(const char *verb, const char *name, const char *path,
	const char *interface, const char *member) {
	const char *const argv[] = {"busctl", "--address", system_bus.address.c_str(),
		verb, name, path, interface, member, static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid <= 0) {
		return {};
	}
	return child_output(child, 10);
}

// True when the mock object at `path` recorded a call to `method` — with
// `arg` in its arguments when `arg` is not empty.
bool mock_called(const char *name, const char *path, const char *method,
	const char *arg = nullptr) {
	std::string calls = busctl("call", name, path, "org.freedesktop.DBus.Mock",
		"GetCalls");
	if (calls.find(std::format("\"{}\"", method)) == std::string::npos) {
		return false;
	}
	return arg == nullptr || calls.find(arg) != std::string::npos;
}

// A property of a stand-in object, as busctl prints it.
std::string mock_property(const char *name, const char *path, const char *interface,
	const char *property) {
	return busctl("get-property", name, path, interface, property);
}

// The keyboard is on a surface of the bar client (so panel keystrokes land).
bool keyboard_on_bar() {
	wlr_surface *s = server->seat->keyboard_state.focused_surface;
	return s != nullptr && bar_client() != nullptr
		&& wl_resource_get_client(s->resource) == bar_client();
}

// ---- screenshots: decode, find the indicator runs ----

cairo_surface_t *load_png(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	cairo_surface_t *s = cairo_image_surface_create_from_png(path.c_str());
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		check(false, std::format("decoding {}", path));
		cairo_surface_destroy(s);
		return nullptr;
	}
	return s;
}

struct Run {
	double x0, x1;
	double centre() const { return (x0 + x1) / 2; }
};

// The lit column runs of the bar strip: each run is one widget's glyphs on
// the bar's own flat background. Gaps of 8 px or less stay inside a widget
// (letter spacing); widget boundaries are wider.
std::vector<Run> bar_runs(cairo_surface_t *s, const wlr_box &bar) {
	std::vector<Run> runs;
	if (s == nullptr) {
		return runs;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	std::vector<bool> lit(bar.width);
	for (int x = 0; x < bar.width; x++) {
		for (int y = 0; y < bar.height; y++) {
			unsigned char *p = data + (bar.y + y) * stride + (bar.x + x) * 4;
			// ARGB32 little-endian: B, G, R, A. The bar background is
			// #1a1b26 (channel sum 91); glyph pixels are far brighter.
			if (p[0] + p[1] + p[2] > 300) {
				lit[x] = true;
				break;
			}
		}
	}
	constexpr int merge_gap = 8;
	int start = -1, gap = 0;
	for (int x = 0; x <= bar.width; x++) {
		bool on = x < bar.width && lit[x];
		if (on && start < 0) {
			start = x;
			gap = 0;
		} else if (on) {
			gap = 0;
		} else if (start >= 0 && ++gap > merge_gap) {
			if (x - gap - start >= 3) { // drop sub-pixel noise
				runs.push_back({double(start), double(x - gap)});
			}
			start = -1;
		}
	}
	return runs;
}

// Bright pixels (text-coloured) inside a rectangle of the screen: password
// dots, or a cursor sliver when nothing was typed.
int bright_count(cairo_surface_t *s, const wlr_box &rect) {
	if (s == nullptr) {
		return -1;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	int n = 0;
	for (int y = rect.y; y < rect.y + rect.height; y++) {
		if (y < 0 || y >= h) {
			continue;
		}
		for (int x = rect.x; x < rect.x + rect.width; x++) {
			if (x < 0 || x >= w) {
				continue;
			}
			unsigned char *p = data + y * stride + x * 4;
			if (p[0] + p[1] + p[2] > 400) {
				n++;
			}
		}
	}
	return n;
}

// ---- notes: the msg client, the plugin's state, its storage ----

// Runs `wayland-canvas msg <method> [<params>]` through /bin/sh, with
// stdout, stderr and the exit code captured under `name` (.out, .err,
// .code); a stale capture from an earlier run is unlinked first. The
// compositor runs in this process, so the child is never waited on here.
void run_msg(std::string_view method, std::string_view params, const char *name) {
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	for (const char *suffix : {".out", ".err", ".code"}) {
		unlink(std::format("{}/{}{}", shot_dir, name, suffix).c_str());
	}
	std::string cmd = std::format("\"{}\" msg {}", bin, method);
	if (!params.empty()) {
		cmd += std::format(" '{}'", params);
	}
	cmd += std::format(" > {}/{}.out 2> {}/{}.err; echo $? > {}/{}.code",
		shot_dir, name, shot_dir, name, shot_dir, name);
	const char *const argv[] = {"/bin/sh", "-c", cmd.c_str(),
		static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid > 0) {
		msg_children.push_back(child.pid);
	}
	if (child.out >= 0) {
		close(child.out); // the captures go to files, not this pipe
	}
}

bool msg_done(const char *name) {
	struct stat st{};
	return stat(std::format("{}/{}.code", shot_dir, name).c_str(), &st) == 0;
}

std::string msg_out(const char *name) {
	return read_file(std::format("{}/{}.out", shot_dir, name));
}

std::string msg_err(const char *name) {
	return read_file(std::format("{}/{}.err", shot_dir, name));
}

int msg_code(const char *name) {
	return std::atoi(read_file(std::format("{}/{}.code", shot_dir, name)).c_str());
}

// Reaps the msg children that finished, so none is left a zombie.
void sweep_children() {
	std::erase_if(msg_children, [](pid_t pid) {
		return waitpid(pid, nullptr, WNOHANG) == pid;
	});
}

// Nothing the scenario started outlives it: a msg child still running at
// the end is killed and reaped.
void stop_children() {
	for (pid_t pid : msg_children) {
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
	}
	msg_children.clear();
}

// One JSON object member as a string, or "" when it is absent.
std::string json_string(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return {};
	}
	JsonNode *node = json_object_get_member(object, name);
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? std::string{json_node_get_string(node)} : std::string{};
}

// One JSON object member as a whole number, or 0 when it is absent.
long long json_int(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return 0;
	}
	JsonNode *node = json_object_get_member(object, name);
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_INT64
		? json_node_get_int(node) : 0;
}

void fill_note(NoteEntry &entry, JsonObject *object) {
	entry.color = json_string(object, "color");
	entry.text = json_string(object, "text");
	entry.x = json_int(object, "x");
	entry.y = json_int(object, "y");
	entry.width = json_int(object, "width");
	entry.height = json_int(object, "height");
}

// The canvas.notes items a `msg state` capture reports.
std::vector<NoteEntry> state_notes(const char *name) {
	std::vector<NoteEntry> out;
	std::string text = msg_out(name);
	JsonParser *parser = json_parser_new();
	if (json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		JsonNode *root = json_parser_get_root(parser);
		JsonObject *state = root != nullptr
				&& json_node_get_node_type(root) == JSON_NODE_OBJECT
			? json_node_get_object(root) : nullptr;
		JsonNode *items = state != nullptr && json_object_has_member(state, "items")
			? json_object_get_member(state, "items") : nullptr;
		if (items != nullptr && json_node_get_node_type(items) == JSON_NODE_ARRAY) {
			JsonArray *array = json_node_get_array(items);
			for (guint i = 0; i < json_array_get_length(array); i++) {
				JsonNode *element = json_array_get_element(array, i);
				JsonObject *item = json_node_get_node_type(element) == JSON_NODE_OBJECT
					? json_node_get_object(element) : nullptr;
				NoteEntry entry;
				entry.id = json_string(item, "id");
				if (item == nullptr || entry.id.rfind("canvas.notes/", 0) != 0) {
					continue;
				}
				fill_note(entry, item);
				out.push_back(entry);
			}
		}
	}
	g_object_unref(parser);
	return out;
}

// The plugin's stored notes, from its notes.json in the scratch state.
std::vector<NoteEntry> stored_notes() {
	std::vector<NoteEntry> out;
	std::string text = read_file(
		shot_dir + "/xdg/state/wayland-canvas/plugins/canvas.notes/notes.json");
	JsonParser *parser = json_parser_new();
	if (json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		JsonNode *root = json_parser_get_root(parser);
		JsonObject *saved = root != nullptr
				&& json_node_get_node_type(root) == JSON_NODE_OBJECT
			? json_node_get_object(root) : nullptr;
		JsonObject *notes = saved != nullptr && json_object_has_member(saved, "notes")
				&& json_node_get_node_type(json_object_get_member(saved, "notes"))
					== JSON_NODE_OBJECT
			? json_node_get_object(json_object_get_member(saved, "notes")) : nullptr;
		if (notes != nullptr) {
			for (GList *member = json_object_get_members(notes); member != nullptr;
					member = member->next) {
				const char *id = static_cast<const char *>(member->data);
				JsonNode *element = json_object_get_member(notes, id);
				JsonObject *fields = element != nullptr
						&& json_node_get_node_type(element) == JSON_NODE_OBJECT
					? json_node_get_object(element) : nullptr;
				NoteEntry entry;
				entry.id = id;
				if (fields == nullptr) {
					continue;
				}
				fill_note(entry, fields);
				out.push_back(entry);
			}
		}
	}
	g_object_unref(parser);
	return out;
}

std::optional<NoteEntry> stored_note(const std::string &id) {
	for (const NoteEntry &entry : stored_notes()) {
		if (entry.id == id) {
			return entry;
		}
	}
	return std::nullopt;
}

// The mapped editor layer surface of the shell client.
LayerSurface *editor_layer() {
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strcmp(l->wlr->namespace_, note_editor_namespace) == 0) {
					return l.get();
				}
			}
		}
	}
	return nullptr;
}

// True when the editor maps and holds the keyboard on its own surface.
bool editor_open_with_keyboard() {
	LayerSurface *editor = editor_layer();
	return editor != nullptr && bar_client() != nullptr
		&& wl_resource_get_client(editor->wlr->surface->resource) == bar_client()
		&& server->seat->keyboard_state.focused_surface == editor->wlr->surface;
}

// The one output, as the compositor has it.
Output *the_output() {
	return server->outputs.empty() ? nullptr : server->outputs.front().get();
}

// A note's box on screen right now: the compositor's own camera math, so
// the editor and the pictures are checked against what is drawn, not
// against a guess.
wlr_box note_box_on_screen(const NoteEntry &entry) {
	Output *output = the_output();
	if (output == nullptr) {
		return {};
	}
	Point top_left = output->canvas_to_local(entry.x, entry.y);
	double zoom = output->camera.drawn().zoom;
	return wlr_box{
		static_cast<int>(std::round(top_left.x)),
		static_cast<int>(std::round(top_left.y)),
		static_cast<int>(std::round(entry.width * zoom)),
		static_cast<int>(std::round(entry.height * zoom)),
	};
}

// True once the overview is where the test wants it and the camera has
// stopped easing: positions read off the screen are stable then.
bool overview_settled(bool active) {
	Output *output = the_output();
	return output != nullptr && output->overview.active() == active
		&& !output->camera.panning() && !output->camera.zooming();
}

// "#rrggbb" to its channels; false when it is not that shape.
bool hex_colour(std::string_view text, unsigned char out[3]) {
	auto hex = [](char ch) -> int {
		if (ch >= '0' && ch <= '9') {
			return ch - '0';
		}
		if (ch >= 'a' && ch <= 'f') {
			return ch - 'a' + 10;
		}
		return -1;
	};
	if (text.size() != 7 || text.front() != '#') {
		return false;
	}
	for (int channel = 0; channel < 3; channel++) {
		int hi = hex(text[1 + channel * 2]);
		int lo = hex(text[2 + channel * 2]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		out[channel] = static_cast<unsigned char>(hi * 16 + lo);
	}
	return true;
}

// The bounding box of the pixels within `tune` of `rgb` inside `region`,
// with how many matched.
struct ColourBox {
	int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive; empty when count is 0
	int count = 0;
	double cx() const { return count > 0 ? (x0 + x1) / 2.0 : 0; }
	double cy() const { return count > 0 ? (y0 + y1) / 2.0 : 0; }
	int w() const { return count > 0 ? x1 - x0 + 1 : 0; }
	int h() const { return count > 0 ? y1 - y0 + 1 : 0; }
};

ColourBox find_colour(cairo_surface_t *s, const wlr_box &region,
		const unsigned char rgb[3], int tune) {
	ColourBox box;
	if (s == nullptr) {
		return box;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	for (int y = region.y; y < region.y + region.height; y++) {
		if (y < 0 || y >= h) {
			continue;
		}
		for (int x = region.x; x < region.x + region.width; x++) {
			if (x < 0 || x >= w) {
				continue;
			}
			// ARGB32 little-endian: B, G, R, A.
			unsigned char *p = data + y * stride + x * 4;
			if (std::abs(p[2] - rgb[0]) <= tune && std::abs(p[1] - rgb[1]) <= tune
					&& std::abs(p[0] - rgb[2]) <= tune) {
				if (box.count == 0 || x < box.x0) {
					box.x0 = x;
				}
				if (box.count == 0 || x > box.x1) {
					box.x1 = x;
				}
				if (box.count == 0 || y < box.y0) {
					box.y0 = y;
				}
				if (box.count == 0 || y > box.y1) {
					box.y1 = y;
				}
				box.count++;
			}
		}
	}
	return box;
}

// Where the strip's controls sit on screen, for clicking them: the dots
// row on its left, the × square on its right.
double strip_h_on_screen(const NoteEntry &entry) {
	wlr_box box = note_box_on_screen(entry);
	return std::round(note_strip * box.width / double(note_size));
}

// The note's card below its darker strip, on a screenshot: the colour's
// bounding box there, expected where the compositor draws it. The strip
// is searched apart because the editor's colour dots sit on it, and one
// of them is the card's own colour.
ColourBox card_on_shot(const char *name, const NoteEntry &entry) {
	unsigned char rgb[3];
	if (!hex_colour(entry.color, rgb)) {
		return {};
	}
	cairo_surface_t *s = load_png(name);
	wlr_box box = note_box_on_screen(entry);
	int strip_h = static_cast<int>(std::round(strip_h_on_screen(entry)));
	wlr_box below{box.x, box.y + strip_h, box.width, box.height - strip_h};
	ColourBox found = s != nullptr
		? find_colour(s, below, rgb, colour_tune) : ColourBox{};
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return found;
}

// The darker strip of the note's card on a screenshot.
ColourBox strip_on_shot(const char *name, const NoteEntry &entry) {
	unsigned char rgb[3], darker[3];
	if (!hex_colour(entry.color, rgb)) {
		return {};
	}
	for (int channel = 0; channel < 3; channel++) {
		// the compositor's strip shade: the card's ink at 85 percent
		darker[channel] = static_cast<unsigned char>(rgb[channel] * 0.85);
	}
	cairo_surface_t *s = load_png(name);
	wlr_box box = note_box_on_screen(entry);
	wlr_box strip{box.x, box.y, box.width,
		static_cast<int>(std::round(note_strip * box.width / double(note_size)))};
	ColourBox found = s != nullptr
		? find_colour(s, strip, darker, colour_tune) : ColourBox{};
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return found;
}

// One colour dot of the strip on a screenshot: the blob of `colour`
// inside the card's strip, where the editor lays its dots out.
ColourBox dot_on_shot(const char *name, const NoteEntry &entry, const char *colour) {
	unsigned char rgb[3];
	if (!hex_colour(colour, rgb)) {
		return {};
	}
	cairo_surface_t *s = load_png(name);
	wlr_box box = note_box_on_screen(entry);
	wlr_box strip{box.x, box.y, box.width,
		static_cast<int>(std::round(strip_h_on_screen(entry)))};
	ColourBox found = s != nullptr
		? find_colour(s, strip, rgb, colour_tune) : ColourBox{};
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return found;
}

// ---- the indicator walk: one phase machine on step 4 ----

// Phase 0 opens the panel, 1 verifies and screenshots it, 2 closes it, 3
// verifies it closed. The typing flow (10-13) runs between 1 and 2; 14 is
// its screenshot-analysis step.
int walk() {
	if (current >= indicators.size()) {
		step = 5; // on to the one-at-a-time check
		return 100;
	}
	Indicator &ind = indicators[current];
	switch (phase) {
	case 0:
		click(ind.x, 13, ind.button);
		phase = 1;
		return 400;
	case 1: {
		LayerSurface *panel = panel_layer(ind.name);
		// Service-backed panels take longer than one tick to build their
		// content: give each a generous window to map.
		if (panel == nullptr && retries++ < 40) {
			return -100;
		}
		retries = 0;
		if (panel == nullptr && ind.button == BTN_RIGHT) {
			// A tray item without a menu secondary-activates instead: by
			// design, nothing to check — but say so.
			std::println("note: the tray item at x={} has no menu of its own; "
				"its panel is not checked", static_cast<int>(ind.x));
			current++;
			return 300;
		}
		check(panel != nullptr, std::format("the {} panel appeared as a layer surface",
			ind.name));
	if (panel != nullptr) {
		check(wl_resource_get_client(panel->wlr->surface->resource) == bar_client(),
			std::format("the {} panel belongs to the bar client", ind.name));
		// The panel's surface holds the keyboard the moment it maps (the
		// seat enters an on-demand layer surface on its own): no click may
		// be needed before its field is typeable, so check before poking.
		if (server->seat->keyboard_state.focused_surface != panel->wlr->surface
				&& retries++ < 20) {
			return -100;
		}
		retries = 0;
		check(server->seat->keyboard_state.focused_surface == panel->wlr->surface,
			std::format("the {} panel holds the keyboard as it opens", ind.name));
		popup_box = card_box(ind.x, ind.width);
		shot(ind.name);
	}
		if (ind.connect_flow) {
			phase = 10;
		} else if (ind.bt_flow) {
			phase = 30;
		} else if (ind.profile_flow) {
			phase = 40;
		} else {
			phase = 2;
		}
		return 700; // let grim finish (handle_timer waits for it)
	}
	case 10: // re-shoot the panel: the rows settle only once the scanner runs
		shot("network");
		phase = 14;
		return 700;
	case 14: { // select the stand-in's unknown secured network: row 0
		cairo_surface_t *panel = load_png("network");
		std::vector<Band> bands = text_bands(panel, popup_box);
		cairo_surface_destroy(panel);
		// Wi-Fi switch row, "Networks" header, then the three stand-in
		// networks — strongest first, so band 2 is CafeGuest, the secured
		// unknown one that asks for a password.
		if (bands.size() < 5 && retries++ < 20) {
			phase = 10; // rows not on screen yet
			return 400;
		}
		retries = 0;
		check(bands.size() >= 5, std::format("the network panel lists the "
			"stand-in's three networks ({} bands: switch row, header, 3 rows)",
			bands.size()));
		if (bands.size() < 5) {
			phase = 2; // nothing was clicked: close without poking the panel
			return 300;
		}
		click(popup_box.x + popup_box.width / 2.0, bands[2].centre());
		phase = 15; // the shot must come a tick later: the panel renders the
		// selection only after the click was delivered
		return 400;
	}
	case 15:
		shot("network-selected");
		phase = 11;
		return 300;
	case 11: { // focus the field and type into it — never Connect
		check(keyboard_on_bar(), "the keyboard is on the bar client while "
			"the network panel is open");
		// Selecting appends the SSID header, the password field and the
		// Connect/Disconnect row below the list. The field is the second
		// band from the bottom; the bottom band — Connect or Disconnect —
		// is never clicked, and nothing is ever submitted.
		cairo_surface_t *panel = load_png("network-selected");
		std::vector<Band> bands = text_bands(panel, popup_box);
		cairo_surface_destroy(panel);
		if (bands.size() < 8 && retries++ < 15) { // the selection UI settles
			phase = 15;
			return 400;
		}
		retries = 0;
		check(bands.size() >= 8, std::format("selecting the network armed "
			"its password field ({} bands: 5 above, header, field, connect)",
			bands.size()));
		if (bands.size() < 8) {
			phase = 2;
			return 300;
		}
		const Band &field = bands[bands.size() - 2];
		field_box = {popup_box.x, field.top, popup_box.width, field.bottom - field.top};
		click(popup_box.x + popup_box.width / 2.0, field.centre());
		phase = 16; // type only once the click has focused the field
		return 400;
	}
	case 16:
		type("correctbattery"); // never Enter: nothing is submitted
		phase = 12;
		return 400;
	case 12:
		shot("network-typed");
		phase = 13;
		return 400;
	case 13: { // the dots must be on screen: keystrokes reached the field
		cairo_surface_t *typed = load_png("network-typed");
		wlr_box rect{
			field_box.x, field_box.y - 10,
			field_box.width, field_box.height + 20,
		};
		int dots = bright_count(typed, rect);
		cairo_surface_destroy(typed);
		if (dots < 80 && retries++ < 10) { // the panel may render the dots late
			phase = 12;
			return 400;
		}
		// each of the 14 dots lights ~9 px; a bare cursor lights under 40
		check(dots >= 80, std::format("typing reached the Wi-Fi password "
			"field ({} bright pixels in it; a bare cursor is under 40)", dots));
		phase = 17; // on to Connect: the flow continues against the mock
		return 300;
	}
	case 17: { // submit the typed password: Connect is the bottom band —
		// but only once the panel has stopped changing, and exactly once
		if (!settle_advance("network-typed")) {
			return 400; // the panel is still settling under load
		}
		std::vector<Band> bands = panel_bands("network-typed");
		if (bands.size() < 8) { // the panel moved on without us
			phase = 2;
			return 300;
		}
		click(popup_box.x + popup_box.width / 2.0, bands.back().centre());
		phase = 18;
		return 400;
	}
	case 18: { // the mock NetworkManager received the activation with the PSK
		bool sent = mock_called(NM_NAME, NM_MANAGER, "AddAndActivateConnection",
			"correctbattery");
		if (!sent && retries++ < 150) { // in flight under load, not lost
			return 400;
		}
		retries = 0;
		check(sent, sent ? "Connect sent CafeGuest's activation with the "
			"typed password to the stand-in NetworkManager"
			: "the Connect click was lost under load");
		shot("network-connected");
		phase = 19;
		return 400;
	}
	case 19: { // pick HomeNet, the connected network: the second row
		std::vector<Band> bands = panel_bands("network-connected");
		if (bands.size() < 5 && retries++ < 10) {
			shot("network-connected");
			return 400;
		}
		retries = 0;
		if (bands.size() < 5) {
			phase = 2;
			return 300;
		}
		click(popup_box.x + popup_box.width / 2.0, bands[3].centre());
		phase = 20;
		return 400;
	}
	case 20:
		shot("network-home");
		phase = 21;
		return 400;
	case 21: { // a connected network's action row is Disconnect
		if (!settle_advance("network-home")) {
			return 400; // the panel is still settling under load
		}
		std::vector<Band> bands = panel_bands("network-home");
		if (bands.size() < 5) { // the panel moved on without us
			phase = 2;
			return 300;
		}
		click(popup_box.x + popup_box.width / 2.0, bands.back().centre());
		phase = 22;
		return 400;
	}
	case 22: {
		bool dropped = mock_called(NM_NAME, NM_DEVICE, "Disconnect");
		if (!dropped && retries++ < 150) { // in flight under load, not lost
			return 400;
		}
		retries = 0;
		check(dropped, dropped ? "Disconnect deactivated the connection on "
			"the stand-in's Wi-Fi device"
			: "the Disconnect click was lost under load");
		phase = 2;
		return 300;
	}
	case 30: // the bluetooth panel: its device rows settle after the scan
		shot("bluetooth-devices");
		phase = 31;
		return 400;
	case 31: { // the paired device's row: Scenario Headphones, the first one
		if (!settle_advance("bluetooth-devices")) {
			return 400; // the rows are still settling after the scan
		}
		std::vector<Band> rows = panel_rows("bluetooth-devices");
		retries = 0;
		check(rows.size() >= 4, std::format("the bluetooth panel lists the "
			"stand-in's two devices ({} rows: switch, scan, 2 devices)",
			rows.size()));
		if (rows.size() < 4) {
			phase = 2;
			return 300;
		}
		// known devices come first, so the paired one is the second row
		// from the bottom; the found one is last
		click(popup_box.x + popup_box.width / 2.0, rows[rows.size() - 2].centre());
		phase = 32;
		return 400;
	}
	case 32: { // Connect went to the mock device; the row reads Connected
		bool connected = mock_property(BT_NAME, BT_HEADPHONES, "org.bluez.Device1",
			"Connected").find("true") != std::string::npos;
		bool called = mock_called(BT_NAME, BT_HEADPHONES, "Connect");
		if (!(called && connected) && retries++ < 150) { // in flight, not lost
			return 400;
		}
		retries = 0;
		check(called && connected, called || connected
			? "clicking the paired device connected it on the stand-in "
			"(Connect called, Connected reported)"
			: "the paired-device click was lost under load");
		shot("bluetooth-connected");
		phase = 33;
		return 400;
	}
	case 33: { // the same row again: a connected device disconnects
		if (!settle_advance("bluetooth-connected")) {
			return 400; // the row is still settling after the connect
		}
		std::vector<Band> rows = panel_rows("bluetooth-connected");
		if (rows.size() < 4) { // the panel moved on without us
			phase = 2;
			return 300;
		}
		click(popup_box.x + popup_box.width / 2.0, rows[rows.size() - 2].centre());
		phase = 34;
		return 400;
	}
	case 34: {
		bool off = mock_called(BT_NAME, BT_HEADPHONES, "Disconnect")
			&& mock_property(BT_NAME, BT_HEADPHONES, "org.bluez.Device1",
				"Connected").find("false") != std::string::npos;
		if (!off && retries++ < 150) { // in flight under load, not lost
			return 400;
		}
		retries = 0;
		check(off, off ? "clicking the connected device disconnected it on "
			"the stand-in (Disconnect called, Connected false)"
			: "the disconnect click was lost under load");
		shot("bluetooth-after");
		phase = 35;
		return 400;
	}
	case 35: { // the found device's row: Scenario Speaker, the last one
		if (!settle_advance("bluetooth-after")) {
			return 400; // the rows are still settling after the scan
		}
		std::vector<Band> rows = panel_rows("bluetooth-after");
		if (rows.size() < 4) { // the panel moved on without us
			phase = 2;
			return 300;
		}
		click(popup_box.x + popup_box.width / 2.0, rows[rows.size() - 1].centre());
		phase = 36;
		return 400;
	}
	case 36: {
		bool paired = mock_called(BT_NAME, BT_SPEAKER, "Pair");
		if (!paired && retries++ < 150) { // in flight under load, not lost
			return 400;
		}
		retries = 0;
		check(paired, paired ? "clicking the found device called Pair on "
			"the stand-in" : "the found-device click was lost under load");
		phase = 2;
		return 300;
	}
	case 40: // the power panel: the profile rows settle
		shot("power-profiles");
		phase = 41;
		return 400;
	case 41: { // "Power saver" is the second row: the battery block above it
		// and nothing below it is ever clicked — the Session rows (Suspend,
		// Restart, Power off, Log out) sit below the other two profiles.
		if (!settle_advance("power-profiles")) {
			return 400; // the rows are still settling under load
		}
		std::vector<Band> rows = panel_rows("power-profiles");
		if (rows.size() < 8) { // the panel moved on without us
			phase = 2;
			return 300;
		}
		// rows: battery, Power saver, Balanced, Performance, 4 session rows
		click(popup_box.x + popup_box.width / 2.0, rows[1].centre());
		phase = 42;
		return 400;
	}
	case 42: {
		std::string profile = mock_property(PPD_NAME, PPD_PATH, PPD_IFACE, "ActiveProfile");
		if (profile.find("power-saver") == std::string::npos && retries++ < 150) {
			return 400; // in flight under load, not lost
		}
		retries = 0;
		check(profile.find("power-saver") != std::string::npos,
			"clicking Power saver made the stand-in's active profile power-saver");
		phase = 2;
		return 300;
	}
	case 2:
		switch (ind.close_mode) {
		case close_escape:
			tap(KEY_ESC);
			break;
		case close_icon:
			click(ind.x, 13, ind.button);
			break;
		default:
			click(60.0, 700.0); // corner of the output, outside every card
			break;
		}
		phase = 3;
		retries = 0;
		return 400;
	default:
		if (panel_layers() > 0 && retries++ < 20) {
			return -100;
		}
		check(panel_layers() == 0, std::format("the {} panel closed ({})", ind.name,
			ind.close_mode == close_escape ? "Escape"
				: ind.close_mode == close_icon ? "a second click of its icon"
				: "a click outside"));
		current++;
		phase = 0;
		retries = 0;
		return 300;
	}
}

int run_step() {
	switch (step) {
	case 0:
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		return 300;
	case 1: {
		if (bar_layer() == nullptr && retries++ < 150) {
			return -100;
		}
		check(bar_layer() != nullptr, "the bar client mapped its layer surface");
		// The isolation proof: what the bar process itself carries. A
		// journal check after the run (`journalctl -u NetworkManager -u
		// bluetooth --since <run start>`) confirms no bar traffic reached
		// the machine's services.
		pid_t bar_pid = 0;
		if (bar_client() != nullptr) {
			wl_client_get_credentials(bar_client(), &bar_pid, nullptr, nullptr);
		}
		std::string sys = proc_env_value(bar_pid, "DBUS_SYSTEM_BUS_ADDRESS");
		std::string ses = proc_env_value(bar_pid, "DBUS_SESSION_BUS_ADDRESS");
		std::println("note: the bar (pid {}) runs with DBUS_SYSTEM_BUS_ADDRESS={} "
			"DBUS_SESSION_BUS_ADDRESS={}", bar_pid, sys, ses);
		check(sys == system_bus.address && ses == session_bus.address,
			"the bar's bus addresses are the private pair (from /proc/environ)");
		// A weather fetch (5 s max) decides whether its widget shows at all;
		// give it room so the indicator positions are final before reading.
		return 15000;
	}
	case 2:
		shot("bar");
		return 500;
	case 3: {
		cairo_surface_t *s = load_png("bar");
		LayerSurface *layer = bar_layer();
		std::vector<Run> runs = s != nullptr && layer != nullptr
			? bar_runs(s, layer->geo) : std::vector<Run>{};
		cairo_surface_destroy(s);

		// Which indicators the bar shows, right to left, in the order the
		// right section lays them out. The stand-ins' fixed world carries a
		// battery, a Wi-Fi device and a Bluetooth adapter; only the
		// backlight comes from this machine.
		std::vector<Indicator> mandatory;
		mandatory.push_back({"power", power_panel});
		if (has_sysfs_class("backlight")) {
			mandatory.push_back({"brightness", brightness_panel});
		}
		mandatory.push_back({"audio", audio_panel});
		mandatory.push_back({"network", network_panel});
		mandatory.push_back({"bluetooth", bluetooth_panel});

		// The logo is the leftmost run. drop it before matching the rest.
		if (!runs.empty()) {
			runs.erase(runs.begin());
		}
		check(runs.size() >= 1 + mandatory.size(),
			std::format("every indicator of this machine is on the bar "
				"({} runs past the logo; the clock and {} indicators)",
				runs.size(), mandatory.size()));
		if (runs.size() < 1 + mandatory.size()) {
			step = 99; // positions cannot be trusted; nothing more to check
			return 300;
		}

		// The right section's indicators are the rightmost runs: `mandatory`
		// lists them right to left, so the i-th counts from the right edge.
		std::size_t keep = runs.size() - mandatory.size();
		std::size_t first_mandatory = 0;
		for (std::size_t i = 0; i < mandatory.size(); i++) {
			Indicator &ind = mandatory[i];
			ind.x = runs[runs.size() - 1 - i].centre();
			if (std::strcmp(ind.name, "network") == 0) {
				ind.connect_flow = true;
				network_x = ind.x;
			}
			if (std::strcmp(ind.name, "bluetooth") == 0) {
				ind.bt_flow = true;
			}
			if (std::strcmp(ind.name, "power") == 0) {
				ind.profile_flow = true;
			}
		}

		// Between logo and the indicators: the tray items (right-click
		// menus), then the centred clock and weather (left to right).
		std::vector<double> centre;
		for (std::size_t i = 0; i < keep; i++) {
			double x = runs[i].centre();
			if (std::abs(x - screen_w / 2.0) <= 250) {
				centre.push_back(x);
			} else {
				indicators.push_back({"tray", tray_panel, x, BTN_RIGHT, close_outside});
			}
		}
		check(!centre.empty(), "the clock widget is on the bar");
		std::size_t clock_index = indicators.size();
		if (!centre.empty()) {
			clock_x = centre.front();
			indicators.push_back({"clock", clock_panel, clock_x, BTN_LEFT, close_outside});
			if (centre.size() > 1) {
				indicators.push_back({"weather", weather_panel, centre.back()});
			} else {
				std::println("note: no weather run (no successful fetch yet) — "
					"the widget hides until one; its panel is not checked");
			}
		}
		first_mandatory = indicators.size();
		for (const Indicator &ind : mandatory) {
			indicators.push_back(ind);
		}

		// Spread the close styles over panels that exist: the rightmost
		// indicator proves the second-click close, another proves Escape.
		if (!mandatory.empty()) {
			indicators[first_mandatory].close_mode = close_icon;
		}
		bool escape_set = false;
		if (mandatory.size() > 1) {
			indicators.back().close_mode = close_escape;
			escape_set = true;
		}
		if (!escape_set && clock_index < indicators.size()) {
			indicators[clock_index].close_mode = close_escape;
		}

		check(network_x > 0, "the network indicator is present (the typing "
			"check needs its panel)");
		return 300; // step advances to the walk
	}
	case 4:
		return walk();
	case 5: // one panel at a time: opening a second closes the first
		if (clock_x > 0 && network_x > 0) {
			click(clock_x, 13);
			return 400;
		}
		step = 99;
		return 100;
	case 6: {
		if (panel_layer("clock") == nullptr && retries++ < 20) {
			return -100;
		}
		check(panel_layer("clock") != nullptr, "the clock panel opened");
		click(network_x, 13);
		return 400;
	}
	case 7: {
		if (!(panel_layer("network") != nullptr && panel_layer("clock") == nullptr)
				&& retries++ < 40) {
			return -100;
		}
		check(panel_layer("network") != nullptr && panel_layer("clock") == nullptr,
			"opening the network panel closed the clock panel (one at a time)");
		shot("one-at-a-time");
		return 700;
	}
	case 8:
		click(60.0, 700.0);
		return 400;
	case 9:
		if (panel_layers() > 0 && retries++ < 20) {
			return -100;
		}
		check(panel_layers() == 0, "the last panel closed on a click outside");
		return 100; // on to the plugin world: the bar as a plugin host
	case 10: // the plain bar, before anything is added to it
		shot("bar-plain");
		return 700;
	case 11: {
		// Every run on the bar, accounted for: the logo, the clock (and the
		// weather when it loaded) centred, the stand-in's one tray item, and
		// this machine's indicators — nothing left over, so the broken
		// manifest's plugin contributed nothing and was skipped.
		cairo_surface_t *s = load_png("bar-plain");
		LayerSurface *layer = bar_layer();
		std::vector<Run> runs = s != nullptr && layer != nullptr
			? bar_runs(s, layer->geo) : std::vector<Run>{};
		cairo_surface_destroy(s);
		check(layer != nullptr && runs.size() >= 1, "the bar is still mapped after the walk");
		std::size_t mandatory = 4 + (has_sysfs_class("backlight") ? 1 : 0);
		std::size_t keep = runs.size() >= 1 + mandatory ? runs.size() - 1 - mandatory : 0;
		std::size_t centre = 0;
		for (std::size_t i = 1; i < 1 + keep; i++) {
			if (std::abs(runs[i].centre() - screen_w / 2.0) <= 250) {
				centre++;
			}
		}
		base_runs = runs.size();
		check(keep - centre == 1, std::format("every run on the bar is accounted for "
			"({} runs: logo, {} centred, 1 tray, {} indicators — the broken "
			"manifest's plugin was skipped)", runs.size(), centre, mandatory));
		// Drop the user plugin's staged files into the scratch data dir: the
		// shell reloads with them, without anything of the shell being touched.
		mkdir_p(user_plugin);
		std::rename((staged_plugin + "/plugin.json").c_str(),
			(user_plugin + "/plugin.json").c_str());
		std::rename((staged_plugin + "/Hello.qml").c_str(),
			(user_plugin + "/Hello.qml").c_str());
		return 500;
	}
	case 12: // the shell reloads with the dropped-in plugin
		shot("bar-user");
		return 700;
	case 13: {
		cairo_surface_t *s = load_png("bar-user");
		LayerSurface *layer = bar_layer();
		std::vector<Run> runs = s != nullptr && layer != nullptr
			? bar_runs(s, layer->geo) : std::vector<Run>{};
		cairo_surface_destroy(s);
		bool appeared = runs.size() == base_runs + 1 && runs.size() >= 2
			&& runs[1].x1 < 300; // the new run sits beside the logo, left of centre
		// A reload takes its own remap, and the walk's own retry counter
		// is reset on every step move, so this phase keeps its own:
		// re-shoot and look again until the widget shows (or gives up).
		if (!appeared && plugin_retries++ < 40) {
			shot("bar-user");
			step = 12; // stay on this check (handle_timer steps forward again)
			return 700;
		}
		check(appeared, std::format("the user plugin's bar widget appeared after it "
			"was dropped into the data dir ({} runs, was {})", runs.size(), base_runs));
		user_x1 = runs.size() >= 2 ? runs[1].x1 : 0;
		// Edit the loaded plugin's own QML while the shell runs: the
		// shell reloads with it — the widget becomes a labelled one.
		write_file(user_plugin + "/Hello.qml",
			"import QtQuick\nimport WaylandCanvas.Ui\nBarButton {\n"
			"\ttext: \"HELLO SHELL\"\n}\n");
		return 500;
	}
	case 14: // the shell reloads with the edited plugin
		plugin_retries = 0;
		shot("bar-wide");
		return 700;
	case 15: {
		cairo_surface_t *s = load_png("bar-wide");
		LayerSurface *layer = bar_layer();
		std::vector<Run> runs = s != nullptr && layer != nullptr
			? bar_runs(s, layer->geo) : std::vector<Run>{};
		cairo_surface_destroy(s);
		// The widget may now merge into the logo's run, so what is
		// measured is the left section's right edge — the rightmost lit
		// pixel short of the centred widgets — which the wider widget
		// must have pushed right.
		double left_right = 0;
		for (std::size_t i = 0; i < runs.size(); i++) {
			if (runs[i].x1 < screen_w / 2.0 - 250 && runs[i].x1 > left_right) {
				left_right = runs[i].x1;
			}
		}
		bool widened = layer != nullptr && left_right >= user_x1 + widen_px;
		if (!widened && plugin_retries++ < 40) {
			shot("bar-wide");
			step = 14;
			return 700;
		}
		check(widened, std::format("editing the loaded plugin's QML file while the "
			"shell runs changed the bar (the left section's right edge {} to {} px)",
			user_x1, left_right));
		// Disable the plugin in the scratch config: the shell reloads
		// without it.
		write_file(shot_dir + "/xdg/config/wayland-canvas/shell.json",
			"{\n\t\"plugins\": {\n\t\t\"canvas.hello\": {\n\t\t\t\"enabled\": false\n"
			"\t\t}\n\t}\n}\n");
		return 500;
	}
	case 16: // the shell reloads again, without the disabled plugin
		plugin_retries = 0;
		shot("bar-disabled");
		return 700;
	case 17: {
		cairo_surface_t *s = load_png("bar-disabled");
		LayerSurface *layer = bar_layer();
		std::vector<Run> runs = s != nullptr && layer != nullptr
			? bar_runs(s, layer->geo) : std::vector<Run>{};
		cairo_surface_destroy(s);
		bool gone = runs.size() == base_runs
			&& (runs.size() < 2 || runs[1].x1 >= 300); // no run beside the logo
		if (!gone && plugin_retries++ < 40) {
			shot("bar-disabled");
			step = 16;
			return 700;
		}
		check(gone, std::format("disabling the user plugin in the config removed its "
			"widget ({} runs, was {} after the drop-in)", runs.size(), base_runs + 1));
		// Then the idle watch: the reloads above were change
		// notifications, so a shell that just sits there must spawn
		// nothing — no watcher poll, ever.
		bar_pid = 0;
		if (bar_client() != nullptr) {
			wl_client_get_credentials(bar_client(), &bar_pid, nullptr, nullptr);
		}
		idle_ticks = idle_sample_count;
		idle_spawned = false;
		return idle_sample_ms;
	}
	case 18: // every few milliseconds, one look at the shell's children
		if (proc_has_children(bar_pid)) {
			idle_spawned = true;
		}
		if (idle_ticks-- > 0) {
			return -idle_sample_ms; // same step again: the watch continues
		}
		check(!idle_spawned && bar_pid > 0, std::format("an idle shell spawned no "
			"processes ({} samples over {} s)", idle_sample_count,
			idle_sample_count * idle_sample_ms / 1000));
		step = 20; // on to the overview: shell surfaces take input over it
		return -100; // without the advance: every other step moves on alone
	// ---- the overview: shell surfaces take input over it ----
	// A step range of its own, after the walk and the plugin world.
	case 20: // Super+Tab, the overview's chord, over the bar's world
		key(KEY_LEFTMETA, true);
		key(KEY_TAB, true);
		key(KEY_TAB, false);
		key(KEY_LEFTMETA, false);
		return 400;
	case 21: {
		Output &o = *server->outputs.front();
		check(o.overview.active(), "Super+Tab opened the overview over the bar");
		shot("overview");
		return 400;
	}
	case 22: // a click on the bar's own background: not the logo, which
		// sends launcher.toggle over the control socket
		click(300.0, 13);
		return 400;
	case 23:
		check(server->outputs.front()->overview.active(),
			"a click on the bar did not leave the overview");
		return 300;
	case 24: // the network indicator opens its panel inside the overview
		click(network_x, 13);
		return 400;
	case 25: { // then a click outside the card closes it
		LayerSurface *panel = panel_layer("network");
		if (panel == nullptr && retries++ < 150) { // in flight under load
			return -100;
		}
		retries = 0;
		check(panel != nullptr, "the bar opens a panel inside the overview");
		if (panel == nullptr) {
			step = 99;
			return 100;
		}
		shot("overview-network-first");
		click(60.0, 700.0); // the panel's overlay takes it, not the canvas
		return 400;
	}
	case 26: { // and the same indicator opens it again: a panel that closed
		// itself is forgotten as the open one, so the next click reopens it
		if (panel_layers() > 0 && retries++ < 40) {
			return -100;
		}
		retries = 0;
		check(panel_layers() == 0 && server->outputs.front()->overview.active(),
			"a click outside closed the panel and the overview stayed");
		click(network_x, 13);
		return 400;
	}
	case 27: {
		LayerSurface *panel = panel_layer("network");
		if (panel == nullptr && retries++ < 150) { // in flight under load
			return -100;
		}
		retries = 0;
		check(panel != nullptr, "the network indicator reopened its panel after "
			"the panel closed itself");
		if (panel == nullptr) {
			step = 99;
			return 100;
		}
		check(wl_resource_get_client(panel->wlr->surface->resource) == bar_client(),
			"the overview's network panel belongs to the bar client");
		if (server->seat->keyboard_state.focused_surface != panel->wlr->surface
				&& retries++ < 20) {
			return -100;
		}
		retries = 0;
		check(server->seat->keyboard_state.focused_surface == panel->wlr->surface,
			"the panel holds the keyboard while the overview is active");
		popup_box = card_box(network_x, network_panel);
		shot("overview-network");
		return 700;
	}
	case 28: { // select CafeGuest: the first row, band 2 on the raw scan
		if (!settle_advance("overview-network")) {
			return -400; // the rows are still settling under load
		}
		std::vector<Band> bands = panel_bands("overview-network");
		if (bands.size() < 5) { // the panel moved on without us
			step = 99;
			return 100;
		}
		check(bands.size() >= 5,
			"the panel lists the stand-in's networks in the overview");
		click(popup_box.x + popup_box.width / 2.0, bands[2].centre());
		return 500;
	}
	case 29: // the shot must come a tick after the selection click
		shot("overview-selected");
		return 400;
	case 30: {
		if (!settle_advance("overview-selected")) {
			return -400; // the panel is still settling under load
		}
		std::vector<Band> bands = panel_bands("overview-selected");
		if (bands.size() < 8) { // the panel moved on without us
			step = 99;
			return 100;
		}
		check(bands.size() >= 8, "selecting the network armed its password "
			"field in the overview");
		const Band &field = bands[bands.size() - 2];
		field_box = {popup_box.x, field.top, popup_box.width, field.bottom - field.top};
		click(popup_box.x + popup_box.width / 2.0, field.centre());
		return 500;
	}
	case 31: // type only once the click has focused the field
		type("correctbattery"); // never Enter: nothing is submitted
		return 400;
	case 32:
		shot("overview-typed");
		return 400;
	case 33: {
		cairo_surface_t *typed = load_png("overview-typed");
		wlr_box rect{
			field_box.x, field_box.y - 10,
			field_box.width, field_box.height + 20,
		};
		int dots = bright_count(typed, rect);
		cairo_surface_destroy(typed);
		if (dots < 80 && retries++ < 10) { // the panel may render the dots late
			shot("overview-typed");
			return -400;
		}
		retries = 0;
		// each of the 14 dots lights ~9 px; a bare cursor lights under 40
		check(dots >= 80, std::format("typing reached the Wi-Fi password "
			"field inside the overview ({} bright pixels in it)", dots));
		return 300;
	}
	case 34: // the panel holds the keyboard: it takes the Escape
		tap(KEY_ESC);
		return 400;
	case 35: {
		if (panel_layers() > 0 && retries++ < 20) {
			return -100;
		}
		retries = 0;
		check(panel_layers() == 0,
			"Escape closed the panel while the overview stayed");
		check(server->outputs.front()->overview.active(),
			"the panel took Escape: the overview is still active");
		return 300;
	}
	case 36: // with the panel gone, the overview hears the next Escape
		tap(KEY_ESC);
		return 400;
	case 37: {
		Output &o = *server->outputs.front();
		if (o.overview.active() && retries++ < 20) {
			return -100;
		}
		check(!o.overview.active(), "the second Escape left the overview");
		step = 110; // on to the notes: the sticky-notes plugin end to end
		return -100; // without the advance: every other step moves on alone
	}
	// ---- the notes: the sticky-notes plugin end to end ----
	// A step range of its own, after the overview.
	case 110: // a note created the way any script would: action.run over the socket
		run_msg("action.run",
			R"({"name":"canvas.notes/create","args":{"color":")" + std::string{note_yellow}
				+ R"("}})",
			"note-create");
		return 400;
	case 111: {
		sweep_children();
		if (!msg_done("note-create") && note_retries++ < 60) {
			return -300;
		}
		note_retries = 0;
		check(msg_code("note-create") == 0 && msg_out("note-create") == "true",
			std::format("`msg action.run` canvas.notes/create answered true and "
				"exited 0 ({})", msg_err("note-create")));
		run_msg("state", "", "note-state");
		return 400;
	}
	case 112: { // the item in the state, the editor mapped over it
		sweep_children();
		if (!msg_done("note-state") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-state");
		if (notes.empty() && note_retries++ < 80) {
			run_msg("state", "", "note-state"); // the note may not have landed yet
			return -400;
		}
		note_retries = 0;
		check(!notes.empty(), "`msg action.run` create put a canvas.notes/... "
			"item in the state");
		if (notes.empty()) {
			step = 99; // nothing to test further
			return 100;
		}
		note = notes.front();
		check(note.color == note_yellow && note.width == note_size
				&& note.height == note_size && note.text.empty(),
			std::format("the new note is {} in its settings' size ({}x{} at {},{})",
				note.color.empty() ? "of no colour" : note.color.c_str(),
				note.width, note.height, static_cast<int>(note.x),
				static_cast<int>(note.y)));
		LayerSurface *editor = editor_layer();
		if (editor == nullptr && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		check(editor != nullptr, "the editor mapped as a layer surface");
		if (editor != nullptr) {
			check(wl_resource_get_client(editor->wlr->surface->resource) == bar_client(),
				"the editor belongs to the shell client");
			if (!editor_open_with_keyboard() && note_retries++ < 50) {
				return -100;
			}
			note_retries = 0;
			check(editor_open_with_keyboard(),
				"the editor holds the keyboard on its own surface as it opens");
		}
		shot("note-editor");
		return 700;
	}
	case 113: { // the editor's card, exactly at the note, in its colour
		wlr_box box = note_box_on_screen(note);
		int strip_h = static_cast<int>(std::round(strip_h_on_screen(note)));
		ColourBox card = card_on_shot("note-editor", note);
		ColourBox strip = strip_on_shot("note-editor", note);
		check(card.count > 1000 && std::abs(card.x0 - box.x) <= edge_tune
				&& std::abs(card.y0 - (box.y + strip_h)) <= edge_tune
				&& std::abs(card.w() - box.width) <= edge_tune
				&& std::abs(card.h() - (box.height - strip_h)) <= edge_tune,
			std::format("the editor's card sits exactly at the note in its colour "
				"({}x{} at {},{}; want {}x{} at {},{})", card.w(), card.h(), card.x0,
				card.y0, box.width, box.height - strip_h, box.x, box.y + strip_h));
		check(strip.count > 200 && std::abs(strip.x0 - box.x) <= edge_tune
				&& std::abs(strip.y0 - box.y) <= edge_tune
				&& std::abs(strip.w() - box.width) <= edge_tune
				&& std::abs(strip.h() - strip_h) <= edge_tune,
			std::format("the darker strip runs along the card's top ({}x{} at "
				"{},{}; want {}x{} at {},{})", strip.w(), strip.h(), strip.x0,
				strip.y0, box.width, strip_h, box.x, box.y));
		type("buy oats"); // straight into the field: the editor has the keyboard
		return 400;
	}
	case 114: // close it: Escape keeps the text
		tap(KEY_ESC);
		run_msg("state", "", "note-text");
		return 400;
	case 115: {
		sweep_children();
		if (!msg_done("note-text") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-text");
		if ((notes.size() != 1 || notes.front().text != "buy oats")
				&& note_retries++ < 80) {
			run_msg("state", "", "note-text");
			return -400;
		}
		note_retries = 0;
		check(notes.size() == 1 && notes.front().text == "buy oats",
			std::format("typing then Escape kept the text in the state ({})",
				notes.size() == 1 ? notes.front().text.c_str() : "no note"));
		std::optional<NoteEntry> stored = stored_note(note.id);
		check(stored.has_value() && stored->text == "buy oats"
				&& stored->color == note_yellow && stored->width == note_size,
			std::format("the closed note is in the plugin's storage with its "
				"colour, text and size ({})", stored.has_value() ? "as typed" : "absent"));
		shot("note-view");
		return 700;
	}
	case 116: { // the note on the canvas in the normal view, then dragged
		wlr_box box = note_box_on_screen(note);
		ColourBox card = card_on_shot("note-view", note);
		ColourBox strip = strip_on_shot("note-view", note);
		check(card.count > 1000 && std::abs(card.x0 - box.x) <= edge_tune
				&& std::abs(card.w() - box.width) <= edge_tune,
			std::format("the note draws on the canvas in the normal view "
				"({}x{} at {},{}; want {} wide at {},{})", card.w(), card.h(),
				card.x0, card.y0, box.width, box.x, box.y + note_strip));
		check(strip.count > 200, "the canvas note carries its darker strip");
		drag_note(box.x + box.width / 2.0, box.y + box.height / 2.0,
			box.x + box.width / 2.0 + 120, box.y + box.height / 2.0 + 80);
		run_msg("state", "", "note-drag");
		return 500;
	}
	case 117: { // the dragged place in the state and the plugin's storage
		sweep_children();
		if (!msg_done("note-drag") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-drag");
		if ((notes.size() != 1 || notes.front().x != note.x + 120
				|| notes.front().y != note.y + 80) && note_retries++ < 80) {
			run_msg("state", "", "note-drag");
			return -400;
		}
		note_retries = 0;
		check(notes.size() == 1 && notes.front().x == note.x + 120
				&& notes.front().y == note.y + 80,
			std::format("the fake pointer's drag moved the note to {},{}",
				notes.size() == 1 ? static_cast<int>(notes.front().x) : -1,
				notes.size() == 1 ? static_cast<int>(notes.front().y) : -1));
		note.x += 120;
		note.y += 80;
		std::optional<NoteEntry> stored = stored_note(note.id);
		if ((!stored.has_value() || stored->x != note.x || stored->y != note.y)
				&& note_retries++ < 40) {
			return -400; // the storage write lands with the event
		}
		note_retries = 0;
		check(stored.has_value() && stored->x == note.x && stored->y == note.y,
			std::format("the dragged place is in the plugin's storage ({})",
				stored.has_value() ? "as dragged" : "absent"));
		// An edit to the scratch config, as-is: the shell reloads with it.
		bar_before_reload = bar_layer();
		write_file(shot_dir + "/xdg/config/wayland-canvas/shell.json", plugin_config);
		return 800;
	}
	case 118: { // the shell reloaded: a new bar surface of the same client
		if ((bar_layer() == nullptr || bar_layer() == bar_before_reload)
				&& note_retries++ < 100) {
			return -300;
		}
		note_retries = 0;
		check(bar_layer() != nullptr && bar_layer() != bar_before_reload,
			"the shell reloaded on the config edit (a fresh bar surface)");
		run_msg("state", "", "note-reload");
		return 1500; // let the reloading plugin put its notes back
	}
	case 119: { // the same one note, same place, same text: nothing lost or doubled
		sweep_children();
		if (!msg_done("note-reload") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-reload");
		if ((notes.size() != 1 || notes.front().id != note.id) && note_retries++ < 80) {
			run_msg("state", "", "note-reload");
			return -400;
		}
		note_retries = 0;
		check(notes.size() == 1 && notes.front().id == note.id
				&& notes.front().x == note.x && notes.front().y == note.y
				&& notes.front().text == "buy oats",
			std::format("a shell reload kept exactly the one note, place and "
				"text ({} canvas.notes items)", notes.size()));
		// The stronger proof: take the note off the compositor, reload
		// again — only the plugin's re-send from storage can bring it back.
		run_msg("item.remove", std::format(R"({{"id":"{}"}})", note.id), "note-remove");
		return 400;
	}
	case 120: { // gone from the compositor; then another reload
		sweep_children();
		if (!msg_done("note-remove") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-remove");
		if (!notes.empty() && note_retries++ < 80) {
			run_msg("state", "", "note-remove");
			return -400;
		}
		note_retries = 0;
		check(notes.empty(), "`msg item.remove` took the note off the canvas");
		bar_before_reload = bar_layer();
		write_file(shot_dir + "/xdg/config/wayland-canvas/shell.json", plugin_config);
		return 800;
	}
	case 121: // the second reload, quiet: on to the re-send proof
		if ((bar_layer() == nullptr || bar_layer() == bar_before_reload)
				&& note_retries++ < 100) {
			return -300;
		}
		note_retries = 0;
		run_msg("state", "", "note-back");
		return 1500;
	case 122: { // the stored note, re-sent by the reloading plugin
		sweep_children();
		if (!msg_done("note-back") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-back");
		if ((notes.size() != 1 || notes.front().id != note.id) && note_retries++ < 80) {
			run_msg("state", "", "note-back");
			return -400;
		}
		note_retries = 0;
		check(notes.size() == 1 && notes.front().id == note.id
				&& notes.front().x == note.x && notes.front().y == note.y,
			std::format("the reloading shell put the stored note back on the "
				"canvas, place and all ({} canvas.notes items)", notes.size()));
		run_msg("overview.toggle", "", "overview-open");
		return 400;
	}
	case 123: { // the overview settles before anything is read off the screen
		if (!overview_settled(true) && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		shot("note-overview");
		return 700;
	}
	case 124: { // the note in the overview, then a double-click on it
		wlr_box box = note_box_on_screen(note);
		ColourBox card = card_on_shot("note-overview", note);
		ColourBox strip = strip_on_shot("note-overview", note);
		check(card.count > 500 && std::abs(card.x0 - box.x) <= edge_tune + 2
				&& std::abs(card.y0 - (box.y + box.height * note_strip / double(note_size)))
					<= edge_tune + 2
				&& std::abs(card.w() - box.width) <= edge_tune + 2,
			std::format("the note shows in the overview where it was put "
				"({}x{} at {},{}; want {} wide at {},{})", card.w(), card.h(),
				card.x0, card.y0, box.width, box.x,
				static_cast<int>(box.y + box.height * note_strip / double(note_size))));
		check(strip.count > 30, "the overview note carries its darker strip");
		double_click(box.x + box.width / 2.0, box.y + box.height / 2.0);
		return 400;
	}
	case 125: { // the overview's double-click opened the editor
		if (!editor_open_with_keyboard() && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		check(editor_open_with_keyboard(),
			"double-clicking the note in the overview opened the editor, "
			"holding the keyboard");
		if (!editor_open_with_keyboard()) {
			step = 99; // nothing further can be trusted
			return 100;
		}
		type(" hi");
		return 400;
	}
	case 126: // close it and ask for the text
		tap(KEY_ESC);
		run_msg("state", "", "note-typed");
		return 400;
	case 127: {
		sweep_children();
		if (!msg_done("note-typed") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-typed");
		if ((notes.size() != 1 || notes.front().text != "buy oats hi")
				&& note_retries++ < 80) {
			run_msg("state", "", "note-typed");
			return -400;
		}
		note_retries = 0;
		check(notes.size() == 1 && notes.front().text == "buy oats hi",
			std::format("typed keys reached the overview's editor (the text is "
				"now '{}')", notes.size() == 1 ? notes.front().text.c_str() : "gone"));
		run_msg("overview.toggle", "", "overview-close");
		return 400;
	}
	case 128: { // back in the normal view, the editor gone, the note clickable
		if ((!overview_settled(false) || editor_layer() != nullptr)
				&& note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		if (editor_layer() != nullptr) {
			check(false, "the overview's editor closed on Escape");
			step = 99;
			return 100;
		}
		wlr_box box = note_box_on_screen(note);
		double_click(box.x + box.width / 2.0, box.y + box.height / 2.0);
		return 400;
	}
	case 129: { // the editor again, over the note in the normal view
		if (!editor_open_with_keyboard() && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		check(editor_open_with_keyboard(),
			"double-clicking the note in the normal view opened the editor");
		shot("note-dots");
		return 700;
	}
	case 130: { // the colour dots on the strip: the green one recolours
		ColourBox dot = dot_on_shot("note-dots", note, note_green);
		check(dot.count >= 30, std::format("the editor's colour dots show on the "
			"strip ({} green pixels)", dot.count));
		if (dot.count < 30) {
			step = 133; // nothing to click; on to closing and the ×
			return -100; // without the advance
		}
		click(dot.cx(), dot.cy());
		run_msg("state", "", "note-recolour");
		return 400;
	}
	case 131: { // recoloured, and a click outside closes without losing it
		sweep_children();
		if (!msg_done("note-recolour") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-recolour");
		if ((notes.size() != 1 || notes.front().color != note_green)
				&& note_retries++ < 80) {
			run_msg("state", "", "note-recolour");
			return -400;
		}
		note_retries = 0;
		note.color = note_green;
		check(notes.size() == 1 && notes.front().color == note_green,
			"a colour dot recoloured the note");
		std::optional<NoteEntry> stored = stored_note(note.id);
		check(stored.has_value() && stored->color == note_green,
			"the new colour is in the plugin's storage");
		click(60.0, 700.0); // a corner of the output, outside the card
		return 400;
	}
	case 132: { // closed by the outside click, then opened once more
		if (editor_layer() != nullptr && note_retries++ < 50) {
			return -100;
		}
		note_retries = 0;
		check(editor_layer() == nullptr, "a click outside closed the editor");
		wlr_box box = note_box_on_screen(note);
		double_click(box.x + box.width / 2.0, box.y + box.height / 2.0);
		return 400;
	}
	case 133: { // the × at the strip's right deletes the note
		if (!editor_open_with_keyboard() && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		if (!editor_open_with_keyboard()) {
			check(false, "the editor reopened for the ×");
			step = 99;
			return 100;
		}
		wlr_box box = note_box_on_screen(note);
		double strip_h = strip_h_on_screen(note);
		click(box.x + box.width - strip_h / 2, box.y + strip_h / 2);
		run_msg("state", "", "note-deleted");
		return 400;
	}
	case 134: { // gone from the canvas and from the storage
		sweep_children();
		if (!msg_done("note-deleted") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-deleted");
		if (!notes.empty() && note_retries++ < 80) {
			run_msg("state", "", "note-deleted");
			return -400;
		}
		note_retries = 0;
		check(notes.empty(), "the × deleted the note from the canvas");
		bool storage_gone = stored_notes().empty();
		check(storage_gone, "the deleted note is gone from the plugin's storage");
		// One more, in another colour, closed empty: it must not survive.
		run_msg("action.run",
			R"({"name":"canvas.notes/create","args":{"color":")" + std::string{note_blue}
				+ R"("}})",
			"note-empty");
		run_msg("state", "", "note-blue");
		return 400;
	}
	case 135: { // created in the second colour, its editor open
		sweep_children();
		if (!msg_done("note-blue") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-blue");
		if ((notes.size() != 1 || notes.front().color != note_blue)
				&& note_retries++ < 80) {
			run_msg("state", "", "note-blue");
			return -400;
		}
		note_retries = 0;
		check(notes.size() == 1 && notes.front().color == note_blue,
			std::format("a second note was created in another settings colour ({})",
				notes.size() == 1 ? notes.front().color.c_str() : "none"));
		if (!editor_open_with_keyboard() && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		check(editor_open_with_keyboard(), "the empty note's editor opened for it");
		return 400;
	}
	case 136: // closed with no text at all
		tap(KEY_ESC);
		run_msg("state", "", "note-removed");
		return 400;
	case 137: {
		sweep_children();
		if (!msg_done("note-removed") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-removed");
		if (!notes.empty() && note_retries++ < 80) {
			run_msg("state", "", "note-removed");
			return -400;
		}
		note_retries = 0;
		check(notes.empty(), "a note closed with no text was removed from the canvas");
		check(stored_notes().empty(), "nothing is left in the plugin's storage");
		return 100; // done: the summary prints and the run ends
	}
	default:
		std::println("{}: {} failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
		wl_display_terminate(server->display);
		return -1;
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
	int before = step;
	int delay = run_step();
	if (delay < -1) {
		wl_event_source_timer_update(timer, -delay);
		return 0;
	}
	if (delay == -1) {
		return 0;
	}
	// The walk manages its own progression inside step 4; every other step
	// moves on when it returns.
	if (before != 4) {
		step++;
		retries = 0;
	}
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0); // PASS/FAIL lines must survive a kill
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp"; // meson passes the repo's tmp/ via env
	shot_dir = std::format("{}/scenarios/bar", root);
	mkdir_p(shot_dir);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	// Isolation first: the bar may only ever start on the private buses
	// with their stand-ins — when either cannot come up, no bar runs at
	// all, and the machine's buses are never touched. The stand-ins and
	// every later child (the bar above all) inherit the addresses.
	if (!start_private_bus(system_bus, "system", "system")
			|| !start_private_bus(session_bus, "session", "session")) {
		std::println("FAILED: the private buses did not start; the bar did not run");
		stop_isolation();
		return 2;
	}
	// Python children must never drop bytecode into the source tree
	setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
	setenv("DBUS_SYSTEM_BUS_ADDRESS", system_bus.address.c_str(), 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", session_bus.address.c_str(), 1);
	if (!start_standins()) {
		std::println("FAILED: the stand-in services did not start; the bar did not run");
		stop_isolation();
		return 2;
	}
	// The plugin world the shell host loads from, on this run's own scratch
	// XDG dirs: the config is read from and written to scratch, user plugins
	// land in scratch data, plugin storage goes to scratch state — nothing
	// on the machine is touched. A broken manifest is there from the start:
	// the shell must skip it (a line in its log, whose effect is checked by
	// the run accounting at step 11) and still come up whole.
	std::string xdg = shot_dir + "/xdg";
	// A previous run's leftovers (a dropped-in plugin, a written config)
	// would leak into this one: the scratch world starts empty.
	std::error_code ec;
	std::filesystem::remove_all(xdg, ec);
	std::string plugins_dir = xdg + "/data/wayland-canvas/plugins";
	mkdir_p(xdg + "/config/wayland-canvas");
	mkdir_p(plugins_dir + "/bad.plugin");
	mkdir_p(xdg + "/state");
	write_file(plugins_dir + "/bad.plugin/plugin.json", "{ not json\n");
	write_file(plugins_dir + "/bad.plugin/Bad.qml",
		"import QtQuick\nimport WaylandCanvas.Ui\nBarButton {\n"
		"\ticon: true\n\ttext: \"\\uf00d\"\n}\n");
	setenv("XDG_CONFIG_HOME", (xdg + "/config").c_str(), 1);
	setenv("XDG_DATA_HOME", (xdg + "/data").c_str(), 1);
	setenv("XDG_STATE_HOME", (xdg + "/state").c_str(), 1);
	// The user plugin this run drops in while the shell runs (step 11): a
	// bar widget beside the logo, first of the left section past it, so it
	// shows as the bar's second run. Its files are staged outside the
	// plugin root until then — dropping them in is what proves the shell
	// reloads with a plugin that appears.
	user_plugin = plugins_dir + "/canvas.hello";
	staged_plugin = xdg + "/staged";
	mkdir_p(staged_plugin);
	write_file(staged_plugin + "/plugin.json",
		"{\n\t\"id\": \"canvas.hello\",\n\t\"name\": \"Hello\",\n"
		"\t\"version\": \"1.0.0\",\n\t\"apiVersion\": 1,\n"
		"\t\"description\": \"A user plugin: a bar widget that says so\",\n"
		"\t\"contributes\": {\n\t\t\"barWidgets\": [\n\t\t\t{\n"
		"\t\t\t\t\"id\": \"hello\",\n\t\t\t\t\"entry\": \"Hello.qml\",\n"
		"\t\t\t\t\"section\": \"left\",\n\t\t\t\t\"order\": 1\n\t\t\t}\n\t\t]\n\t}\n}\n");
	write_file(staged_plugin + "/Hello.qml",
		"import QtQuick\nimport WaylandCanvas.Ui\nBarButton {\n"
		"\ticon: true\n\ttext: \"\\uf120\"\n}\n");

	// The bar is the point of this scenario: it starts with the compositor
	// (qs on PATH and the config beside the binary — the meson guard checked
	// qs, the dbus daemon and the stand-in environment before registering
	// the suite).
	server.emplace();
	Options options;
	if (!server->init(options) || !server->start()) {
		server.reset();
		stop_isolation();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *data) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display);
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	stop_children();
	stop_isolation();
	return failures != 0;
}
