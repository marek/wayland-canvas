// End-to-end session scenario: the shell plugins a login needs — the
// notification server, the locker's PAM check and the password dialog —
// driven in a private world. Before the compositor starts, the scenario
// brings up a dbus-daemon system bus and session bus of its own under
// tmp/scenarios/session/bus/ and points every child at them, so the
// machine's own notification server, polkit agent and logind session are
// never reached. XDG config, data and state are scratch directories, and
// the PAM service the locker checks against is a scratch one that accepts
// one known password through pam_exec — never the real login stack and
// never a real password.
#include <algorithm>
#include <cairo.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <poll.h>
#include <print>
#include <ranges>
#include <signal.h>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core/config.h"
#include "core/server.h"
#include "core/wlr.h"
#include "platform/output.h"
#include "windows/view.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false;
uint32_t t = 90000;
pid_t grim_pid = -1;
std::string shot_dir;
std::string xdg;
std::string bus_dir; // the private buses' sockets, kept short (see below)
std::string pam_dir; // the scratch PAM service directory
std::string marker_dir; // what the notification actions write into
wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
wlr_keyboard kbd{};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};

constexpr const char *password = "canvas-scratch-password";
constexpr int screen_w = 1280, screen_h = 720;

struct PrivateBus {
	pid_t pid = -1;
	std::string address;
};

PrivateBus private_system, private_session;

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
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
}

std::string read_text(const std::string &path) {
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
	return out;
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

// ---------------------------------------------------------- children

struct Spawned {
	pid_t pid = -1;
	int out = -1;
};

Spawned spawn_child(const char *const argv[]) {
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
		prctl(PR_SET_PDEATHSIG, SIGKILL); // a killed scenario leaks no bus
		execvp(argv[0], const_cast<char *const *>(argv));
		_exit(127);
	}
	close(fds[1]);
	return pid > 0 ? Spawned{pid, fds[0]} : Spawned{};
}

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

// A dbus-daemon of the run's own: everything on it is allowed, it exists
// for this run only, and no machine service is reachable through it.
bool start_private_bus(PrivateBus &bus, const char *type, const char *name) {
	// A unix socket path is capped near 108 bytes, which a scenario folder
	// under a worktree already spends: the buses live in the shortest
	// scratch path there is.
	std::string dir = bus_dir;
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
	Spawned child = spawn_child(argv);
	if (child.pid <= 0) {
		return false;
	}
	if (!first_line(child).starts_with("unix:")) {
		kill(child.pid, SIGKILL);
		waitpid(child.pid, nullptr, 0);
		return false;
	}
	bus.pid = child.pid;
	bus.address = "unix:path=" + socket;
	return true;
}

void stop_isolation() {
	reap(private_session.pid);
	reap(private_system.pid);
}

// ------------------------------------------------------------- shots

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

cairo_surface_t *load_png(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	cairo_surface_t *raw = cairo_image_surface_create_from_png(path.c_str());
	if (cairo_surface_status(raw) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(raw);
		return nullptr;
	}
	return raw;
}

// How many pixels of the shot differ from the shell's dark background in
// `box` — ink of any kind drawn there.
long ink_in(const char *name, wlr_box box) {
	cairo_surface_t *image = load_png(name);
	if (image == nullptr) {
		return -1;
	}
	cairo_surface_flush(image);
	int w = cairo_image_surface_get_width(image);
	int h = cairo_image_surface_get_height(image);
	int stride = cairo_image_surface_get_stride(image);
	const unsigned char *data = cairo_image_surface_get_data(image);
	long ink = 0;
	for (int y = box.y; y < box.y + box.height && y < h; y++) {
		const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * stride);
		for (int x = box.x; x < box.x + box.width && x < w; x++) {
			if ((row[x] & 0x00ffffffu) != 0) {
				ink++;
			}
		}
	}
	cairo_surface_destroy(image);
	return ink;
}

// How many pixels differ between two shots in `box`.
long pixels_changed(const char *before, const char *after, wlr_box box) {
	cairo_surface_t *a = load_png(before);
	cairo_surface_t *b = load_png(after);
	long changed = -1;
	if (a != nullptr && b != nullptr) {
		cairo_surface_flush(a);
		cairo_surface_flush(b);
		int w = std::min(cairo_image_surface_get_width(a), cairo_image_surface_get_width(b));
		int h = std::min(cairo_image_surface_get_height(a), cairo_image_surface_get_height(b));
		changed = 0;
		for (int y = box.y; y < box.y + box.height && y < h; y++) {
			const auto *ra = reinterpret_cast<const uint32_t *>(
				cairo_image_surface_get_data(a) + y * cairo_image_surface_get_stride(a));
			const auto *rb = reinterpret_cast<const uint32_t *>(
				cairo_image_surface_get_data(b) + y * cairo_image_surface_get_stride(b));
			for (int x = box.x; x < box.x + box.width && x < w; x++) {
				if ((ra[x] & 0x00ffffffu) != (rb[x] & 0x00ffffffu)) {
					changed++;
				}
			}
		}
	}
	if (a != nullptr) {
		cairo_surface_destroy(a);
	}
	if (b != nullptr) {
		cairo_surface_destroy(b);
	}
	return changed;
}

// ------------------------------------------------- the shell's surfaces

// A mapped layer surface of the shell with that namespace, if any.
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

// ------------------------------------------------------ input fakes

void move_to(double x, double y) {
	t += 8;
	wlr_pointer_motion_absolute_event ev = {
		.pointer = &ptr, .time_msec = t, .x = x / screen_w, .y = y / screen_h};
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

void key(uint32_t code, bool pressed) {
	t += 8;
	wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

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
constexpr uint32_t SUPER_SHIFT[] = {KEY_LEFTMETA, KEY_LEFTSHIFT};

// The keycodes of a lowercase ASCII string, so a password can be typed.
void type_text(std::string_view text) {
	static const std::pair<char, uint32_t> keys[] = {
		{'a', KEY_A}, {'b', KEY_B}, {'c', KEY_C}, {'d', KEY_D}, {'e', KEY_E},
		{'f', KEY_F}, {'g', KEY_G}, {'h', KEY_H}, {'i', KEY_I}, {'j', KEY_J},
		{'k', KEY_K}, {'l', KEY_L}, {'m', KEY_M}, {'n', KEY_N}, {'o', KEY_O},
		{'p', KEY_P}, {'q', KEY_Q}, {'r', KEY_R}, {'s', KEY_S}, {'t', KEY_T},
		{'u', KEY_U}, {'v', KEY_V}, {'w', KEY_W}, {'x', KEY_X}, {'y', KEY_Y},
		{'z', KEY_Z}, {'-', KEY_MINUS},
	};
	for (char c : text) {
		for (const auto &[letter, code] : keys) {
			if (letter == c) {
				key(code, true);
				key(code, false);
				break;
			}
		}
	}
}

// --------------------------------------------------------- the world

// A shell action over the control socket, the way a key or a script runs
// one.
void run_action(std::string_view name, std::string_view args) {
	spawn(std::format("'{}' msg action.run '{{\"name\":\"{}\",\"args\":{}}}'"
		" >/dev/null 2>&1", std::getenv("WAYLAND_CANVAS_BIN"), name, args).c_str());
}

// What the stand-in polkit flow recorded, from the plugin's own storage in
// the run's scratch state.
std::string flow_log() {
	return read_text(xdg + "/state/wayland-canvas/plugins/canvas.polkit/test-flow.json");
}

// notify-send on the private session bus, where the shell's own server is
// the only one listening.
void notify(std::string_view args) {
	spawn(std::format("notify-send {} >/dev/null 2>&1", args).c_str());
}


// ------------------------------------------------------------- steps

int run_step() {
	switch (step) {
	case 0: { // the fake devices; the shell is coming up with the compositor
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		move_to(40, 400);
		return 1500;
	}
	case 1: { // the shell is up: its bar is on screen
		if (layer_named("wayland-canvas-bar") == nullptr && retries++ < 200) {
			return -100;
		}
		check(layer_named("wayland-canvas-bar") != nullptr,
			"the shell came up on the private buses");
		return 500;
	}
	case 2: { // a notification arrives
		notify("-a Tests 'Scenario summary' 'The body of it'");
		return 1200;
	}
	case 3: {
		if (layer_named("wayland-canvas-notifications") == nullptr && retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-notifications") != nullptr,
			"a popup appeared as the shell's own surface");
		shot("popup");
		return 300;
	}
	case 4: { // it is drawn top-right, and it took no keyboard
		long ink = ink_in("popup", {screen_w - 360, 26, 352, 160});
		check(ink > 500, std::format("the popup is drawn top-right ({} px of ink)", ink));
		check(server->focused_layer == nullptr,
			"no popup ever holds the keyboard");
		return 0;
	}
	case 5: { // the dismiss key takes it off the screen
		chord(KEY_N, SUPER);
		return 800;
	}
	case 6: {
		if (layer_named("wayland-canvas-notifications") != nullptr && retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-notifications") == nullptr,
			"Super+N took the popup off the screen");
		// Three at once, then all of them at once.
		notify("-a Tests 'One' 'first'");
		notify("-a Tests 'Two' 'second'");
		notify("-a Tests -u critical 'Three' 'critical one'");
		return 1500;
	}
	case 7: {
		if (layer_named("wayland-canvas-notifications") == nullptr && retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-notifications") != nullptr,
			"three notifications show as popups");
		chord(KEY_N, SUPER_SHIFT);
		return 800;
	}
	case 8: {
		if (layer_named("wayland-canvas-notifications") != nullptr && retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-notifications") == nullptr,
			"Super+Shift+N took every popup off the screen");
		return 300;
	}
	case 9: { // the history kept them: the bell's panel lists them
		// Which slot of the bar's right section the bell took depends on
		// what this machine's own widgets (battery, bluetooth) offer, so
		// the strip is walked from the right until the history panel is
		// the one that opens. A panel another widget opens is closed again
		// before the next try, and the theme switch's own slot is left
		// alone.
		if (layer_named("wayland-canvas-panel-history") != nullptr) {
			return 300;
		}
		if (retries > 28) {
			check(false, "the bell was not found along the bar");
			return 300;
		}
		if (layer_named("wayland-canvas-panel-history") == nullptr) {
			key(KEY_ESC, true);
			key(KEY_ESC, false);
		}
		click(screen_w - 40 - 9 * retries, 13);
		retries++;
		return -1000;
	}
	case 10: {
		const LayerSurface *panel = layer_named("wayland-canvas-panel-history");
		check(panel != nullptr, "the bell opens the history panel");
		shot("history");
		return 300;
	}
	case 11: {
		const LayerSurface *panel = layer_named("wayland-canvas-panel-history");
		if (panel != nullptr) {
			long ink = ink_in("history", {panel->geo.x, panel->geo.y,
				panel->geo.width, panel->geo.height});
			check(ink > 1000, std::format("the history panel lists what arrived "
				"({} px of ink)", ink));
		}
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		return 500;
	}
	case 12: { // silenced: a notification arrives and no popup shows
		spawn(std::format("'{}' msg action.run "
			"'{{\"name\":\"canvas.notifications/silence\",\"args\":{{\"on\":true}}}}'"
			" >/dev/null 2>&1", std::getenv("WAYLAND_CANVAS_BIN")).c_str());
		return 800;
	}
	case 13: {
		notify("-a Tests 'Silent one' 'nothing should show'");
		return 1500;
	}
	case 14: {
		check(layer_named("wayland-canvas-notifications") == nullptr,
			"a silenced notification shows no popup");
		spawn(std::format("'{}' msg action.run "
			"'{{\"name\":\"canvas.notifications/silence\",\"args\":{{\"on\":false}}}}'"
			" >/dev/null 2>&1", std::getenv("WAYLAND_CANVAS_BIN")).c_str());
		return 800;
	}
	case 15: { // an action button on a popup reaches the sender
		spawn(std::format("notify-send -a Tests -A go=Open 'With an action' "
			"'click it' > '{}/action.txt' 2>/dev/null", shot_dir).c_str());
		return 1500;
	}
	case 16: {
		if (layer_named("wayland-canvas-notifications") == nullptr
				&& retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-notifications") != nullptr,
			"a notification with an action shows its popup");
		shot("action-popup");
		return 300;
	}
	case 17: {
		// The action button sits at the bottom left of the card, inside
		// the column the plugin lays out: the card is 340 wide against the
		// output's right edge, below the bar.
		click(screen_w - 340 - 8 + 10 + 40, 26 + 8 + 86);
		return 1500;
	}
	case 18: {
		std::string chosen = read_text(shot_dir + "/action.txt");
		if (chosen.find("go") == std::string::npos && retries < 20) {
			// The card's height follows its text: walk down its left edge
			// until the button is hit.
			click(screen_w - 340 - 8 + 10 + 40, 26 + 8 + 70 + 6 * retries);
			retries++;
			return -600;
		}
		check(chosen.find("go") != std::string::npos, std::format(
			"clicking the action reached the sender ('{}')", chosen));
		return 500;
	}
	case 19: { // expiry: a normal popup goes by itself, a critical one stays
		spawn("notify-send -a Tests -t 1000 'Short' 'goes by itself'");
		return 600;
	}
	case 20: {
		check(layer_named("wayland-canvas-notifications") != nullptr,
			"the short notification is on screen");
		return 2500; // past its own timeout
	}
	case 21: {
		if (layer_named("wayland-canvas-notifications") != nullptr
				&& retries++ < 40) {
			return -300;
		}
		check(layer_named("wayland-canvas-notifications") == nullptr,
			"a notification with a timeout leaves the screen by itself");
		spawn("notify-send -a Tests -u critical 'Critical' 'waits for the user'");
		return 1200;
	}
	case 22: {
		if (layer_named("wayland-canvas-notifications") == nullptr
				&& retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-notifications") != nullptr,
			"a critical notification shows");
		return 4000; // longer than any normal popup's life
	}
	case 23: {
		check(layer_named("wayland-canvas-notifications") != nullptr,
			"a critical notification stays until it is dismissed");
		chord(KEY_N, SUPER_SHIFT);
		return 800;
	}
	case 24: { // the bus name is the session's one notification server
		check(layer_named("wayland-canvas-notifications") == nullptr,
			"and the dismiss-all key takes it off the screen");
		spawn(std::format(
			"busctl --address='{}' --user call org.freedesktop.DBus "
			"/org/freedesktop/DBus org.freedesktop.DBus RequestName su "
			"org.freedesktop.Notifications 4 > '{}/name.txt' 2>&1",
			private_session.address, shot_dir).c_str());
		return 1200;
	}
	case 25: {
		std::string answer = read_text(shot_dir + "/name.txt");
		// 1 is "you are the owner now", 3 "someone else holds it".
		check(answer.find("u 3") != std::string::npos, std::format(
			"a second notification daemon does not take the name from the "
			"shell ('{}')", answer));
		spawn("notify-send -a Tests 'Still ours' 'the shell is the server'");
		return 1500;
	}
	case 26: {
		if (layer_named("wayland-canvas-notifications") == nullptr
				&& retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-notifications") != nullptr,
			"and it keeps serving the session's notifications");
		chord(KEY_N, SUPER_SHIFT);
		return 800;
	}
	case 27: { // the lock plugin: the shortcut locks the session
		chord(KEY_L, SUPER);
		return 1500;
	}
	case 28: {
		if (!server->lock.locked() && retries++ < 100) {
			return -100;
		}
		check(server->lock.locked(), "the lock shortcut locked the session");
		shot("locked");
		return 300;
	}
	case 29: { // the clock keeps time while locked: a shot just past the next minute
		int second = static_cast<int>(std::time(nullptr) % 60);
		return (60 - second) * 1000 + 2000;
	}
	case 30:
		shot("locked-next-minute");
		return 300;
	case 31: { // a wrong password keeps it locked
		long moved = pixels_changed("locked", "locked-next-minute",
			{screen_w / 2 - 150, 280, 300, 80});
		check(moved > 100, std::format("the lock clock moved on to the next minute "
			"({} px of it changed)", moved));
		long ink = ink_in("locked", {screen_w / 2 - 200, screen_h / 2 - 120, 400, 240});
		check(ink > 1000, std::format("the locker drew its card ({} px of ink)", ink));
		type_text("wrong-password");
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 2500;
	}
	case 32: {
		check(server->lock.locked(), "a wrong password leaves the session locked");
		type_text(password);
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 2500;
	}
	case 33: {
		if (server->lock.locked() && retries++ < 100) {
			return -100;
		}
		check(!server->lock.locked(), "the right password unlocked the session");
		return 300;
	}
	case 34: { // the password dialog, on the plugin's stand-in flow: the
		// machine's own polkitd is never registered with.
		run_action("canvas.polkit/test-dialog",
			"{\"message\":\"Install a package\",\"user\":\"tester\","
			"\"password\":\"canvas-scratch-password\"}");
		return 1200;
	}
	case 35: {
		if (layer_named("wayland-canvas-polkit") == nullptr && retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-polkit") != nullptr,
			"a privileged action opens the password dialog");
		check(server->focused_layer != nullptr
				|| server->seat->keyboard_state.focused_surface != nullptr,
			"the dialog holds the keyboard while it is open");
		shot("polkit");
		return 300;
	}
	case 36: { // what it says, then a wrong password
		long ink = ink_in("polkit", {screen_w / 2 - 190, screen_h / 2 - 90, 380, 180});
		check(ink > 2000, std::format("the dialog shows the action and its field "
			"({} px of ink)", ink));
		type_text("wrong-password");
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 1200;
	}
	case 37: {
		check(layer_named("wayland-canvas-polkit") != nullptr,
			"a wrong password leaves the dialog open");
		check(flow_log().find("refused") != std::string::npos,
			"polkit was told the answer was wrong");
		type_text(password);
		key(KEY_ENTER, true);
		key(KEY_ENTER, false);
		return 1200;
	}
	case 38: {
		if (layer_named("wayland-canvas-polkit") != nullptr && retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-polkit") == nullptr,
			"the right password closes the dialog");
		check(flow_log().find("authenticated") != std::string::npos,
			"polkit was told the password was right");
		// And a cancel: Escape ends the request without an answer.
		run_action("canvas.polkit/test-dialog",
			"{\"message\":\"Something else\",\"password\":\"never typed\"}");
		return 1200;
	}
	case 39: {
		if (layer_named("wayland-canvas-polkit") == nullptr && retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-polkit") != nullptr,
			"a second request opens the dialog again");
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		return 1200;
	}
	case 40: {
		if (layer_named("wayland-canvas-polkit") != nullptr && retries++ < 100) {
			return -100;
		}
		check(layer_named("wayland-canvas-polkit") == nullptr,
			"Escape cancels the dialog");
		check(flow_log().find("cancelled") != std::string::npos,
			"polkit was told the request was cancelled");
		return 300;
	}
	case 41: {
		finished = true;
		wl_display_terminate(server->display);
		return -1;
	}
	}
	finished = true;
	wl_display_terminate(server->display);
	return -1;
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

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr) {
		std::fprintf(stderr, "session: $WAYLAND_CANVAS_BIN is not set\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/session", root);
	bus_dir = std::format("{}/sb", root);
	std::error_code ec;
	std::filesystem::remove_all(shot_dir, ec);
	mkdir_p(shot_dir);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	setenv("GSETTINGS_BACKEND", "keyfile", 1);
	setenv("PYTHONDONTWRITEBYTECODE", "1", 1);

	// Isolation first: without both private buses nothing of the shell may
	// start, so the machine's notification server and polkit agent are
	// never reached.
	if (!start_private_bus(private_system, "system", "y")
			|| !start_private_bus(private_session, "session", "n")) {
		std::println("FAILED: the private buses did not start; the shell did not run");
		stop_isolation();
		return 2;
	}
	setenv("DBUS_SYSTEM_BUS_ADDRESS", private_system.address.c_str(), 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", private_session.address.c_str(), 1);

	// The scratch PAM service the locker checks against: pam_exec hands the
	// typed password to a script that compares it with one known string.
	// The real login stack is never read and no real password is typed.
	pam_dir = shot_dir + "/pam.d";
	mkdir_p(pam_dir);
	std::string checker = shot_dir + "/check-password.sh";
	write_file(checker, std::format(
		"#!/usr/bin/env bash\n"
		"# The scenario's own password check: pam_exec hands the typed\n"
		"# password on stdin (expose_authtok).\n"
		"read -r given\n"
		"[ \"$given\" = '{}' ]\n", password));
	chmod(checker.c_str(), 0755);
	write_file(pam_dir + "/wayland-canvas-scenario", std::format(
		"auth required pam_exec.so expose_authtok quiet {}\n"
		"account required pam_permit.so\n", checker));

	xdg = shot_dir + "/xdg";
	mkdir_p(xdg + "/config/wayland-canvas");
	mkdir_p(xdg + "/data");
	mkdir_p(xdg + "/state");
	// The locker is pointed at the scratch service through the plugin's
	// own settings; everything else is the shipped default.
	write_file(xdg + "/config/wayland-canvas/shell.json", std::format(
		"{{\n\t\"plugins\": {{\n\t\t\"canvas.lock\": {{\n\t\t\t\"settings\": {{\n"
		"\t\t\t\t\"pamService\": \"wayland-canvas-scenario\",\n"
		"\t\t\t\t\"pamConfigDirectory\": \"{}\"\n\t\t\t}}\n\t\t}}\n\t}}\n}}\n",
		pam_dir));
	setenv("XDG_CONFIG_HOME", (xdg + "/config").c_str(), 1);
	setenv("XDG_DATA_HOME", (xdg + "/data").c_str(), 1);
	setenv("XDG_STATE_HOME", (xdg + "/state").c_str(), 1);
	marker_dir = shot_dir + "/markers";
	mkdir_p(marker_dir);

	server.emplace();
	Options options; // the shell starts with the compositor: it is the point
	if (!server->init(options) || !server->start()) {
		server.reset();
		stop_isolation();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display);
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	stop_isolation();
	return failures != 0;
}
