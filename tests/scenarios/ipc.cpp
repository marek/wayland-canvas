// End-to-end control-IPC scenario: the JSON-lines protocol on the socket the
// compositor exports — every method, every error case (a malformed line
// answered with the connection kept, unknown methods, bad params, a line
// over 64 KiB answered then dropped, a slow subscriber dropped), each event
// type triggered for real, the canvas-surface role's answers (a place
// with no area refused, one ahead of its surface remembered, a drag with
// no button held refused, an edge that is not one, an unplace), the calls a
// client leaves pending (a `surface.move` drag, a `capture.pick`, a
// `shortcut.capture`) outliving it, and the `wayland-canvas msg` client
// with its exit codes. Writes only under $CANVAS_TMP (meson passes the
// repo's tmp/); meson passes the compositor binary as $WAYLAND_CANVAS_BIN.
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <signal.h>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>
#include <sys/poll.h>
#include <sys/socket.h>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <json-glib/json-glib.h>
#include "platform/layer.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/theme.h"
#include "core/wlr.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
long long free_window_id = 0; // the window moved onto free canvas, for its jump
// The desktops lifecycle step's progress: the change it makes next, the two
// desktops it works on, and the desktops events heard before it began.
int lifecycle = 0, lifecycle_heard = 0;
std::optional<DesktopId> life_a, life_b;
int flooded = 0; // desktops the flood step has created
std::string conf_before; // canvas.conf's text before a settings.set that must change nothing
bool finished = false; // the run reached its end; see main()
uint32_t t = 90000;
pid_t grim_pid = -1;
pid_t raw_pid = -1;
pid_t sub_pid = -1;
int raw_status = 0;
std::string shot_dir;
std::string socket_path;
std::string events_path;
// A plugin surface name no client draws: what the socket says of it.
const std::string surface_name = "wayland-canvas-canvas-ipc.scenario/one";
std::string output_name; // the headless output's name, for `output` params
wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
wlr_keyboard kbd{};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
pid_t layer_pid = -1; // the layer-shell client drawing the plugin surface
pid_t pending_pid = -1; // a client that disconnects with a call pending

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

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
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
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}

bool file_exists(const std::string &path) {
	struct stat st{};
	return stat(path.c_str(), &st) == 0;
}

// Runs the real client through /bin/sh with stdout, stderr and the exit code
// captured under `name` (.out, .err, .code). Params, when given, are
// single-quoted for the shell.
void run_client(std::string_view method, std::string_view params, const char *name) {
	// An earlier run's answer under this name is not this one's.
	std::error_code stale;
	std::filesystem::remove(std::format("{}/{}.code", shot_dir, name), stale);
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	std::string cmd = std::format("\"{}\" msg {}", bin, method);
	if (!params.empty()) {
		cmd += std::format(" '{}'", params);
	}
	cmd += std::format(" > {}/{}.out 2> {}/{}.err; echo $? > {}/{}.code",
		shot_dir, name, shot_dir, name, shot_dir, name);
	spawn(cmd.c_str());
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
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&ptr.events.button, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
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

// The plugin surface the layer client draws, once the compositor maps it.
LayerSurface *plugin_surface() {
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (const auto &layer : output->layers) {
			for (const std::unique_ptr<LayerSurface> &surface : layer) {
				if (surface->mapped && surface->name() == surface_name) {
					return surface.get();
				}
			}
		}
	}
	return nullptr;
}

// The compositor runs a drag the plugin handed it.
bool surface_drag_running() {
	return std::holds_alternative<PointerGrab::SurfaceDrag>(server->input.grab.state());
}

// The foot window the scenario opened, whatever the canvas did around it.
View *foot_window() {
	for (View *v : server->views) {
		if (v->mapped) {
			return v;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------- raw clients

// Connects to the control socket from a child process (the compositor runs
// in this one, so nothing here may block the parent). -1 on failure.
int child_connect() {
	const char *path = std::getenv("WAYLAND_CANVAS_SOCKET");
	if (path == nullptr || *path == '\0') {
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return -1;
	}
	strcpy(addr.sun_path, path);
	if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool child_write_all(int fd, std::string_view bytes) {
	std::size_t done = 0;
	while (done < bytes.size()) {
		ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
		if (n > 0) {
			done += static_cast<std::size_t>(n);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return false;
		}
	}
	return true;
}

// Forks a child that sends `to_send` as raw lines, optionally half-closes
// its write side, then captures everything the compositor sends back until
// it goes quiet (800 ms), drops the connection, or 10 s pass. A trailing
// "EOF" line is appended when the compositor closed the connection.
pid_t fork_raw(const std::string &to_send, bool half_close, const char *out) {
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}
	int fd = child_connect();
	FILE *f = std::fopen((shot_dir + "/" + out).c_str(), "w");
	if (fd < 0 || f == nullptr) {
		if (f != nullptr) {
			std::fclose(f);
		}
		_exit(125);
	}
	if (!child_write_all(fd, to_send)) {
		std::fclose(f);
		close(fd);
		_exit(124);
	}
	if (half_close) {
		shutdown(fd, SHUT_WR);
	}
	char buf[1024];
	int idle = 0;
	bool eof = false;
	// Read while data keeps arriving; two quiet polls of 400 ms end it, and
	// EOF or a socket error breaks out. No read cap: a large state answer
	// (hundreds of desktops plus the items) must still be drained whole.
	while (!eof && idle < 2) {
		pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&pfd, 1, 400) == 0) {
			idle++;
			continue;
		}
		idle = 0;
		if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
			break;
		}
		ssize_t n = read(fd, buf, sizeof buf);
		if (n <= 0) {
			eof = true;
			break;
		}
		std::fwrite(buf, 1, static_cast<std::size_t>(n), f);
	}
	if (eof) {
		std::fputs("\nEOF\n", f);
	}
	std::fflush(f);
	std::fclose(f);
	close(fd);
	_exit(0);
}

// Forks a child that sends one request line, lingers `linger_ms` without
// reading the answer, and hangs up: a call left pending when its client goes.
pid_t fork_hangup(const std::string &request, int linger_ms) {
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}
	int fd = child_connect();
	if (fd < 0 || !child_write_all(fd, request + "\n")) {
		_exit(125);
	}
	usleep(static_cast<useconds_t>(linger_ms) * 1000);
	close(fd);
	_exit(0);
}

// Forks a child that subscribes, writes "ready" to `ready`, then reads
// nothing for 3 s while the parent floods events, and finally drains: it
// writes "dropped <bytes>" when the compositor dropped it (EOF), or
// "alive <bytes>" when the connection survived 10 s.
pid_t fork_slow_subscriber(const char *ready, const char *result) {
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}
	int fd = child_connect();
	FILE *f = std::fopen((shot_dir + "/" + result).c_str(), "w");
	if (fd < 0 || f == nullptr) {
		if (f != nullptr) {
			std::fclose(f);
		}
		_exit(125);
	}
	if (!child_write_all(fd, "{\"id\":1,\"method\":\"subscribe\"}\n")) {
		std::fclose(f);
		close(fd);
		_exit(124);
	}
	// The subscribe response: read it, so `ready` means really subscribed.
	char line[512];
	std::size_t len = 0;
	bool got_response = false;
	for (int elapsed = 0; elapsed < 5000 && !got_response; elapsed += 100) {
		pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&pfd, 1, 100) == 0) {
			continue;
		}
		ssize_t n = read(fd, line + len, 1);
		if (n <= 0) {
			break;
		}
		if (line[len] == '\n') {
			got_response = true;
			break;
		}
		if (++len == sizeof line - 1) {
			break;
		}
	}
	if (got_response) {
		FILE *r = std::fopen((shot_dir + "/" + ready).c_str(), "w");
		if (r != nullptr) {
			std::fputs("ready\n", r);
			std::fflush(r);
			std::fclose(r);
		}
	}
	// Not a single read while the parent floods: the backlog must build.
	sleep(3);
	long bytes = 0;
	bool eof = false;
	char buf[65536];
	for (int elapsed = 0; elapsed < 10000 && !eof; elapsed += 200) {
		pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&pfd, 1, 200) == 0) {
			continue;
		}
		if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
			break;
		}
		ssize_t n = read(fd, buf, sizeof buf);
		if (n <= 0) {
			eof = true;
			break;
		}
		bytes += n;
	}
	std::fprintf(f, "%s %ld\n", eof ? "dropped" : "alive", bytes);
	std::fflush(f);
	std::fclose(f);
	close(fd);
	_exit(0);
}

// True when the subscriber printed an event line containing `needle`.
bool events_has(std::string_view needle) {
	return read_file(events_path).find(needle) != std::string::npos;
}

int events_count(std::string_view needle) {
	std::string text = read_file(events_path);
	int n = 0;
	for (std::size_t pos = text.find(needle); pos != std::string::npos;
			pos = text.find(needle, pos + needle.size())) {
		n++;
	}
	return n;
}

// ------------------------------------------------------ state checking

// The first failed expectation about a `msg state` result, or empty when the
// result is well-formed: `want_theme`, at least `want_outputs` outputs (the
// first reporting `want_overview`), at least `want_desktops` desktops.
std::string state_mismatch(const std::string &text, std::string_view want_theme,
		int want_outputs, bool want_overview, int want_desktops) {
	JsonParser *parser = json_parser_new();
	std::string problem;
	if (!json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		problem = "the result is not JSON";
	} else {
		JsonNode *root = json_parser_get_root(parser);
		JsonObject *object =
			root != nullptr && json_node_get_node_type(root) == JSON_NODE_OBJECT
			? json_node_get_object(root) : nullptr;
		if (object == nullptr) {
			problem = "the result is not an object";
		} else if (!json_object_has_member(object, "theme")) {
			problem = "the result has no theme";
		} else {
			JsonNode *theme = json_object_get_member(object, "theme");
			if (json_node_get_node_type(theme) != JSON_NODE_VALUE
					|| json_node_get_value_type(theme) != G_TYPE_STRING) {
				problem = "theme is not a string";
			} else if (std::string_view{json_node_get_string(theme)} != want_theme) {
				problem = std::format("theme is {}, not {}", json_node_get_string(theme),
					want_theme);
			}
		}
		if (problem.empty() && json_object_has_member(object, "outputs")) {
			JsonNode *outputs = json_object_get_member(object, "outputs");
			if (json_node_get_node_type(outputs) != JSON_NODE_ARRAY) {
				problem = "outputs is not an array";
			} else if (static_cast<int>(json_array_get_length(
					json_node_get_array(outputs))) < want_outputs) {
				problem = std::format("only {} outputs",
					json_array_get_length(json_node_get_array(outputs)));
			} else {
				JsonObject *first = json_array_get_object_element(
					json_node_get_array(outputs), 0);
				if (!json_object_has_member(first, "name")
						|| !json_object_has_member(first, "overview")
						|| !json_object_has_member(first, "scale")) {
					problem = "an output entry lacks name, overview or scale";
				} else if (json_object_get_boolean_member(first, "overview")
						!= want_overview) {
					problem = want_overview
						? "the output is not in the overview" : "the output is in the overview";
				}
			}
		}
		if (problem.empty() && json_object_has_member(object, "desktops")) {
			JsonNode *desktops = json_object_get_member(object, "desktops");
			if (json_node_get_node_type(desktops) != JSON_NODE_ARRAY) {
				problem = "desktops is not an array";
			} else if (static_cast<int>(json_array_get_length(
					json_node_get_array(desktops))) < want_desktops) {
				problem = std::format("only {} desktops",
					json_array_get_length(json_node_get_array(desktops)));
			}
		}
	}
	g_object_unref(parser);
	return problem;
}

// ------------------------------------------------------ state checking

// An integer member: nullopt when absent or not a whole number.
std::optional<long long> member_int(JsonObject *object, const char *name) {
	if (!json_object_has_member(object, name)) {
		return std::nullopt;
	}
	JsonNode *node = json_object_get_member(object, name);
	return json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_INT64
		? std::optional{json_node_get_int(node)} : std::nullopt;
}

// A number member as a double: whole numbers and fractions alike.

// A string member; empty when absent or not a string.
std::string member_string(JsonObject *object, const char *name) {
	if (!json_object_has_member(object, name)) {
		return {};
	}
	JsonNode *node = json_object_get_member(object, name);
	return json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? std::string{json_node_get_string(node)} : std::string{};
}


// One entry of a `msg state` result's windows array, by its id: what the app
// bar draws (docs/intents/shell/app-bar.md).
struct WindowEntry {
	std::string app_id, title;
	bool has_desktop = false, minimized = false, focused = false;
};

std::optional<WindowEntry> parse_state_window(const std::string &text,
		long long want_id) {
	JsonParser *parser = json_parser_new();
	std::optional<WindowEntry> out;
	if (json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		JsonNode *root = json_parser_get_root(parser);
		JsonObject *object = root != nullptr
				&& json_node_get_node_type(root) == JSON_NODE_OBJECT
			? json_node_get_object(root) : nullptr;
		JsonNode *windows = object != nullptr
				&& json_object_has_member(object, "windows")
			? json_object_get_member(object, "windows") : nullptr;
		if (windows != nullptr && json_node_get_node_type(windows) == JSON_NODE_ARRAY) {
			JsonArray *array = json_node_get_array(windows);
			for (guint i = 0; i < json_array_get_length(array); i++) {
				JsonObject *entry = json_array_get_object_element(array, i);
				if (entry == nullptr || member_int(entry, "id") != want_id) {
					continue;
				}
				JsonNode *desktop = json_object_has_member(entry, "desktop")
					? json_object_get_member(entry, "desktop") : nullptr;
				out = WindowEntry{
					.app_id = member_string(entry, "appId"),
					.title = member_string(entry, "title"),
					.has_desktop = desktop != nullptr
						&& json_node_get_node_type(desktop) != JSON_NODE_NULL,
					.minimized = json_object_get_boolean_member(entry, "minimized") != FALSE,
					.focused = json_object_get_boolean_member(entry, "focused") != FALSE,
				};
				break;
			}
		}
	}
	g_object_unref(parser);
	return out;
}

// The first way a `settings.get` answer fails to describe the settings
// table, or empty when its `fields` give every row its section, key, kind,
// bounds or names, and default, and its sections hold a value for each.
std::string settings_get_problem(const std::string &text) {
	JsonParser *parser = json_parser_new();
	std::string problem;
	if (!json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		problem = "the answer is not JSON";
	}
	JsonNode *root = problem.empty() ? json_parser_get_root(parser) : nullptr;
	JsonObject *answer = root != nullptr && json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
	JsonObject *fields = answer != nullptr && json_object_has_member(answer, "fields")
		? json_object_get_object_member(answer, "fields") : nullptr;
	if (problem.empty() && fields == nullptr) {
		problem = "the answer has no fields";
	}
	for (const settings::Field &field : settings::fields) {
		if (!problem.empty()) {
			break;
		}
		std::string param{field.param};
		JsonObject *section = json_object_has_member(answer, std::string{field.section}.c_str())
			? json_object_get_object_member(answer, std::string{field.section}.c_str())
			: nullptr;
		JsonObject *row = json_object_has_member(fields, param.c_str())
			? json_object_get_object_member(fields, param.c_str()) : nullptr;
		if (section == nullptr || !json_object_has_member(section, param.c_str())) {
			problem = std::format("{} has no value in [{}]", param, field.section);
		} else if (row == nullptr) {
			problem = std::format("{} has no row in fields", param);
		} else if (member_string(row, "section") != field.section
				|| member_string(row, "key") != field.key) {
			problem = std::format("{} is not given its section and key", param);
		} else if (field.kind == settings::Kind::integer) {
			std::optional<long long> low = member_int(row, "min");
			std::optional<long long> high = member_int(row, "max");
			std::optional<long long> fallback = member_int(row, "default");
			if (member_string(row, "kind") != "integer"
					|| low != (field.off ? 0 : field.min) || high != field.max
					|| fallback != field.fallback) {
				problem = std::format("{} does not report its bounds and default", param);
			}
		} else if (field.kind == settings::Kind::toggle) {
			if (member_string(row, "kind") != "toggle"
					|| !json_object_has_member(row, "default")
					|| (json_object_get_boolean_member(row, "default") != FALSE)
						!= (field.fallback != 0)) {
				problem = std::format("{} does not report its default", param);
			}
		} else {
			JsonArray *choices = json_object_has_member(row, "choices")
				? json_object_get_array_member(row, "choices") : nullptr;
			bool names = choices != nullptr
				&& json_array_get_length(choices) == field.values().size();
			for (guint i = 0; names && i < json_array_get_length(choices); i++) {
				names = json_array_get_string_element(choices, i) == field.values()[i];
			}
			if (member_string(row, "kind") != "choice" || !names
					|| member_string(row, "default") != field.values()[field.fallback]) {
				problem = std::format("{} does not report its names and default", param);
			}
		}
	}
	g_object_unref(parser);
	return problem;
}

// A settings.set the table says must fail, named for its client's files.
struct Refusal {
	std::string name, params;
	const settings::Field *field;
};

// For every setting, the values it does not take: a name no choice has, a
// string for a switch, a number past each end of an integer's bounds.
std::vector<Refusal> refusals() {
	std::vector<Refusal> out;
	for (const settings::Field &field : settings::fields) {
		std::vector<std::string> bad;
		switch (field.kind) {
		case settings::Kind::choice:
			bad = {"\"sideways\""};
			break;
		case settings::Kind::toggle:
			bad = {"\"yes\""};
			break;
		case settings::Kind::integer:
			bad = {std::to_string(static_cast<long long>(field.max) + 1),
				field.off ? "-1" : std::to_string(field.min - 1)};
			break;
		}
		for (const std::string &value : bad) {
			out.push_back({.name = std::format("refuse-{}-{}", field.param, out.size()),
				.params = std::format("{{\"{}\":{}}}", field.param, value),
				.field = &field});
		}
	}
	return out;
}

// Waits for a forked raw client (or any pid) to have exited.
bool child_exited(pid_t pid) {
	return pid > 0 && waitpid(pid, &raw_status, WNOHANG) == pid;
}

// Commits a new output scale; the layout follows, and the outputs event.
void commit_scale(float scale) {
	Output *output = server->output_under_cursor();
	wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_scale(&state, scale);
	wlr_output_commit_state(output->wlr, &state);
	wlr_output_state_finish(&state);
}

// --------------------------------------------------------------- steps

int run_step() {
	switch (step) {
	case 0: {
		const char *env_socket = std::getenv("WAYLAND_CANVAS_SOCKET");
		socket_path = env_socket != nullptr ? env_socket : "";
		struct stat st{};
		bool is_socket = stat(socket_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
		check(!socket_path.empty() && is_socket,
			std::format("the compositor exports a socket at $WAYLAND_CANVAS_SOCKET ({})",
				socket_path));
		output_name = server->outputs.empty() ? "" : server->outputs.front()->wlr->name;
		check(!output_name.empty(), "the headless output has a name for params");
		return 200;
	}
	case 1: // a real foot window, so the title bars have something to frame
		spawn("foot -T ipc sh -c 'echo ipc scenario; exec sleep 600'");
		return 400;
	case 2: {
		int mapped = 0;
		View *view = nullptr;
		for (View *v : server->views) {
			if (v->mapped) {
				mapped++;
				view = v;
			}
		}
		if (mapped < 1 && retries++ < 100) {
			return -100;
		}
		check(mapped == 1, std::format("one foot window mapped ({})", mapped));
		check(view != nullptr && view->deco.ssd,
			"foot negotiated a server-side title bar");
		if (view != nullptr) {
			server->focus_view(view); // an active title bar for the screenshot
		}
		shot("titlebar-dark");
		return 500;
	}
	case 3: // the state method through the real client
		run_client("state", "", "state-dark");
		return 300;
	case 4: {
		if (!client_done("state-dark") && retries++ < 50) {
			return -100;
		}
		std::string out = client_out("state-dark");
		std::string problem = state_mismatch(out, "dark", 1, false, 1);
		check(problem.empty(),
			std::format("`msg state` reports the dark theme, the output, the desktops ({})",
				problem.empty() ? out : problem));
		check(client_code("state-dark") == 0, "`msg state` exits 0");
		return 200;
	}
	case 5: // ids echoed, responses in order, on one connection
		raw_pid = fork_raw(std::format(
			"{{\"id\":1,\"method\":\"state\"}}\n{{\"id\":\"two\",\"method\":\"subscribe\"}}\n"),
			false, "raw-order.out");
		return 600;
	case 6: {
		if (!child_exited(raw_pid) && retries++ < 100) {
			return -100;
		}
		std::string out = read_file(shot_dir + "/raw-order.out");
		std::size_t first = out.find("\"id\":1,");
		std::size_t second = out.find("\"id\":\"two\"");
		check(first != std::string::npos && second != std::string::npos
				&& first < second && out.find("\"result\"") != std::string::npos,
			std::format("responses come in request order with their ids ({})", out));
		return 200;
	}
	case 7: // malformed lines are answered, and the connection stays
		raw_pid = fork_raw("nonsense\n"
			"{\"id\":\"abc\"}\n"
			"{\"id\":5,\"method\":\"theme.set\",\"params\":\"x\"}\n"
			"{\"id\":7,\"method\":\"state\"}\n",
			false, "raw-malformed.out");
		return 700;
	case 8: {
		if (!child_exited(raw_pid) && retries++ < 100) {
			return -100;
		}
		std::string out = read_file(shot_dir + "/raw-malformed.out");
		check(out.find("\"id\":null") != std::string::npos
				&& out.find("error") != std::string::npos,
			std::format("a non-JSON line is answered with an id-null error ({})", out));
		check(out.find("\"id\":\"abc\"") != std::string::npos
				&& out.find("no method") != std::string::npos,
			"an object without a method is answered with its readable id");
		check(out.find("\"id\":5") != std::string::npos
				&& out.find("params is not an object") != std::string::npos,
			"a request whose params is not an object is answered naming params");
		check(out.find("\"id\":7") != std::string::npos
				&& out.find("\"result\"") != std::string::npos,
			"the connection stayed: the next request is answered");
		return 200;
	}
	case 9: // an unknown method is answered with an error naming it
		raw_pid = fork_raw("{\"id\":9,\"method\":\"frobnicate\"}\n",
			false, "raw-unknown.out");
		return 500;
	case 10: {
		if (!child_exited(raw_pid) && retries++ < 100) {
			return -100;
		}
		std::string out = read_file(shot_dir + "/raw-unknown.out");
		check(out.find("\"id\":9") != std::string::npos
				&& out.find("frobnicate") != std::string::npos
				&& out.find("error") != std::string::npos,
			std::format("an unknown method is answered naming it ({})", out));
		return 200;
	}
	case 11: // the msg client's own error paths
		run_client("theme.set", R"({"mode":"blue"})", "theme-blue");
		run_client("launcher.toggle", R"({"output":"NOPE"})", "nope-output");
		run_client("state", "{oops", "bad-params");
		spawn(std::format(
			"\"{}\" msg > /dev/null 2> {}/no-method.err; echo $? > {}/no-method.code",
			std::getenv("WAYLAND_CANVAS_BIN"), shot_dir, shot_dir).c_str());
		return 400;
	case 12: {
		if ((!client_done("theme-blue") || !client_done("nope-output")
				|| !client_done("bad-params") || !file_exists(shot_dir + "/no-method.code"))
				&& retries++ < 50) {
			return -100;
		}
		check(client_code("theme-blue") == 1
				&& client_out("theme-blue").empty()
				&& client_err("theme-blue").find("mode") != std::string::npos,
			std::format("`msg theme.set` with a bad mode: exit 1, the error on stderr ({})",
				client_err("theme-blue")));
		check(client_code("nope-output") == 1
				&& client_err("nope-output").find("NOPE") != std::string::npos,
			std::format("`msg launcher.toggle` naming an unknown output: exit 1 ({})",
				client_err("nope-output")));
		check(client_code("bad-params") == 1
				&& client_err("bad-params").find("params") != std::string::npos,
			std::format("`msg` with params that are not JSON: exit 1 ({})",
				client_err("bad-params")));
		check(std::atoi(read_file(shot_dir + "/no-method.code").c_str()) == 2,
			"`msg` without a method: usage, exit 2");
		return 200;
	}
	case 13: // launcher.toggle, default output
		run_client("launcher.toggle", "", "launcher-open");
		return 300;
	case 14: {
		if (!client_done("launcher-open") && retries++ < 50) {
			return -100;
		}
		if (!server->launcher.is_open() && retries++ < 50) {
			return -100;
		}
		check(server->launcher.is_open() && client_code("launcher-open") == 0,
			"`msg launcher.toggle` opened the launcher");
		shot("launcher-open");
		return 500;
	}
	case 15: // launcher.toggle with a named output closes it again
		run_client("launcher.toggle", std::format(R"({{"output":"{}"}})", output_name),
			"launcher-close");
		return 300;
	case 16: {
		if (!client_done("launcher-close") && retries++ < 50) {
			return -100;
		}
		if (server->launcher.is_open() && retries++ < 50) {
			return -100;
		}
		check(!server->launcher.is_open(), "`msg launcher.toggle` closed the open launcher");
		return 200;
	}
	case 17: // overview.toggle, default output
		run_client("overview.toggle", "", "overview-enter");
		return 300;
	case 18: {
		if (!client_done("overview-enter") && retries++ < 50) {
			return -100;
		}
		Output *output = server->output_under_cursor();
		if ((output == nullptr || !output->overview.active()) && retries++ < 50) {
			return -100;
		}
		check(output != nullptr && output->overview.active(),
			"`msg overview.toggle` entered the overview");
		shot("overview-dark");
		return 500;
	}
	case 19: // overview.toggle with a named output leaves it
		run_client("overview.toggle", std::format(R"({{"output":"{}"}})", output_name),
			"overview-exit");
		return 300;
	case 20: {
		if (!client_done("overview-exit") && retries++ < 50) {
			return -100;
		}
		Output *output = server->output_under_cursor();
		if ((output == nullptr || output->overview.active()) && retries++ < 50) {
			return -100;
		}
		check(output != nullptr && !output->overview.active(),
			"`msg overview.toggle` left the overview");
		return 200;
	}
	case 21: // theme.set light: the palette switch every output repaints in
		run_client("theme.set", R"({"mode":"light"})", "theme-light");
		return 300;
	case 22: {
		if (!client_done("theme-light") && retries++ < 50) {
			return -100;
		}
		if (server->theme.mode() != ThemeMode::light && retries++ < 50) {
			return -100;
		}
		check(server->theme.mode() == ThemeMode::light,
			"`msg theme.set {\"mode\":\"light\"}` switched the palette");
		check(client_code("theme-light") == 0 && client_out("theme-light") == "true",
			"`msg theme.set` prints true and exits 0");
		shot("titlebar-light");
		return 500;
	}
	case 23: // the overview in light mode, then its state while inside it
		run_client("overview.toggle", "", "overview-light-enter");
		return 300;
	case 24: {
		if (!client_done("overview-light-enter") && retries++ < 50) {
			return -100;
		}
		Output *output = server->output_under_cursor();
		if ((output == nullptr || !output->overview.active()) && retries++ < 50) {
			return -100;
		}
		check(output != nullptr && output->overview.active(),
			"the overview opened again, in light mode");
		shot("overview-light");
		run_client("state", "", "state-overview");
		return 400;
	}
	case 25: {
		if (!client_done("state-overview") && retries++ < 50) {
			return -100;
		}
		std::string problem = state_mismatch(client_out("state-overview"), "light", 1, true, 1);
		check(problem.empty(),
			std::format("`msg state` reports the output in the overview ({})",
				problem.empty() ? "as it should be" : problem));
		run_client("overview.toggle", "", "overview-light-exit");
		return 300;
	}
	case 26: {
		if (!client_done("overview-light-exit") && retries++ < 50) {
			return -100;
		}
		Output *output = server->output_under_cursor();
		if ((output == nullptr || output->overview.active()) && retries++ < 50) {
			return -100;
		}
		check(output != nullptr && !output->overview.active(), "the overview closed");
		return 200;
	}
	case 27: // `msg subscribe`: forked, its stdout captured to a file
		events_path = shot_dir + "/events.out";
		sub_pid = fork();
		if (sub_pid == 0) {
			// Clear the compositor's blocked-signal mask so the later SIGINT
			// can end this child.
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
		return 300;
	case 28: // subscription goes live: desktop creations are events
		if (!events_has("\"event\":\"desktops\"")) {
			if (retries++ < 60) {
				if ((retries & 1) == 0) {
					server->canvas.desktops.create(wlr_box{2560, 0, 1280, 720});
				}
				return -200;
			}
		}
		check(events_has("\"event\":\"desktops\""),
			"`msg subscribe` receives desktops events");
		return 200;
	case 29: { // the rest of the desktops lifecycle: create, move, combine, abandon
		// The subscribers hear a change once per frame: each change gets a
		// frame of its own, so each is told.
		Desktops &desktops = server->canvas.desktops;
		switch (lifecycle++) {
		case 0:
			lifecycle_heard = events_count("\"event\":\"desktops\"");
			life_a = desktops.create(wlr_box{3840, 0, 1280, 720}).id();
			return -100;
		case 1:
			life_b = desktops.create(wlr_box{5120, 0, 1280, 720}).id();
			return -100;
		case 2:
			server->canvas.move_desktop(*life_b, 5120, 720);
			return -100;
		case 3: {
			// combine() removes `a`; nothing may touch it afterwards.
			wlr_box from = desktops.find(*life_a)->frame();
			server->canvas.combine(*life_b, *life_a, from);
			return -100;
		}
		default:
			server->canvas.remove_desktop(*life_b);
			return 300;
		}
	}
	case 30: {
		int told = events_count("\"event\":\"desktops\"") - lifecycle_heard;
		if (told < 4 && retries++ < 50) {
			return -100;
		}
		check(told >= 4,
			std::format("create, move, combine and abandon each fired a desktops event ({})",
				told));
		return 200;
	}
	case 31: // overview events reach the subscriber
		run_client("overview.toggle", "", "sub-overview");
		return 300;
	case 32: {
		if (!events_has("\"event\":\"overview\"") && retries++ < 50) {
			return -100;
		}
		check(events_has("\"event\":\"overview\""),
			"`msg subscribe` receives overview events");
		run_client("overview.toggle", "", "sub-overview-exit");
		return 300;
	}
	case 33: { // outputs events: a scale change and a second output
		if (!client_done("sub-overview-exit") && retries++ < 50) {
			return -100;
		}
		commit_scale(2);
		return 300;
	}
	case 34: {
		if (!events_has("\"event\":\"outputs\"") && retries++ < 50) {
			return -100;
		}
		check(events_has("\"event\":\"outputs\"") && events_has("\"scale\":2"),
			"a committed scale change fired an outputs event naming the scale");
		commit_scale(1);
		return 300;
	}
	case 35: { // a second headless output appears, then goes
		// autocreate() wraps the headless backend in a multi backend.
		wlr_backend *headless = nullptr;
		wlr_multi_for_each_backend(server->backend,
			[](wlr_backend *backend, void *data) {
				if (wlr_backend_is_headless(backend)) {
					*static_cast<wlr_backend **>(data) = backend;
				}
			}, &headless);
		wlr_output *second = headless != nullptr
			? wlr_headless_add_output(headless, 640, 480) : nullptr;
		check(second != nullptr, "a second headless output was added");
		return 400;
	}
	case 36: {
		if (server->outputs.size() < 2 && retries++ < 50) {
			return -100;
		}
		check(events_has("\"event\":\"outputs\"") && server->outputs.size() == 2,
			"an appearing output fired an outputs event");
		wlr_output_destroy(server->outputs.back()->wlr);
		return 400;
	}
	case 37: {
		if (server->outputs.size() != 1 && retries++ < 50) {
			return -100;
		}
		check(server->outputs.size() == 1,
			"a going output fired an outputs event and left one output");
		// the theme event: back to dark through the client
		run_client("theme.set", R"({"mode":"dark"})", "sub-theme");
		return 300;
	}
	case 38: {
		if (!client_done("sub-theme") && retries++ < 50) {
			return -100;
		}
		if (!events_has("\"event\":\"theme\"") && retries++ < 50) {
			return -100;
		}
		check(events_has("\"event\":\"theme\"") && events_has("\"mode\":\"dark\""),
			"a theme change fired a theme event naming the mode");
		if (sub_pid > 0) {
			kill(sub_pid, SIGINT);
		}
		return 300;
	}
	case 39: { // the subscriber is interrupted, as the intent says
		bool exited = sub_pid > 0
			&& waitpid(sub_pid, &raw_status, WNOHANG) == sub_pid;
		if (!exited && retries++ < 50) {
			return -100;
		}
		check(exited, "`msg subscribe` runs until interrupted");
		return 200;
	}
	case 40: // a subscriber that stops reading is dropped, not waited for
		raw_pid = fork_slow_subscriber("slow.ready", "slow.result");
		return 300;
	case 41: {
		if (!file_exists(shot_dir + "/slow.ready") && retries++ < 50) {
			return -100;
		}
		// A flood of desktops events: a long list first, then one change a
		// frame, each event carrying the whole list. Past 1 MiB of backlog the
		// drop is immediate, while the child sleeps without reading.
		int batch = flooded == 0 ? 300 : 1;
		for (int i = 0; i < batch; i++, flooded++) {
			server->canvas.desktops.create(wlr_box{5120 + (flooded % 8) * 1280,
				720 + (flooded / 8) * 720, 1280, 720});
		}
		if (flooded < 420) {
			return -20;
		}
		return 300;
	}
	case 42: {
		std::string result = read_file(shot_dir + "/slow.result");
		if (result.empty() && retries++ < 100) {
			return -200;
		}
		check(result.rfind("dropped", 0) == 0,
			std::format("a subscriber with over 1 MiB unread events was dropped ({})",
				result));
		// The compositor stayed responsive through the flood and the drop.
		run_client("state", "", "state-after-drop");
		return 300;
	}
	case 43: {
		if ((!client_done("state-after-drop") || !child_exited(raw_pid))
				&& retries++ < 100) {
			return -100;
		}
		check(client_code("state-after-drop") == 0,
			"the compositor still answered after dropping the slow subscriber");
		return 200;
	}
	case 44: { // a line over 64 KiB: answered, then the connection is dropped
		std::string big(70 * 1024, 'a');
		big += '\n';
		raw_pid = fork_raw("{\"id\":2,\"method\":\"state\"}\n" + big, false,
			"raw-oversized.out");
		return 700;
	}
	case 45: {
		if (!child_exited(raw_pid) && retries++ < 100) {
			return -100;
		}
		std::string out = read_file(shot_dir + "/raw-oversized.out");
		check(out.find("\"id\":2") != std::string::npos
				&& out.find("\"result\"") != std::string::npos,
			"the request before the oversized line was answered");
		check(out.find("64 KiB") != std::string::npos && out.find("EOF") != std::string::npos,
			std::format("the oversized line was answered with an error, then dropped ({})",
				out));
		return 200;
	}
	case 46: { // a connection that ends mid-line is dropped with its partial line
		raw_pid = fork_raw("{\"id\":3,\"method\":\"state\"", true, "raw-midline.out");
		return 700;
	}
	case 47: {
		if (!child_exited(raw_pid) && retries++ < 100) {
			return -100;
		}
		std::string out = read_file(shot_dir + "/raw-midline.out");
		check(out.find("\"result\"") == std::string::npos
				&& out.find("error") == std::string::npos && out.find("EOF") != std::string::npos,
			std::format("a connection ending mid-line got no answer and was dropped ({})",
				out));
		return 200;
	}
	case 48: // the surface role's own answers, for a name no client draws
		// The run ends by finishing the fake devices, so they come up here.
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		run_client("surface.place", std::format(
			R"({{"surface":"{}","x":10,"y":20,"width":0,"height":5}})", surface_name),
			"place-flat");
		run_client("surface.place", std::format(
			R"({{"surface":"{}","x":10,"y":20,"width":40,"height":30}})", surface_name),
			"place-ahead");
		run_client("surface.move", std::format(R"({{"surface":"{}"}})", surface_name),
			"move-unheld");
		run_client("surface.resize", std::format(
			R"({{"surface":"{}","edges":["sideways"]}})", surface_name), "resize-edges");
		return 400;
	case 49: {
		for (const char *name : {"place-flat", "place-ahead", "move-unheld", "resize-edges"}) {
			if (!client_done(name) && retries++ < 50) {
				return -100;
			}
		}
		retries = 0;
		check(client_code("place-flat") == 1
				&& client_err("place-flat").find("area") != std::string::npos,
			std::format("`surface.place` with no area: an error naming it ({})",
				client_err("place-flat")));
		auto remembered = server->canvas_places.find(surface_name);
		check(client_code("place-ahead") == 0 && client_out("place-ahead") == "true"
				&& remembered != server->canvas_places.end()
				&& remembered->second.x == 10 && remembered->second.width == 40,
			"`surface.place` for a name no surface carries yet is remembered for it");
		check(client_code("move-unheld") == 1
				&& client_err("move-unheld").find("no button") != std::string::npos,
			std::format("`surface.move` with no button held on the surface: an error ({})",
				client_err("move-unheld")));
		check(client_code("resize-edges") == 1
				&& client_err("resize-edges").find("edges") != std::string::npos,
			std::format("`surface.resize` naming an edge there is not: the usage ({})",
				client_err("resize-edges")));
		check(!server->input.grab.active(), "no drag began from any of them");
		run_client("surface.unplace", std::format(R"({{"surface":"{}"}})", surface_name),
			"unplace");
		return 300;
	}
	case 50: {
		if (!client_done("unplace") && retries++ < 50) {
			return -100;
		}
		retries = 0;
		check(client_code("unplace") == 0
				&& !server->canvas_places.contains(surface_name),
			"`surface.unplace` forgets the remembered place");
		run_client("state", "", "state-windows"); // read by the window list's check
		return 300;
	}
	case 51: // a plugin's surface on the plane, drawn by a real layer-shell client
		layer_pid = spawn(std::format("exec \"$CANVAS_LAYER_CLIENT\" '{}' > '{}/layer.out' 2>&1",
			surface_name, shot_dir).c_str());
		return 500;
	case 52: { // it maps, and `surface.place` puts it where the pointer reaches it
		Output *output = server->output_under_cursor();
		LayerSurface *layer = plugin_surface();
		if ((layer == nullptr || output == nullptr) && retries++ < 100) {
			return -100;
		}
		if (layer == nullptr || output == nullptr) {
			check(false, "the layer client's surface mapped");
			step = 55; // nothing to press: on to the picks
			return 1;
		}
		retries = 0;
		Rect area = output->viewport();
		run_client("surface.place", std::format(
			R"({{"surface":"{}","x":{},"y":{},"width":200,"height":100}})", surface_name,
			static_cast<int>(area.x) + 40, static_cast<int>(area.y) + 400), "layer-place");
		return 400;
	}
	case 53: { // the press the plugin answers, then a `surface.move` whose client leaves
		Output *output = server->output_under_cursor();
		LayerSurface *layer = plugin_surface();
		if ((!client_done("layer-place") || layer == nullptr || !layer->canvas_box.has_value())
				&& retries++ < 50) {
			return -100;
		}
		retries = 0;
		check(client_code("layer-place") == 0 && layer != nullptr
				&& layer->canvas_box.has_value(),
			"the layer client's surface is on the plane at its place");
		if (layer == nullptr || output == nullptr || !layer->canvas_box.has_value()) {
			step = 55;
			return 1;
		}
		Point at = output->canvas_to_local(layer->canvas_box->x + 100, layer->canvas_box->y + 50);
		move_to(at.x, at.y);
		button(BTN_LEFT, true);
		check(server->input.router.button_held_on(layer->wlr->surface),
			"the pointer's press is held on the plugin's surface");
		pending_pid = fork_hangup(std::format(
			R"({{"id":1,"method":"surface.move","params":{{"surface":"{}"}}}})", surface_name),
			1000);
		return 500;
	}
	case 54: { // the drag runs while its client lingers, and goes on after it hangs up
		check(surface_drag_running(), "the compositor began the drag the client asked for");
		if (!child_exited(pending_pid) && retries++ < 100) {
			return -100;
		}
		retries = 0;
		return 400; // the compositor reads the hang-up and drops the client
	}
	case 55: { // the drag ends at the release with nobody left to answer
		Output *output = server->output_under_cursor();
		LayerSurface *layer = plugin_surface();
		if (layer != nullptr && output != nullptr && layer->canvas_box.has_value()) {
			int before = layer->canvas_box->x;
			check(surface_drag_running(), "the drag outlived the client that asked for it");
			Point at = output->canvas_to_local(before + 100, layer->canvas_box->y + 50);
			for (int i = 1; i <= 10; i++) {
				move_to(at.x - 6 * i, at.y - 4 * i);
			}
			button(BTN_LEFT, false);
			double zoom = output->camera.current().zoom;
			check(!server->input.grab.active()
					&& std::abs(layer->canvas_box->x - (before - 60 / zoom)) <= 1,
				std::format("the release ended the drag after the surface followed it ({} to {})",
					before, layer->canvas_box->x));
		}
		return 300;
	}
	case 56: // a `capture.pick` whose client leaves while the picker is up
		pending_pid = fork_hangup(R"({"id":1,"method":"capture.pick","params":{"what":"window"}})",
			1000);
		return 500;
	case 57: {
		check(server->capture_ui.picking(), "the picker opened for the client's pick");
		if (!child_exited(pending_pid) && retries++ < 100) {
			return -100;
		}
		retries = 0;
		return 400;
	}
	case 58: { // the picker stays up for the user; cancelling it answers nobody
		check(server->capture_ui.picking(), "the picker outlived the client that asked for it");
		tap(KEY_ESC);
		check(!server->capture_ui.picking(), "Escape closed the picker with its client gone");
		return 300;
	}
	case 59: // a `shortcut.capture` whose client leaves while the capture waits
		pending_pid = fork_hangup(R"({"id":1,"method":"shortcut.capture","params":{}})", 1000);
		return 500;
	case 60: {
		if (!child_exited(pending_pid) && retries++ < 100) {
			return -100;
		}
		retries = 0;
		return 400;
	}
	case 61: { // the capture ended with its client: keys act again
		constexpr uint32_t SUPER[] = {KEY_LEFTMETA};
		key(SUPER[0], true);
		tap(KEY_SPACE);
		key(SUPER[0], false);
		check(server->launcher.is_open(),
			"Super+Space opened the launcher: no capture swallowed it for a dead client");
		return 300;
	}
	case 62: {
		tap(KEY_ESC);
		check(!server->launcher.is_open(), "Escape closed the launcher again");
		run_client("state", "", "pending-alive");
		return 400;
	}
	case 63: { // the socket still answers; the plugin's surface goes with its client
		if (!client_done("pending-alive") && retries++ < 50) {
			return -100;
		}
		retries = 0;
		check(client_code("pending-alive") == 0 && !client_out("pending-alive").empty(),
			"the control socket still answers after three clients left calls pending");
		if (layer_pid > 0) {
			kill(layer_pid, SIGTERM);
		}
		step = 74; // on to the window list
		return 300;
	}
	case 75: { // the state lists the open window, and a jump goes to it
		if (!client_done("state-windows") && retries++ < 50) {
			return -100;
		}
		retries = 0;
		View *view = foot_window();
		auto id = static_cast<long long>(view != nullptr ? view->session_id : 0);
		std::optional<WindowEntry> entry =
			parse_state_window(client_out("state-windows"), id);
		check(entry.has_value() && entry->app_id == "foot" && !entry->minimized,
			std::format("the state lists the open foot window (app id '{}')",
				entry.has_value() ? entry->app_id : "none"));
		// Away from the window, then back to it through window.focus.
		Output *output = server->output_under_cursor();
		if (output != nullptr && view != nullptr) {
			Camera cam = output->camera.current();
			cam.cx += 4 * output->width;
			output->camera.jump(cam);
			check(!view->frame_rect().intersects(output->viewport()),
				"the view was moved off the window before the jump");
		}
		run_client("window.focus", std::format(R"({{"window":{}}})", id),
			"window-focus");
		run_client("window.focus", R"({"window":99999})", "window-focus-bad");
		return 400;
	}
	case 76: { // the camera came back to the window; the unknown id errored
		Output *output = server->output_under_cursor();
		View *view = foot_window();
		if ((!client_done("window-focus") || !client_done("window-focus-bad")
				|| output == nullptr || output->camera.panning()
				|| output->camera.zooming()) && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(client_code("window-focus") == 0 && view != nullptr
				&& output != nullptr
				&& view->frame_rect().intersects(output->viewport())
				&& server->focused_view == view,
			"window.focus brought the view back to the window and focused it");
		check(client_code("window-focus-bad") != 0,
			"window.focus with an unknown id answers an error");
		// A window on no desktop: moved far out onto free canvas, with a
		// second desktop beside the first so the jump has a choice to make
		// (docs/intents/shell/app-bar.md).
		if (view != nullptr && output != nullptr) {
			server->canvas.desktops.create(wlr_box{4 * output->width, 0,
				output->width, output->height});
			view->move(40000, 40000); // clear of every desktop
			Camera cam = output->camera.current();
			cam.cx = 0;
			cam.cy = 0;
			output->camera.jump(cam);
			free_window_id = static_cast<long long>(view->session_id);
		}
		run_client("window.focus",
			std::format(R"({{"window":{}}})", free_window_id), "window-free");
		return 500;
	}
	case 77: { // the view centred on the window, no desktop involved
		Output *output = server->output_under_cursor();
		View *view = foot_window();
		if ((!client_done("window-free") || output == nullptr
				|| output->camera.panning() || output->camera.zooming())
				&& retries++ < 100) {
			return -100;
		}
		retries = 0;
		wlr_box frame = view != nullptr ? view->frame_box() : wlr_box{};
		Camera cam = output != nullptr ? output->camera.current() : Camera{};
		double off_x = cam.cx - (frame.x + frame.width / 2.0);
		double off_y = cam.cy - (frame.y + frame.height / 2.0);
		check(view != nullptr && server->canvas.desktop_of(*view) == nullptr,
			"the window sits on no desktop");
		check(std::fabs(off_x) <= 1 && std::fabs(off_y) <= 1
				&& std::fabs(cam.zoom - 1) < 1e-9,
			std::format("window.focus centred the view on a window with no"
				" desktop (off by {:.0f},{:.0f} at zoom {:.2f})",
				off_x, off_y, cam.zoom));
		return 300;
	}
	case 78: { // settings.set compares the number with its range before narrowing it
		conf_before = server->conf_text;
		// 4294967301 is 2^32 + 5 and 4294971296 is 2^32 + 4000: as an int,
		// the first is a valid idle time and the second a valid temperature.
		run_client("settings.set", R"({"lockIdleMinutes":4294967301})", "idle-wraps");
		run_client("settings.set", R"({"nightLightTemperature":4294971296})",
			"temperature-wraps");
		return 400;
	}
	case 79: {
		for (const char *name : {"idle-wraps", "temperature-wraps"}) {
			if (!client_done(name) && retries++ < 50) {
				return -100;
			}
		}
		retries = 0;
		check(client_code("idle-wraps") == 1
				&& client_err("idle-wraps").find("lockIdleMinutes") != std::string::npos,
			std::format("`settings.set` of a lockIdleMinutes past an int is an error ({})",
				client_err("idle-wraps")));
		check(client_code("temperature-wraps") == 1
				&& client_err("temperature-wraps").find("nightLightTemperature")
					!= std::string::npos,
			std::format("`settings.set` of a temperature past an int is an error ({})",
				client_err("temperature-wraps")));
		check(server->conf_text == conf_before
				&& server->conf.lock_idle_minutes == settings::lock_idle.fallback
				&& server->conf.night_light_k == settings::night_light_temperature.fallback,
			"canvas.conf and the settings in force are unchanged");
		run_client("settings.set", R"({"lockIdleMinutes":7})", "idle-seven");
		return 400;
	}
	case 80: {
		if ((!client_done("idle-seven") || server->conf.lock_idle_minutes != 7)
				&& retries++ < 100) {
			return -100;
		}
		check(client_code("idle-seven") == 0 && server->conf.lock_idle_minutes == 7,
			"a lockIdleMinutes in range is still written");
		// The settings table, end to end: what settings.get reports of every
		// row, what settings.set refuses of each, and the usage naming all.
		conf_before = server->conf_text;
		run_client("settings.get", "", "settings-get");
		run_client("settings.set", "", "settings-usage");
		for (const Refusal &refusal : refusals()) {
			run_client("settings.set", refusal.params, refusal.name.c_str());
		}
		return 600;
	}
	case 81: {
		std::vector<Refusal> asked = refusals();
		bool done = client_done("settings-get") && client_done("settings-usage");
		for (const Refusal &refusal : asked) {
			done = done && client_done(refusal.name.c_str());
		}
		if (!done && retries++ < 100) {
			return -100;
		}
		std::string problem = settings_get_problem(client_out("settings-get"));
		check(client_code("settings-get") == 0 && problem.empty(),
			std::format("`settings.get` describes every row of the settings table ({})",
				problem));
		bool names_all = client_code("settings-usage") == 1;
		for (const settings::Field &field : settings::fields) {
			names_all = names_all
				&& client_err("settings-usage").find(field.param) != std::string::npos;
		}
		check(names_all, std::format("`settings.set` with no params names every setting "
			"in its usage ({})", client_err("settings-usage")));
		for (const Refusal &refusal : asked) {
			check(client_code(refusal.name.c_str()) == 1
					&& client_err(refusal.name.c_str()).find(refusal.field->param)
						!= std::string::npos,
				std::format("`settings.set` {} is refused naming the setting ({})",
					refusal.params, client_err(refusal.name.c_str())));
		}
		check(server->conf_text == conf_before, "no refused settings.set changed canvas.conf");
		finished = true; // the quit is the run's end
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
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

} // namespace

int main() {
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr
			|| std::getenv("CANVAS_LAYER_CLIENT") == nullptr) {
		std::fprintf(stderr, "ipc: $WAYLAND_CANVAS_BIN or $CANVAS_LAYER_CLIENT is not set"
			" (meson passes them)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/ipc", root);
	mkdir_p(shot_dir);
	// Before the server starts: canvas.conf lives in this run's scratch,
	// never in the user's real ~/.config, and starts as the defaults: the
	// settings.set of an earlier run must not be this run's.
	std::string xdg_config = std::format("{}/scenarios/ipc/xdg-config", root);
	std::error_code ec;
	std::filesystem::remove_all(xdg_config, ec);
	setenv("XDG_CONFIG_HOME", xdg_config.c_str(), 1);
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
	// A compositor that stops by itself (its own shutdown, a lost backend)
	// ends the run early: without this the scenario would still exit 0.
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset(); // releases the control socket
	check(access(socket_path.c_str(), F_OK) != 0,
		"the control socket was removed when the compositor exited");
	return failures != 0;
}
