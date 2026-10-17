// End-to-end night-light scenario: the settings that hold the filter
// (`settings.set`, the toggle action and canvas.conf), the temperature's
// own multipliers, and the wlr gamma-control protocol answered by a client
// of the scenarios' own (tests/clients/gamma-client.c). A headless output
// holds no gamma ramp, so nothing here can show a warmed frame: that the
// filter rides the ramp is the real backend's to prove. Writes only under
// $CANVAS_TMP, with canvas.conf in its own scratch config dir.
#include <cairo.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
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
#include "core/settings.h"
#include "core/wlr.h"
#include "platform/output.h"
#include "render/night_light.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false;
uint32_t t = 90000;
pid_t grim_pid = -1;
pid_t gamma_pid = -1;
int gamma_out = -1;
std::string gamma_lines;
std::string shot_dir;
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
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
}

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

// The average red, green and blue of the shot, 0 to 1 each.
bool average_rgb(const char *name, double out[3]) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	cairo_surface_t *image = cairo_image_surface_create_from_png(path.c_str());
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(image);
		return false;
	}
	cairo_surface_flush(image);
	int w = cairo_image_surface_get_width(image);
	int h = cairo_image_surface_get_height(image);
	int stride = cairo_image_surface_get_stride(image);
	const unsigned char *data = cairo_image_surface_get_data(image);
	double sums[3] = {0, 0, 0};
	for (int y = 0; y < h; y++) {
		const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * stride);
		for (int x = 0; x < w; x++) {
			sums[0] += (row[x] >> 16) & 0xff;
			sums[1] += (row[x] >> 8) & 0xff;
			sums[2] += row[x] & 0xff;
		}
	}
	cairo_surface_destroy(image);
	double pixels = static_cast<double>(w) * h;
	for (int i = 0; i < 3; i++) {
		out[i] = sums[i] / pixels / 255.0;
	}
	return pixels > 0;
}

std::string_view gamma_said() {
	char buf[256];
	ssize_t got = 0;
	while (gamma_out >= 0 && (got = read(gamma_out, buf, sizeof buf)) > 0) {
		gamma_lines.append(buf, static_cast<std::size_t>(got));
	}
	return gamma_lines;
}

bool start_gamma_client() {
	const char *bin = std::getenv("CANVAS_GAMMA_CLIENT");
	if (bin == nullptr || *bin == '\0') {
		return false;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}
	gamma_pid = fork();
	if (gamma_pid == 0) {
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, nullptr);
		close(fds[0]);
		dup2(fds[1], 1);
		close(fds[1]);
		execl(bin, "gamma-client", static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	gamma_out = fds[0];
	gamma_lines.clear();
	return gamma_pid > 0;
}

void reap_gamma() {
	if (gamma_pid > 0) {
		kill(gamma_pid, SIGKILL);
		waitpid(gamma_pid, nullptr, 0);
		gamma_pid = -1;
	}
	if (gamma_out >= 0) {
		close(gamma_out);
		gamma_out = -1;
	}
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

constexpr uint32_t SUPER_CTRL[] = {KEY_LEFTMETA, KEY_LEFTCTRL};

// Sets one [session] value the way the settings window does.
void set_session(std::string_view params) {
	spawn(std::format("'{}' msg settings.set '{}' >/dev/null 2>&1",
		std::getenv("WAYLAND_CANVAS_BIN"), params).c_str());
}

// A white wallpaper of sorts: the whole output filled by the compositor's
// own clear colour would be dark, so the frames are measured relative to
// each other rather than against an absolute.
double plain[3] = {0, 0, 0};

int run_step() {
	switch (step) {
	case 0: {
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		spawn("foot -T night sh -c 'printf \"\\033[47m\\033[2J\"; exec sleep 600'");
		return 2000;
	}
	case 1: {
		if (server->views.empty() && retries++ < 100) {
			return -100;
		}
		check(!server->views.empty(), "a window on the output");
		check(!server->conf.night_light, "the night light starts off");
		// A headless output has no gamma ramp, so nothing it shows can
		// carry the filter: what the frames look like is a check for the
		// real backend (the plan's on-screen box).
		check(!NightLight::holds_a_ramp(*server->outputs.front()),
			"the headless output holds no gamma ramp");
		return 1200;
	}
	case 2: {
		shot("plain");
		return 300;
	}
	case 3: {
		check(average_rgb("plain", plain), "the plain frame was read");
		set_session("{\"nightLight\":true}");
		return 1200;
	}
	case 4: {
		if (!server->conf.night_light && retries++ < 100) {
			return -100;
		}
		check(server->conf.night_light, "the night light is on in canvas.conf");
		check(server->conf.night_light_k == settings::default_night_light_k,
			"at the default temperature");
		float want[3];
		night_light_multipliers(settings::default_night_light_k, want);
		check(want[2] < 0.8 && want[0] > 0.99,
			"4000 K takes blue down and leaves red alone");
		float warmer[3];
		night_light_multipliers(2000, warmer);
		check(warmer[2] < want[2], "2000 K is warmer than 4000 K");
		server->damage_all();
		return 800;
	}
	case 5: {
		shot("filter-on");
		return 300;
	}
	case 6: { // an output with no ramp is left as it was
		double on[3];
		check(average_rgb("filter-on", on), "the frame was read with the filter on");
		check(std::abs(on[2] - plain[2]) < 0.02,
			"an output with no gamma ramp shows the frame unchanged");
		set_session("{\"nightLightTemperature\":2000}");
		return 1200;
	}
	case 7: {
		if (server->conf.night_light_k != 2000 && retries++ < 100) {
			return -100;
		}
		check(server->conf.night_light_k == 2000,
			"the temperature is written to canvas.conf");
		set_session("{\"nightLightTemperature\":99}");
		return 1000;
	}
	case 8: {
		check(server->conf.night_light_k == 2000,
			"a temperature outside the range changes nothing");
		chord(KEY_N, SUPER_CTRL);
		return 1200;
	}
	case 9: {
		if (server->conf.night_light && retries++ < 100) {
			return -100;
		}
		check(!server->conf.night_light, "the toggle key turned the night light off");
		chord(KEY_N, SUPER_CTRL);
		return 1200;
	}
	case 10: {
		if (!server->conf.night_light && retries++ < 100) {
			return -100;
		}
		check(server->conf.night_light, "and the next press turned it back on");
		check(start_gamma_client(), "a gamma-control client started");
		return 1500;
	}
	case 11: { // the protocol is offered; this output has no ramp to give
		std::string_view said = gamma_said();
		check(!said.empty(), "the gamma-control client was answered");
		check(said.find("FAILED") != std::string_view::npos, std::format(
			"an output with no ramp tells a client so ({})",
			said.empty() ? "nothing said" : said.substr(0, said.find('\n'))));
		kill(gamma_pid, SIGUSR1);
		return 1200;
	}
	case 12: {
		reap_gamma();
		check(server->conf.night_light,
			"the built-in setting is untouched by a client coming and going");
		return 300;
	}
	case 13: {
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
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr
			|| std::getenv("CANVAS_GAMMA_CLIENT") == nullptr) {
		std::fprintf(stderr, "nightlight: $WAYLAND_CANVAS_BIN or "
			"$CANVAS_GAMMA_CLIENT is not set (meson passes both)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/nightlight", root);
	std::error_code ec;
	std::filesystem::remove_all(shot_dir, ec);
	mkdir_p(shot_dir);
	setenv("XDG_CONFIG_HOME", std::format("{}/xdg-config", shot_dir).c_str(), 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false;
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
	reap_gamma();
	wl_event_source_remove(timer);
	wlr_keyboard_finish(&kbd);
	server.reset();
	return failures != 0;
}
