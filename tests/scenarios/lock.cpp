// End-to-end lock scenario: ext-session-lock-v1 against the scenarios' own
// locker (tests/clients/lock-client.c). Locking hides the windows on screen
// and in a capture, keys, clicks and gestures reach only the locker, a
// killed locker leaves the session locked, the next locker takes it over
// and unlocks it, and the triggers — the `lock` action and the idle
// timeout, with an idle inhibitor holding it off — ask the shell to lock.
// Writes only under $CANVAS_TMP; never touches the machine's logind
// session, and the run's canvas.conf lives in its own scratch config dir.
#include <cairo.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
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
#include "core/config.h"
#include "core/server.h"
#include "core/wlr.h"
#include "platform/output.h"
#include "render/capture.h"
#include "windows/view.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false;
uint32_t t = 90000;
pid_t grim_pid = -1;
pid_t locker_pid = -1; // the locker under test
pid_t sub_pid = -1; // the events subscriber
int locker_out = -1; // its stdout, read line by line
std::string locker_lines; // everything it has said so far
std::string shot_dir;
std::string events_path;
wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
wlr_keyboard kbd{};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};

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

cairo_surface_t *load_png(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	cairo_surface_t *raw = cairo_image_surface_create_from_png(path.c_str());
	if (cairo_surface_status(raw) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(raw);
		return nullptr;
	}
	return raw;
}

// config.h's palette is 0xRRGGBBAA; a cairo pixel is 0xAARRGGBB.
constexpr uint32_t as_argb(uint32_t rgba) {
	return (rgba >> 8) | (rgba << 24);
}

// The share of pixels of `name` that are the colour `argb`, ignoring alpha.
double share_of_colour(const char *name, uint32_t argb) {
	cairo_surface_t *image = load_png(name);
	if (image == nullptr) {
		return -1;
	}
	cairo_surface_flush(image);
	int w = cairo_image_surface_get_width(image);
	int h = cairo_image_surface_get_height(image);
	int stride = cairo_image_surface_get_stride(image);
	const unsigned char *data = cairo_image_surface_get_data(image);
	long hits = 0;
	for (int y = 0; y < h; y++) {
		const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * stride);
		for (int x = 0; x < w; x++) {
			if ((row[x] & 0x00ffffffu) == (argb & 0x00ffffffu)) {
				hits++;
			}
		}
	}
	cairo_surface_destroy(image);
	return w * h > 0 ? static_cast<double>(hits) / (w * h) : -1;
}

// ------------------------------------------------------ the locker

// Starts the scenarios' locker with its stdout on a pipe.
bool start_locker(bool inhibit_idle) {
	const char *bin = std::getenv("CANVAS_LOCK_CLIENT");
	if (bin == nullptr || *bin == '\0') {
		return false;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}
	locker_pid = fork();
	if (locker_pid == 0) {
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, nullptr);
		close(fds[0]);
		dup2(fds[1], 1);
		close(fds[1]);
		if (inhibit_idle) {
			execl(bin, "lock-client", "--inhibit-idle",
				static_cast<char *>(nullptr));
		} else {
			execl(bin, "lock-client", static_cast<char *>(nullptr));
		}
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	locker_out = fds[0];
	locker_lines.clear();
	return locker_pid > 0;
}

// Everything the locker has printed since it started.
std::string_view locker_said() {
	char buf[512];
	ssize_t got = 0;
	while (locker_out >= 0 && (got = read(locker_out, buf, sizeof buf)) > 0) {
		locker_lines.append(buf, static_cast<std::size_t>(got));
	}
	return locker_lines;
}

int locker_count(std::string_view word) {
	std::string_view said = locker_said();
	int count = 0;
	for (std::size_t at = said.find(word); at != std::string_view::npos;
			at = said.find(word, at + 1)) {
		count++;
	}
	return count;
}

void reap_locker() {
	if (locker_pid > 0) {
		kill(locker_pid, SIGKILL);
		waitpid(locker_pid, nullptr, 0);
		locker_pid = -1;
	}
	if (locker_out >= 0) {
		close(locker_out);
		locker_out = -1;
	}
}

// ------------------------------------------------------ input fakes

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

// A three-finger swipe through the same entry points libinput's events take.
void swipe(int fingers, double dx, double dy) {
	t += 8;
	server->gestures.swipe_begin(t, static_cast<uint32_t>(fingers));
	for (int i = 0; i < 10; i++) {
		t += 8;
		server->gestures.swipe_update(t, dx / 10, dy / 10);
	}
	t += 8;
	server->gestures.swipe_end(t, false);
}

// ------------------------------------------------------ the scene

View *foot_window() {
	for (View *view : server->views) {
		if (view->mapped) {
			return view;
		}
	}
	return nullptr;
}

Output &output() {
	return *server->outputs.front();
}

bool output_settled() {
	return !output().camera.panning() && !output().camera.zooming();
}

// ------------------------------------------------------------- steps

int run_step() {
	switch (step) {
	case 0: { // the fake devices, one foot window, and an events subscriber
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		spawn("foot -T lock sh -c 'echo lock scenario; exec sleep 600'");
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
	case 1: { // the window is up, nothing is locked
		View *view = foot_window();
		if ((view == nullptr || !output_settled()) && retries++ < 100) {
			return -100;
		}
		check(view != nullptr, "one foot window mapped");
		check(!server->lock.locked(), "the session starts unlocked");
		move_to(40, 40);
		return 1500;
	}
	case 2: { // the unlocked screen: the window is on it
		shot("unlocked");
		return 300;
	}
	case 3: { // the lock action asks the shell's locker to lock
		double fill = share_of_colour("unlocked", as_argb(config::lock_fill));
		check(fill >= 0 && fill < 0.9,
			std::format("the unlocked screen is not an opaque fill ({:.2f})", fill));
		chord(KEY_L, SUPER);
		return 400;
	}
	case 4: { // ...as the `action` event the shell runs
		std::string events = read_file(events_path);
		check(events.find(config::lock_action) != std::string::npos,
			"the lock shortcut pushed the locker's action to the shell");
		check(!server->lock.locked(),
			"nothing is locked until a locker takes the protocol");
		check(start_locker(false), "the scenarios' locker started");
		return 600;
	}
	case 5: { // the locker took the session
		if (locker_count("LOCKED") == 0 && retries++ < 100) {
			return -100;
		}
		check(locker_count("LOCKED") == 1, "the compositor confirmed the lock");
		check(server->lock.locked(), "the session is locked");
		check(locker_count("DRAWN") >= 1, "the locker drew the output");
		return 600;
	}
	case 6: { // on screen: only the locker
		shot("locked");
		return 300;
	}
	case 7: { // what the screen and a capture show while locked
		double fill = share_of_colour("locked", 0xff3050a0);
		check(fill > 0.99, std::format(
			"the locked screen is the locker's own fill ({:.3f})", fill));
		View *view = foot_window();
		bool refused = view == nullptr || capture_view(*view, 1.0f) == nullptr;
		check(refused, "a window capture is refused while locked");
		return 0;
	}
	case 8: { // keys reach the locker and nothing else
		bool tiled_before = output().desktop_showing() != nullptr;
		chord(KEY_T, SUPER); // the tiling shortcut, if anything heard it
		key(KEY_A, true);
		key(KEY_A, false);
		check(output().desktop_showing() != nullptr ? tiled_before : !tiled_before,
			"a canvas shortcut did nothing while locked");
		return 300;
	}
	case 9: {
		check(locker_count("KEY") >= 2, std::format(
			"the keys reached the locker ({} seen)", locker_count("KEY")));
		Camera before = output().camera.current();
		swipe(3, 300, 0);
		check(output().camera.current().cx == before.cx
				&& output().camera.current().cy == before.cy,
			"a three-finger swipe moved no camera while locked");
		click(400, 300);
		return 300;
	}
	case 10: {
		check(locker_count("BUTTON") >= 1, "the click reached the locker");
		check(server->seat->pointer_state.focused_surface
				== server->lock.surface_for(output()),
			"the pointer focus is the locker's surface");
		// The locker dies: the session must stay locked.
		kill(locker_pid, SIGKILL);
		waitpid(locker_pid, nullptr, 0);
		locker_pid = -1;
		return 600;
	}
	case 11: {
		check(server->lock.locked(), "a killed locker leaves the session locked");
		check(server->lock.abandoned(), "with nothing drawing it");
		output().damage();
		return 400;
	}
	case 12: {
		shot("abandoned");
		return 300;
	}
	case 13: { // the opaque fill, and the next locker taking over
		double fill = share_of_colour("abandoned", as_argb(config::lock_fill));
		check(fill > 0.99, std::format(
			"a dead locker leaves an opaque fill ({:.3f})", fill));
		reap_locker();
		check(start_locker(false), "a second locker started");
		return 600;
	}
	case 14: {
		if (locker_count("DRAWN") == 0 && retries++ < 100) {
			return -100;
		}
		check(locker_count("LOCKED") == 1, "the new locker took the lock over");
		kill(locker_pid, SIGUSR1); // unlock_and_destroy
		return 600;
	}
	case 15: {
		if (server->lock.locked() && retries++ < 100) {
			return -100;
		}
		check(!server->lock.locked(), "the locker unlocked the session");
		check(locker_count("UNLOCKED") == 1, "it said so itself");
		reap_locker();
		output().damage();
		return 600;
	}
	case 16: {
		shot("unlocked-again");
		return 300;
	}
	case 17: { // the windows are back
		double fill = share_of_colour("unlocked-again", 0xff3050a0);
		check(fill < 0.01, std::format(
			"nothing of the locker is left on screen ({:.3f})", fill));
		check(foot_window() != nullptr, "the window is still there");
		return 0;
	}
	case 18: { // the idle timeout: armed from canvas.conf, and what it does
		check(server->conf.lock_idle_minutes == settings::lock_idle.fallback,
			"canvas.conf starts with the default idle lock");
		check(server->lock.idle_armed(), "the idle countdown is running");
		server->lock.idle_elapsed();
		check(server->lock.requested(), "the idle timeout asked for the lock");
		return 400;
	}
	case 19: {
		std::string events = read_file(events_path);
		int asks = 0;
		for (std::size_t at = events.find(config::lock_action);
				at != std::string::npos; at = events.find(config::lock_action, at + 1)) {
			asks++;
		}
		check(asks >= 2, std::format(
			"the idle timeout asked the shell to lock ({} asks)", asks));
		// An idle inhibitor holds the idle lock off.
		check(start_locker(true), "a locker holding an idle inhibitor started");
		return 800;
	}
	case 20: {
		if (locker_count("DRAWN") == 0 && retries++ < 100) {
			return -100;
		}
		check(!server->lock.idle_armed(),
			"an idle inhibitor stops the idle countdown");
		kill(locker_pid, SIGUSR1);
		return 800;
	}
	case 21: {
		if (server->lock.locked() && retries++ < 100) {
			return -100;
		}
		check(server->lock.idle_armed(),
			"the countdown runs again once the inhibitor is gone");
		reap_locker();
		return 300;
	}
	case 22: { // a headless run is no login: it starts no session services
		std::string calls = read_file(shot_dir + "/systemctl.log");
		check(calls.empty(), std::format(
			"no systemctl ran in a headless run ({})",
			calls.empty() ? "none" : calls.substr(0, calls.find('\n'))));
		return 300;
	}
	case 23: {
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
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr
			|| std::getenv("CANVAS_LOCK_CLIENT") == nullptr) {
		std::fprintf(stderr, "lock: $WAYLAND_CANVAS_BIN or $CANVAS_LOCK_CLIENT "
			"is not set (meson passes both)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/lock", root);
	mkdir_p(shot_dir);
	setenv("XDG_CONFIG_HOME", std::format("{}/xdg-config", shot_dir).c_str(), 1);
	// A stub systemctl first on PATH: a run that is not a login must
	// start no session services, and the machine's own user manager must
	// never be spoken to from a test.
	std::string stub_bin = shot_dir + "/bin";
	mkdir_p(stub_bin);
	{
		std::string path = stub_bin + "/systemctl";
		FILE *f = std::fopen(path.c_str(), "w");
		if (f != nullptr) {
			std::fprintf(f, "#!/usr/bin/env bash\nprintf '%%s\\n' \"$*\" >> '%s/systemctl.log'\n",
				shot_dir.c_str());
			std::fclose(f);
			chmod(path.c_str(), 0755);
		}
	}
	setenv("PATH", std::format("{}:{}", stub_bin,
		std::getenv("PATH") != nullptr ? std::getenv("PATH") : "").c_str(), 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false; // the suites never spawn the shell
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display);
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	reap_locker();
	if (sub_pid > 0) {
		kill(sub_pid, SIGTERM);
		waitpid(sub_pid, nullptr, 0);
	}
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	return failures != 0;
}
