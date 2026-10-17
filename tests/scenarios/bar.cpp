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
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/theme.h"
#include "core/wlr.h"
#include "render/render.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false; // the run reached its end; see main()
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
double theme_x = 0; // the theme switch's centre: the right-most bar run
double power_x = 0; // the power indicator's centre: the theme phase's panel
std::string shell_config; // the scratch shell.json the theme switch writes
std::string scheme_start; // the machine's real color-scheme, read before the run

// ---- the notes phase, over the public control socket ----

// The plugin's settings defaults, kept in sync with
// plugins/canvas.notes/plugin.json (the swatch colours the test creates
// and looks for on screen).
constexpr const char *note_yellow = "#ffd866";
constexpr const char *note_green = "#a6e3a1";
constexpr const char *note_blue = "#7dcfff";
constexpr const char *note_pink = "#f4b8e4";
constexpr const char *note_purple = "#caa9f7";
constexpr const char *note_orange = "#ff9e64";
// The manifest's colour order, the dots' and the menu's (docs/intents/
// shell/notes.md, Recolour): the settings map arrives alphabetised, so
// this array is the order the run checks for.
constexpr const char *note_palette[] = {note_yellow, note_green, note_blue,
	note_pink, note_purple, note_orange};
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
double note_button_y = 0; // the note button's band centre, on the strip
wlr_box note_flyout{}; // its menu's card: left of the strip, centred on it
LayerSurface *bar_before_reload = nullptr;
std::vector<pid_t> msg_children; // `wayland-canvas msg` runs, swept when done
// ---- the agents phase's state (its helpers live by run_step) ----
pid_t usage_standin_pid = -1;
int usage_retries = 0;
double agents_x = 0;  // the widget's run centre, once it shows
double bt_x = 0;      // the bluetooth indicator's run centre, to find it
std::size_t agents_base_runs = 0;
int usage_requests_base = 0; // the stand-in's log lines before the reopen
LayerSurface *agents_bar_before_reload = nullptr;
std::string scratch_home;
wlr_box agents_box{}; // the panel's card on screen


// ---- isolation: private buses and the stand-in services on them ----

struct PrivateBus {
	pid_t pid = -1;
	std::string address; // what DBUS_*_BUS_ADDRESS becomes for the bar
};

PrivateBus private_system, private_session; // the world's own buses
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
constexpr const char *toolbar_namespace = "wayland-canvas-toolbar";

// The shell's own colours the theme phase samples (the UI kit's palette;
// the compositor's half comes from config.h's palettes). Keep in sync with
// shell/ui/WaylandCanvas/Ui/UiTheme.qml.
constexpr uint32_t shell_bg_dark = 0x1a1b26ff;
constexpr uint32_t shell_bg_light = 0xe1e2e7ff;
constexpr uint32_t shell_panel_light = 0xf4f5f8ff;

// Output geometry the headless backend gives: 1280x720, one output.
constexpr int screen_w = 1280, screen_h = 720;

// The toolbar strip's geometry the plugin commits (docs/intents/shell/
// toolbar.md): a 44 px square per button, 8 px from the right edge,
// vertically centred on the output. The flyout and a toolbar-opened
// panel hang a further 8 px left of the strip, and both the scratch
// panel and the flyout are 200 px wide.
constexpr int toolbar_button = 44;
constexpr int toolbar_margin = 8;
constexpr int strip_left = screen_w - toolbar_margin - toolbar_button;
constexpr int beside_gap = toolbar_margin;
constexpr int scratch_card = 200;
// The note menu's card, the theme's metrics (shell/ui/.../UiTheme.qml):
// six rows of 32 px with 6 px between them, in 10 px padding; the 1 px
// border hairline draws inside, so this is the card's height.
constexpr int note_menu_rows = 6;
constexpr int note_menu_height = note_menu_rows * 32
	+ (note_menu_rows - 1) * 6 + 2 * 10;
// A row's swatch centre: the card's padding (10), the row's left margin
// (8) and half the swatch (16 / 2) from the card's left edge.
constexpr int note_menu_swatch_x = 10 + 8 + 16 / 2;
// The card hangs from the button's top (the flyout anchors itself
// there), so its rows are read from a region that starts a margin above
// the button's band and runs the card's height, with a margin's slack
// on either side.
constexpr int note_menu_margin = 16;

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
	bool wx_flow = false; // the forecast rows read against the fixture
};

std::vector<Indicator> indicators;
std::size_t current = 0;
int phase = 0;
wlr_box popup_box{}; // the open panel's card, from the walk's phase 1
wlr_box field_box{}; // where the Wi-Fi password field ended up
double clock_x = 0, network_x = 0; // where the walk found the clock, network
std::size_t wifi_base = 0; // the network panel's settled row count, baseline
int wifi_last_top = 0; // its last row's top: a prompt under a row above
// shifts every row below it, so the shift proves the prompt opened
double tb_x = 0, tb_y = 0; // the toolbar's own re-issued click point: a
// click can be lost while the bar's render loop is stalled on a
// screenshot, so the proof phase re-issues it until the storage says so

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

// ---- the applications' preference, as the user's own session sees it ----

// The environment this run replaces, saved at start: a child that restores
// it reads — and only reads — the machine's own settings over its own bus.
struct SavedEnv {
	const char *name;
	bool set = false;
	std::string value;
};
SavedEnv saved_env[] = {
	{"DBUS_SESSION_BUS_ADDRESS"},
	{"DBUS_SYSTEM_BUS_ADDRESS"},
	{"XDG_CONFIG_HOME"},
	{"GSETTINGS_BACKEND"},
};

// The user's real color-scheme (org.gnome.desktop.interface), through a
// child with the machine's own environment restored — empty when there is
// no session bus to read it from, in which case the run cannot verify it.
std::string real_color_scheme() {
	int fds[2];
	if (pipe(fds) != 0) {
		return "";
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		for (SavedEnv &saved : saved_env) {
			if (saved.set) {
				setenv(saved.name, saved.value.c_str(), 1);
			} else {
				unsetenv(saved.name);
			}
		}
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		prctl(PR_SET_PDEATHSIG, SIGKILL); // a killed run must not leak it
		execlp("gsettings", "gsettings", "get", "org.gnome.desktop.interface",
			"color-scheme", static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	std::string out;
	char chunk[256];
	ssize_t n;
	while ((n = read(fds[0], chunk, sizeof chunk)) > 0) {
		out.append(chunk, static_cast<std::size_t>(n));
	}
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return "";
	}
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}

// An outside client's theme.set on the compositor's control socket, from a
// double-forked child (the compositor runs in this process, so nothing
// here may block its loop) — the way a user's script or keybinding would
// switch the compositor alone, for the shell to adopt.
void outside_theme_set(const char *mode) {
	const char *path = std::getenv("WAYLAND_CANVAS_SOCKET");
	if (path == nullptr || *path == '\0') {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		if (fork() == 0) {
			alarm(5); // never outlive the run on a stalled socket
			int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
			sockaddr_un addr{};
			addr.sun_family = AF_UNIX;
			if (fd >= 0 && strlen(path) < sizeof(addr.sun_path)) {
				strcpy(addr.sun_path, path);
				if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0) {
					std::string line = std::format(
						"{{\"id\": 1, \"method\": \"theme.set\", "
						"\"params\": {{\"mode\": \"{}\"}}}}\n", mode);
					(void)write(fd, line.data(), line.size());
					// The response, read out so the compositor's write is
					// never blocked; then this end goes quiet.
					char chunk[256];
					ssize_t got = 0;
					while ((got = read(fd, chunk, sizeof chunk)) > 0) {
						if (memchr(chunk, '\n', static_cast<std::size_t>(got)) != nullptr) {
							break;
						}
					}
				}
			}
			if (fd >= 0) {
				close(fd);
			}
		}
		_exit(0);
	}
	int status = 0;
	waitpid(pid, &status, 0); // the middle child, at once: no zombie
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

// The curl stand-in: the weather widget fetches wttr.in with curl, and
// the scenario's world must never reach the internet. A wrapper script
// goes first on the PATH the bar inherits: a wttr.in request is answered
// at once from a response captured once (tests/scenarios/bar-standins/
// wttr-j1.json), so the widget always shows and its panel always lists
// those days; anything else goes to the real curl. Every invocation is
// logged under the run's scratch, so a run can prove what was asked for.
bool start_curl_standin() {
	const char *script = std::getenv("CANVAS_BAR_STANDINS");
	if (script == nullptr || *script == '\0') {
		std::fprintf(stderr, "CANVAS_BAR_STANDINS is unset: the curl stand-in "
			"cannot find its fixture\n");
		return false;
	}
	std::string fixture = (std::filesystem::path(script).parent_path()
		/ "bar-standins/wttr-j1.json").string();
	if (!std::filesystem::exists(fixture)) {
		std::fprintf(stderr, "the curl stand-in is missing its fixture: %s\n",
			fixture.c_str());
		return false;
	}
	const char *const which[] = {"sh", "-c", "command -v curl", nullptr};
	Spawned child = spawn(which);
	std::string real = first_line(child);
	reap(child.pid);
	if (real.empty()) {
		std::fprintf(stderr, "the curl stand-in found no real curl to pass "
			"other URLs to\n");
		return false;
	}
	std::string dir = shot_dir + "/curl-bin";
	mkdir_p(dir);
	std::string wrapper = dir + "/curl";
	std::string sh = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar.cpp: the bar scenario's curl\n"
		"# stand-in. wttr.in — the weather widget's fetch — is answered at\n"
		"# once from the canned response captured once; anything else goes\n"
		"# to the real curl. Every invocation is logged under the run's\n"
		"# scratch, so a run can prove it never asked the network for\n"
		"# weather.\n"
		"log=${CANVAS_TMP:-tmp}/scenarios/bar/curl-standin.log\n"
		"printf '%s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"for arg in \"$@\"; do\n"
		"\tcase \"$arg\" in\n"
		"\t*wttr.in*)\n"
		"\t\tcat " + fixture + "\n"
		"\t\texit 0\n"
		"\t\t;;\n"
		"\tesac\n"
		"done\n"
		"exec " + real + " \"$@\"\n";
	write_file(wrapper, sh);
	chmod(wrapper.c_str(), 0755);
	const char *path = std::getenv("PATH");
	setenv("PATH", std::format("{}:{}", dir, path != nullptr ? path : "").c_str(), 1);
	return true;
}

// The network panel's fixture world, as the tool stand-ins below answer
// it and tests/scenarios/bar-standins/nm.py's mock matches by name:
// HomeNet connected on 2.4 GHz with 5 and 6 reachable, the interface,
// address and gateway the status read reports, and the PSK the secrets
// read returns.
constexpr const char *wifi_iface = "wlan0";
constexpr const char *wifi_ssid = "HomeNet";
constexpr const char *wifi_ip = "192.168.8.42";
constexpr const char *wifi_gateway = "192.168.8.1";
constexpr const char *wifi_psk = "correcthorse";

// The fixture world's connection state, as the nmcli stand-in reads it:
// the walk's disconnect takes it offline and the known join brings it
// back, so the panel's reads always agree with the mock's device state.
// The stand-in's key() keeps the last line, so appending is enough.
void fixture_online(bool on) {
	FILE *f = std::fopen((shot_dir + "/nmcli-state").c_str(), "a");
	if (f == nullptr) {
		return;
	}
	std::fprintf(f, "offline=%d\n", on ? 0 : 1);
	std::fclose(f);
}

// The tool stand-ins: the network panel shells out to nmcli, ping,
// qrencode, speedtest and wl-copy, and the scenario's world must answer
// them deterministically without ever reaching the machine's services.
// Wrappers go first on the PATH the bar inherits: nmcli, ping, qrencode
// and speedtest answer from the fixture world — nmcli keeps the panel's
// changes in a state file, so its reads follow its writes — and wl-copy
// passes through to the real one under the scenario's own display. Every
// invocation lands in one log, so a run can prove what was asked for and
// that nothing named the machine's own network.
bool start_tool_standins() {
	std::string dir = shot_dir + "/tool-bin";
	mkdir_p(dir);
	// a stale state file or log from a killed run would leak into this one
	unlink((shot_dir + "/nmcli-state").c_str());
	unlink((shot_dir + "/tool-standin.log").c_str());
	const char *const which[] = {"sh", "-c", "command -v wl-copy", nullptr};
	Spawned child = spawn(which);
	std::string real = first_line(child);
	reap(child.pid);
	if (real.empty()) {
		std::fprintf(stderr, "the tool stand-ins found no real wl-copy to "
			"pass the copy through to\n");
		return false;
	}
	std::string nmcli = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar.cpp: the bar scenario's nmcli\n"
		"# stand-in. It answers the network panel's queries from the\n"
		"# fixture world, records the changes the panel makes in a state\n"
		"# file so the panel's reads follow its writes, and logs every\n"
		"# call. The passphrase in an enterprise join arrives on stdin and\n"
		"# is proven received, never logged.\n"
		"log=${CANVAS_TMP:-tmp}/scenarios/bar/tool-standin.log\n"
		"state=${CANVAS_TMP:-tmp}/scenarios/bar/nmcli-state\n"
		"printf 'nmcli %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"key(){ grep \"$1\" \"$state\" 2>/dev/null | tail -n 1 | cut -d= -f2-; }\n"
		"case \"$*\" in\n"
		"*\"device show\"*)\n"
		"\t# the scenario marks the world offline once the walk's click\n"
		"\t# disconnects the fixture: the reads agree with the mock then\n"
		"\tif [ \"$(key '^offline=')\" = 1 ]; then\n"
		"\t\tprintf '\\n\\n\\n'\n"
		"\telse\n"
		"\t\tprintf '" + std::string(wifi_ssid) + "\\n" + std::string(wifi_ip)
		+ "/24\\n" + std::string(wifi_gateway) + "\\n'\n"
		"\tfi\n"
		"\t;;\n"
		"*\"dev wifi list\"*)\n"
		"\tif [ \"$(key '^offline=')\" = 1 ]; then\n"
		"\t\tprintf ':2437:" + std::string(wifi_ssid) + "\\n:5180:" + std::string(wifi_ssid)
		+ "\\n:6175:" + std::string(wifi_ssid) + "\\n'\n"
		"\telse\n"
		"\t\tprintf '*:2437:" + std::string(wifi_ssid) + "\\n:5180:" + std::string(wifi_ssid)
		+ "\\n:6175:" + std::string(wifi_ssid) + "\\n'\n"
		"\tfi\n"
		"\tprintf ':2437:CafeGuest\\n:5240:CorpNet\\n:2462:LibraryFree\\n:2462:OldCafe\\n'\n"
		"\t;;\n"
		"*\"802-11-wireless.band connection show\"*)\n"
		"\tkey '^band='\n"
		"\t;;\n"
		"*\"802-11-wireless-security.psk connection show\"*)\n"
		"\tprintf '" + std::string(wifi_psk) + "\\n'\n"
		"\t;;\n"
		"*\"connection modify\"*)\n"
		"\tprev=\n"
		"\tfor arg in \"$@\"; do\n"
		"\t\tcase \"$prev\" in\n"
		"\t\t802-11-wireless.band) printf 'band=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tipv4.dns) printf 'dns4=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tipv4.ignore-auto-dns) printf 'ignore4=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tipv6.dns) printf 'dns6=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tipv6.ignore-auto-dns) printf 'ignore6=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tesac\n"
		"\t\tprev=$arg\n"
		"\tdone\n"
		"\t;;\n"
		"*\"connection delete\"*)\n"
		"\t# the stand-in world's connection objects, by SSID\n"
		"\tcase \"$3\" in\n"
		"\tOldCafe) obj=oldcafe ;;\n"
		"\tHomeNet) obj=home ;;\n"
		"\t*) exit 0 ;;\n"
		"\tesac\n"
		"\tbusctl --address \"$DBUS_SYSTEM_BUS_ADDRESS\" call "
		"org.freedesktop.NetworkManager \"/org/freedesktop/NetworkManager/"
		"Settings/$obj\" org.freedesktop.NetworkManager.Settings."
		"Connection Delete >/dev/null 2>&1\n"
		"\t;;\n"
		"*\"ipv4.dns\"*)\n"
		"\tprintf '%s\\n%s\\n%s\\n%s\\n' \"$(key '^dns4=')\" \"$(key '^ignore4=' || :)\""
		" \"$(key '^dns6=')\" \"$(key '^ignore6=' || :)\"\n"
		"\t;;\n"
		"*\"connection edit\"*)\n"
		"\twhile IFS= read -r line; do\n"
		"\t\tcase \"$line\" in\n"
		"\t\t\"set 802-1x.password \"*) printf 'nmcli-eap-secret-set\\n' >>\"$log\" ;;\n"
		"\t\tesac\n"
		"\tdone\n"
		"\t;;\n"
		"esac\n"
		"exit 0\n";
	write_file(dir + "/nmcli", nmcli);
	std::string ping = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar.cpp: the bar scenario's ping\n"
		"# stand-in. Every probe of the fixture gateway answers 12.3 ms, so\n"
		"# the panel's ping and packet-loss figures are deterministic.\n"
		"log=${CANVAS_TMP:-tmp}/scenarios/bar/tool-standin.log\n"
		"printf 'ping %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"printf 'PING " + std::string(wifi_gateway) + " 56 data bytes\\n'\n"
		"printf '64 bytes from " + std::string(wifi_gateway)
		+ ": icmp_seq=0 ttl=64 time=12.3 ms\\n'\n"
		"printf -- '--- ping statistics ---\\n'\n"
		"printf -- '1 packets transmitted, 1 packets received, 0%% packet loss\\n'\n"
		"exit 0\n";
	write_file(dir + "/ping", ping);
	std::string qrencode = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar.cpp: the bar scenario's\n"
		"# qrencode stand-in: the call is logged, and a real encoded\n"
		"# matrix of the fixture's payload answers — the drawn code\n"
		"# has to scan, not just show ink. The matrix is the ASCII\n"
		"# type that the panel itself parses: two characters a module,\n"
		"# quiet zone included, so the run's reader is the panel's own.\n"
		"log=${CANVAS_TMP:-tmp}/scenarios/bar/tool-standin.log\n"
		"printf 'qrencode %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"# the matrix's lines carry their light modules as trailing\n"
		"# spaces, which nothing downstream may rely on: awk pads every\n"
		"# line back out to the matrix's full width, 37 modules of two\n"
		"# characters\n"
		"awk '{printf \"%-74s\\n\", $0}' <<'QR'\n"
		"                                                                          \n"
		"                                                                          \n"
		"                                                                          \n"
		"                                                                          \n"
		"        ##############    ##    ######  ##    ####  ##############        \n"
		"        ##          ##  ##    ############  ##      ##          ##        \n"
		"        ##  ######  ##  ############    ##  ######  ##  ######  ##        \n"
		"        ##  ######  ##    ######  ########  ####    ##  ######  ##        \n"
		"        ##  ######  ##  ##        ######        ##  ##  ######  ##        \n"
		"        ##          ##  ####  ##  ####  ######      ##          ##        \n"
		"        ##############  ##  ##  ##  ##  ##  ##  ##  ##############        \n"
		"                        ####    ##  ##  ########                          \n"
		"        ####  ##    ####    ##              ##      ######  ####          \n"
		"          ########          ####    ##    ##  ##  ##  ####      ##        \n"
		"        ##########  ##    ####        ######  ##  ##    ####  ####        \n"
		"          ##  ####    ##            ##      ####  ######    ##  ##        \n"
		"        ##  ##    ######        ##        ####    ##      ##              \n"
		"        ##      ##        ########    ##  ##  ##      ##########          \n"
		"        ##################  ##  ##      ##    ########      ######        \n"
		"            ##        ##  ##    ######  ####      ######    ##            \n"
		"                ######  ##  ####  ####  ##    ##########  ##  ##          \n"
		"            ##  ##    ####  ##              ####            ######        \n"
		"        ##  ##      ######  ##    ##      ##      ########  ##  ##        \n"
		"            ####      ######  ##  ##      ##  ######      ##              \n"
		"        ##  ####    ##    ########  ##  ##  ##  ##########  ##            \n"
		"                        ####  ######      ####  ##      ##  ##            \n"
		"        ##############  ##  ##########  ####  ####  ##  ##    ##          \n"
		"        ##          ##        ######    ##  ##  ##      ##  ##            \n"
		"        ##  ######  ##        ##    ##          ##########  ######        \n"
		"        ##  ######  ##  ##  ####  ####    ##    ##  ##  ##    ####        \n"
		"        ##  ######  ##        ##  ########    ##  ######  ##    ##        \n"
		"        ##          ##  ####  ##        ##    ####      ##########        \n"
		"        ##############  ######  ####        ##          ##                \n"
		"                                                                          \n"
		"                                                                          \n"
		"                                                                          \n"
		"                                                                          \n"
		"QR\n"
		"exit 0\n";
	write_file(dir + "/qrencode", qrencode);
	std::string speedtest = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar.cpp: the bar scenario's\n"
		"# speedtest stand-in, answering Ookla's JSON shape.\n"
		"log=${CANVAS_TMP:-tmp}/scenarios/bar/tool-standin.log\n"
		"printf 'speedtest %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"printf '{\"ping\":{\"latency\":9.5},\"download\":"
		"{\"bandwidth\":12345678},\"upload\":{\"bandwidth\":2345678}}\\n'\n"
		"exit 0\n";
	write_file(dir + "/speedtest", speedtest);
	std::string wlcopy = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar.cpp: the bar scenario's\n"
		"# wl-copy stand-in: the copy passes through to the real one under\n"
		"# the scenario's display, and the call is logged so a run can\n"
		"# prove which figure was copied.\n"
		"log=${CANVAS_TMP:-tmp}/scenarios/bar/tool-standin.log\n"
		"printf 'wl-copy\\n' >>\"$log\" 2>/dev/null\n"
		"exec " + real + " \"$@\"\n";
	write_file(dir + "/wl-copy", wlcopy);
	for (const char *name : {"nmcli", "ping", "qrencode", "speedtest", "wl-copy"}) {
		chmod((dir + "/" + name).c_str(), 0755);
	}
	const char *path = std::getenv("PATH");
	setenv("PATH", std::format("{}:{}", dir, path != nullptr ? path : "").c_str(), 1);
	return true;
}

// The network panel's tool log, one line a call: what the panel asked of
// the fixture world's tools, and the proof it asked nothing of the real
// one.
std::string standin_log() {
	return read_file(shot_dir + "/tool-standin.log");
}

int standin_calls(std::string_view text, std::string_view needle) {
	int count = 0;
	std::size_t at = 0;
	while ((at = text.find(needle, at)) != std::string_view::npos) {
		count++;
		at += needle.size();
	}
	return count;
}

// The network panel's row centres, in card coordinates (the card's top
// edge is 0), as plugins/canvas.network/NetworkPanel.qml lays it out in
// its connected shape: the hero, four 16 px figure rows, the 18 px band
// header (with 24 px pills under it while a band is pinned), the 18 px
// DNS header with its 24 px pills (and a 30 px field while servers are
// typed), then the networks — an 18 px "Known networks" header, the
// connected row (46 px: it carries its detail), the other known row, an
// 18 px "Other networks" header, and the three unknown rows (32 px).
// Padding is 10 px, spacing 6, separators 1 px. Keep in sync with the
// panel.
struct WifiRows {
	double hero;
	double figures[4];
	double band;
	double pills;
	double dns;
	double dns_pills;
	double dns_field;
	double known;
	double home;
	double oldcafe;
	double other;
	double cafe;
	double corp;
	double library;
};

WifiRows wifi_rows(bool pinned, bool editing) {
	WifiRows r{};
	double y = 10;
	r.hero = y + 22;
	y += 44 + 6;
	for (int i = 0; i < 4; i++) {
		r.figures[i] = y + 8;
		y += 16 + 6;
	}
	y += 1 + 6;
	r.band = y + 9;
	y += 18 + 6;
	if (pinned) {
		r.pills = y + 12;
		y += 24 + 6;
	}
	y += 1 + 6;
	r.dns = y + 9;
	y += 18 + 6;
	r.dns_pills = y + 12;
	y += 24 + 6;
	if (editing) {
		r.dns_field = y + 15;
		y += 30 + 6;
	}
	y += 1 + 6;
	r.known = y + 9;
	y += 18 + 6;
	r.home = y + 23;
	y += 46 + 6;
	r.oldcafe = y + 16;
	y += 32 + 6;
	r.other = y + 9;
	y += 18 + 6;
	r.cafe = y + 16;
	y += 32 + 6;
	r.corp = y + 16;
	y += 32 + 6;
	r.library = y + 16;
	return r;
}

// The same panel, disconnected: the head, then the networks straight away
// (the connected row is 32 px now — no detail without a connection).
struct WifiOffRows {
	double hero;
	double known;
	double home;
	double oldcafe;
	double other;
	double cafe;
	double corp;
	double library;
};

WifiOffRows wifi_off_rows() {
	WifiOffRows r{};
	double y = 10;
	r.hero = y + 22;
	y += 44 + 6 + 1 + 6; // the hero, then the networks separator
	r.known = y + 9;
	y += 18 + 6;
	r.home = y + 16;
	y += 32 + 6;
	r.oldcafe = y + 16;
	y += 32 + 6;
	r.other = y + 9;
	y += 18 + 6;
	r.cafe = y + 16;
	y += 32 + 6;
	r.corp = y + 16;
	y += 32 + 6;
	r.library = y + 16;
	return r;
}

// The x of a pill, measured from the card's left edge, in the four-up DNS
// row (DHCP, Cloudflare, Google, Custom) and the three-up band row
// (2.4, 5, 6 GHz): the rows run from the padding to the far edge with
// 6 px between pills.
double wifi_dns_pill_x(int index) {
	return 10 + (320.0 - 3 * 6) / 4 * index + (320.0 - 3 * 6) / 8;
}

double wifi_band_pill_x(int index) {
	return 10 + (320.0 - 2 * 6) / 3 * index + (320.0 - 2 * 6) / 6;
}

cairo_surface_t *load_png(const char *name); // defined with the screenshots
bool pixel_is(cairo_surface_t *s, int x, int y, uint32_t want); // palette check

// Whether a DNS pill carries the highlight on a shot: the active pill
// carries the accent's tint over the card's flat background, and its
// clear top-left corner (the pill's text is centred, lower) stands off
// the background exactly there.
bool dns_pill_lit(const char *name, int pill) {
	cairo_surface_t *s = load_png(name);
	int x = int(std::round(popup_box.x + 10
		+ ((320.0 - 18) / 4 + 6) * pill + 3)); // the stride counts the gap
	int y = int(std::round(popup_box.y + wifi_rows(false, false).dns_pills - 9));
	bool lit = s != nullptr && !pixel_is(s, x, y, shell_bg_dark);
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return lit;
}

// Everything the scenario started, in reverse order: the launcher (which
// terminates the dbusmock mocks with it), then the buses.
void stop_isolation() {
	reap(usage_standin_pid);
	reap(private_system.pid);
	reap(private_session.pid);
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

// A key with modifiers held: Super+Tab toggles the overview.
void chord(uint32_t code, uint32_t mod) {
	key(mod, true);
	tap(code);
	key(mod, false);
}

// Lower-case letters, digits, the dot and the space, enough for a
// password, an address or a note's text.
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
		{'.', KEY_DOT},
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

// The pointer at (x, y), without a click: parking it where no widget or
// panel sits before a shot.
void park(double x, double y) {
	t += 8;
	wlr_pointer_motion_absolute_event m = {
		.pointer = &ptr, .time_msec = t, .x = x / double(screen_w),
		.y = y / double(screen_h)};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &m);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
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

// The scratch plugin's storage file, read back as text: the proof an
// action ran is what it wrote (api.storage writes JSON under the
// scratch state dir).
std::string read_storage(const char *name) {
	std::string path = std::format(
		"{}/xdg/state/wayland-canvas/plugins/canvas.scratch/{}", shot_dir, name);
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		return {};
	}
	std::string out;
	char buf[512];
	std::size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.append(buf, n);
	}
	std::fclose(f);
	return out;
}

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
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

constexpr uint32_t SUPER[] = {KEY_LEFTMETA};

// ---- the bar client and its surfaces ----

// The mapped layer surface with this namespace: the shell client's
// surfaces are told apart by name (the bar, each panel, the toolbar).
LayerSurface *named_layer(const char *ns) {
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strcmp(l->wlr->namespace_, ns) == 0) {
					return l.get();
				}
			}
		}
	}
	return nullptr;
}

LayerSurface *bar_layer() {
	return named_layer(bar_namespace);
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
	return named_layer(ns.c_str());
}

// The overview toolbar surface, shown only while its output is in the
// overview.
LayerSurface *toolbar_layer() {
	return named_layer(toolbar_namespace);
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

std::vector<Band> tb_buttons; // the toolbar's button bands, from the shot

// The lit row bands inside a rectangle of the screen, top to bottom: one
// per line of glyphs or text, so positions are read off the screenshot
// instead of assumed from the layout. Clamped to the image on every side.
std::vector<Band> bands_in(cairo_surface_t *s, const wlr_box &rect) {
	std::vector<Band> bands;
	if (s == nullptr) {
		return bands;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	int x0 = std::max(0, rect.x), x1 = std::min(w, rect.x + rect.width);
	bool in_band = false;
	int band_top = 0, band_lit = 0;
	for (int y = std::max(0, rect.y); y < std::min(h, rect.y + rect.height); y++) {
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
		bands.push_back({band_top, std::min(h, rect.y + rect.height), band_lit});
	}
	return bands;
}

// The lit text bands inside a card's column, top to bottom: one per row,
// header or field of the panel's content.
std::vector<Band> text_bands(cairo_surface_t *s, const wlr_box &card) {
	return bands_in(s, {card.x + 8, card.y, card.width - 16, screen_h - card.y});
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

// The last row of a panel's own content, not the background's ink below
// the card: that ink sits a margin plus a border past the card's last
// row, wider than any gap between rows inside the card.
const Band &last_content_row(const std::vector<Band> &rows) {
	if (rows.size() >= 2 && rows.back().top - rows[rows.size() - 2].bottom
			> 12) {
		return rows[rows.size() - 2];
	}
	return rows.back();
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
	const char *const argv[] = {"busctl", "--address", private_system.address.c_str(),
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

// The provider chips of the open agents panel, as runs in its hero row.
std::vector<Run> agents_chip_runs;

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

// ---- the theme phase: sampling flat palette fills off screenshots ----

// The renderer's rounding of a flat fill stays within this of the palette.
constexpr int palette_tolerance = 8;

// One screenshot pixel as 0xRRGGBB (grim's ARGB32: B, G, R, A in memory).
uint32_t pixel_at(cairo_surface_t *s, int x, int y) {
	unsigned char *p = cairo_image_surface_get_data(s)
		+ y * cairo_image_surface_get_stride(s) + x * 4;
	return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | uint32_t(p[0]);
}

bool near_channel(unsigned char got, unsigned char want) {
	int d = got > want ? got - want : want - got;
	return d <= palette_tolerance;
}

// True when the pixel is the palette colour: the bar, panel cards, title
// bars and the target tab are flat fills of one palette entry, so sampling
// one clear pixel tells the mode. The palette carries 0xRRGGBBAA with the
// alpha in the low byte; a screenshot pixel has no alpha, so the alpha is
// shifted out before the channels are compared.
bool pixel_is(cairo_surface_t *s, int x, int y, uint32_t want) {
	if (s == nullptr) {
		return false;
	}
	want >>= 8;
	uint32_t got = pixel_at(s, x, y);
	return near_channel(got >> 16, want >> 16)
		&& near_channel(got >> 8, want >> 8) && near_channel(got, want);
}

// A shot's one pixel against a palette colour.
bool shot_pixel_is(const char *name, int x, int y, uint32_t want) {
	cairo_surface_t *s = load_png(name);
	bool ok = pixel_is(s, x, y, want);
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return ok;
}

// The mapped view with this title, as the input scenario finds its windows.
View *titled(const char *title) {
	for (View *v : server->views) {
		if (v->mapped && v->wlr->title && std::strcmp(v->wlr->title, title) == 0) {
			return v;
		}
	}
	return nullptr;
}

// ---- notes: the msg client, the plugin's state, its storage ----

// Runs `wayland-canvas msg <method> [<params>]` through /bin/sh, with
// stdout, stderr and the exit code captured under `name` (.out, .err,
// .code); a stale capture from an earlier run is unlinked first. The
// compositor runs in this process, so the child is never waited on here.
void run_msg(std::string_view method, std::string_view params, const char *name) {
	// Meson passes the built CLI's path; a manual run without it falls
	// back to the name on PATH, never to a null format argument.
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	if (bin == nullptr) {
		bin = "wayland-canvas";
	}
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

// The one output the headless backend gives.
Output *out() {
	return server->outputs.front().get();
}

// The target tab's fill pixel in a shot: the tab layout is read from the
// compositor itself, the sample point is the tab's clear top-left corner.
// A note in the log names the rect and pixel, so a failure says why.
bool target_tab_is(const char *name, uint32_t want) {
	Output *o = out();
	Desktop *d = o != nullptr
		? o->server.canvas.desktops.find(o->overview.target().desktop) : nullptr;
	std::optional<TabLayout> layout = d != nullptr
		? o->server.painter.overview_tab_layout(*o, *d) : std::nullopt;
	cairo_surface_t *s = load_png(name);
	int x = layout.has_value() ? layout->tab.x + 4 : 0;
	int y = layout.has_value() ? layout->tab.y + 4 : 0;
	bool ok = layout.has_value() && pixel_is(s, x, y, want);
	std::println("note: tab sample ({},{}): {:06x}, want {:06x}, layout {}, desktop {}",
		x, y, s != nullptr ? pixel_at(s, x, y) : 0, want,
		layout.has_value() ? "found" : "missing", d != nullptr ? "found" : "missing");
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return ok;
}

// The themed window's title bar in a shot: one of the mode's two title
// colours (which one depends on focus; both follow the mode), sampled mid
// bar where no title or button is drawn. A note in the log names the
// sample point and pixel, so a failure says why.
bool title_bar_is(const char *name, uint32_t active, uint32_t inactive) {
	View *v = titled("themed");
	wlr_box f = v != nullptr ? v->frame_box() : wlr_box{};
	int x = f.x + f.width / 2, y = f.y + config::titlebar_height / 2;
	cairo_surface_t *s = load_png(name);
	bool ok = v != nullptr && s != nullptr
		&& (pixel_is(s, x, y, active) || pixel_is(s, x, y, inactive));
	std::println("note: title sample ({},{}): {:06x}, want {:06x} or {:06x}, view {}",
		x, y, s != nullptr ? pixel_at(s, x, y) : 0, active, inactive,
		v != nullptr ? "found" : "missing");
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return ok;
}

// The left section's right edge, by ink: glyph pixels stand off the
// background in either mode — bright on the dark bar, dark on the light
// one — so the edge reads the same before and after a switch. The
// re-enabled plugin's label sits past the logo, pushing the edge right.
double bar_ink_edge(cairo_surface_t *s, const wlr_box &bar, bool light) {
	if (s == nullptr) {
		return 0;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	double edge = 0;
	int limit = screen_w / 2 - 250; // short of the centred widgets
	for (int x = 0; bar.x + x < limit; x++) {
		for (int y = 0; y < bar.height; y++) {
			unsigned char *p = data + (bar.y + y) * stride + (bar.x + x) * 4;
			int sum = p[0] + p[1] + p[2];
			if (light ? sum < 550 : sum > 300) { // ink, this mode's way
				edge = bar.x + x;
				break;
			}
		}
	}
	return edge;
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
		} else if (ind.wx_flow) {
			phase = 50;
		} else {
			phase = 2;
		}
		return 700; // let grim finish (handle_timer waits for it)
	}
	case 10: // re-shoot the panel: the rows settle only once the scanner runs
		shot("network");
		phase = 14;
		return 700;
	case 14: { // click the stand-in's unknown secured network: CafeGuest,
		// in "Other networks" — the row that asks for a password
		WifiRows wifi = wifi_rows(false, false);
		std::vector<Band> rows = panel_rows("network");
		// The head, four figure rows, the band header, the DNS header and
		// pills, "Known networks" with HomeNet and OldCafe, then "Other
		// networks" with CafeGuest, CorpNet and LibraryFree: 15 rows.
		if (rows.size() < 15 && retries++ < 20) {
			phase = 10; // rows not on screen yet
			return 400;
		}
		retries = 0;
		check(rows.size() >= 15, std::format("the network panel lists the "
			"stand-in's five networks ({} rows: head, figures, band, DNS, "
			"two headers, 5 rows)", rows.size()));
		if (rows.size() < 15) {
			phase = 2; // nothing was clicked: close without poking the panel
			return 300;
		}
		wifi_base = int(rows.size());
		wifi_last_top = rows.back().top;
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.cafe);
		phase = 15; // the shot must come a tick later: the prompt renders
		// only after the click was delivered
		return 400;
	}
	case 15:
		shot("network-selected");
		phase = 11;
		return 300;
	case 11: { // the click asked for a password: the prompt's field is
		// inline under the row — focus it and type into it
		check(keyboard_on_bar(), "the keyboard is on the bar client while "
			"the network panel is open");
		WifiRows wifi = wifi_rows(false, false);
		std::vector<Band> rows = panel_rows("network-selected");
		// The prompt's field lands inline under the row: every row below
		// shifts by its height, and the row count grows when the field's
		// own ink is lit enough to read — either way the shift proves it.
		bool armed = rows.size() > wifi_base
			|| (rows.size() == wifi_base && rows.back().top > wifi_last_top + 25);
		if (!armed && retries++ < 15) { // the prompt settles
			phase = 15;
			return 400;
		}
		retries = 0;
		check(armed, std::format("clicking the secured network armed its "
			"password field ({} rows, was {}; the rows below shifted)",
			rows.size(), wifi_base));
		if (!armed) {
			phase = 2;
			return 300;
		}
		field_box = wlr_box{popup_box.x,
			static_cast<int>(std::round(popup_box.y + wifi.cafe + 22)),
			popup_box.width, 30};
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.cafe + 37);
		phase = 16; // type only once the click has focused the field
		return 400;
	}
	case 16:
		type("correctbattery"); // never Enter yet: nothing is submitted
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
		phase = 17; // on to Enter: the flow continues against the mock
		return 300;
	}
	case 17: { // submit the typed password: Enter in the field — the field
		// holds it only once the panel has stopped changing
		if (!settle_advance("network-typed")) {
			return 400; // the panel is still settling under load
		}
		tap(KEY_ENTER);
		phase = 18;
		return 400;
	}
	case 18: { // Enter submitted: the mock NetworkManager received the
		// activation with the typed password, and nothing connected
		// before it was typed
		bool sent = mock_called(NM_NAME, NM_MANAGER, "AddAndActivateConnection",
			"correctbattery");
		if (!sent && retries++ < 150) { // in flight under load, not lost
			return 400;
		}
		retries = 0;
		check(sent, sent ? "Enter in CafeGuest's field sent its activation "
			"with the typed password to the stand-in NetworkManager"
			: "the Enter submit was lost under load");
		shot("network-connected");
		phase = 19;
		return 400;
	}
	case 19: { // the connected network's row click disconnects it: the
		// click lands only once the prompt has closed
		if (!settle_advance("network-connected")) {
			return 400; // the panel is still settling under load
		}
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.home);
		phase = 20;
		return 400;
	}
	case 20:
		shot("network-home");
		phase = 21;
		return 400;
	case 21: { // let the panel settle on its disconnected shape
		if (!settle_advance("network-home")) {
			return 400; // the panel is still settling under load
		}
		phase = 22;
		return 300;
	}
	case 22: {
		bool dropped = mock_called(NM_NAME, NM_DEVICE, "Disconnect");
		if (!dropped && retries++ < 150) { // in flight under load, not lost
			return 400;
		}
		retries = 0;
		check(dropped, dropped ? "a click on the connected network's row "
			"disconnected it on the stand-in's Wi-Fi device"
			: "the row click was lost under load");
		if (dropped) {
			fixture_online(false); // the reads go offline with the device
		}
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
	case 50: { // the forecast rows: the fixture carries three days
		std::vector<Band> rows = panel_rows(ind.name);
		if (rows.size() < 3 && retries++ < 10) { // the shot may render late
			shot(ind.name);
			return -400;
		}
		retries = 0;
		check(rows.size() >= 3, std::format("the weather panel lists the "
			"fixture's forecast days ({} rows)", rows.size()));
		phase = 2;
		return 400;
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

// ---- the agents phase: the AI usage panel end to end ----
//
// The world is a scratch HOME the collector reads (never this machine's
// own ~/.claude, ~/.codex or ~/.omp), a fake codex app-server and the
// built collector on the PATH the bar inherits, and the usage endpoint
// pointed at this run's own loopback stand-in: no network, no real
// credential, and every probe recorded.

constexpr int UiThemePadding = 10; // UiTheme.panelPadding, mirrored here
// The panel's palette colours as pixel_is takes them: 0xRRGGBBAA, the
// alpha shifted out before the channels are compared.
constexpr uint32_t accent_dark = 0x7aa2f7ff;   // UiTheme.accent, dark
constexpr uint32_t attention_dark = 0xf7768eff; // UiTheme.attention, dark
constexpr uint32_t attention_light = 0xf52a65ff; // UiTheme.attention, light
constexpr uint32_t claude_mark = 0xd97757ff;   // the starburst's clay
constexpr uint32_t muted_dark = 0x565f89ff;    // UiTheme.muted, dark
constexpr uint32_t muted_light = 0x8489a3ff;   // UiTheme.muted, light
constexpr uint32_t panel_light = 0xf4f5f8ff;   // UiTheme.panel, light



// The stand-in's READY line ("READY <port>"), or empty on a timeout.
std::string ready_line(Spawned &child, int timeout_s) {
	std::string buffer;
	char chunk[256];
	for (int tick = 0; tick < timeout_s * 2; tick++) {
		pollfd p = {.fd = child.out, .events = POLLIN, .revents = 0};
		if (poll(&p, 1, 500) > 0 && (p.revents & (POLLIN | POLLHUP)) != 0) {
			ssize_t n = read(child.out, chunk, sizeof chunk);
			if (n <= 0) {
				break;
			}
			buffer.append(chunk, static_cast<std::size_t>(n));
			std::size_t newline = buffer.find('\n');
			if (newline != std::string::npos) {
				return buffer.substr(0, newline);
			}
		}
		if (waitpid(child.pid, nullptr, WNOHANG) == child.pid) {
			break;
		}
	}
	return {};
}

// The local date `offset` days from today: the collector buckets at local
// midnight, so the fixtures speak local dates.
std::string day_iso(int offset) {
	std::time_t when = std::time(nullptr)
		+ static_cast<std::time_t>(offset) * 86400;
	std::tm local{};
	if (localtime_r(&when, &local) == nullptr) {
		return {};
	}
	return std::format("{:04d}-{:02d}-{:02d}", local.tm_year + 1900,
		local.tm_mon + 1, local.tm_mday);
}

// An ISO-8601 UTC instant `offset` seconds away, for the payload's
// reset times.
std::string iso_in(long long offset) {
	std::time_t when = std::time(nullptr) + offset;
	std::tm utc{};
	if (gmtime_r(&when, &utc) == nullptr) {
		return {};
	}
	return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
		utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
		utc.tm_min, utc.tm_sec);
}

// The usage endpoint's answer, at whatever shares the phase wants.
void write_usage_payload(const char *session, const char *weekly) {
	write_file(std::format("{}/usage-response.json", shot_dir),
		std::format(R"({{"five_hour":{{"utilization":"{}",)"
			R"("resets_at":"{}"}},"seven_day_oauth_apps":)"
			R"({{"utilization":"{}","resets_at":"{}"}}}})",
			session, iso_in(7500), weekly, iso_in(5 * 86400)));
}

// One omp session line: the shape the pi/omp tools write, which the
// collector counts by provider family.
std::string usage_line(const std::string &id, const std::string &date,
	const char *clock, const char *provider, const char *model,
	long long input, long long output) {
	return std::format(R"({{"type":"message","id":"{}",)"
		R"("timestamp":"{}T{}","message":{{"role":"assistant",)"
		R"("provider":"{}","model":"{}","usage":{{"input":{},)"
		R"("output":{}}}}}}})"
		"\n", id, date, clock, provider, model, input, output);
}

// The fixture world: every family the panel shows — Claude's subscription
// (exact `anthropic`), GPT, Fireworks, a custom Anthropic gateway — plus
// the lines that must not count, the CLI's saved sign-in, and a Codex
// session file. Claude's day figures deliberately equal Omarchy's own
// record for the days the reference screenshot shows.
void drop_usage_fixtures() {
	std::string sessions = scratch_home + "/.omp/agent/sessions/fixture";
	mkdir_p(sessions);
	std::string d5 = day_iso(-5), d3 = day_iso(-3), d2 = day_iso(-2),
		d1 = day_iso(-1), d0 = day_iso(0);
	std::string lines;
	lines += usage_line("a1", d5, "10:00:00", "anthropic",
		"claude-opus-5-5", 1500, 0);
	lines += usage_line("a2", d3, "10:00:00", "anthropic",
		"claude-opus-5-5", 2300000, 0);
	lines += usage_line("a3", d3, "11:00:00", "anthropic",
		"claude-haiku-4-5", 100000, 0);
	lines += usage_line("a4", d2, "12:00:00", "anthropic",
		"claude-opus-5-5", 246132594, 0);
	// The same id again: deduplicated, never counted twice.
	lines += usage_line("a4", d2, "12:00:00", "anthropic",
		"claude-opus-5-5", 246132594, 0);
	lines += usage_line("a5", d1, "13:00:00", "anthropic",
		"claude-opus-5-5", 324632448, 0);
	lines += usage_line("a6", d0, "14:00:00", "anthropic",
		"claude-opus-5-5", 811500, 0);
	// A user turn and a model_usage entry: neither is a message.
	lines += std::format(R"({{"type":"message","id":"x1",)"
		R"("timestamp":"{}T15:00:00","message":{{"role":"user",)"
		R"("provider":"anthropic","model":"claude-opus-5-5",)"
		R"("usage":{{"input":999999}}}}}})"
		"\n", d0);
	lines += std::format(R"({{"type":"model_usage","id":"x2",)"
		R"("timestamp":"{}T15:30:00","provider":"anthropic",)"
		R"("model":"claude-opus-5-5","usage":{{"totalTokens":888888}}}})"
		"\n", d0);
	lines += usage_line("b1", d1, "16:00:00", "openai", "gpt-5.5",
		500000, 0);
	lines += usage_line("b2", d0, "17:00:00", "openai", "gpt-5.5",
		700000, 0);
	lines += usage_line("c1", d0, "18:00:00", "fireworks-300k",
		"glm-5p3-fast", 1250000, 0);
	lines += usage_line("g1", d0, "19:00:00",
		"anthropic-250k-prefer-using-this-one", "claude-opus-5-5", 40000, 0);
	write_file(sessions + "/usage.jsonl", lines);

	// The CLI's saved sign-in: a token an hour from expiring and the plan
	// label the hero's meta line shows.
	std::string claude_dir = scratch_home + "/.claude";
	mkdir_p(claude_dir);
	write_file(claude_dir + "/.credentials.json",
		std::format(R"({{"claudeAiOauth":{{"accessToken":"fixture-token",)"
			R"("expiresAt":{},"rateLimitTier":"max_20x",)"
			R"("subscriptionType":""}}}})",
			static_cast<long long>(std::time(nullptr)) * 1000 + 3600000));

	// A Codex session file: the model on turn_context, the tokens on
	// token_count's last_token_usage, cached already inside input.
	std::string codex_dir = scratch_home + "/.codex/sessions/fixture";
	mkdir_p(codex_dir);
	write_file(codex_dir + "/rollout.jsonl",
		std::format(R"({{"timestamp":"{}T20:00:00","type":"turn_context",)"
			R"("payload":{{"model":"gpt-5.5-codex"}}}})"
			"\n"
			R"({{"timestamp":"{}T20:00:05","type":"response_item",)"
			R"("payload":{{"payload":{{"type":"token_count","info":)"
			R"({{"last_token_usage":{{"input_tokens":5000,)"
			R"("cached_input_tokens":1000,)"
			R"("cache_write_input_tokens":0,)"
			R"("output_tokens":1000}}}}}}}}}})"
			"\n", d1, d1));
}

// The whole fixture world removed: the panel's own quiet-world check.
void remove_usage_fixtures() {
	std::error_code ec;
	for (const char *dir : {"/.omp", "/.claude", "/.codex", "/.cache"}) {
		std::filesystem::remove_all(scratch_home + dir, ec);
	}
}

// Rewrites the scratch shell.json as it stands: the shell sees an edit
// and reloads, leaving everything the config carries in place.
void reload_shell() {
	write_file(shell_config, read_file(shell_config));
}

// One JSON object member as a decimal, or 0 when it is absent.
double json_double(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return 0;
	}
	JsonNode *node = json_object_get_member(object, name);
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_VALUE
		? json_node_get_double(node) : 0;
}

// The record's provider objects, or empty when the collector said
// nothing. The parser that owns them stays for the run — one parse.
std::vector<JsonObject *> collector_providers() {
	static JsonParser *parser = nullptr;
	std::vector<JsonObject *> providers;
	const char *binary = std::getenv("WAYLAND_CANVAS_AGENT_USAGE");
	if (binary == nullptr || *binary == '\0') {
		check(false, "WAYLAND_CANVAS_AGENT_USAGE is unset: the collector "
			"is not on the scenario's PATH");
		return providers;
	}
	const char *const argv[] = {binary, static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	std::string out = child_output(child, 20);
	if (parser != nullptr) {
		g_object_unref(parser);
	}
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, out.c_str(),
		    static_cast<gssize>(out.size()), nullptr)) {
		check(false, "the collector's record is not JSON");
		return providers;
	}
	JsonNode *root = json_parser_get_root(parser);
	JsonObject *record = root != nullptr
		&& json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
	JsonNode *list = record != nullptr
		&& json_object_has_member(record, "providers")
			? json_object_get_member(record, "providers") : nullptr;
	if (list == nullptr || json_node_get_node_type(list) != JSON_NODE_ARRAY) {
		return providers;
	}
	for (GList *it = json_array_get_elements(json_node_get_array(list));
			it != nullptr; it = it->next) {
		JsonNode *element = static_cast<JsonNode *>(it->data);
		if (json_node_get_node_type(element) == JSON_NODE_OBJECT) {
			providers.push_back(json_node_get_object(element));
		}
	}
	return providers;
}

// Pixels of one colour (within the palette tolerance) inside a rect.
int colour_count(cairo_surface_t *s, const wlr_box &rect, uint32_t want) {
	if (s == nullptr) {
		return -1;
	}
	int count = 0;
	for (int y = rect.y; y < rect.y + rect.height; y++) {
		for (int x = rect.x; x < rect.x + rect.width; x++) {
			if (pixel_is(s, x, y, want)) {
				count++;
			}
		}
	}
	return count;
}

// ---- the recording-dot phase ----

// This phase's one recording, and where its dot sat on the bar.
std::string dot_recording;
double dot_x = 0;
bool dot_acted = false; // a step's chord or click has happened (retry-safe)

// The capture toolbar's mode button `index` (0-5, the recordings from 3),
// its click point: the geometry CaptureUi::toolbar_layout() builds, so the
// real toolbar is clicked where it draws itself.
std::pair<double, double> capture_mode_click(int index) {
	int fixed = 2 * config::capture_toolbar_pad + 5 * config::capture_toolbar_gap
		+ config::capture_toolbar_group_gap;
	int avail = screen_w - 2 * config::capture_thumbnail_margin - fixed;
	int bw = (avail - 5 * config::capture_toolbar_gap) / 6;
	if (bw > config::capture_toolbar_button_w) {
		bw = config::capture_toolbar_button_w;
	}
	if (bw < config::capture_toolbar_button_w / 2) {
		bw = config::capture_toolbar_button_w / 2; // tiny outputs clip, not crush
	}
	int inner = 6 * bw + 5 * config::capture_toolbar_gap
		+ config::capture_toolbar_group_gap;
	int box_x = (screen_w - (inner + 2 * config::capture_toolbar_pad)) / 2;
	int box_y = screen_h - config::capture_toolbar_margin
		- (2 * config::capture_toolbar_pad + config::capture_toolbar_options_h
			+ config::capture_toolbar_gap + config::capture_toolbar_button_h
			+ config::capture_toolbar_gap + config::capture_toolbar_footer_h);
	int x = box_x + config::capture_toolbar_pad
		+ index * (bw + config::capture_toolbar_gap)
		+ (index >= 3 ? config::capture_toolbar_group_gap : 0);
	int y = box_y + config::capture_toolbar_pad
		+ config::capture_toolbar_options_h + config::capture_toolbar_gap;
	return {x + bw / 2.0, y + config::capture_toolbar_button_h / 2.0};
}

// The attention-coloured pixels inside the bar strip: how many, and their
// bounding box (empty when none). The recording dot is the only thing that
// wears the attention colour on this bar — a widget it displaces can slide
// under where it sits, so runs are read by colour, not by what moved.
std::pair<int, wlr_box> attention_span(cairo_surface_t *s, const wlr_box &bar,
		uint32_t want) {
	int count = 0;
	wlr_box box{0, 0, 0, 0};
	if (s == nullptr) {
		return {count, box};
	}
	for (int y = bar.y; y < bar.y + bar.height; y++) {
		for (int x = bar.x; x < bar.x + bar.width; x++) {
			if (!pixel_is(s, x, y, want)) {
				continue;
			}
			if (count == 0) {
				box = {x, y, 1, 1};
			} else {
				if (x < box.x) {
					box.width += box.x - x;
					box.x = x;
				}
				if (y < box.y) {
					box.height += box.y - y;
					box.y = y;
				}
				if (x + 1 > box.x + box.width) {
					box.width = x + 1 - box.x;
				}
				if (y + 1 > box.y + box.height) {
					box.height = y + 1 - box.y;
				}
			}
			count++;
		}
	}
	return {count, box};
}

// The widths of the meter fills in the panel's card: one entry per meter
// (a run of accent pixels a few rows tall), the session window first. An
// empty vector means no meter drew.
std::vector<int> meter_fills(cairo_surface_t *s, const wlr_box &card,
	uint32_t colour) {
	std::vector<int> fills;
	if (s == nullptr) {
		return fills;
	}
	int inner = card.width - 2 * UiThemePadding; // the meter track's width
	int best = 0, rows = 0;
	// The box carries no height (card_box names the top edge and width);
	// like text_bands the scan runs to the screen's bottom, where the
	// card itself ends.
	for (int y = card.y; y < screen_h; y++) {
		int lit = 0;
		for (int x = card.x + UiThemePadding;
				x < card.x + card.width - UiThemePadding; x++) {
			if (pixel_is(s, x, y, colour)) {
				lit++;
			}
		}
		if (lit > inner / 4) { // a meter row, not stray text pixels
			best = std::max(best, lit);
			rows++;
			continue;
		}
		if (rows > 0) { // the meter ended
			fills.push_back(best);
			best = 0;
			rows = 0;
		}
	}
	if (rows > 0) {
		fills.push_back(best);
	}
	return fills;
}

// Lines in a text file: the stand-ins' logs, counted.
int log_lines(const std::string &path) {
	std::string text = read_file(path);
	if (text.empty()) {
		return 0;
	}
	int lines = 0;
	for (char c : text) {
		if (c == '\n') {
			lines++;
		}
	}
	return text.back() == '\n' ? lines : lines + 1;
}

// Shots `name` on the first call and reports true on the next: a tick is
// enough time for grim (handle_timer waits for it first), so the load
// that follows sees a whole file. A retry loop that comes back through
// here re-shots by itself — the pending name is consumed on the second
// call, so the third is a fresh shot.
std::string shot_pending;
bool shot_landed(const char *name) {
	if (shot_pending != name) {
		shot(name);
		shot_pending = name;
		return false;
	}
	shot_pending.clear();
	return true;
}

// ---- the toolbar edges phase (300+) ----
// The strip on every edge the config can name, left, top and bottom
// written into the scratch shell.json as an edit the shell reloads
// with, then right again at the end. The placed box is read from the
// compositor's layer surfaces, never assumed; the note button's colour
// flyout must open toward the screen's centre and fold again; and the
// overview's shelf, with a minimized window on it, must sit above a
// bottom-edge strip.

// The shelf's bottom line: the help line's 12 px margin and 28 px
// height, 16 px above it (src/render/render.cpp's overview_shelf_layout
// and the toolbar it clears). Keep in sync with it.
constexpr int shelf_bottom_gap = 12 + 28 + 16;
// The note button's place in the strip: order 3, the fourth square —
// the scratch plugin's three buttons above it, the settings gear
// (order 4) below. Keep in sync with the plugins' manifests.
constexpr int notes_index = 3;

// Writes the scratch shell.json with the toolbar's edge named, keeping
// everything else it holds: an edit the host sees, so the shell
// reloads with it.
bool set_toolbar_position(const char *edge) {
	JsonParser *parser = json_parser_new();
	std::string text = read_file(shell_config);
	bool ok = !text.empty()
		&& json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)
		&& json_node_get_node_type(json_parser_get_root(parser))
			== JSON_NODE_OBJECT;
	if (ok) {
		JsonObject *config = json_node_get_object(json_parser_get_root(parser));
		JsonNode *toolbar = json_object_get_member(config, "toolbar");
		if (toolbar == nullptr
				|| json_node_get_node_type(toolbar) != JSON_NODE_OBJECT) {
			json_object_set_object_member(config, "toolbar", json_object_new());
		}
		json_object_set_string_member(
			json_node_get_object(json_object_get_member(config, "toolbar")),
			"position", edge);
		JsonGenerator *generator = json_generator_new();
		json_generator_set_root(generator, json_parser_get_root(parser));
		gchar *out = json_generator_to_data(generator, nullptr);
		write_file(shell_config, out);
		g_free(out);
		g_object_unref(generator);
	}
	g_object_unref(parser);
	return ok;
}

// The toolbar.position the scratch shell.json names now.
std::string toolbar_position_in_config() {
	JsonParser *parser = json_parser_new();
	std::string named;
	if (json_parser_load_from_file(parser, shell_config.c_str(), nullptr)
		&& json_node_get_node_type(json_parser_get_root(parser))
			== JSON_NODE_OBJECT) {
		JsonObject *config = json_node_get_object(json_parser_get_root(parser));
		JsonNode *toolbar = json_object_get_member(config, "toolbar");
		JsonObject *toolbar_object = toolbar != nullptr
			&& json_node_get_node_type(toolbar) == JSON_NODE_OBJECT
			? json_node_get_object(toolbar) : nullptr;
		if (toolbar_object != nullptr
				&& json_object_has_member(toolbar_object, "position")) {
			JsonNode *position = json_object_get_member(toolbar_object, "position");
			if (position != nullptr
					&& json_node_get_value_type(position) == G_TYPE_STRING) {
				named = json_node_get_string(position);
			}
		}
	}
	g_object_unref(parser);
	return named;
}

// The strip's settled box, from the surface's placed one: the surface
// is the strip plus the pocket it slides into — overhanging the screen
// edge by a button square on the left, right and bottom (the pocket
// past it), and past the screen's top on the top edge (the row sits
// below the bar) — so the strip's near edge is the surface's own on
// the right and bottom, the pocket's far one on the left and top.
wlr_box toolbar_strip_box(const wlr_box &geo) {
	if (geo.width == toolbar_button + toolbar_margin + toolbar_button) { // a column
		return {geo.x > screen_w / 2 ? geo.x : geo.x + toolbar_margin + toolbar_button,
			geo.y, toolbar_button, geo.height};
	}
	// a row: the top edge's pocket reaches past the screen's top
	int pocket = geo.y < 0 ? bar_h() + toolbar_margin + toolbar_button
		: toolbar_margin + toolbar_button;
	return {geo.x, geo.y < 0 ? geo.y + pocket : geo.y, geo.width, toolbar_button};
}

// The strip's box as the compositor placed it for this edge, read from
// the mapped toolbar surface: a 44 px column or row of whole buttons,
// on the edge's margin (the top row below the bar). The wait loops on
// it, so the reload has landed when it holds — the old surface stays
// mapped at the old edge until the reload, so the geo, not the mapping,
// is what moves.
bool toolbar_on_edge(const char *edge) {
	LayerSurface *toolbar = toolbar_layer();
	if (toolbar == nullptr) {
		return false;
	}
	wlr_box geo = toolbar_strip_box(toolbar->geo);
	bool column = std::strcmp(edge, "left") == 0 || std::strcmp(edge, "right") == 0;
	if (column && (geo.width != toolbar_button || geo.height % toolbar_button != 0)) {
		return false;
	}
	if (!column && (geo.height != toolbar_button || geo.width % toolbar_button != 0)) {
		return false;
	}
	if (std::strcmp(edge, "right") == 0) {
		return geo.x == strip_left;
	}
	if (std::strcmp(edge, "left") == 0) {
		return geo.x == toolbar_margin;
	}
	if (std::strcmp(edge, "top") == 0) {
		return geo.y == bar_h() + toolbar_margin;
	}
	return geo.y + geo.height == screen_h - toolbar_margin;
}

// The strip is centred along its edge, from the placed box.
bool toolbar_centred(const wlr_box &geo) {
	if (geo.width == toolbar_button) {
		return geo.y + geo.height / 2 == screen_h / 2;
	}
	return geo.x + geo.width / 2 == screen_w / 2;
}

// The notes button's click point, from the placed surface.
std::pair<double, double> notes_click(const wlr_box &geo) {
	wlr_box strip = toolbar_strip_box(geo);
	int along = notes_index * toolbar_button + toolbar_button / 2;
	if (strip.width == toolbar_button) {
		return {strip.x + toolbar_button / 2.0, double(strip.y + along)};
	}
	return {double(strip.x + along), strip.y + toolbar_button / 2.0};
}

// The generous region the notes flyout opens into: the card's own
// place — a 200 px card of the six-row menu's height, one margin past
// the strip on its inner side (the side the box's own place names),
// its near edge at the button's near one — with slack on every side
// for the clamping and the rounding.
wlr_box flyout_region(const wlr_box &geo) {
	wlr_box strip = toolbar_strip_box(geo);
	int near = notes_index * toolbar_button; // the button's near edge
	int x = 0, y = 0;
	if (strip.width == toolbar_button) { // a column: the card beside it
		x = strip.x > screen_w / 2 ? strip.x - beside_gap - scratch_card
			: strip.x + toolbar_button + beside_gap;
		y = strip.y + near;
	} else { // a row: the card above or below it
		y = strip.y > screen_h / 2 ? strip.y - beside_gap - note_menu_height
			: strip.y + toolbar_button + beside_gap;
		x = strip.x + near;
	}
	return {x - note_menu_margin, y - note_menu_margin,
		scratch_card + 2 * note_menu_margin,
		note_menu_height + 2 * note_menu_margin};
}

// The note menu's yellow swatch as pixel_is takes it: 0xRRGGBBAA.
uint32_t note_swatch_colour() {
	unsigned char rgb[3];
	hex_colour(note_yellow, rgb);
	return uint32_t(rgb[0]) << 24 | uint32_t(rgb[1]) << 16
		| uint32_t(rgb[2]) << 8 | 0xff;
}

// The checks every edge shares, once the strip has arrived on it: the
// placed box and the centring along the edge, then the shot. The shelf
// checks differ — only a bottom toolbar moves it.
void edge_checks(const char *edge, const char *shot_name) {
	LayerSurface *toolbar = toolbar_layer();
	wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
	const char *place = std::strcmp(edge, "left") == 0
		? "8 px from the left edge"
		: std::strcmp(edge, "top") == 0 ? "below the bar, 8 px down"
		: std::strcmp(edge, "bottom") == 0 ? "8 px above the bottom edge"
		: "8 px from the right edge";
	check(toolbar != nullptr && toolbar_on_edge(edge),
		std::format("the strip moved to {} (strip {},{} {}x{})", place,
			toolbar != nullptr ? strip.x : -1, toolbar != nullptr ? strip.y : -1,
			toolbar != nullptr ? strip.width : 0, toolbar != nullptr ? strip.height : 0));
	check(toolbar != nullptr && toolbar_centred(strip),
		"the strip is centred along its edge");
	shot(shot_name);
}

// Clicks the notes button (its square, from the placed strip), then
// parks the pointer clear of every edge's strip and card.
void open_notes_flyout() {
	LayerSurface *toolbar = toolbar_layer();
	if (toolbar != nullptr) {
		auto [x, y] = notes_click(toolbar->geo);
		click(x, y);
	}
	park(640, 360);
}

// The open-flyout checks on a landed shot: the yellow swatch row, and
// the rows with it. False while the click that opens it may have been
// lost under load — the caller retries on the next tick (this
// re-clicks on every third), and the shot re-takes itself through
// shot_landed.
bool flyout_open_checks(const char *shot_name, const char *toward) {
	cairo_surface_t *s = load_png(shot_name);
	LayerSurface *toolbar = toolbar_layer();
	wlr_box region = toolbar != nullptr ? flyout_region(toolbar->geo) : wlr_box{};
	int swatch = colour_count(s, region, note_swatch_colour());
	std::vector<Band> rows = bands_in(s, region);
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	if (swatch <= 20 && retries++ < 10) {
		if (retries % 3 == 1) {
			open_notes_flyout();
		}
		return false;
	}
	retries = 0;
	check(swatch > 20, std::format("the note flyout opened {} the strip, "
		"toward the screen's centre ({} yellow swatch pixels)", toward, swatch));
	check(rows.size() >= 2, std::format("the flyout's rows with it "
		"({} bands)", rows.size()));
	return true;
}

// The folded checks on a landed shot: the swatch gone. False while the
// second click — the fold — may have been lost.
bool flyout_folded_checks(const char *shot_name) {
	cairo_surface_t *s = load_png(shot_name);
	LayerSurface *toolbar = toolbar_layer();
	wlr_box region = toolbar != nullptr ? flyout_region(toolbar->geo) : wlr_box{};
	int swatch = colour_count(s, region, note_swatch_colour());
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	if (swatch > 20 && retries++ < 10) {
		if (retries % 3 == 1) {
			open_notes_flyout(); // the button again: the toggle folds
		}
		return false;
	}
	retries = 0;
	check(swatch <= 20, "the flyout folded on the button's second click");
	return true;
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
		check(sys == private_system.address && ses == private_session.address,
			"the bar's bus addresses are the private pair (from /proc/environ)");
		// The weather fetch is answered at once by the curl stand-in, so
		// the widget is up quickly; a short wait still lets the bar lay
		// its widgets out before the positions are read.
		return 2000;
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
		// The theme switch (canvas.theme) is the right-most widget of all:
		// record it for the theme phase and take it out before matching the
		// indicators, which are the right-most runs of what is left.
		if (!runs.empty()) {
			theme_x = runs.back().centre();
			runs.pop_back();
		}
		check(runs.size() >= 2 + mandatory.size(),
			std::format("every indicator of this machine is on the bar "
				"({} runs past the logo and the theme switch; the clock, the "
				"weather and {} indicators)", runs.size(), mandatory.size()));
		if (runs.size() < 2 + mandatory.size()) {
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
				power_x = ind.x; // right-anchored: stable across the whole run
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
			// The canned fetch always answers, so the widget is always
			// there: its panel is checked like any other, its rows read
			// against the fixture's days.
			check(centre.size() >= 2, "the weather widget is on the bar "
				"(the canned fetch decided at once)");
			if (centre.size() >= 2) {
				indicators.push_back({"weather", weather_panel, centre.back()});
				indicators.back().wx_flow = true;
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
		// weather when it loaded) centred, the stand-in's one tray item, the
		// theme switch, and this machine's indicators — nothing left over,
		// so the broken manifest's plugin contributed nothing and was
		// skipped. base_runs counts the theme switch too: the later reload
		// checks compare against a bar that still carries it.
		cairo_surface_t *s = load_png("bar-plain");
		LayerSurface *layer = bar_layer();
		std::vector<Run> runs = s != nullptr && layer != nullptr
			? bar_runs(s, layer->geo) : std::vector<Run>{};
		cairo_surface_destroy(s);
		check(layer != nullptr && runs.size() >= 1, "the bar is still mapped after the walk");
		base_runs = runs.size();
		if (!runs.empty()) { // the theme switch, right-most of all
			runs.pop_back();
		}
		std::size_t mandatory = 4 + (has_sysfs_class("backlight") ? 1 : 0);
		std::size_t keep = runs.size() >= 1 + mandatory ? runs.size() - 1 - mandatory : 0;
		std::size_t centre = 0;
		for (std::size_t i = 1; i < 1 + keep; i++) {
			if (std::abs(runs[i].centre() - screen_w / 2.0) <= 250) {
				centre++;
			}
		}
		check(keep - centre == 1, std::format("every run on the bar is accounted for "
			"({} runs: logo, {} centred, 1 tray, {} indicators, the theme switch "
			"— the broken manifest's plugin was skipped)", base_runs, centre, mandatory));
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
		if (!appeared && plugin_retries++ < 150) { // a reload recompiles
			// every plugin and remakes the bar: in flight under load
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
		if (!widened && plugin_retries++ < 150) { // the reload is in flight under load
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
		if (!gone && plugin_retries++ < 150) { // the reload is in flight under load
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
		// The overview's toolbar sits over this card's right edge, the
		// card drawn above it: keep the scan column short of the strip,
		// or the toolbar's glyphs pose as the card's rows.
		popup_box.width = std::min(popup_box.width, strip_left - popup_box.x);
		shot("overview-network");
		return 700;
	}
	case 28: { // click CafeGuest: the secured unknown network's row, in
		// "Other networks" — the row that asks for a password
		if (!settle_advance("overview-network")) {
			return -400; // the rows are still settling under load
		}
		WifiOffRows off = wifi_off_rows();
		std::vector<Band> rows = panel_rows("overview-network");
		// The head, "Known networks" with HomeNet and OldCafe, "Other
		// networks" with CafeGuest, CorpNet and LibraryFree: 8 rows.
		// a taller panel means the walk's disconnect never landed: the
		// fixture is still connected, and no row can be trusted
		if (rows.size() < 8 || rows.size() > 9) {
			step = 99; // the panel moved on without us
			return 100;
		}
		check(rows.size() >= 8 && rows.size() <= 9, std::format("the panel "
			"lists the stand-in's five networks in the overview ({} rows: "
			"head, two headers, 5 rows)", rows.size()));
		wifi_base = int(rows.size());
		wifi_last_top = rows.back().top;
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + off.cafe);
		return 500;
	}
	case 29: // the shot must come a tick after the selection click
		shot("overview-selected");
		return 400;
	case 30: {
		if (!settle_advance("overview-selected")) {
			return -400; // the panel is still settling under load
		}
		WifiOffRows off = wifi_off_rows();
		std::vector<Band> rows = panel_rows("overview-selected");
		// as in the walk: the field lands under the row, and the rows
		// below shift by its height
		bool armed = rows.size() > wifi_base
			|| (rows.size() == wifi_base && rows.back().top > wifi_last_top + 25);
		if (!armed) { // the panel moved on without us
			step = 99;
			return 100;
		}
		check(armed, "clicking the secured network armed its password field "
			"in the overview (the rows below shifted)");
		field_box = wlr_box{popup_box.x,
			static_cast<int>(std::round(popup_box.y + off.cafe + 22)),
			popup_box.width, 30};
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + off.cafe + 37);
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
		step = 40; // on to the toolbar: the same world, in the overview
		return -100; // without the advance: every other step moves on alone
	}
	// ---- the overview's toolbar ----
	// A step range of its own, after the overview phase.
	case 40: // the toolbar shows only in the overview: none yet
		check(toolbar_layer() == nullptr,
			"the toolbar is not mapped while the output is out of the overview");
		chord(KEY_TAB, KEY_LEFTMETA); // Super+Tab enters the overview again
		return 400;
	case 41: {
		LayerSurface *toolbar = toolbar_layer();
		if (toolbar == nullptr && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(toolbar != nullptr, "entering the overview mapped the toolbar "
			"(a layer surface of the shell client)");
		if (toolbar != nullptr) {
			check(wl_resource_get_client(toolbar->wlr->surface->resource) == bar_client(),
				"the toolbar belongs to the shell client");
		}
		shot("toolbar");
		return 700;
	}
	case 42: { // the buttons, read off the screenshot: the strip's column
		// is opaque — its own glyphs are the only lit pixels in it — and
		// the strip is the cluster of bands closest to the output's
		// vertical centre, so stray lit pixels cannot pose as buttons.
		cairo_surface_t *s = load_png("toolbar");
		std::vector<Band> all = bands_in(s, {strip_left, 0, toolbar_button, screen_h});
		cairo_surface_destroy(s);
		std::vector<Band> best;
		double best_distance = screen_h;
		for (std::size_t i = 0; i < all.size();) {
			std::vector<Band> cluster{all[i]};
			for (std::size_t j = i + 1; j < all.size()
					&& all[j].top - cluster.back().bottom <= toolbar_button; j++) {
				cluster.push_back(all[j]);
			}
			double distance = std::abs((cluster.front().top + cluster.back().bottom) / 2.0
				- screen_h / 2.0);
			if (cluster.size() > best.size()
					|| (cluster.size() == best.size() && distance < best_distance)) {
				best = cluster;
				best_distance = distance;
			}
			i += cluster.size();
		}
		if (best.size() < 3 && retries++ < 20) { // the slide-in may still finish
			shot("toolbar");
			return -700;
		}
		retries = 0;
		check(best.size() >= 3, std::format("the toolbar holds every registered "
			"button ({} glyph bands in its column: act, panel, menu)", best.size()));
		if (best.size() < 3) {
			step = 99; // positions cannot be trusted; nothing more to check
			return 300;
		}
		check(std::abs((best.front().top + best.back().bottom) / 2.0
				- screen_h / 2.0) <= toolbar_button,
			"the strip is vertically centred on the output");
		tb_buttons = best;
		// The action button is the first (order 0): click it.
		tb_x = strip_left + toolbar_button / 2.0;
		tb_y = tb_buttons[0].centre();
		click(tb_x, tb_y);
		return 400;
	}
	case 43: { // the proof the action ran: its storage, read back
		std::string marks = read_storage("marks");
		if (marks.find("\"kind\": \"button\"") == std::string::npos && retries++ < 100) {
			if (retries % 10 == 3) {
				click(tb_x, tb_y); // a lost click: send it again
			}
			return -300;
		}
		retries = 0;
		check(marks.find("\"kind\": \"button\"") != std::string::npos,
			"the action button ran its action with its args (the plugin's "
			"storage says so)");
		check(toolbar_layer() != nullptr, "the toolbar stays after an action");
		// The pointer rests on the action button: its label shows beside
		// it, left of the strip.
		shot("toolbar-hover");
		return 700;
	}
	case 44: { // the label, beside the strip on hover
		cairo_surface_t *s = load_png("toolbar-hover");
		std::vector<Band> labels = bands_in(s,
			{strip_left - 160, int(tb_buttons[0].top) - 20, 160 - beside_gap, 40});
		cairo_surface_destroy(s);
		check(!labels.empty(), "hovering a button shows its label beside the strip");
		// The panel button is the second (order 1).
		click(strip_left + toolbar_button / 2.0, tb_buttons[1].centre());
		return 400;
	}
	case 45: {
		LayerSurface *panel = panel_layer("scratch");
		if (panel == nullptr && retries++ < 40) {
			return -100;
		}
		retries = 0;
		check(panel != nullptr, "the panel button opened the panel through the "
			"common trigger");
		if (panel != nullptr) {
			check(wl_resource_get_client(panel->wlr->surface->resource) == bar_client(),
				"the toolbar's panel belongs to the shell client");
		}
		shot("toolbar-panel");
		return 700;
	}
	case 46: { // the panel card: left of the strip, level with the button
		// The card sits left of the strip, centred on the panel button;
		// scan that window, so overview content elsewhere cannot pose as
		// rows.
		cairo_surface_t *s = load_png("toolbar-panel");
		wlr_box card{strip_left - beside_gap - scratch_card,
			int(tb_buttons[1].centre()) - 80, scratch_card, 160};
		std::vector<Band> rows = bands_in(s, card);
		cairo_surface_destroy(s);
		if (rows.size() < 2 && retries++ < 10) {
			shot("toolbar-panel");
			return -700;
		}
		retries = 0;
		check(rows.size() >= 2, "the panel opened beside the toolbar (its two rows "
			"are left of the strip)");
		if (!rows.empty()) {
			double level = (rows.front().top + rows.back().bottom) / 2.0;
			check(std::abs(level - tb_buttons[1].centre()) <= 32,
				"the panel is level with the panel button");
		}
		// Escape closes the panel itself (it holds the keyboard); a click
		// on the empty canvas would instead leave the overview.
		tap(KEY_ESC);
		return 400;
	}
	case 47: {
		if (panel_layers() > 0 && retries++ < 20) {
			return -100;
		}
		retries = 0;
		check(panel_layers() == 0, "the toolbar's panel closed on Escape");
		// The menu button is the third (order 2).
		click(strip_left + toolbar_button / 2.0, tb_buttons[2].centre());
		return 400;
	}
	case 48: // the flyout, out of the toolbar to the left of the button
		shot("toolbar-flyout");
		return 700;
	case 49: { // its rows: a glyph row and a colour-swatch row
		// The flyout sits left of the strip, centred on the menu button.
		cairo_surface_t *s = load_png("toolbar-flyout");
		wlr_box card{strip_left - beside_gap - scratch_card,
			int(tb_buttons[2].centre()) - 80, scratch_card, 160};
		std::vector<Band> rows = bands_in(s, card);
		cairo_surface_destroy(s);
		if (rows.size() < 2 && retries++ < 10) {
			shot("toolbar-flyout");
			return -700;
		}
		retries = 0;
		check(rows.size() >= 2, "the menu button expanded a flyout beside the "
			"toolbar (a glyph row and a swatch row)");
		if (rows.size() < 2) {
			// Nothing was clicked: the fold shot still happens, the
			// choice proof is skipped.
			shot("toolbar-folded");
			step = 50;
			return 700;
		}
		// Choose the first entry: its action runs and the flyout folds.
		tb_x = strip_left - beside_gap - scratch_card / 2.0;
		tb_y = rows[0].centre();
		click(tb_x, tb_y);
		return 400;
	}
	case 50: { // the choice's proof, and the fold
		std::string marks = read_storage("marks");
		if (marks.find("\"kind\": \"menu-one\"") == std::string::npos && retries++ < 100) {
			if (retries % 10 == 3) {
				click(tb_x, tb_y); // a lost click: send it again
			}
			return -300;
		}
		retries = 0;
		check(marks.find("\"kind\": \"menu-one\"") != std::string::npos,
			"choosing a flyout entry ran its action with its args (the "
			"plugin's storage says so)");
		shot("toolbar-folded");
		return 700;
	}
	case 51: { // the flyout must have folded; leaving the overview ends it
		cairo_surface_t *s = load_png("toolbar-folded");
		wlr_box card{strip_left - beside_gap - scratch_card,
			int(tb_buttons[2].centre()) - 80, scratch_card, 160};
		std::vector<Band> rows = bands_in(s, card);
		cairo_surface_destroy(s);
		check(rows.empty(), "choosing an entry folded the flyout");
		check(toolbar_layer() != nullptr, "the toolbar stays after a menu choice");
		tap(KEY_ESC); // Escape leaves the overview, folding everything
		return 400;
	}
	case 52: { // the slide out unmaps the toolbar, panel and flyout with it
		bool gone = toolbar_layer() == nullptr && panel_layers() == 0;
		if (!gone && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(toolbar_layer() == nullptr, "leaving the overview slid the toolbar "
			"out (unmapped)");
		check(panel_layers() == 0, "no panel and no flyout outlived the toolbar");
		step = 60; // on to the theme phase: the switch, both palettes, the reload
		return -100; // without the advance: every other step moves on alone
	}
	case 60: // the theme phase: a window for its title bar, then both modes
		if (theme_x <= 0) {
			std::println("note: the theme switch was not found on the bar; "
				"the theme phase is skipped");
			step = 99;
			return 100;
		}
		::spawn("foot -T themed sh -c 'exec sleep 600'"); // Server's double-forked spawn
		return 500;
	case 61:
		if (titled("themed") == nullptr && retries++ < 100) {
			return -300;
		}
		retries = 0;
		check(titled("themed") != nullptr, "the themed window mapped");
		shot("dark-window");
		return 700;
	case 62: { // the title bar the compositor draws, in dark
		bool dark = title_bar_is("dark-window", config::palette_dark.title_active,
			config::palette_dark.title_inactive);
		if (!dark && retries++ < 10) {
			shot("dark-window"); // re-shoot on this step: retries survive it
			return -700;
		}
		retries = 0;
		check(dark, "the window's title bar is dark (a dark-mode title colour)");
		chord(KEY_TAB, SUPER); // the overview, with its tabs
		return 400;
	}
	case 63: { // the overview opens; the target's tab is the accent fill
		Output *o = out();
		if (!(o != nullptr && o->overview.active()) && retries++ < 40) {
			return -300;
		}
		retries = 0;
		check(o != nullptr && o->overview.active(), "Super+Tab opened the overview");
		if (Desktop *d = o->server.canvas.desktops.find(o->overview.target().desktop);
			d != nullptr) {
			o->overview.set_target(d->frame(), d->id(), false);
		}
		shot("dark-overview");
		return 700;
	}
	case 64: { // the target tab's fill is the dark accent
		bool accent = target_tab_is("dark-overview", config::palette_dark.accent);
		if (!accent && retries++ < 10) {
			shot("dark-overview");
			return -700;
		}
		retries = 0;
		check(accent, "the overview's target tab is the dark accent fill");
		tap(KEY_ESC);
		return 400;
	}
	case 65:
		if (out() != nullptr && out()->overview.active() && retries++ < 20) {
			return -300;
		}
		check(out() != nullptr && !out()->overview.active(), "the overview closed");
		click(theme_x, 13); // the switch: dark -> light
		return 500;
	case 66: { // the switch's two promises: the config and the compositor
		std::string cfg = read_file(shell_config);
		bool written = cfg.find("\"mode\": \"light\"") != std::string::npos;
		bool told = server->theme.mode() == ThemeMode::light;
		if (!(written && told) && retries++ < 50) {
			return -300;
		}
		retries = 0;
		check(written, "clicking the switch wrote theme.mode: light to the scratch shell.json");
		check(told, "the compositor reports theme light (told over the control socket)");
		shot("light-bar");
		return 700;
	}
	case 67: { // the bar itself follows: its background is the light one
		LayerSurface *l = bar_layer();
		bool light = l != nullptr
			&& shot_pixel_is("light-bar", l->geo.x + 3, l->geo.y + 13, shell_bg_light);
		if (!light && retries++ < 40) {
			shot("light-bar");
			return -700;
		}
		retries = 0;
		check(light, "the bar is light (its background pixel is #e1e2e7)");
		click(power_x, 13); // a panel, to see the light panel surface
		return 400;
	}
	case 68:
		if (panel_layer("power") == nullptr && retries++ < 40) {
			return -300;
		}
		retries = 0;
		check(panel_layer("power") != nullptr, "the power panel opened in light mode");
		shot("light-panel");
		return 700;
	case 69: { // the panel's card, against the palette
		wlr_box card = card_box(power_x, power_panel);
		bool light = shot_pixel_is("light-panel", card.x + 5, card.y + 5, shell_panel_light);
		if (!light && retries++ < 10) {
			shot("light-panel");
			return -700;
		}
		retries = 0;
		check(light, "the open panel is light (its card is #f4f5f8)");
		click(60.0, 700.0); // outside: closed again
		return 400;
	}
	case 70:
		if (panel_layers() > 0 && retries++ < 20) {
			return -300;
		}
		check(panel_layers() == 0, "the light panel closed on a click outside");
		shot("light-window");
		return 700;
	case 71: { // the title bar followed too
		bool light = title_bar_is("light-window", config::palette_light.title_active,
			config::palette_light.title_inactive);
		if (!light && retries++ < 10) {
			shot("light-window");
			return -700;
		}
		retries = 0;
		check(light, "the window's title bar is light (a light-mode title colour)");
		chord(KEY_TAB, SUPER);
		return 400;
	}
	case 72: { // the overview again, now in light
		Output *o = out();
		if (!(o != nullptr && o->overview.active()) && retries++ < 40) {
			return -300;
		}
		retries = 0;
		check(o != nullptr && o->overview.active(), "the overview opened in light mode");
		if (Desktop *d = o->server.canvas.desktops.find(o->overview.target().desktop);
			d != nullptr) {
			o->overview.set_target(d->frame(), d->id(), false);
		}
		shot("light-overview");
		return 700;
	}
	case 73: { // the target tab's fill is the light accent
		bool accent = target_tab_is("light-overview", config::palette_light.accent);
		if (!accent && retries++ < 10) {
			shot("light-overview");
			return -700;
		}
		retries = 0;
		check(accent, "the overview's target tab is the light accent fill");
		tap(KEY_ESC);
		return 400;
	}
	case 74: { // a reload must keep the mode: an edit the shell reloads on
		if (out() != nullptr && out()->overview.active() && retries++ < 20) {
			return -300;
		}
		check(out() != nullptr && !out()->overview.active(), "the overview closed");
		// The bar as it is before the edit, for the before/after comparison.
		shot("light-prereload");
		return 700;
	}
	case 75: {
		// Re-enable the user plugin in the config: different bytes, so the
		// shell reloads — and its widget coming back (its files are still in
		// the scratch data dir, its label pushing the left section's ink edge
		// right) is what proves the reload happened, leaving theme.mode:
		// light to prove the mode survived it.
		std::string cfg = read_file(shell_config);
		std::size_t at = cfg.find("\"enabled\": false");
		if (at != std::string::npos) {
			cfg.replace(at, 16, "\"enabled\": true");
		}
		write_file(shell_config, cfg);
		// The after picture, taken as the edit lands: the next step reads it
		// before anything else, and re-shoots it until the reload shows.
		shot("light-reloaded");
		return 700;
	}
	case 76: { // the reload's two proofs: the widget back, the bar still light
		LayerSurface *l = bar_layer();
		cairo_surface_t *pre = load_png("light-prereload");
		cairo_surface_t *re = load_png("light-reloaded");
		double before = l != nullptr && pre != nullptr ? bar_ink_edge(pre, l->geo, true) : 0;
		double after = l != nullptr && re != nullptr ? bar_ink_edge(re, l->geo, true) : 0;
		if (pre != nullptr) {
			cairo_surface_destroy(pre);
		}
		if (re != nullptr) {
			cairo_surface_destroy(re);
		}
		bool reloaded = after >= before + widen_px;
		if (!reloaded && retries++ < 150) { // a reload recompiles every plugin: in flight under load
			shot("light-reloaded");
			return -700;
		}
		retries = 0;
		check(reloaded, std::format("the shell reloaded on the config edit "
			"(the re-enabled plugin's label pushed the left section's edge "
			"from {} to {} px)", before, after));
		bool light = l != nullptr
			&& shot_pixel_is("light-reloaded", l->geo.x + 3, l->geo.y + 13, shell_bg_light);
		if (!light && retries++ < 40) {
			shot("light-reloaded");
			return -700;
		}
		retries = 0;
		check(light, "the reloaded shell kept the light mode (the bar stayed #e1e2e7)");
		click(theme_x, 13); // and back: light -> dark
		return 500;
	}
	case 77: { // the switch's promises, the other way round
		std::string cfg = read_file(shell_config);
		bool written = cfg.find("\"mode\": \"dark\"") != std::string::npos;
		bool told = server->theme.mode() == ThemeMode::dark;
		if (!(written && told) && retries++ < 50) {
			return -300;
		}
		retries = 0;
		check(written, "clicking the switch again wrote theme.mode: dark to the scratch shell.json");
		check(told, "the compositor reports theme dark again");
		shot("dark-restored-bar");
		return 700;
	}
	case 78: { // the bar is dark again
		LayerSurface *l = bar_layer();
		bool dark = l != nullptr
			&& shot_pixel_is("dark-restored-bar", l->geo.x + 3, l->geo.y + 13, shell_bg_dark);
		if (!dark && retries++ < 40) {
			shot("dark-restored-bar");
			return -700;
		}
		retries = 0;
		check(dark, "switching back restored the dark bar (its background is #1a1b26)");
		click(power_x, 13); // and the panel with it
		return 400;
	}
	case 79:
		if (panel_layer("power") == nullptr && retries++ < 40) {
			return -300;
		}
		retries = 0;
		check(panel_layer("power") != nullptr, "the power panel opened in dark mode");
		shot("dark-restored-panel");
		return 700;
	case 80: { // the panel's card is the dark surface again
		wlr_box card = card_box(power_x, power_panel);
		bool dark = shot_pixel_is("dark-restored-panel", card.x + 5, card.y + 5, shell_bg_dark);
		if (!dark && retries++ < 10) {
			shot("dark-restored-panel");
			return -700;
		}
		retries = 0;
		check(dark, "switching back restored the dark panel (its card is #1a1b26)");
		click(60.0, 700.0);
		return 400;
	}
	case 81:
		if (panel_layers() > 0 && retries++ < 20) {
			return -300;
		}
		check(panel_layers() == 0, "the last panel closed");
		step = 82; // on to the adoption phase: an outside theme.set
		return -100; // without the advance: every other step moves on alone
	// ---- adoption and the applications' preference ----
	// A step range of its own, after the theme's: an outside theme.set (a
	// user's script, a keybinding) switches the compositor alone, and the
	// shell must adopt it — the same run also proves the headless
	// compositor never writes the applications' preference the switch
	// would set on a real session.
	case 82: { // the machine's real color-scheme, read before any of it
		scheme_start = real_color_scheme();
		if (scheme_start.empty()) {
			std::println("note: the machine's color-scheme could not be read"
				" (no session bus); the untouched check is skipped");
		} else {
			std::println("note: the machine's color-scheme starts as {}",
				scheme_start);
		}
		return 300;
	}
	case 83: // an outside theme.set, the way a keybinding would send it
		outside_theme_set("light");
		return 400;
	case 84: { // the compositor switched; the shell was told nothing else
		bool light = server->theme.mode() == ThemeMode::light;
		if (!light && retries++ < 50) {
			return -300;
		}
		retries = 0;
		check(light, "the outside theme.set switched the compositor to light");
		shot("adopt-light-bar"); // the shell's answer, caught by the next step
		return 700;
	}
	case 85: { // the shell adopted it: the bar's own palette flipped
		LayerSurface *l = bar_layer();
		bool light = l != nullptr
			&& shot_pixel_is("adopt-light-bar", l->geo.x + 3, l->geo.y + 13, shell_bg_light);
		if (!light && retries++ < 40) {
			shot("adopt-light-bar");
			return -700;
		}
		retries = 0;
		check(light, "the shell adopted the outside switch (the bar is #e1e2e7)");
		return 400;
	}
	case 86: { // adoption keeps the mode, like a click of the switch would
		std::string cfg = read_file(shell_config);
		bool kept = cfg.find("\"mode\": \"light\"") != std::string::npos;
		if (!kept && retries++ < 50) {
			return -300;
		}
		retries = 0;
		check(kept, "the adopted mode was kept (theme.mode: light in the scratch shell.json)");
		return 300;
	}
	case 87: // the bar before the reload, for the before/after comparison
		shot("adopt-prereload");
		return 700;
	case 88: { // an edit the shell reloads on: the user plugin disabled again
		std::string cfg = read_file(shell_config);
		std::size_t at = cfg.find("\"enabled\": true");
		if (at != std::string::npos) {
			cfg.replace(at, 15, "\"enabled\": false");
		}
		write_file(shell_config, cfg);
		return 500;
	}
	case 89: // the shell reloads; the shot of it comes first
		shot("adopt-reloaded");
		return 700;
	case 90: { // the reload's two proofs: it happened, and the mode survived
		LayerSurface *l = bar_layer();
		cairo_surface_t *pre = load_png("adopt-prereload");
		cairo_surface_t *re = load_png("adopt-reloaded");
		double before = l != nullptr && pre != nullptr ? bar_ink_edge(pre, l->geo, true) : 0;
		double after = l != nullptr && re != nullptr ? bar_ink_edge(re, l->geo, true) : 0;
		if (pre != nullptr) {
			cairo_surface_destroy(pre);
		}
		if (re != nullptr) {
			cairo_surface_destroy(re);
		}
		bool reloaded = after <= before - widen_px;
		if (!reloaded && retries++ < 150) { // a reload recompiles every plugin: in flight under load
			shot("adopt-reloaded");
			return -700;
		}
		retries = 0;
		check(reloaded, std::format("the shell reloaded on the config edit "
			"(the disabled plugin's label pulled the left section's edge "
			"from {} to {} px)", before, after));
		bool light = l != nullptr
			&& shot_pixel_is("adopt-reloaded", l->geo.x + 3, l->geo.y + 13, shell_bg_light);
		if (!light && retries++ < 40) {
			shot("adopt-reloaded");
			return -700;
		}
		retries = 0;
		check(light, "the reloaded shell kept the adopted mode (the bar stayed #e1e2e7)");
		return 300;
	}
	case 91: { // and the switch itself still works on the adopted mode
		click(theme_x, 13); // light -> dark, the ordinary way back
		return 500;
	}
	case 92: { // the switch's promises, back from an adopted mode
		std::string cfg = read_file(shell_config);
		bool written = cfg.find("\"mode\": \"dark\"") != std::string::npos;
		bool told = server->theme.mode() == ThemeMode::dark;
		if (!(written && told) && retries++ < 50) {
			return -300;
		}
		retries = 0;
		check(written, "clicking the switch wrote theme.mode: dark to the scratch shell.json");
		check(told, "the compositor reports theme dark again");
		shot("adopt-dark-bar");
		return 700;
	}
	case 93: { // the bar is dark again
		LayerSurface *l = bar_layer();
		bool dark = l != nullptr
			&& shot_pixel_is("adopt-dark-bar", l->geo.x + 3, l->geo.y + 13, shell_bg_dark);
		if (!dark && retries++ < 40) {
			shot("adopt-dark-bar");
			return -700;
		}
		retries = 0;
		check(dark, "the switch restored the dark bar (its background is #1a1b26)");
		return 300;
	}
	case 94: { // the headless run never wrote the applications' preference
		// Every settings write in this process (the compositor's own
		// included) was pointed at the scratch keyfile backend in main():
		// the headless backend is not the real session, so through every
		// switch of this run nothing may have landed even there.
		std::string store = shot_dir + "/xdg/config/glib-2.0/settings/keyfile";
		check(!std::filesystem::exists(store),
			"the headless run wrote no settings store (the preference is "
			"the real session's to set)");
		return 300;
	}
	case 95: { // and the machine's own setting is what it was
		if (scheme_start.empty()) {
			std::println("PASS (unchecked: no session bus to read it)");
			step = 110; // on to the notes either way: they still run
			return -100; // without the advance: every other step moves on alone
		}
		std::string scheme_end = real_color_scheme();
		check(scheme_end == scheme_start, std::format("the machine's "
			"color-scheme is untouched (still {})", scheme_start));
		step = 110; // on to the notes: the sticky-notes plugin end to end
		return -100; // without the advance: every other step moves on alone
	}
	// ---- the notes: the sticky-notes plugin end to end ----
// A step range of its own, after the theme and its adoption.
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
		// The dots follow the manifest's colour order: each colour's dot
		// sits right of the one before it, left to right on the strip.
		double dot_left = -1;
		bool dot_order = true;
		for (const char *colour : note_palette) {
			ColourBox one = dot_on_shot("note-dots", note, colour);
			if (one.count < 30 || (dot_left >= 0 && one.x0 <= dot_left)) {
				dot_order = false;
			}
			if (one.count >= 30) {
				dot_left = one.x0;
			}
		}
		check(dot_order, "the dots sit in the manifest's colour order (yellow, "
			"green, blue, pink, purple, orange)");
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
		step = 138; // on to the note button: the feature's own entry point
		return -100; // without the advance: every other step moves on alone
	}
	// ---- the note button: the feature's own entry point ----
	// The whole feature by its toolbar button, in the overview, after the
	// phases that built the world it lives in.
	case 138: // the overview again: the note button lives on its toolbar
		chord(KEY_TAB, KEY_LEFTMETA);
		return 400;
	case 139: { // the toolbar maps with the overview
		if (toolbar_layer() == nullptr && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		check(toolbar_layer() != nullptr, "the toolbar mapped with the overview");
		shot("note-toolbar");
		return 700;
	}
	case 140: { // the strip's bands: the note button is the last of them
		cairo_surface_t *s = load_png("note-toolbar");
		std::vector<Band> all = bands_in(s, {strip_left, 0, toolbar_button, screen_h});
		cairo_surface_destroy(s);
		std::vector<Band> best;
		double best_distance = screen_h;
		for (std::size_t i = 0; i < all.size();) {
			std::vector<Band> cluster{all[i]};
			for (std::size_t j = i + 1; j < all.size()
					&& all[j].top - cluster.back().bottom <= toolbar_button; j++) {
				cluster.push_back(all[j]);
			}
			double distance = std::abs((cluster.front().top + cluster.back().bottom) / 2.0
				- screen_h / 2.0);
			if (cluster.size() > best.size()
					|| (cluster.size() == best.size() && distance < best_distance)) {
				best = cluster;
				best_distance = distance;
			}
			i += cluster.size();
		}
		if (best.size() < 4 && note_retries++ < 20) { // the slide-in may still finish
			shot("note-toolbar");
			return -700;
		}
		note_retries = 0;
		check(best.size() >= 4, std::format("the strip holds the scratch "
			"plugin's three buttons and the note button below them ({} bands)",
			best.size()));
		if (best.size() < 4) {
			step = 99; // nothing to click; the run ends at its checks
			return 300;
		}
		// The note button is the last band (order 3) and its menu hangs a
		// card left of the strip, from the button's top (the toolbar's
		// geometry).
		note_button_y = best.back().centre();
		note_flyout = {strip_left - beside_gap - scratch_card,
			int(best.back().top) - note_menu_margin, scratch_card,
			note_menu_height + 2 * note_menu_margin};
		click(strip_left + toolbar_button / 2.0, note_button_y);
		return 400;
	}
	case 141: // the note menu unfolds beside the button
		shot("note-menu");
		return 700;
	case 142: { // six colour rows, in the manifest's order
		cairo_surface_t *s = load_png("note-menu");
		std::vector<Band> rows = bands_in(s, note_flyout);
		if (rows.size() != note_menu_rows && note_retries++ < 10) {
			cairo_surface_destroy(s);
			shot("note-menu");
			return -700;
		}
		note_retries = 0;
		check(rows.size() == note_menu_rows, std::format("the note button "
			"expanded a flyout of six colour rows ({} rows)", rows.size()));
		// Each row's swatch, at the row's left: the manifest's colours, in
		// its order, top to bottom.
		bool menu_order = rows.size() == note_menu_rows;
		for (int i = 0; i < note_menu_rows && i < int(rows.size()); i++) {
			unsigned char rgb[3];
			hex_colour(note_palette[i], rgb);
			uint32_t want = uint32_t(rgb[0]) << 24 | uint32_t(rgb[1]) << 16
				| uint32_t(rgb[2]) << 8 | 0xff;
			if (!pixel_is(s, note_flyout.x + note_menu_swatch_x,
					int(rows[i].centre()), want)) {
				menu_order = false;
			}
		}
		cairo_surface_destroy(s);
		check(menu_order, "the flyout's rows are the manifest's colours in its "
			"order (yellow, green, blue, pink, purple, orange)");
		if (rows.size() < 3) { // no Blue row to choose: nothing more can be trusted
			step = 99;
			return 300;
		}
		// Choose Blue, the third row: its action creates the note.
		tb_x = note_flyout.x + note_flyout.width / 2.0;
		tb_y = rows[2].centre();
		click(tb_x, tb_y);
		run_msg("state", "", "note-menu-state");
		return 400;
	}
	case 143: { // the choice created a blue note, its editor over it
		sweep_children();
		if (!msg_done("note-menu-state") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-menu-state");
		if ((notes.size() != 1 || notes.front().color != note_blue)
				&& note_retries++ < 80) {
			if (note_retries % 10 == 3) {
				click(tb_x, tb_y); // a lost click: send it again
			}
			run_msg("state", "", "note-menu-state");
			return -400;
		}
		note_retries = 0;
		check(notes.size() == 1 && notes.front().color == note_blue,
			std::format("choosing Blue on the note menu created a note in that "
				"colour ({})", notes.size() == 1 ? notes.front().color.c_str() : "none"));
		if (!editor_open_with_keyboard() && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		check(editor_open_with_keyboard(),
			"the created note's editor opened over it, holding the keyboard");
		note = notes.front();
		shot("note-menu-folded");
		return 700;
	}
	case 144: { // the flyout folded; closing the editor keeps nothing
		cairo_surface_t *s = load_png("note-menu-folded");
		std::vector<Band> rows = bands_in(s, note_flyout);
		cairo_surface_destroy(s);
		check(rows.empty(), "choosing a colour folded the flyout");
		tap(KEY_ESC); // the editor's Escape: no text, so the note goes too
		run_msg("state", "", "note-menu-closed");
		return 400;
	}
	case 145: { // nothing left: the closed-empty note removed itself
		sweep_children();
		if (!msg_done("note-menu-closed") && note_retries++ < 60) {
			return -300;
		}
		std::vector<NoteEntry> notes = state_notes("note-menu-closed");
		if (!notes.empty() && note_retries++ < 80) {
			run_msg("state", "", "note-menu-closed");
			return -400;
		}
		note_retries = 0;
		check(notes.empty(), "the note closed with no text was removed");
		check(stored_notes().empty(), "nothing is left in the plugin's storage");
		tap(KEY_ESC); // Escape leaves the overview, folding everything
		return 400;
	}
	case 146: { // the run's end: the overview closed, nothing left over
		if (!overview_settled(false) && note_retries++ < 100) {
			return -100;
		}
		note_retries = 0;
		check(overview_settled(false), "the overview closed behind the notes");
		check(editor_layer() == nullptr, "no editor outlived the overview");
		check(toolbar_layer() == nullptr, "the toolbar slid out with the overview");
		step = 150; // on to the network panel's own phase, then the agents
		// panel's (180+)
		return -100;
	}
	// ---- the network panel's own flow: 150-179 ----
	// The walk proved the join-with-password and the connected row's
	// disconnect; this phase drives the rest against the fixture world:
	// the figures and the copy, the band pin, every DNS choice including
	// a typed one, the head's share-as-QR and speed test, the enterprise
	// join, the open join, the forget — and that the panel's tools were
	// only ever asked about the fixture world.
	case 150: // open the network panel on the stand-in's disconnected Wi-Fi
		click(network_x, 13);
		return 400;
	case 151: {
		LayerSurface *panel = panel_layer("network");
		if (panel == nullptr && retries++ < 40) {
			return -100;
		}
		retries = 0;
		check(panel != nullptr, "the network panel opened on its indicator");
		if (panel != nullptr
				&& server->seat->keyboard_state.focused_surface
					!= panel->wlr->surface
				&& retries++ < 20) {
			return -100;
		}
		retries = 0;
		check(server->seat->keyboard_state.focused_surface
				== (panel != nullptr ? panel->wlr->surface : nullptr),
			"the network panel holds the keyboard as it opens");
		popup_box = card_box(network_x, network_panel);
		shot("wifi-open");
		return 700;
	}
	case 152: { // the disconnected shape, then the known join: HomeNet's row
		if (!settle_advance("wifi-open")) {
			return -400; // negative: this step re-runs until the shot settles
		}
		WifiOffRows off = wifi_off_rows();
		std::vector<Band> rows = panel_rows("wifi-open");
		// disconnected: the head, two headers, 5 rows — 8 (a 15-row panel
		// means the walk's disconnect never landed)
		check(rows.size() >= 8 && rows.size() <= 9, std::format("the panel "
			"opened on the disconnected stand-in with its five networks "
			"({} rows: head, two headers, 5 rows)", rows.size()));
		if (rows.size() < 8 || rows.size() > 9) {
			step = 99; // no row to click safely: the run ends at its checks
			return 100;
		}
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + off.home);
		return 500;
	}
	case 153: { // the known join reconnected the stand-in's Wi-Fi, and the
		// figures came with the connection
		std::string state = mock_property(NM_NAME, NM_DEVICE,
			"org.freedesktop.NetworkManager.Device", "State");
		if (state.find("100") == std::string::npos && retries++ < 100) {
			return -400; // the activation is in flight under load
		}
		// (the shape wait below shares this counter: a reset here would
		// let a lagging shape wait forever, as every re-entry resets it)
		check(state.find("100") != std::string::npos,
			"clicking the known network's row reconnected the stand-in's Wi-Fi");
		if (state.find("100") != std::string::npos) {
			fixture_online(true); // the reads come back with the device
		}
		if (!settle_advance("wifi-connected")) {
			return -400; // the connected shape settles: figures, band, DNS
		}
		wifi_base = panel_rows("wifi-connected").size();
		// The figures and the DNS section come from the panel's status
		// read of the fixture, which lags the activation by a poll or
		// more — and the known list's own read lands later still. Keep
		// settling until the fully grown shape is up: every later click's
		// position, and the row counts the later checks compare against,
		// are modelled on it. Sixteen bands: head, four figure rows,
		// band, DNS header and pills, two list headers, five networks,
		// and the background's own ink below the card.
		if (wifi_base < 16 && retries++ < 160) {
			return -400; // the status poll lags the activation
		}
		retries = 0;
		wifi_last_top = panel_rows("wifi-connected").back().top;
		check(wifi_base >= 16, std::format("the panel grew into its "
			"connected shape ({} bands: head, figures, band, DNS, "
			"headers, five networks)",
			wifi_base));
		std::string log = standin_log();
		check(log.find("device show " + std::string(wifi_iface))
				!= std::string::npos,
			"the figures read the fixture interface through its tools");
		check(log.find(std::format("ping -n -c 1 -W 1 {}", wifi_gateway))
				!= std::string::npos,
			"the ping figure probes the fixture gateway");
		return 400;
	}
	case 154: { // the figures copy: click the IP address's value
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + 120, popup_box.y + wifi.figures[3]);
		return 400;
	}
	case 155: { // the copy reached the clipboard under this display
		// The wl-copy stand-in logs the call before the real one under
		// this display has actually set the clipboard: paste until it
		// holds the figure, not just until the call was logged. The
		// paste writes to a file rather than a pipe: reading a child's
		// pipe blocks this process, and this process is also the
		// compositor — its loop must keep answering the paste client
		// or the paste can never answer back. Each entry reaps the
		// previous paste, reads what it wrote, and starts the next.
		static Spawned paste_child{};
		static bool paste_live = false;
		std::string out;
		if (paste_live) {
			if (paste_child.out >= 0) {
				close(paste_child.out);
				paste_child.out = -1;
			}
			reap(paste_child.pid);
			paste_live = false;
			out = read_file(std::format("{}/paste.out", shot_dir));
			while (!out.empty()
					&& (out.back() == '\n' || out.back() == '\r')) {
				out.pop_back();
			}
		}
		if (out != wifi_ip && retries++ < 40) {
			const std::string redirect = std::format(
				"wl-paste > \"{}/paste.out\" 2>/dev/null", shot_dir);
			const char *const paste[] = {"sh", "-c",
				redirect.c_str(), nullptr};
			paste_child = spawn(paste);
			paste_live = paste_child.pid > 0;
			return -400; // the copy lands on the clipboard a beat later
		}
		paste_live = false;
		retries = 0;
		check(out == wifi_ip, std::format("the IP address copied itself "
			"(the clipboard holds {})",
			out.empty() ? "nothing" : out.c_str()));
		return 400;
	}
	case 156: { // the band lock: Automatic off pins the connection's band
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + 311, popup_box.y + wifi.band);
		return 500;
	}
	case 157: { // the pin went to NetworkManager, and the pills appeared
		std::string log = standin_log();
		bool applied = log.find("connection modify " + std::string(wifi_ssid)
				+ " 802-11-wireless.band bg") != std::string::npos
			&& log.find("connection up " + std::string(wifi_ssid))
				!= std::string::npos;
		if (!applied && retries++ < 100) {
			return -400; // the pin is still in flight under load
		}
		retries = 0;
		check(applied, "switching Automatic off pinned the connection to the "
			"2.4 GHz band and reassociated it");
		if (!settle_advance("wifi-band")) {
			return -400; // the pills settle under the header
		}
		std::vector<Band> rows = panel_rows("wifi-band");
		check(rows.size() > wifi_base, std::format("the band pills appeared "
			"under the header ({} rows, {} before)", rows.size(), wifi_base));
		WifiRows pinned = wifi_rows(true, false);
		click(popup_box.x + wifi_band_pill_x(1), popup_box.y + pinned.pills);
		return 500;
	}
	case 158: { // the 5 GHz pill re-pinned the band; Automatic comes back
		std::string log = standin_log();
		bool applied = log.find("connection modify " + std::string(wifi_ssid)
				+ " 802-11-wireless.band a") != std::string::npos;
		if (!applied && retries++ < 100) {
			return -400; // the re-pin is still in flight under load
		}
		retries = 0;
		check(applied, "the 5 GHz pill re-pinned the connection's band");
		if (!settle_advance("wifi-band-5")) {
			return -400; // the highlighted pill settles
		}
		WifiRows wifi = wifi_rows(false, false); // the header's row is fixed
		click(popup_box.x + 311, popup_box.y + wifi.band); // Automatic on
		return 500;
	}
	case 159: { // Automatic restored: three pins in the log, the pills gone
		std::string log = standin_log();
		int pins = standin_calls(log, "connection modify " + std::string(wifi_ssid)
			+ " 802-11-wireless.band");
		if (pins < 3 && retries++ < 100) {
			return -400; // the third pin is still in flight under load
		}
		retries = 0;
		check(pins >= 3, std::format("Automatic came back on ({} band pins "
			"in the log: 2.4, 5, auto)", pins));
		if (!settle_advance("wifi-auto")) {
			return -400; // the pills fold away as the shape settles
		}
		std::vector<Band> rows = panel_rows("wifi-auto");
		check(rows.size() <= wifi_base, std::format("the band pills folded "
			"away with Automatic ({} rows, {} with them)", rows.size(),
			wifi_base));
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + wifi_dns_pill_x(1), popup_box.y + wifi.dns_pills);
		return 500;
	}
	case 160: { // Cloudflare applied to the active connection
		std::string log = standin_log();
		bool applied = log.find("ipv4.dns 1.1.1.1 1.0.0.1") != std::string::npos
			&& log.find("device reapply " + std::string(wifi_iface))
				!= std::string::npos;
		if (!applied && retries++ < 100) {
			return -400; // the DNS change is still in flight under load
		}
		// (the highlight wait below shares this counter: a reset here
		// would let a lagging highlight wait forever, as every re-entry
		// resets it)
		check(applied, "the Cloudflare pill set the connection's DNS and "
			"reapplied it to the interface");
		if (!settle_advance("wifi-dns")) {
			return -400; // the highlighted pill settles
		}
		// The pill's highlight follows the panel's DNS read, a slow poll:
		// re-shot until the tint has moved to the chosen pill.
		bool lit = dns_pill_lit("wifi-dns", 1) && !dns_pill_lit("wifi-dns", 0);
		if (!lit && retries++ < 130) { // shares the applied counter above
			shot("wifi-dns");
			return -400; // the DNS read poll lags the click by seconds
		}
		retries = 0;
		check(lit, "the Cloudflare pill carries the highlight, DHCP lost it");
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + wifi_dns_pill_x(2), popup_box.y + wifi.dns_pills);
		return 500;
	}
	case 161: { // Google applied
		std::string log = standin_log();
		bool applied = log.find("ipv4.dns 8.8.8.8 8.8.4.4") != std::string::npos;
		if (!applied && retries++ < 100) {
			return -400; // the DNS change is still in flight under load
		}
		// (the highlight wait below shares this counter, as in step 160)
		check(applied, "the Google pill set the connection's DNS");
		if (!settle_advance("wifi-dns")) {
			return -400; // the highlighted pill settles
		}
		bool lit = dns_pill_lit("wifi-dns", 2) && !dns_pill_lit("wifi-dns", 1);
		if (!lit && retries++ < 130) { // shares the applied counter above
			shot("wifi-dns");
			return -400; // the DNS read poll lags the click by seconds
		}
		retries = 0;
		check(lit, "the Google pill carries the highlight, Cloudflare lost it");
		wifi_last_top = panel_rows("wifi-dns").back().top;
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + wifi_dns_pill_x(3), popup_box.y + wifi.dns_pills);
		return 500;
	}
	case 162: { // the Custom pill opens its servers field
		if (!settle_advance("wifi-custom")) {
			return -400; // the servers field settles under the pills
		}
		std::vector<Band> rows = panel_rows("wifi-custom");
		// the servers field lands under the pills: the rows below shift
		bool armed = rows.size() > wifi_base
			|| (rows.size() == wifi_base && rows.back().top > wifi_last_top + 25);
		check(armed, std::format("the Custom pill opened its servers field "
			"({} rows, was {}; the rows below shifted)", rows.size(), wifi_base));
		if (!armed) {
			step = 99;
			return 100;
		}
		WifiRows editing = wifi_rows(false, true);
		click(popup_box.x + popup_box.width / 2.0,
			popup_box.y + editing.dns_field);
		return 400;
	}
	case 163: // type the servers and submit: Enter in the field
		type("9.9.9.9 149.112.112.112");
		tap(KEY_ENTER);
		return 400;
	case 164: { // the typed servers applied; the field folded away
		std::string log = standin_log();
		bool applied = log.find("ipv4.dns 9.9.9.9 149.112.112.112")
				!= std::string::npos;
		if (!applied && retries++ < 100) {
			return -400; // the typed servers are still in flight
		}
		// (the highlight wait below shares this counter, as in step 160)
		check(applied,
			"Enter applied the typed DNS servers to the connection");
		if (!settle_advance("wifi-custom-set")) {
			return -400; // the folded shape settles
		}
		std::vector<Band> rows = panel_rows("wifi-custom-set");
		check(rows.size() <= wifi_base, std::format("the servers field folded "
			"away ({} rows, {} with it)", rows.size(), wifi_base));
		bool lit = dns_pill_lit("wifi-custom-set", 3);
		if (!lit && retries++ < 130) { // shares the applied counter above
			shot("wifi-custom-set");
			return -400; // the DNS read poll lags the submit by seconds
		}
		retries = 0;
		check(lit, "the Custom pill carries the highlight once its servers "
			"are set");
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + wifi_dns_pill_x(0), popup_box.y + wifi.dns_pills);
		return 500;
	}
	case 165: { // DHCP: the DNS back to the connection's own
		std::string log = standin_log();
		bool applied = log.find("ipv4.ignore-auto-dns no") != std::string::npos;
		if (!applied && retries++ < 100) {
			return -400; // the DHCP change is still in flight
		}
		// (the highlight wait below shares this counter, as in step 160)
		check(applied, "the DHCP pill gave the DNS back to the connection");
		if (!settle_advance("wifi-dhcp")) {
			return -400; // the highlighted pill settles
		}
		bool lit = dns_pill_lit("wifi-dhcp", 0) && !dns_pill_lit("wifi-dhcp", 3);
		if (!lit && retries++ < 130) { // shares the applied counter above
			shot("wifi-dhcp");
			return -400; // the DNS read poll lags the click by seconds
		}
		check(lit, "the DHCP pill carries the highlight, Custom lost it");
		click(popup_box.x + 241, popup_box.y + wifi_rows(false, false).hero);
		return 500;
	}
	case 166: { // the share view: the payload through the secrets, drawn
		// as its modules — and the matrix has to be a real QR of the
		// fixture's payload, so the ink proves both the light card and
		// the dark modules on it, not just any bright patch
		std::string log = standin_log();
		bool shared = log.find("-s -g 802-11-wireless-security.psk connection show "
				+ std::string(wifi_ssid)) != std::string::npos
			&& log.find(std::format(
				"qrencode -t ASCII -m 4 WIFI:T:WPA;S:{};P:{};;",
				wifi_ssid, wifi_psk)) != std::string::npos;
		if (!shared && retries++ < 100) {
			return -400; // the secrets read and the encode are in flight
		}
		retries = 0;
		check(shared, "the share button fetched the PSK through the "
			"connection's secrets and encoded the WIFI: payload");
		if (!settle_advance("wifi-qr")) {
			return -400; // the QR settles into the panel
		}
		cairo_surface_t *s = load_png("wifi-qr");
		wlr_box qr{popup_box.x + 70, popup_box.y + 45, 200, 180};
		int ink = bright_count(s, qr);
		cairo_surface_destroy(s);
		check(ink > 10000 && ink < 34000, std::format("the QR is drawn "
			"as its modules on the light card ({} bright pixels: card "
			"and quiet zone light, the modules dark)", ink));
		std::vector<Band> rows = panel_rows("wifi-qr");
		if (rows.size() < 2) { // no Back row to click: nothing more to drive
			step = 99;
			return 100;
		}
		click(popup_box.x + popup_box.width / 2.0,
			last_content_row(rows).centre());
		return 500;
	}
	case 167: { // the normal view back; the speed test
		if (!settle_advance("wifi-open2")) {
			return -400; // the normal view settles back
		}
		check(panel_rows("wifi-open2").size() >= wifi_base,
			"the normal view came back behind the share");
		click(popup_box.x + 273, popup_box.y + wifi_rows(false, false).hero);
		return 500;
	}
	case 168: { // the speed view: the tool's answer as rows
		std::string log = standin_log();
		bool ran = log.find("speedtest --accept-license --accept-gdpr -f json")
				!= std::string::npos;
		if (!ran && retries++ < 100) {
			return -400; // the tool is still running under load
		}
		retries = 0;
		check(ran, "the speed-test button ran the tool");
		if (!settle_advance("wifi-speed")) {
			return -400; // the tool's answer settles as rows
		}
		std::vector<Band> rows = panel_rows("wifi-speed");
		check(rows.size() >= 6, std::format("the tool's answer is drawn as "
			"rows ({} rows: header, download, upload, ping, back)",
			rows.size()));
		if (rows.size() < 2) { // no Back row to click
			step = 99;
			return 100;
		}
		click(popup_box.x + popup_box.width / 2.0,
			last_content_row(rows).centre());
		return 500;
	}
	case 169: { // the normal view back; the password join, through the prompt
		if (!settle_advance("wifi-open3")) {
			return -400; // the normal view settles back
		}
		check(panel_rows("wifi-open3").size() >= wifi_base,
			"the normal view came back behind the speed test");
		wifi_last_top = panel_rows("wifi-open3").back().top;
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.cafe);
		return 500;
	}
	case 170: { // the prompt's field, inline under the row
		if (!settle_advance("wifi-join")) {
			return -400; // the prompt settles under its row
		}
		std::vector<Band> rows = panel_rows("wifi-join");
		// as before: the field lands under the row, the rows below shift
		bool armed = rows.size() > wifi_base
			|| (rows.size() == wifi_base && rows.back().top > wifi_last_top + 25);
		check(armed, std::format("clicking the secured network armed its "
			"password field ({} rows, was {}; the rows below shifted)",
			rows.size(), wifi_base));
		if (!armed) {
			step = 99;
			return 100;
		}
		WifiRows wifi = wifi_rows(false, false);
		field_box = wlr_box{popup_box.x,
			static_cast<int>(std::round(popup_box.y + wifi.cafe + 22)),
			popup_box.width, 30};
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.cafe + 37);
		return 400;
	}
	case 171: // type the password and submit: Enter in the field
		type("correctbattery");
		tap(KEY_ENTER);
		return 400;
	case 172: { // the join went out with the password; on to the enterprise
		bool sent = mock_called(NM_NAME, NM_MANAGER, "AddAndActivateConnection",
			"correctbattery");
		if (!sent && retries++ < 150) {
			return -400; // the activation is in flight under load
		}
		retries = 0;
		check(sent, "Enter in the field sent the activation with the typed "
			"password to the stand-in NetworkManager");
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.corp);
		return 500;
	}
	case 173: { // the enterprise prompt asks for the identity first
		if (!settle_advance("wifi-corp")) {
			return -400; // the identity field settles under the row
		}
		std::vector<Band> rows = panel_rows("wifi-corp");
		// the prompt arms the identity field; the passphrase field
		// takes over only after the identity's Enter (step 174)
		bool armed = rows.size() > wifi_base
			|| (rows.size() == wifi_base
				&& rows.back().top > wifi_last_top + 25);
		check(armed, std::format("clicking the enterprise network armed "
			"its identity field ({} rows, {} before; the rows below "
			"shifted)",
			rows.size(), wifi_base));
		if (!armed) {
			step = 99;
			return 100;
		}
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.corp + 37);
		return 400;
	}
	case 174: // the identity, then Enter: the passphrase field takes over
		type("marek");
		tap(KEY_ENTER);
		return 300;
	case 175: { // the passphrase, then Enter: the join is submitted — the
		// passphrase never travels on a command line
		type("vagrantpass");
		tap(KEY_ENTER);
		return 400;
	}
	case 176: { // the enterprise join: profile created with the identity,
		// the secret fed over stdin, the connection activated
		std::string log = standin_log();
		bool joined = log.find("nmcli connection add") != std::string::npos
			&& log.find("wpa-eap") != std::string::npos
			&& log.find("802-1x.identity marek") != std::string::npos
			&& log.find("nmcli-eap-secret-set") != std::string::npos
			&& log.find("nmcli connection up") != std::string::npos
			&& log.find("vagrantpass") == std::string::npos;
		if (!joined && retries++ < 150) {
			return -400; // the profile creation and activation are in flight
		}
		retries = 0;
		check(joined, "the enterprise join created its profile with the "
			"identity, fed the passphrase to nmcli over stdin (never on the "
			"command line) and activated it");
		// the prompt must fold away first: until it does, the rows below
		// sit shifted and the open network's row is not where the model
		// puts it. A settled shape the size of the base one is the fold.
		if (!settle_advance("wifi-enterprise")) {
			return -400; // the shape stops changing first
		}
		std::vector<Band> rows = panel_rows("wifi-enterprise");
		bool folded = rows.size() <= wifi_base + 1
			&& rows.back().top < wifi_last_top + 15;
		if (!folded && retries++ < 210) {
			return -400; // the prompt folds as the join's write lands
		}
		// the join made CorpNet known: the list re-sorted and the open
		// network is no longer at its modelled place — it is the last
		// content row of the folded shape
		click(popup_box.x + popup_box.width / 2.0,
			last_content_row(rows).centre());
		return 500;
	}
	case 177: { // the open network joins on its click alone; then Forget
		// the mock logs the call with the access point's object path,
		// not its SSID
		bool sent = mock_called(NM_NAME, NM_MANAGER,
			"AddAndActivateConnection", "/AccessPoint/library");
		if (!sent && retries++ < 150) {
			return -400; // the activation is in flight under load
		}
		retries = 0;
		check(sent, "clicking the open network joined it without asking for "
			"anything");
		if (!settle_advance("wifi-library")) {
			return -400; // the joined row settles
		}
		WifiRows wifi = wifi_rows(false, false);
		click(popup_box.x + 315, popup_box.y + wifi.oldcafe);
		return 500;
	}
	case 178: { // the known network was forgotten; and the machine's real
		// NetworkManager was never asked anything
		// The Delete call removes the connection object, so its own call
		// log is gone with it: the settings' connection list is the proof.
		std::string connections = busctl("call", NM_NAME,
			"/org/freedesktop/NetworkManager/Settings",
			"org.freedesktop.NetworkManager.Settings", "ListConnections");
		bool gone = connections.find("oldcafe") == std::string::npos;
		if (!gone && retries++ < 150) {
			return -400; // the Forget is in flight under load
		}
		retries = 0;
		check(gone, "Forget removed the known network from the settings");
		std::string log = standin_log();
		// The machine's own interface and SSID: had the panel reached the
		// real NetworkManager, either would be all over this log.
		check(log.find("wlp0s20f3") == std::string::npos
				&& log.find("Acme") == std::string::npos,
			"the panel's tools were only ever asked about the fixture world: "
			"no real interface or SSID in any call");
		shot("wifi-final");
		tap(KEY_ESC); // close the panel: the run leaves nothing open
		return 400;
	}
	case 179: { // the panel closed behind the run
		if (panel_layers() > 0 && retries++ < 20) {
			return -100;
		}
		retries = 0;
		check(panel_layers() == 0, "the network panel closed behind the run");
		step = 180; // the agents panel's own phase follows
		return -100;
	}
	case 180: { // the agents phase starts in the dark palette, whatever
		// the theme phase left: its pixel checks read the dark colours
		if (!shot_landed("agents-mode")) {
			return -100;
		}
		cairo_surface_t *s = load_png("agents-mode");
		LayerSurface *bar = bar_layer();
		bool dark = s != nullptr && bar != nullptr
			&& pixel_is(s, bar->geo.x + 4, bar->geo.y + 13, shell_bg_dark);
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		if (!dark) {
			click(theme_x, 13); // one switch back to dark
		}
		return 700;
	}
	case 181: { // the quiet world: no usage, the widget hidden
		if (!shot_landed("agents-hidden")) {
			return -100;
		}
		LayerSurface *bar = bar_layer();
		cairo_surface_t *s = load_png("agents-hidden");
		std::vector<Run> runs = s != nullptr && bar != nullptr
			? bar_runs(s, bar->geo) : std::vector<Run>{};
		bool dark = s != nullptr && bar != nullptr
			&& pixel_is(s, bar->geo.x + 4, bar->geo.y + 13, shell_bg_dark);
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		check(dark, "the agents phase runs in the dark palette");
		for (const Indicator &ind : indicators) {
			if (std::strcmp(ind.name, "bluetooth") == 0) {
				bt_x = ind.x;
			}
		}
		check(bt_x != 0, "the bluetooth indicator's place is known");
		agents_base_runs = runs.size();
		agents_bar_before_reload = bar_layer();
		drop_usage_fixtures();
		reload_shell();
		return 700;
	}
	case 182: { // the reload brings the widget: one new run, left of
		// bluetooth, between the tray and it
	if ((bar_layer() == nullptr || bar_layer() == agents_bar_before_reload)
			&& usage_retries++ < 100) {
		return -100;
	}
	// The counter carries into the runs wait below: resetting it here would
	// re-zero it on every retry tick and loop forever.
	if (!shot_landed("agents-live")) {
		return -100;
	}
	cairo_surface_t *s = load_png("agents-live");
	LayerSurface *bar = s != nullptr ? bar_layer() : nullptr;
	std::vector<Run> runs = s != nullptr && bar != nullptr
		? bar_runs(s, bar->geo) : std::vector<Run>{};
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	if (runs.size() != agents_base_runs + 1 && usage_retries++ < 130) {
		return -1000; // the collector may still be running
	}
		usage_retries = 0;
		check(runs.size() == agents_base_runs + 1,
			std::format("the agents widget appeared with usage ({} runs, "
				"was {})", runs.size(), agents_base_runs));
		// The run left of the bluetooth indicator's run is the widget:
		// its order sits between the tray's and bluetooth's.
		std::size_t bt = runs.size();
		for (std::size_t i = 0; i < runs.size(); i++) {
			if (bt_x >= runs[i].x0 && bt_x <= runs[i].x1) {
				bt = i;
			}
		}
		if (bt > 0) {
			agents_x = runs[bt - 1].centre();
		}
		check(bt > 0 && agents_x < bt_x,
			"the agents widget sits left of the bluetooth indicator");
		return 500;
	}
	case 183: { // the collector's own record, run directly against the
		// fixture world: every family, figure and fault it should say
		std::vector<JsonObject *> providers = collector_providers();
		check(providers.size() == 4, std::format("four provider families "
			"({} of 4)", providers.size()));
		if (providers.size() != 4) {
			step = 206; // the panel checks below would only cascade
			return 300;
		}
		check(json_string(providers[0], "id") == "claude"
			&& json_string(providers[1], "id") == "gpt"
			&& json_string(providers[2], "id") == "fireworks"
			&& json_string(providers[3], "id") == "anthropic",
			"Claude, GPT, Fireworks and the gateway, in that order");
		check(json_string(providers[0], "title") == "Claude Code"
			&& json_string(providers[0], "tierLabel") == "Max 20x",
			"the Claude tab is Claude Code, its plan Max 20x");
		check(json_string(providers[3], "title") == "Claude (gateway)"
			&& json_string(providers[3], "name") == "Claude GW",
			"the gateway tab is Claude (gateway), chipped Claude GW");
		check(json_string(providers[0], "usageStatusText") == "",
			"the signed-in Claude tab reports no fault");
		JsonArray *limits = json_object_get_array_member(providers[0],
			"limits");
		check(limits != nullptr && json_array_get_length(limits) == 2,
			"the Claude tab carries both limits windows");
		if (limits != nullptr && json_array_get_length(limits) == 2) {
			JsonObject *session = json_array_get_object_element(limits, 0);
			JsonObject *weekly = json_array_get_object_element(limits, 1);
			check(json_string(session, "title") == "Session"
				&& json_string(weekly, "title") == "Weekly",
				"the windows are titled Session and Weekly");
			check(std::abs(json_double(session, "percent") - 0.37) < 0.002
				&& std::abs(json_double(weekly, "percent") - 0.684) < 0.002,
				"the probed shares are 37% and 68.4%");
			check(json_string(session, "resetsAt") != "",
				"the reset times normalise to ISO");
		}
		// The day figures the reference screenshot shows, today flagged
		// and labelled, bold in the panel below.
		JsonArray *days = json_object_get_array_member(providers[0], "days");
		check(days != nullptr && json_array_get_length(days) == 7,
			"seven days on the Claude tab");
		if (days != nullptr && json_array_get_length(days) == 7) {
			JsonObject *today = json_array_get_object_element(days, 6);
			JsonObject *wed = json_array_get_object_element(days, 4);
			JsonObject *thu = json_array_get_object_element(days, 5);
			check(json_object_get_boolean_member(today, "today")
				&& json_string(today, "label") == "Today"
				&& json_int(today, "tokens") == 811500
				&& json_string(today, "tokensText") == "811.5K",
				"today is flagged, labelled and 811.5K");
			check(json_int(wed, "tokens") == 246132594
				&& json_string(wed, "tokensText") == "246.1M",
				"two days ago is 246.1M");
			check(json_int(thu, "tokens") == 324632448
				&& json_string(thu, "tokensText") == "324.6M",
				"yesterday is 324.6M");
		}
		JsonArray *models = json_object_get_array_member(providers[0],
			"models");
		check(models != nullptr && json_array_get_length(models) == 2
			&& json_string(json_array_get_object_element(models, 0), "id")
				== "claude-opus-5-5"
			&& json_string(json_array_get_object_element(models, 0), "name")
				== "Opus 5.5"
			&& json_int(json_array_get_object_element(models, 0), "total")
				== 573878042,
			"Opus 5.5 leads the models at 573.9M, Haiku 4.5 behind it");
		check(json_string(providers[1], "usageStatusText")
				== "Waiting for auth"
			&& json_string(providers[1], "authHelpText")
				== "Run `codex login` to authenticate.",
			"the GPT tab is signed out, with the command that fixes it");
		JsonArray *gpt_limits = json_object_get_array_member(providers[1],
			"limits");
		check(gpt_limits != nullptr && json_array_get_length(gpt_limits) == 0,
			"a signed-out tab carries no limits");
		JsonArray *gpt_models = json_object_get_array_member(providers[1],
			"models");
		check(gpt_models != nullptr && json_array_get_length(gpt_models) >= 2,
			"the GPT tab still shows its figures");
		JsonArray *fw_limits = json_object_get_array_member(providers[2],
			"limits");
		check(fw_limits != nullptr && json_array_get_length(fw_limits) == 0
			&& json_string(providers[2], "title") == "Fireworks",
			"the Fireworks tab is tokens-only");
		usage_requests_base = log_lines(
			std::format("{}/usage-requests.log", shot_dir));
		check(usage_requests_base >= 1, "the bar's own start probe reached "
			"the stand-in");
		return 500;
	}
	case 184: // open the panel on the widget
		click(agents_x, 13);
		return 400;
	case 185: { // the panel maps, belongs to the bar and holds the keyboard
		LayerSurface *panel = panel_layer("agents");
		if (panel == nullptr && usage_retries++ < 60) {
			return -100;
		}
		usage_retries = 0;
		check(panel != nullptr, "the agents panel appeared as a layer surface");
		if (panel == nullptr) {
			step = 206;
			return 300;
		}
		check(wl_resource_get_client(panel->wlr->surface->resource)
				== bar_client(),
			"the agents panel belongs to the bar client");
		if (server->seat->keyboard_state.focused_surface
				!= panel->wlr->surface && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		check(server->seat->keyboard_state.focused_surface
				== panel->wlr->surface,
			"the agents panel holds the keyboard as it opens");
		agents_box = card_box(agents_x, 360);
		shot("agents");
		return 700;
	}
	case 186: { // the panel's own drawing, pixel by pixel: the drawn
		// Claude mark, the two meters at their probed shares, the rows
		cairo_surface_t *s = load_png("agents");
		wlr_box mark{agents_box.x + UiThemePadding, agents_box.y + 16, 32, 32};
		int orange = colour_count(s, mark, claude_mark);
		check(orange > 20, std::format("the hero shows the drawn Claude "
			"mark ({} orange pixels)", orange));
		std::vector<int> fills = meter_fills(s, agents_box, accent_dark);
		int inner = agents_box.width - 2 * UiThemePadding;
		check(fills.size() == 2, std::format("both limits meters drew "
			"({} of 2)", fills.size()));
		if (fills.size() == 2) {
			check(fills[0] >= inner / 4 && fills[0] <= inner / 2
				&& fills[1] > fills[0] && fills[1] >= inner * 0.55
				&& fills[1] <= inner * 0.82,
				std::format("the meters sit at their probed shares "
					"({} and {} of {})", fills[0], fills[1], inner));
		}
		std::vector<Band> bands = text_bands(s, agents_box);
		int rows = 0;
		for (const Band &b : bands) {
			if (b.lit >= 60) {
				rows++;
			}
		}
		check(rows >= 11, std::format("the panel's rows are all there "
			"({} rows: two limits, seven days, two models)", rows));
		// The day bars: each fill scaled to the busiest day of the
		// seven, the second-busiest at three quarters of it. The scan
		// keeps to the bar's own column — meter_fills pads the box it
		// takes, so the column is named through the padding — so the
		// weekday and value texts stay out of the count.
		int track = agents_box.width - 2 * UiThemePadding - 116;
		wlr_box dayBars{agents_box.x + UiThemePadding + 50, agents_box.y,
			track + 2 * UiThemePadding, 0};
		std::vector<int> dayFills = meter_fills(s, dayBars, muted_dark);
		int busiestDay = 0, secondDay = 0;
		for (int fill : dayFills) {
			if (fill > busiestDay) {
				secondDay = busiestDay;
				busiestDay = fill;
			} else if (fill > secondDay) {
				secondDay = fill;
			}
		}
		check(dayFills.size() >= 2 && busiestDay >= track * 0.95
				&& secondDay >= track * 0.68 && secondDay <= track * 0.82,
			std::format("the day bars scale to the busiest day of the "
				"week ({} and {} of a {} px track)", busiestDay, secondDay,
				track));
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		return 700;
	}
	case 187: { // the provider switch: an equal-width chip per family, under
		// the hero; GPT is the second of the four
		cairo_surface_t *s = load_png("agents");
		wlr_box chipRow{agents_box.x + 8, agents_box.y + 58,
			agents_box.width - 16, 28};
		agents_chip_runs = s != nullptr ? bar_runs(s, chipRow)
			: std::vector<Run>{};
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		check(agents_chip_runs.size() >= 4,
			std::format("the switch row carries the four chips "
				"({} runs)", agents_chip_runs.size()));
		if (agents_chip_runs.size() < 4) {
			step = 206;
			return 300;
		}
		// The chips run left to right: Claude, GPT, Fireworks, the gateway.
		// The runs name x inside the chip row's box, so the row's own
		// left edge joins them for the click.
		click(agents_box.x + 8 + agents_chip_runs[1].centre(),
			agents_box.y + 72);
		return 400;
	}
	case 188: { // the GPT tab: the sign-in card, no meters, figures kept
		LayerSurface *panel = panel_layer("agents");
		if (panel == nullptr && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		if (!shot_landed("agents-gpt")) {
			return -100;
		}
		cairo_surface_t *s = load_png("agents-gpt");
		wlr_box card{agents_box.x + 8, agents_box.y + 88,
			agents_box.width - 16, 52};
		int attention = colour_count(s, card, attention_dark);
		check(attention > 30, std::format("the GPT tab shows its sign-in "
			"card ({} attention pixels)", attention));
		check(meter_fills(s, agents_box, accent_dark).empty(),
			"a signed-out tab draws no meters");
		std::vector<Band> bands = text_bands(s, agents_box);
		int rows = 0;
		for (const Band &b : bands) {
			if (b.lit >= 60) {
				rows++;
			}
		}
		check(rows >= 9, std::format("the GPT tab keeps its day and model "
			"figures ({} rows)", rows));
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		return 700;
	}
	case 189: // the Fireworks tab: tokens only
		click(agents_box.x + 8 + agents_chip_runs[2].centre(),
			agents_box.y + 72);
		return 400;
	case 190: { // no card, no meters, the star mark of a token-only family
		LayerSurface *panel = panel_layer("agents");
		if (panel == nullptr && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		if (!shot_landed("agents-fireworks")) {
			return -100;
		}
		cairo_surface_t *s = load_png("agents-fireworks");
		wlr_box card{agents_box.x + 8, agents_box.y + 88,
			agents_box.width - 16, 52};
		wlr_box mark{agents_box.x + UiThemePadding, agents_box.y + 16, 32, 32};
		check(colour_count(s, card, attention_dark) < 10,
			"a token-only tab draws no sign-in card");
		check(meter_fills(s, agents_box, accent_dark).empty(),
			"a token-only tab draws no meters");
		check(colour_count(s, mark, accent_dark) > 20,
			"a token-only family shows the star mark");
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		return 700;
	}
	case 191: // close the panel; the payload moves under it
		click(20, screen_h - 20);
		return 400;
	case 192: { // closed; a new answer waits, and the probe window passes
		if (panel_layers() > 0 && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		check(panel_layers() == 0, "the agents panel closed");
		write_usage_payload("95.0", "72.0");
		return 20000; // past the probe-reuse window, so a reopen re-asks
	}
	case 193: // reopen: the panel-open refresh
		click(agents_x, 13);
		return 400;
	case 194: { // the panel kept its tab (Fireworks, tokens-only); the
		// refreshed limits live on the Claude tab, so switch to it
		LayerSurface *panel = panel_layer("agents");
		if (panel == nullptr && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		click(agents_box.x + 8 + agents_chip_runs[0].centre(),
			agents_box.y + 72);
		return 400;
	}
	case 195: { // the refresh probe happened: the new shares drew, the
		// nearly-spent window turned the attention colour
		LayerSurface *panel = panel_layer("agents");
		if (panel == nullptr && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		if (!shot_landed("agents-refreshed")) {
			return -100;
		}
		cairo_surface_t *s = load_png("agents-refreshed");
		int requests = log_lines(
			std::format("{}/usage-requests.log", shot_dir));
		check(requests > usage_requests_base,
			std::format("the reopen probed again ({} requests, was {})",
				requests, usage_requests_base));
		check(!meter_fills(s, agents_box, attention_dark).empty(),
			"the 95% session window draws in the attention colour");
		check(!meter_fills(s, agents_box, accent_dark).empty(),
			"the weekly window still draws in the accent");
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		return 700;
	}
	case 196: // close; the sign-in itself goes away
		click(20, screen_h - 20);
		return 400;
	case 197: { // closed; the credentials removed, the shell reloads
		if (panel_layers() > 0 && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		std::error_code ec;
		std::filesystem::remove(scratch_home + "/.claude/.credentials.json", ec);
		agents_bar_before_reload = bar_layer();
		reload_shell();
		return 700;
	}
	case 198: { // the reloaded bar keeps the widget: its figures still count
		// As at the first reload: the bar returns at once, the widget only
		// with the collector's answer — the codex handshake takes seconds —
		// so the count is waited for, never assumed.
		if ((bar_layer() == nullptr || bar_layer() == agents_bar_before_reload)
				&& usage_retries++ < 100) {
			return -100;
		}
		if (!shot_landed("agents-signedout-bar")) {
			return -100;
		}
		cairo_surface_t *s = load_png("agents-signedout-bar");
		LayerSurface *bar = s != nullptr ? bar_layer() : nullptr;
		std::vector<Run> runs = s != nullptr && bar != nullptr
			? bar_runs(s, bar->geo) : std::vector<Run>{};
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		if (runs.size() != agents_base_runs + 1 && usage_retries++ < 130) {
			return -1000; // the collector may still be answering
		}
		usage_retries = 0;
		check(runs.size() == agents_base_runs + 1,
			std::format("the widget stays with its figures, signed out or "
				"not ({} runs)", runs.size()));
		return 500;
	}
	case 199: // open the signed-out panel
		click(agents_x, 13);
		return 400;
	case 200: { // the card and the attention meta line, no meters, figures
		LayerSurface *panel = panel_layer("agents");
		if (panel == nullptr && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		if (!shot_landed("agents-signedout")) {
			return -100;
		}
		cairo_surface_t *s = load_png("agents-signedout");
		wlr_box hero{agents_box.x, agents_box.y, agents_box.width, 54};
		wlr_box card{agents_box.x + 8, agents_box.y + 88,
			agents_box.width - 16, 52};
		check(colour_count(s, card, attention_dark) > 30,
			"the signed-out Claude tab shows its sign-in card");
		check(colour_count(s, hero, attention_dark) > 10,
			"the fault names itself in the hero's meta line");
		check(meter_fills(s, agents_box, accent_dark).empty(),
			"no probe, no meters");
		std::vector<Band> bands = text_bands(s, agents_box);
		int rows = 0;
		for (const Band &b : bands) {
			if (b.lit >= 60) {
				rows++;
			}
		}
		check(rows >= 9, std::format("the figures stay beside the card "
			"({} rows)", rows));
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		return 700;
	}
	case 201: // close, then the palette switches
		click(20, screen_h - 20);
		return 400;
	case 202: { // closed; the switch to the light palette
		if (panel_layers() > 0 && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		click(theme_x, 13);
		return 700;
	}
	case 203: { // the light bar confirmed, the panel reopened on it
		if (!shot_landed("agents-light-bar")) {
			return -100;
		}
		LayerSurface *bar = bar_layer();
		cairo_surface_t *s = load_png("agents-light-bar");
		check(bar != nullptr && s != nullptr
			&& pixel_is(s, bar->geo.x + 4, bar->geo.y + 13, shell_bg_light),
			"the bar switched to the light palette");
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		click(agents_x, 13);
		return 400;
	}
	case 204: { // the light panel: its own card colour, the mark unchanged
		LayerSurface *panel = panel_layer("agents");
		if (panel == nullptr && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		if (!shot_landed("agents-light")) {
			return -100;
		}
		cairo_surface_t *s = load_png("agents-light");
		wlr_box mark{agents_box.x + UiThemePadding, agents_box.y + 16, 32, 32};
		int edge_x = agents_box.x + agents_box.width - 6;
		check(pixel_is(s, edge_x, agents_box.y + 70, panel_light),
			"the panel card is the light surface");
		check(colour_count(s, mark, claude_mark) > 20,
			"the Claude mark keeps its colour in the light");
		// The day bars carry the light palette's own muted fill.
		int track = agents_box.width - 2 * UiThemePadding - 116;
		wlr_box dayBars{agents_box.x + UiThemePadding + 50, agents_box.y,
			track + 2 * UiThemePadding, 0};
		check(!meter_fills(s, dayBars, muted_light).empty(),
			"the day bars keep drawing in the light palette");
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		return 700;
	}
	case 205: { // close; back to the dark palette the count reads
		click(20, screen_h - 20);
		if (panel_layers() > 0 && usage_retries++ < 20) {
			return -100;
		}
		usage_retries = 0;
		run_msg("theme.set", R"({"mode":"dark"})", "agents-dark-again");
		return 900; // the palette settles before the reload joins it
	}
	case 206: { // empty the world, reload: the widget hides again
		remove_usage_fixtures();
		agents_bar_before_reload = bar_layer();
		reload_shell();
		return 700;
	}
	case 207: { // the quiet world again, and the run's own proofs: what the
		// stand-ins received, and that nothing real was ever reached
		if ((bar_layer() == nullptr || bar_layer() == agents_bar_before_reload
				|| bar_layer()->geo.height < 20)
				&& usage_retries++ < 100) {
			return -100;
		}
		if (!shot_landed("agents-quiet")) {
			return -100;
		}
		cairo_surface_t *s = load_png("agents-quiet");
		LayerSurface *bar = s != nullptr ? bar_layer() : nullptr;
		std::vector<Run> runs = s != nullptr && bar != nullptr
			? bar_runs(s, bar->geo) : std::vector<Run>{};
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		// The rebuilt bar fills in as each widget's source answers again
		// (the weather waits for its first fetch): look again until it has.
		if (runs.size() != agents_base_runs && retries++ < 40) {
			return -300;
		}
		usage_retries = 0;
		check(runs.size() == agents_base_runs,
			std::format("the widget hid again with the world empty "
				"({} runs, was {} before it)", runs.size(),
				agents_base_runs));
		// The probes: the panel's own token at the stand-in, and the
		// codex RPC the fake app-server answered.
		std::string requests = read_file(
			std::format("{}/usage-requests.log", shot_dir));
		check(requests.find("fixture-token") != std::string::npos,
			"the probe carried the fixture's own token");
		check(requests.find("oauth-2025-04-20") != std::string::npos,
			"the probe carried the oauth beta header");
		std::string codex = read_file(
			std::format("{}/codex-standin.log", shot_dir));
		check(codex.find("initialize") != std::string::npos
			&& codex.find("account/read") != std::string::npos
			&& codex.find("account/rateLimits/read") != std::string::npos,
			"the codex stand-in answered the handshake and both reads");
		// Nothing real: no request ever left for the real endpoint, and
		// the bar's own HOME is the scratch one, not the machine's.
		std::string curls = read_file(
			std::format("{}/curl-standin.log", shot_dir));
		check(curls.find("api.anthropic.com") == std::string::npos,
			"no request ever named the real Anthropic endpoint");
		check(curls.find("usage") != std::string::npos,
			"the usage probe went through the curl stand-in");
		pid_t shell_pid = 0;
		if (bar_client() != nullptr) {
			wl_client_get_credentials(bar_client(), &shell_pid, nullptr,
				nullptr);
		}
		check(shell_pid > 0
				&& proc_env_value(shell_pid, "HOME") == scratch_home,
			"the bar's HOME is the scenario's scratch, never the "
			"machine's");
		check(panel_layers() == 0, "no panel outlived the agents phase");
		check(editor_layer() == nullptr && toolbar_layer() == nullptr,
			"no editor or toolbar outlived the agents phase");
		check(overview_settled(false), "the overview stayed closed");
		step = 208; // on to the recording dot: the bar plugin's own proof
		return -100;
	}

	// ---- the recording-dot phase: 208-214 ----
	// Alt+Print opens the capture toolbar on its recordings, a click records
	// the screen through the real input path, and the plugin's dot is read
	// off the bar itself, clicked with the fake pointer, and seen gone once
	// the recording has stopped — its file left playable in the scratch
	// Videos, never the machine's.
	case 208: { // the quiet bar, the pointer parked clear of everything
		park(640, 400);
		dot_acted = false;
		return shot_landed("norec-bar") ? 400 : -100;
	}
	case 209: { // Alt+Print: the toolbar, its recordings accented
		if (!dot_acted) {
			chord(KEY_SYSRQ, KEY_LEFTALT);
			dot_acted = true;
		}
		if (!server->capture_ui.owns_keyboard() && retries++ < 30) {
			return -100;
		}
		retries = 0;
		check(server->capture_ui.owns_keyboard(),
			"Alt+Print opened the capture toolbar");
		auto [mx, my] = capture_mode_click(3);
		click(mx, my); // record the screen
		dot_acted = false; // the flag next guards the dot's click, not this one
		return 600;
	}
	case 210: { // the recording runs, into the scratch Videos
		if (!server->capture_ui.recording() && retries++ < 40) {
			return -100;
		}
		retries = 0;
		dot_recording = server->capture_ui.recording_path();
		check(server->capture_ui.recording() && !dot_recording.empty()
				&& dot_recording.rfind(scratch_home, 0) == 0
				&& dot_recording.find("/Videos/") != std::string::npos,
			std::format("the toolbar's click records the screen, into the "
				"bar world's scratch Videos ({})", dot_recording));
		return shot_landed("rec-bar") ? 500 : -100;
	}
	case 211: { // the dot: attention red, in the bar's right half
		cairo_surface_t *after = load_png("rec-bar");
		LayerSurface *bar = bar_layer();
		// The palette this run ended in, read off the bar's own background
		// at its right edge, picks the attention colour the dot must wear.
		bool dark = after != nullptr && bar != nullptr
			&& pixel_is(after, screen_w - 3, bar->geo.y + bar->geo.height / 2,
				shell_bg_dark);
		auto [red, dot] = bar != nullptr && after != nullptr
			? attention_span(after, bar->geo,
				dark ? attention_dark : attention_light)
			: std::pair<int, wlr_box>{0, wlr_box{0, 0, 0, 0}};
		check(red > 15,
			std::format("the recording dot is on the bar, in the attention "
				"colour ({} px)", red));
		check(red > 15 && dot.x + dot.width / 2 > screen_w / 2,
			"the dot sits in the bar's right section");
		if (red > 0) {
			dot_x = dot.x + dot.width / 2.0;
		}
		if (after != nullptr) {
			cairo_surface_destroy(after);
		}
		return 400;
	}
	case 212: { // a click on the dot stops it, through the plugin
		if (!dot_acted) { // once: a re-click could open an indicator's panel
			click(dot_x, double(bar_h()) / 2.0);
			dot_acted = true;
		}
		if (server->capture_ui.recording() && retries++ < 40) {
			return -100;
		}
		retries = 0;
		check(!server->capture_ui.recording(),
			"a click on the dot stopped the recording");
		run_msg("state", "", "dot-state");
		return 400;
	}
	case 213: { // the state agrees, and the bar is quiet again
		if (!msg_done("dot-state") && retries++ < 30) {
			return -100;
		}
		retries = 0;
		JsonParser *dot_state_parser = json_parser_new();
		bool dot_state_read = json_parser_load_from_file(dot_state_parser,
			(shot_dir + "/dot-state.out").c_str(), nullptr);
		JsonObject *dot_state = dot_state_read
			&& json_node_get_node_type(json_parser_get_root(dot_state_parser))
				== JSON_NODE_OBJECT
			? json_node_get_object(json_parser_get_root(dot_state_parser))
			: nullptr;
		check(msg_code("dot-state") == 0 && dot_state != nullptr
				&& json_object_has_member(dot_state, "recording")
				&& json_node_get_node_type(json_object_get_member(dot_state,
					"recording")) == JSON_NODE_NULL,
			"the state reports no recording any more");
		g_object_unref(dot_state_parser);
		return shot_landed("stopped-bar") ? 500 : -100;
	}
	case 214: { // the dot gone, the file playable
		cairo_surface_t *after = load_png("stopped-bar");
		LayerSurface *bar = bar_layer();
		bool dark = after != nullptr && bar != nullptr
			&& pixel_is(after, screen_w - 3, bar->geo.y + bar->geo.height / 2,
				shell_bg_dark);
		auto [red, dot] = bar != nullptr && after != nullptr
			? attention_span(after, bar->geo,
				dark ? attention_dark : attention_light)
			: std::pair<int, wlr_box>{0, wlr_box{0, 0, 0, 0}};
		check(red == 0,
			std::format("the dot left the bar with the recording ({} red px)",
				red));
		if (after != nullptr) {
			cairo_surface_destroy(after);
		}
		std::system(std::format(
			"ffprobe -v error -select_streams v:0 "
				"-show_entries stream=width,height -show_entries format=duration "
				"-of csv=p=0 \"{}\" > {}/dot-probe.txt 2>&1",
			dot_recording, shot_dir).c_str());
		int w = 0, h = 0;
		double dur = 0;
		std::string probe = read_file(std::format("{}/dot-probe.txt", shot_dir));
		bool played = std::sscanf(probe.c_str(), "%d,%d %lf", &w, &h, &dur) == 3
			&& w == screen_w && h == screen_h && dur >= 0.5;
		check(played,
			std::format("ffprobe reads the recording as a playing {}x{} file ({})",
				screen_w, screen_h, probe));
		step = 300; // on to the toolbar's edges; the settings phase chains there
		return -100; // without the advance: every other step moves on alone
	}
	case 300: { // the themed window focused, for the shelf's item
		View *v = titled("themed");
		wlr_box f = v != nullptr ? v->frame_box() : wlr_box{};
		if (v != nullptr && server->focused_view != v) {
			click(f.x + f.width / 2.0, f.y + f.height / 2.0);
		}
		if (server->focused_view != v && retries++ < 20) {
			return -400;
		}
		retries = 0;
		check(server->focused_view == v, "the themed window is focused");
		chord(KEY_M, KEY_LEFTMETA); // Super+M minimizes it
		return 400;
	}
	case 301: { // minimized, then the overview the toolbar lives in
		View *v = titled("themed");
		if (v != nullptr && !v->minimized && retries++ < 20) {
			if (retries % 5 == 2) {
				chord(KEY_M, KEY_LEFTMETA); // a chord may be lost: send it again
			}
			return -400;
		}
		retries = 0;
		check(v != nullptr && v->minimized,
			"Super+M minimized the themed window (the shelf's item)");
		chord(KEY_TAB, KEY_LEFTMETA); // the overview: the toolbar's home
		return 400;
	}
	case 302: { // the toolbar on its default right edge, then the first write
		if ((!overview_settled(true) || !toolbar_on_edge("right")) && retries++ < 150) {
			return -400;
		}
		retries = 0;
		LayerSurface *toolbar = toolbar_layer();
		wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
		check(overview_settled(true), "the overview settled for the toolbar");
		check(toolbar != nullptr && toolbar_on_edge("right"),
			"the toolbar mapped 8 px from the right edge, its default");
		check(toolbar != nullptr && toolbar_centred(strip),
			"the strip is centred along its edge");
		check(toolbar != nullptr && strip.height / toolbar_button >= 4,
			std::format("the strip holds its buttons ({} squares)",
				toolbar != nullptr ? strip.height / toolbar_button : 0));
		check(set_toolbar_position("left"),
			"toolbar.position: left written to the scratch shell.json");
		return 800; // the reload the edit triggers takes its time
	}
	case 303: { // the left edge: a side toolbar clears nothing below
		if (!toolbar_on_edge("left") && retries++ < 150) {
			return -400;
		}
		retries = 0;
		edge_checks("left", "edges-left");
		Shelf shelf = overview_shelf_layout(*server->outputs.front());
		check(!shelf.empty(), "the minimized window sits on the shelf");
		check(!shelf.empty()
			&& shelf.front().box.y + shelf.front().box.height
				== screen_h - shelf_bottom_gap,
			"the shelf keeps its place along the bottom (a side toolbar "
			"clears nothing)");
		return 700;
	}
	case 304: { // the note button: its colour flyout, right of the strip
		if (toolbar_layer() == nullptr && retries++ < 20) {
			return -400;
		}
		retries = 0;
		open_notes_flyout();
		return 400;
	}
	case 305: // the flyout open, then folded by the button again
		if (!shot_landed("edges-left-flyout")
				|| !flyout_open_checks("edges-left-flyout", "right of")) {
			return -100;
		}
		open_notes_flyout(); // the button again: the toggle folds
		return 400;
	case 306: { // folded; on to the top edge
		if (!shot_landed("edges-left-folded")
				|| !flyout_folded_checks("edges-left-folded")) {
			return -100;
		}
		check(set_toolbar_position("top"),
			"toolbar.position: top written to the scratch shell.json");
		return 800;
	}
	case 307: { // the top edge: a row below the bar
		if (!toolbar_on_edge("top") && retries++ < 150) {
			return -400;
		}
		retries = 0;
		edge_checks("top", "edges-top");
		return 700;
	}
	case 308: { // the note button: its colour flyout, below the strip
		if (toolbar_layer() == nullptr && retries++ < 20) {
			return -400;
		}
		retries = 0;
		open_notes_flyout();
		return 400;
	}
	case 309: // the flyout open, then folded by the button again
		if (!shot_landed("edges-top-flyout")
				|| !flyout_open_checks("edges-top-flyout", "below")) {
			return -100;
		}
		open_notes_flyout(); // the button again: the toggle folds
		return 400;
	case 310: { // folded; on to the bottom edge
		if (!shot_landed("edges-top-folded")
				|| !flyout_folded_checks("edges-top-folded")) {
			return -100;
		}
		check(set_toolbar_position("bottom"),
			"toolbar.position: bottom written to the scratch shell.json");
		return 800;
	}
	case 311: { // the bottom edge: the shelf above the toolbar
		if (!toolbar_on_edge("bottom") && retries++ < 150) {
			return -400;
		}
		retries = 0;
		edge_checks("bottom", "edges-bottom");
		LayerSurface *toolbar = toolbar_layer();
		wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
		Shelf shelf = overview_shelf_layout(*server->outputs.front());
		check(!shelf.empty(), "the minimized window sits on the shelf");
		check(!shelf.empty() && toolbar != nullptr
			&& shelf.front().box.y + shelf.front().box.height
				== strip.y - shelf_bottom_gap,
			std::format("the shelf rose above the bottom toolbar (its bottom "
				"at {}, the toolbar's top at {})",
				!shelf.empty() ? shelf.front().box.y + shelf.front().box.height : 0,
				toolbar != nullptr ? strip.y : 0));
		return 700;
	}
	case 312: { // the note button: its colour flyout, above the strip
		if (toolbar_layer() == nullptr && retries++ < 20) {
			return -400;
		}
		retries = 0;
		open_notes_flyout();
		return 400;
	}
	case 313: // the flyout open, then folded by the button again
		if (!shot_landed("edges-bottom-flyout")
				|| !flyout_open_checks("edges-bottom-flyout", "above")) {
			return -100;
		}
		open_notes_flyout(); // the button again: the toggle folds
		return 400;
	case 314: { // folded; back to the default right edge
		if (!shot_landed("edges-bottom-folded")
				|| !flyout_folded_checks("edges-bottom-folded")) {
			return -100;
		}
		check(set_toolbar_position("right"),
			"toolbar.position: right written to the scratch shell.json");
		return 800;
	}
	case 315: { // the right edge again, as the run leaves it
		if (!toolbar_on_edge("right") && retries++ < 150) {
			return -400;
		}
		retries = 0;
		edge_checks("right", "edges-right");
		return 700;
	}
	case 316: { // the note button: its colour flyout, left of the strip
		if (toolbar_layer() == nullptr && retries++ < 20) {
			return -400;
		}
		retries = 0;
		open_notes_flyout();
		return 400;
	}
	case 317: // the flyout open, then folded by the button again
		if (!shot_landed("edges-right-flyout")
				|| !flyout_open_checks("edges-right-flyout", "left of")) {
			return -100;
		}
		open_notes_flyout(); // the button again: the toggle folds
		return 400;
	case 318: { // folded; the overview left behind
		if (!shot_landed("edges-right-folded")
				|| !flyout_folded_checks("edges-right-folded")) {
			return -100;
		}
		check(toolbar_layer() != nullptr, "the toolbar stays after its edges");
		tap(KEY_ESC); // Escape leaves the overview, folding everything
		return 400;
	}
	case 319: { // the run's end: the overview closed, the world left tidy
		if ((!overview_settled(false) || toolbar_layer() != nullptr) && retries++ < 150) {
			return -400;
		}
		retries = 0;
		check(overview_settled(false), "the overview closed behind the toolbar's edges");
		check(toolbar_layer() == nullptr, "the toolbar slid out with the overview");
		check(panel_layers() == 0, "no panel outlived the toolbar's edges");
		check(toolbar_position_in_config() == "right",
			"the scratch shell.json ends with toolbar.position: right");
		return 100; // done: the summary prints and the run ends
		// The settings phase (400+) chains here at the merge.
	}
	default:
		finished = true;
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
	// The machine's own environment, saved before this run's isolation
	// replaces it: the adoption phase's last checks read the user's real
	// color-scheme through a child with it restored. And every settings
	// write in this process — the compositor's own preference write above
	// all — is pointed at the scratch keyfile backend, so the machine's
	// dconf is never a write away; the headless backend means even that
	// store should stay empty, which the run checks.
	for (SavedEnv &saved : saved_env) {
		const char *value = std::getenv(saved.name);
		saved.set = value != nullptr && *value != '\0';
		if (saved.set) {
			saved.value = value;
		}
	}
	setenv("GSETTINGS_BACKEND", "keyfile", 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	// Isolation first: the bar may only ever start on the private buses
	// with their stand-ins — when either cannot come up, no bar runs at
	// all, and the machine's buses are never touched. The stand-ins and
	// every later child (the bar above all) inherit the addresses.
	if (!start_private_bus(private_system, "system", "system")
			|| !start_private_bus(private_session, "session", "session")) {
		std::println("FAILED: the private buses did not start; the bar did not run");
		stop_isolation();
		return 2;
	}
	// Python children must never drop bytecode into the source tree
	setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
	setenv("DBUS_SYSTEM_BUS_ADDRESS", private_system.address.c_str(), 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", private_session.address.c_str(), 1);
	if (!start_standins()) {
		std::println("FAILED: the stand-in services did not start; the bar did not run");
		stop_isolation();
		return 2;
	}
	if (!start_curl_standin()) {
		std::println("FAILED: the curl stand-in did not start; the bar did not run");
		stop_isolation();
		return 2;
	}
	if (!start_tool_standins()) {
		std::println("FAILED: the tool stand-ins did not start; the bar did not run");
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
	// The toolbar scenario's own scratch plugin (there from the start; it
	// contributes no bar widget, so the run accounting keeps holding):
	// one action button, one panel button and one menu button, whose
	// action appends its args to the plugin's storage — the read-back is
	// the proof a click ran what it should. The menu's second row names a
	// colour, the first a glyph, so the flyout draws both kinds.
	std::string scratch_plugin = plugins_dir + "/canvas.scratch";
	mkdir_p(scratch_plugin);
	write_file(scratch_plugin + "/plugin.json",
		"{\n\t\"id\": \"canvas.scratch\",\n\t\"name\": \"Scratch\",\n"
		"\t\"version\": \"1.0.0\",\n\t\"apiVersion\": 1,\n"
		"\t\"description\": \"The toolbar scenario's plugin: an action, a panel and a menu button\",\n"
		"\t\"contributes\": {\n"
		"\t\t\"toolbarButtons\": [\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"act\",\n\t\t\t\t\"icon\": \"A\",\n"
		"\t\t\t\t\"label\": \"Act\",\n\t\t\t\t\"action\": \"mark\",\n"
		"\t\t\t\t\"args\": {\"kind\": \"button\"},\n\t\t\t\t\"order\": 0\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"panel\",\n\t\t\t\t\"icon\": \"P\",\n"
		"\t\t\t\t\"label\": \"Panel\",\n\t\t\t\t\"panel\": \"panel\",\n"
		"\t\t\t\t\"order\": 1\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"menu\",\n\t\t\t\t\"icon\": \"M\",\n"
		"\t\t\t\t\"label\": \"Menu\",\n\t\t\t\t\"menu\": [\n"
		"\t\t\t\t\t{\n\t\t\t\t\t\t\"label\": \"One\",\n\t\t\t\t\t\t\"icon\": \"1\",\n"
		"\t\t\t\t\t\t\"action\": \"mark\",\n"
		"\t\t\t\t\t\t\"args\": {\"kind\": \"menu-one\"}\n\t\t\t\t\t},\n"
		"\t\t\t\t\t{\n\t\t\t\t\t\t\"label\": \"Yellow\",\n\t\t\t\t\t\t\"color\": \"#e0af68\",\n"
		"\t\t\t\t\t\t\"action\": \"mark\",\n"
		"\t\t\t\t\t\t\"args\": {\"kind\": \"menu-yellow\"}\n\t\t\t\t\t}\n"
		"\t\t\t\t],\n\t\t\t\t\"order\": 2\n\t\t\t}\n"
		"\t\t],\n"
		"\t\t\"panels\": [\n\t\t\t{\n\t\t\t\t\"id\": \"panel\",\n"
		"\t\t\t\t\"entry\": \"ScratchPanel.qml\"\n\t\t\t}\n\t\t],\n"
		"\t\t\"actions\": [\n\t\t\t{\n\t\t\t\t\"id\": \"mark\",\n"
		"\t\t\t\t\"entry\": \"Mark.qml\"\n\t\t\t}\n\t\t]\n\t}\n}\n");
	write_file(scratch_plugin + "/Mark.qml",
		"import QtQuick\n// The scratch action: appends its args to the plugin's\n"
		"// storage, so the scenario reads back that it ran and what with.\n"
		"Item {\n\tproperty var api: null\n"
	"\tfunction run(args) {\n"
		"\t\tvar marks = api.storage.read(\"marks\")\n"
		"\t\tif (marks == null) {\n\t\t\tmarks = []\n\t\t}\n"
		"\t\tmarks.push(args == null ? {} : args)\n"
		"\t\tapi.storage.write(\"marks\", marks)\n\t}\n}\n");
	write_file(scratch_plugin + "/ScratchPanel.qml",
		"import QtQuick\nimport WaylandCanvas.Ui\n"
		"// The scratch panel: two labelled rows, so the scenario finds the\n"
		"// card where it should be on the screenshot.\n"
		"PanelCard {\n\tpanelWidth: 200\n\tpanelName: \"scratch\"\n"
		"\tPanelRow { glyph: \"1\"; label: \"Scratch one\"; clickable: false }\n"
		"\tPanelRow { glyph: \"2\"; label: \"Scratch two\"; clickable: false }\n}\n");

	setenv("XDG_CONFIG_HOME", (xdg + "/config").c_str(), 1);
	setenv("XDG_DATA_HOME", (xdg + "/data").c_str(), 1);
	setenv("XDG_STATE_HOME", (xdg + "/state").c_str(), 1);
	// The scratch config the theme switch writes theme.mode into (and the
	// theme phase reads back and rewrites to prove the reload keeps it).
	shell_config = xdg + "/config/wayland-canvas/shell.json";
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

	// The agents panel's own world, on this run's scratch: a HOME the
	// collector reads instead of this machine's (any real CLAUDE_CONFIG_DIR
	// or CODEX_HOME would point it back at the machine, so both go), the
	// built collector and a fake codex app-server first on the PATH the
	// bar inherits, and the usage endpoint pointed at this run's own
	// loopback stand-in. The fixtures themselves land only in the agents
	// phase — until then the panel's world is empty and its widget hidden,
	// which is the phase's first check.
	scratch_home = shot_dir + "/home";
	mkdir_p(scratch_home);
	// A run that died inside the agents phase leaves its fixture world
	// behind, and a stale agents widget would throw off every run count
	// after it: each run starts from the quiet world, its stand-ins' logs
	// empty, so the proofs at the end read only this run's traffic.
	remove_usage_fixtures();
	for (const char *log :
			{"usage-requests.log", "codex-standin.log", "curl-standin.log"}) {
		write_file(std::format("{}/{}", shot_dir, log), "");
	}
	unsetenv("CLAUDE_CONFIG_DIR");
	unsetenv("CODEX_HOME");
	setenv("HOME", scratch_home.c_str(), 1);
	std::string tool_bin = shot_dir + "/tool-bin";
	mkdir_p(tool_bin);
	write_file(tool_bin + "/codex",
		"#!/usr/bin/env python3\n"
		"# Generated by tests/scenarios/bar.cpp: the codex app-server\n"
		"# stand-in. It answers the JSON-RPC handshake and the account\n"
		"# reads the usage collector makes; account/rateLimits/read is\n"
		"# refused — the signed-out answer — so the GPT tab shows its\n"
		"# sign-in card. Every method is logged.\n"
		"import json, os, sys\n"
		"log = os.path.join(os.environ.get('CANVAS_TMP', 'tmp'),\n"
		"    'scenarios', 'bar', 'codex-standin.log')\n"
		"def reply(obj):\n"
		"    sys.stdout.write(json.dumps(obj) + '\\n')\n"
		"    sys.stdout.flush()\n"
		"for line in sys.stdin:\n"
		"    line = line.strip()\n"
		"    if not line:\n"
		"        continue\n"
		"    with open(log, 'a') as out:\n"
		"        out.write(line + '\\n')\n"
		"    try:\n"
		"        req = json.loads(line)\n"
		"    except ValueError:\n"
		"        continue\n"
		"    if 'id' not in req:\n"
		"        continue\n"
		"    if req.get('method') == 'initialize':\n"
		"        reply({'id': req['id'], 'result': {'info':\n"
		"            {'name': 'codex', 'version': 'standin'}}})\n"
		"    elif req.get('method') == 'account/read':\n"
		"        reply({'id': req['id'], 'result':\n"
		"            {'account': {'planType': 'pro'}}})\n"
		"    elif req.get('method') == 'account/rateLimits/read':\n"
		"        reply({'id': req['id'], 'error': {'message': 'Not signed in'}})\n"
		"    else:\n"
		"        reply({'id': req['id'], 'result': {}})\n");
	chmod((tool_bin + "/codex").c_str(), 0755);
	const char *usage_bin = std::getenv("WAYLAND_CANVAS_AGENT_USAGE");
	if (usage_bin != nullptr && *usage_bin != '\0') {
		std::string dir = std::filesystem::path(usage_bin).parent_path();
		const char *path = std::getenv("PATH");
		setenv("PATH", std::format("{}:{}:{}", tool_bin, dir,
			path != nullptr ? path : "").c_str(), 1);
	}
	write_usage_payload("37.0", "68.4");
	const char *py = std::getenv("CANVAS_BAR_STANDIN_PY");
	const char *standins = std::getenv("CANVAS_BAR_STANDINS");
	std::string agent_script = standins != nullptr && *standins != '\0'
		? (std::filesystem::path(standins).parent_path()
			/ "bar-standins/agent-usage.py").string() : std::string{};
	if (py == nullptr || *py == '\0' || agent_script.empty()) {
		std::println("FAILED: the usage stand-in environment is missing; "
			"the bar did not run");
		stop_isolation();
		return 2;
	}
	const char *const standin_argv[] = {py, agent_script.c_str(),
		static_cast<const char *>(nullptr)};
	Spawned standin_child = spawn(standin_argv);
	std::string ready = standin_child.pid > 0
		? ready_line(standin_child, 30) : std::string{};
	if (!ready.starts_with("READY ")) {
		std::println("FAILED: the usage endpoint stand-in did not become "
			"ready; the bar did not run");
		reap(standin_child.pid);
		stop_isolation();
		return 2;
	}
	usage_standin_pid = standin_child.pid;
	close(standin_child.out);
	setenv("WAYLAND_CANVAS_CLAUDE_USAGE_URL",
		std::format("http://127.0.0.1:{}/usage",
			ready.substr(6)).c_str(), 1);


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
	// A compositor that stops by itself (its own shutdown, a lost backend)
	// ends the run early: without this the scenario would still exit 0.
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	stop_children();
	stop_isolation();
	return failures != 0;
}
