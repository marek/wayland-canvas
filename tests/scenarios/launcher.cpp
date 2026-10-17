// End-to-end launcher scenario through fake keyboard/pointer devices, plus a
// custom desktop entry installed into $XDG_DATA_HOME before the server starts.
// Writes only under $CANVAS_TMP (meson passes the repo's tmp/).
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/wlr.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0, views_before = 0;
bool finished = false; // the run reached its end; see main()
uint32_t t = 90000;
pid_t grim_pid = -1;
wlr_pointer ptr{};
wlr_keyboard kbd{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
std::string shot_dir;
std::string xdg_dir;
std::string marker_path;
std::string canvas_conf; // this run's scratch canvas.conf (written on start)
bool harness_ignores_sigpipe = false; // as this process was started; see main()

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

int mapped() {
	int n = 0;
	for (View *v : server->views) {
		n += v->mapped;
	}
	return n;
}

// What the custom entry's command wrote into the marker file — its SigIgn
// mask and its XDG_CURRENT_DESKTOP — once both lines are there in full.
struct Marker {
	unsigned long long sigign;
	std::string desktop;
};
std::optional<Marker> read_marker() {
	FILE *f = std::fopen(marker_path.c_str(), "r");
	if (f == nullptr) {
		return std::nullopt;
	}
	char first[128] = {}, second[128] = {};
	bool read = std::fgets(first, sizeof first, f) != nullptr
		&& std::fgets(second, sizeof second, f) != nullptr;
	std::fclose(f);
	if (!read || std::strncmp(first, "SigIgn:", 7) != 0
			|| std::strchr(second, '\n') == nullptr) {
		return std::nullopt;
	}
	std::string desktop = second;
	desktop.pop_back();
	return Marker{std::strtoull(first + 7, nullptr, 16), desktop};
}

// A whole file's text, or nullopt when it is missing.
std::optional<std::string> read_file(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		return std::nullopt;
	}
	std::string text;
	char chunk[4096];
	std::size_t n = 0;
	while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
		text.append(chunk, n);
	}
	std::fclose(f);
	return text;
}

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

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

void type(std::string_view s) {
	struct KeyMap {
		char c;
		uint32_t code;
	};
	constexpr KeyMap map[] = {
		{'a', KEY_A}, {'b', KEY_B}, {'c', KEY_C}, {'d', KEY_D}, {'e', KEY_E}, {'f', KEY_F},
		{'g', KEY_G}, {'h', KEY_H}, {'i', KEY_I}, {'j', KEY_J}, {'k', KEY_K}, {'l', KEY_L},
		{'m', KEY_M}, {'n', KEY_N}, {'o', KEY_O}, {'p', KEY_P}, {'q', KEY_Q}, {'r', KEY_R},
		{'s', KEY_S}, {'t', KEY_T}, {'u', KEY_U}, {'v', KEY_V}, {'w', KEY_W}, {'x', KEY_X},
		{'y', KEY_Y}, {'z', KEY_Z}, {' ', KEY_SPACE},
	};
	for (char c : s) {
		for (const KeyMap &m : map) {
			if (m.c == c) {
				tap(m.code);
			}
		}
	}
}

void click(double x, double y) {
	t += 8;
	wlr_pointer_motion_absolute_event m = {
		.pointer = &ptr, .time_msec = t, .x = x / 1280.0, .y = y / 720.0};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &m);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
	for (int pressed = 1; pressed >= 0; pressed--) {
		t += 8;
		wlr_pointer_button_event b = {.pointer = &ptr, .time_msec = t, .button = BTN_LEFT,
			.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
		wl_signal_emit_mutable(&ptr.events.button, &b);
		wl_signal_emit_mutable(&ptr.events.frame, &ptr);
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
	case 1:
		// The first run wrote canvas.conf with every default.
		if (std::optional<std::string> text = read_file(canvas_conf)) {
			check(*text == settings::template_text(),
				"the first run wrote canvas.conf with every default");
		} else {
			check(false, "canvas.conf was written on the first run");
		}
		// A lone Super tap runs nothing by default; Super+Space opens.
		tap(KEY_LEFTMETA);
		check(!server->launcher.is_open(),
			"a lone Super tap runs nothing by default");
		key(KEY_LEFTMETA, true);
		tap(KEY_SPACE);
		key(KEY_LEFTMETA, false);
		check(server->launcher.is_open(), "Super+Space opened the launcher");
		shot("open");
		return 500;
	case 2: {
		Output *o = server->output_under_cursor();
		std::optional<Launcher::Panel> panel = server->launcher.panel(*o);
		int pw = panel ? panel->box.width : 0, ph = panel ? panel->box.height : 0;
		check(panel && pw > 0, std::format("launcher panel rendered ({}x{})", pw, ph));
		type("foot");
		shot("filtered");
		return 500;
	}
	case 3:
		views_before = mapped();
		tap(KEY_ENTER);
		check(!server->launcher.is_open(), "Enter launched the selection and closed the launcher");
		return 300;
	case 4:
		if (mapped() <= views_before && retries++ < 100) return -100;
		check(mapped() == views_before + 1, "the launched app (foot) mapped a window");
		shot("launched");
		return 400;
	case 5: // logo used as a modifier must not open the launcher
		key(KEY_LEFTMETA, true);
		tap(KEY_T);
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(), "Super+T did not open the launcher on release");
		return 100;
	case 6: // its own chord closes the open launcher; Escape does too
		key(KEY_LEFTMETA, true);
		tap(KEY_SPACE);
		key(KEY_LEFTMETA, false);
		check(server->launcher.is_open(), "Super+Space opened it");
		key(KEY_LEFTMETA, true);
		tap(KEY_SPACE);
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(), "its own chord closed it");
		key(KEY_LEFTMETA, true);
		tap(KEY_SPACE);
		key(KEY_LEFTMETA, false);
		check(server->launcher.is_open(), "Super+Space opened it again");
		tap(KEY_ESC);
		check(!server->launcher.is_open(), "Escape closed it");
		key(KEY_LEFTMETA, true);
		tap(KEY_SPACE);
		key(KEY_LEFTMETA, false);
		type("zzqqx");
		shot("no-match");
		return 500;
	case 7:
		click(20, 20);
		check(!server->launcher.is_open(), "a click outside closed it");
		key(KEY_LEFTMETA, true);
		tap(KEY_SPACE);
		key(KEY_LEFTMETA, false);
		type("fo");
		return 300;
	case 8: {
		// Click the first result row: launches it.
		Output *o = server->output_under_cursor();
		std::optional<Launcher::Panel> panel = server->launcher.panel(*o);
		wlr_box box = panel ? panel->box : wlr_box{};
		views_before = mapped();
		click(box.x + box.width / 2.0,
			box.y + 12 + config::launcher_search_height + 8 + config::launcher_row_height / 2.0);
		check(!server->launcher.is_open(), "clicking a result launched it and closed the launcher");
		return 300;
	}
	case 9:
		if (mapped() <= views_before && retries++ < 100) return -100;
		check(mapped() == views_before + 1, "clicked app mapped a window");
		return 300;
	case 10: // the custom desktop entry from $XDG_DATA_HOME: find it by name
		key(KEY_LEFTMETA, true);
		tap(KEY_SPACE);
		key(KEY_LEFTMETA, false);
		check(server->launcher.is_open(), "launcher reopened for the custom entry");
		type("zydeco");
		shot("custom-entry");
		return 300;
	case 11:
		views_before = mapped();
		tap(KEY_ENTER);
		check(!server->launcher.is_open(), "Enter launched the custom entry and closed the launcher");
		return 400;
	case 12: {
		std::optional<Marker> marker = read_marker();
		if (!marker && retries++ < 100) return -100;
		check(marker.has_value(), "the custom entry's Exec wrote the marker file");
		// The compositor ignores SIGPIPE for itself; what it starts must not
		// inherit that ignore.
		unsigned long long ign = marker ? marker->sigign : 0;
		bool ignored = (ign & (1ull << (SIGPIPE - 1))) != 0;
		check(marker && ignored == harness_ignores_sigpipe, std::format(
			"the launched command got SIGPIPE as the compositor found it ({}, SigIgn {:x})",
			harness_ignores_sigpipe ? "ignored" : "default", ign));
		// Apps and the portal they talk to know which desktop they are in.
		check(marker && marker->desktop == "wayland-canvas:wlroots", std::format(
			"the launched command sees XDG_CURRENT_DESKTOP=wayland-canvas:wlroots ({})",
			marker ? marker->desktop : "-"));
		return 300;
	}
	case 13:
		if (mapped() <= views_before && retries++ < 200) return -100;
		check(mapped() == views_before + 1, "the custom entry opened foot (window mapped)");
		return 0;
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

// Installs a spec-valid custom entry into $XDG_DATA_HOME/applications; its
// Exec uses double quotes, writes its signal dispositions into the marker
// file and opens foot.
void install_custom_entry() {
	std::string apps = xdg_dir + "/applications";
	mkdir_p(apps);
	unlink(marker_path.c_str());
	std::string desktop = apps + "/scenario-custom.desktop";
	FILE *f = std::fopen(desktop.c_str(), "w");
	if (f == nullptr) {
		std::fprintf(stderr, "fopen %s: %s\n", desktop.c_str(), std::strerror(errno));
		std::exit(2);
	}
	std::fprintf(f, "[Desktop Entry]\n");
	std::fprintf(f, "Type=Application\n");
	std::fprintf(f, "Name=Zydeco Marker\n");
	std::fprintf(f, "Exec=sh -c \"grep SigIgn /proc/self/status > %s;"
		" printenv XDG_CURRENT_DESKTOP >> %s; exec foot -T scenariocustom\"\n",
		marker_path.c_str(), marker_path.c_str());
	std::fclose(f);
}

} // namespace

int main() {
	wlr_log_init(WLR_ERROR, nullptr);
	struct sigaction found{};
	sigaction(SIGPIPE, nullptr, &found);
	harness_ignores_sigpipe = found.sa_handler == SIG_IGN;
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp"; // meson passes the repo's tmp/ via env
	shot_dir = std::format("{}/scenarios/launcher", root);
	xdg_dir = std::format("{}/scenarios/launcher/xdg", root);
	marker_path = std::format("{}/scenarios/launcher/custom-marker", root);
	canvas_conf = std::format("{}/scenarios/launcher/xdg-config"
		"/wayland-canvas/canvas.conf", root);
	mkdir_p(shot_dir);
	install_custom_entry();
	// Before the server starts: the launcher scans $XDG_DATA_HOME/applications first.
	setenv("XDG_DATA_HOME", xdg_dir.c_str(), 1);
	// A conf left by an earlier run would make this run's start no first
	// run at all, and the template it checks is the build's own.
	unlink(canvas_conf.c_str());
	// Before the server starts: canvas.conf lives in this run's scratch,
	// never in the user's real ~/.config.
	setenv("XDG_CONFIG_HOME",
		std::format("{}/scenarios/launcher/xdg-config", root).c_str(), 1);
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
	return failures != 0;
}
