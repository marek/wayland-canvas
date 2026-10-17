// End-to-end control-IPC scenario: the JSON-lines protocol on the socket the
// compositor exports — every method, every error case (a malformed line
// answered with the connection kept, unknown methods, bad params, a line
// over 64 KiB answered then dropped, a slow subscriber dropped), each event
// type triggered for real, canvas items (created, updated, removed,
// dragged with a fake pointer, double-clicked, drawn in both views and
// themes), and the `wayland-canvas msg` client with its exit codes. Writes
// only under $CANVAS_TMP (meson passes the repo's tmp/); meson passes the
// compositor binary as $WAYLAND_CANVAS_BIN.
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <print>
#include <signal.h>
#include <source_location>
#include <string>
#include <string_view>
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
#include "canvas/items.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/theme.h"
#include "core/wlr.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
long long free_window_id = 0; // the window moved onto free canvas, for its jump
bool finished = false; // the run reached its end; see main()
uint32_t t = 90000;
pid_t grim_pid = -1;
pid_t raw_pid = -1;
pid_t sub_pid = -1;
pid_t item_sub_pid = -1;
int raw_status = 0;
std::string shot_dir;
std::string socket_path;
std::string events_path;
std::string item_events_path; // the canvas-items subscriber's capture
std::string output_name; // the headless output's name, for `output` params
wlr_pointer ptr{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};

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

// ------------------------------------------------------ item checking

// One item entry: from a `msg item.set` result or a state items-array slot.
struct ItemEntry {
	std::string id, color, text;
	long long x = 0, y = 0, width = 0, height = 0;
	bool placed = false; // x, y, width and height present and whole
};

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
std::optional<double> member_number(JsonObject *object, const char *name) {
	if (!json_object_has_member(object, name)) {
		return std::nullopt;
	}
	JsonNode *node = json_object_get_member(object, name);
	if (json_node_get_node_type(node) != JSON_NODE_VALUE) {
		return std::nullopt;
	}
	GType type = json_node_get_value_type(node);
	if (type == G_TYPE_INT64) {
		return static_cast<double>(json_node_get_int(node));
	}
	return type == G_TYPE_DOUBLE ? std::optional{json_node_get_double(node)}
		: std::nullopt;
}

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

void fill_item(ItemEntry &entry, JsonObject *object) {
	entry.id = member_string(object, "id");
	entry.color = member_string(object, "color");
	entry.text = member_string(object, "text");
	std::optional<long long> x = member_int(object, "x");
	std::optional<long long> y = member_int(object, "y");
	std::optional<long long> width = member_int(object, "width");
	std::optional<long long> height = member_int(object, "height");
	entry.placed = x.has_value() && y.has_value() && width.has_value()
		&& height.has_value();
	if (entry.placed) {
		entry.x = *x;
		entry.y = *y;
		entry.width = *width;
		entry.height = *height;
	}
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

// A `msg item.set` result parsed as one item entry.
std::optional<ItemEntry> parse_item_result(const std::string &text) {
	JsonParser *parser = json_parser_new();
	std::optional<ItemEntry> out;
	if (json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		JsonNode *root = json_parser_get_root(parser);
		JsonObject *object = root != nullptr
				&& json_node_get_node_type(root) == JSON_NODE_OBJECT
			? json_node_get_object(root) : nullptr;
		if (object != nullptr) {
			out = ItemEntry{};
			fill_item(*out, object);
		}
	}
	g_object_unref(parser);
	return out;
}

// The entry with `want_id` in a `msg state` result's items array.
std::optional<ItemEntry> parse_state_item(const std::string &text,
		std::string_view want_id) {
	JsonParser *parser = json_parser_new();
	std::optional<ItemEntry> out;
	if (json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)) {
		JsonNode *root = json_parser_get_root(parser);
		JsonObject *object = root != nullptr
				&& json_node_get_node_type(root) == JSON_NODE_OBJECT
			? json_node_get_object(root) : nullptr;
		if (object != nullptr && json_object_has_member(object, "items")) {
			JsonNode *items = json_object_get_member(object, "items");
			if (json_node_get_node_type(items) == JSON_NODE_ARRAY) {
				JsonArray *array = json_node_get_array(items);
				for (guint i = 0; i < json_array_get_length(array); i++) {
					JsonObject *element = json_array_get_object_element(array, i);
					if (element == nullptr) {
						continue;
					}
					ItemEntry entry;
					fill_item(entry, element);
					if (entry.id == want_id) {
						out = entry;
						break;
					}
				}
			}
		}
	}
	g_object_unref(parser);
	return out;
}

// The last `kind` (item.moved / item.activated) event for `id` in the item
// subscriber's capture: its place, and for an activation the output and the
// box on it.
struct ItemEventLine {
	long long x = 0, y = 0; // item.moved
	std::string output; // item.activated
	double bx = 0, by = 0, bw = 0, bh = 0; // item.activated box
};

std::optional<ItemEventLine> item_event(std::string_view kind,
		std::string_view id) {
	std::string text = read_file(item_events_path);
	std::optional<ItemEventLine> out;
	// read_file() drops the final newline, so the newest event runs to the end
	// of the text. A line still being written fails to parse; the next poll
	// reads it whole.
	std::size_t start = 0;
	while (start < text.size()) {
		std::size_t end = std::min(text.find('\n', start), text.size());
		std::string line = text.substr(start, end - start);
		start = end + 1;
		JsonParser *parser = json_parser_new();
		if (json_parser_load_from_data(parser, line.c_str(),
				static_cast<gssize>(line.size()), nullptr)) {
			JsonNode *root = json_parser_get_root(parser);
			JsonObject *object = root != nullptr
					&& json_node_get_node_type(root) == JSON_NODE_OBJECT
				? json_node_get_object(root) : nullptr;
			if (object != nullptr && member_string(object, "event") == kind
					&& json_object_has_member(object, "params")) {
				JsonNode *params_node = json_object_get_member(object, "params");
				JsonObject *params = json_node_get_node_type(params_node) == JSON_NODE_OBJECT
					? json_node_get_object(params_node) : nullptr;
				if (params != nullptr && member_string(params, "id") == id) {
					ItemEventLine event;
					std::optional<long long> x = member_int(params, "x");
					std::optional<long long> y = member_int(params, "y");
					if (x.has_value() && y.has_value()) {
						event.x = *x;
						event.y = *y;
					}
					event.output = member_string(params, "output");
					if (json_object_has_member(params, "box")) {
						JsonNode *box_node = json_object_get_member(params, "box");
						JsonObject *box =
							json_node_get_node_type(box_node) == JSON_NODE_OBJECT
							? json_node_get_object(box_node) : nullptr;
						if (box != nullptr) {
							event.bx = member_number(box, "x").value_or(-1);
							event.by = member_number(box, "y").value_or(-1);
							event.bw = member_number(box, "width").value_or(-1);
							event.bh = member_number(box, "height").value_or(-1);
						}
					}
					out = event; // the last one wins
				}
			}
		}
		g_object_unref(parser);
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
	case 29: { // the rest of the desktops lifecycle: move, combine, abandon
		Desktop &a = server->canvas.desktops.create(wlr_box{3840, 0, 1280, 720});
		Desktop &b = server->canvas.desktops.create(wlr_box{5120, 0, 1280, 720});
		server->canvas.desktops.move(b.id(), 5120, 720);
		// combine() removes `a`; nothing may touch it afterwards.
		server->canvas.combine(b.id(), a.id(), a.frame());
		server->canvas.desktops.remove(b.id());
		return 300;
	}
	case 30:
		if (events_count("\"event\":\"desktops\"") < 4 && retries++ < 50) {
			return -100;
		}
		check(events_count("\"event\":\"desktops\"") >= 4,
			std::format("create, move, combine and abandon each fired a desktops event ({})",
				events_count("\"event\":\"desktops\"")));
		return 200;
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
		// A flood of desktops events: past 1 MiB of backlog, the drop is
		// immediate, while the child sleeps without reading.
		for (int i = 0; i < 300; i++) {
			server->canvas.desktops.create(wlr_box{5120 + (i % 8) * 1280,
				720 + (i / 8) * 720, 1280, 720});
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
	case 48: { // canvas items: a fake pointer, like the input scenario's
		ptr = wlr_pointer{};
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		Output *output = server->outputs.front().get();
		check(output != nullptr && output->width == 1280 && output->height == 720,
			"one 1280x720 output, as the fake pointer's coordinates assume");
		return 300;
	}
	case 49: // item.set creates: centred without x/y, placed with them
		run_client("item.set",
			R"({"id":"canvas.notes/a","color":"#ffcf5c","text":"Remember the milk","width":220,"height":220})",
			"item-centre");
		run_client("item.set",
			R"({"id":"canvas.notes/b","color":"#7bd88f","text":"Green","width":220,"height":220,"x":260,"y":140})",
			"item-xy");
		return 300;
	case 50: {
		if ((!client_done("item-centre") || !client_done("item-xy"))
				&& retries++ < 50) {
			return -100;
		}
		Output *output = server->output_under_cursor();
		Rect viewport = output->viewport();
		int want_x = static_cast<int>(std::round(viewport.cx() - 110));
		int want_y = static_cast<int>(std::round(viewport.cy() - 110));
		std::optional<ItemEntry> centred = parse_item_result(client_out("item-centre"));
		check(centred.has_value() && centred->placed && centred->id == "canvas.notes/a"
				&& centred->x == want_x && centred->y == want_y
				&& centred->width == 220 && centred->height == 220,
			std::format("`msg item.set` without x/y centres on what the output shows ({})",
				centred.has_value() ? client_out("item-centre") : "no result"));
		check(client_code("item-centre") == 0, "`msg item.set` exits 0");
		std::optional<ItemEntry> placed = parse_item_result(client_out("item-xy"));
		check(placed.has_value() && placed->placed && placed->id == "canvas.notes/b"
				&& placed->x == 260 && placed->y == 140,
			std::format("`msg item.set` with x/y returns the given place ({})",
				placed.has_value() ? client_out("item-xy") : "no result"));
		return 200;
	}
	case 51: // an update keeps its place; bad params and unknown ids error
		run_client("item.set",
			R"({"id":"canvas.notes/a","color":"#ff9f0a","text":"Orange now","width":200,"height":240})",
			"item-update");
		run_client("item.set", R"({"color":"#ffcf5c","text":"x","width":10,"height":10})",
			"item-no-id");
		run_client("item.set",
			R"({"id":"canvas.notes/a","color":"red","text":"x","width":10,"height":10})",
			"item-bad-color");
		run_client("item.set",
			R"({"id":"canvas.notes/a","color":"#ffcf5c","text":"x","width":0,"height":10})",
			"item-zero-width");
		run_client("item.set",
			R"({"id":"canvas.notes/a","color":"#ffcf5c","text":"x","width":10,"height":10,"x":"left"})",
			"item-bad-x");
		run_client("item.remove", R"({"id":"canvas.notes/zzz"})", "item-unknown");
		return 300;
	case 52: {
		const char *names[] = {"item-update", "item-no-id", "item-bad-color",
			"item-zero-width", "item-bad-x", "item-unknown"};
		for (const char *name : names) {
			if (!client_done(name) && retries++ < 50) {
				return -100;
			}
		}
		Output *output = server->output_under_cursor();
		Rect viewport = output->viewport();
		int want_x = static_cast<int>(std::round(viewport.cx() - 110));
		int want_y = static_cast<int>(std::round(viewport.cy() - 110));
		std::optional<ItemEntry> updated = parse_item_result(client_out("item-update"));
		Item *a = server->items.find("canvas.notes/a");
		check(updated.has_value() && updated->placed && a != nullptr
				&& updated->x == want_x && updated->y == want_y
				&& updated->width == 200 && updated->height == 240
				&& a->x == want_x && a->y == want_y && a->color == 0xff9f0aff
				&& a->text == "Orange now",
			std::format("an `item.set` update keeps the item's place ({})",
				updated.has_value() ? client_out("item-update") : "no result"));
		check(client_code("item-no-id") == 1
				&& client_err("item-no-id").find("id") != std::string::npos,
			std::format("`msg item.set` without an id: exit 1 naming it ({})",
				client_err("item-no-id")));
		check(client_code("item-bad-color") == 1
				&& client_err("item-bad-color").find("color") != std::string::npos,
			std::format("a colour that is not #rrggbb: exit 1 naming it ({})",
				client_err("item-bad-color")));
		check(client_code("item-zero-width") == 1
				&& client_err("item-zero-width").find("width") != std::string::npos,
			std::format("a zero width: exit 1 naming it ({})",
				client_err("item-zero-width")));
		check(client_code("item-bad-x") == 1
				&& client_err("item-bad-x").find("x") != std::string::npos,
			std::format("an x that is not a number: exit 1 naming it ({})",
				client_err("item-bad-x")));
		check(client_code("item-unknown") == 1
				&& client_err("item-unknown").find("canvas.notes/zzz") != std::string::npos,
			std::format("`msg item.remove` of an unknown id: exit 1 naming it ({})",
				client_err("item-unknown")));
		return 200;
	}
	case 53: // the state lists the items
		run_client("state", "", "item-state");
		return 300;
	case 54: {
		if (!client_done("item-state") && retries++ < 50) {
			return -100;
		}
		std::string state = client_out("item-state");
		std::optional<ItemEntry> a = parse_state_item(state, "canvas.notes/a");
		std::optional<ItemEntry> b = parse_state_item(state, "canvas.notes/b");
		check(a.has_value() && a->placed && a->color == "#ff9f0a" && a->text == "Orange now"
				&& a->width == 200 && a->height == 240,
			std::format("the state lists the item as last set ({})",
				a.has_value() ? "with its colour, text and size" : "missing"));
		check(b.has_value() && b->placed && b->x == 260 && b->y == 140,
			"the state lists the second item at its given place");
		return 200;
	}
	case 55: // item.remove of a live id
		run_client("item.remove", R"({"id":"canvas.notes/b"})", "item-remove");
		return 300;
	case 56: {
		if (!client_done("item-remove") && retries++ < 50) {
			return -100;
		}
		check(client_code("item-remove") == 0 && client_out("item-remove") == "true"
				&& server->items.find("canvas.notes/b") == nullptr,
			"`msg item.remove` removed the item and printed true");
		return 200;
	}
	case 57: { // a clean canvas for the pictures, a subscriber for the item events
		item_events_path = shot_dir + "/item-events.out";
		item_sub_pid = fork();
		if (item_sub_pid == 0) {
			// The compositor blocks signals for its signalfd loop: without
			// clearing the mask, a later SIGINT would stay pending forever.
			sigset_t empty;
			sigemptyset(&empty);
			sigprocmask(SIG_SETMASK, &empty, nullptr);
			int out = open(item_events_path.c_str(),
				O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (out < 0 || dup2(out, 1) < 0) {
				_exit(126);
			}
			close(out);
			execlp(std::getenv("WAYLAND_CANVAS_BIN"), "wayland-canvas", "msg",
				"subscribe", static_cast<char *>(nullptr));
			_exit(127);
		}
		// The flood desktops go: the pictures want a clean canvas.
		std::vector<DesktopId> doomed;
		for (const Desktop &d : server->canvas.desktops.all()) {
			if (d.number() != 1) {
				doomed.push_back(d.id());
			}
		}
		for (DesktopId id : doomed) {
			server->canvas.desktops.remove(id);
		}
		check(server->canvas.desktops.size() == 1,
			"the flood desktops went, leaving the first");
		View *view = foot_window();
		check(view != nullptr, "the foot window is still there");
	if (view != nullptr) {
		Output *output = server->output_under_cursor();
		Desktop *home = output->desktop_showing();
		// The camera comes to rest at the desktop's scale, which may not
		// be 1: the item tests below must hold at any resting zoom.
		output->camera.animate_to(camera_for_box(view->frame_box(),
			home != nullptr ? home->scale() : 1.0));
	}
		return 700; // the camera glide settles
	}
	case 58: { // the item over the window: it owns the pointer, the window does not
		View *view = foot_window();
		Output *output = server->output_under_cursor();
		Item *a = server->items.find("canvas.notes/a");
		// The subscriber proves itself before anything depends on it: a probe
		// desktop appears and goes far away, and its event line must show up
		// in the capture — only then do the drag and the double-click run.
		if (read_file(item_events_path).find("\"event\":\"desktops\"")
				== std::string::npos) {
			if ((retries & 3) == 0) {
				Desktop &probe =
					server->canvas.desktops.create(wlr_box{12800, 12800, 1280, 720});
				server->canvas.desktops.remove(probe.id());
			}
			if (retries++ < 150) {
				return -200;
			}
		}
		if ((view == nullptr || a == nullptr
				|| output->camera.panning() || output->camera.zooming())
				&& retries++ < 100) {
			return -200;
		}
		wlr_box frame = view->frame_box();
		wlr_box content = view->content_box();
		server->items.set("canvas.notes/a", ItemUpdate{},
			static_cast<int>(frame.x + frame.width / 2.0) - a->width / 2,
			static_cast<int>(frame.y + frame.height / 2.0) - a->height / 2);
		// A second card beside the window for the pictures, left of it and
		// clear of the first card so the pointer tests keep hitting the first.
		int c_x = frame.x - 240; // 220 wide with a 20 px gap
		int c_y = static_cast<int>(frame.y + frame.height / 2.0) - 110;
		Item *c = server->items.set("canvas.notes/c",
			ItemUpdate{.color = 0xff7eb6ff,
				.text = "Water the ferns by the window, and call mum back",
				.width = 220, .height = 220},
			c_x, c_y);
		check(c != nullptr && c->x == c_x && c->y == c_y && c->width == 220,
			"the second card is beside the window at its given place");
		a = server->items.find("canvas.notes/a");
		Rect a_box{static_cast<double>(a->x), static_cast<double>(a->y),
			static_cast<double>(a->width), static_cast<double>(a->height)};
		// Hover the window's surface beside the item: it takes pointer focus.
		const double spots[3][2] = {{0.08, 0.08}, {0.92, 0.92}, {0.08, 0.92}};
		bool window_focus = false;
		for (const auto &spot : spots) {
			double wx = content.x + content.width * spot[0];
			double wy = content.y + content.height * spot[1];
			if (a_box.contains(wx, wy)) {
				continue;
			}
			Point local = output->canvas_to_local(wx, wy);
			move_to(local.x, local.y);
			if (server->seat->pointer_state.focused_surface != nullptr) {
				window_focus = true;
				break;
			}
		}
		check(window_focus, "the window takes pointer focus beside the item");
		// Then the item itself: no window keeps pointer focus under it.
		Point centre = output->canvas_to_local(a_box.cx(), a_box.cy());
		move_to(centre.x, centre.y);
		check(server->seat->pointer_state.focused_surface == nullptr,
			"hovering the item gives the window under it no pointer focus");
		return 300;
	}
	case 59: { // a drag past the threshold moves the item, in the normal view
		Output *output = server->output_under_cursor();
		Item *a = server->items.find("canvas.notes/a");
		int start_x = a->x, start_y = a->y;
		Point centre = output->canvas_to_local(a->x + a->width / 2.0,
			a->y + a->height / 2.0);
		drag(centre.x, centre.y, centre.x + 150, centre.y + 80);
		check(a->x == start_x + 150 && a->y == start_y + 80,
			std::format("the drag moved the item by (150,80) -> ({},{})",
				a->x - start_x, a->y - start_y));
		check(!server->input.grab.active() && !server->input.grab.panning(*output),
			"the drag ended with the release");
		check(server->seat->pointer_state.focused_surface == nullptr,
			"dragging the item never focused the window under it");
		return 300;
	}
	case 60: { // the drag's release reported item.moved
	std::optional<ItemEventLine> moved = item_event("item.moved", "canvas.notes/a");
	Item *a = server->items.find("canvas.notes/a");
	if ((!moved.has_value() || moved->x != a->x || moved->y != a->y)
			&& retries++ < 50) {
		return -100;
	}
	moved = item_event("item.moved", "canvas.notes/a");
	check(moved.has_value() && moved->x == a->x && moved->y == a->y,
		std::format("the drop fired item.moved with the new place ({},{})",
			moved.has_value() ? moved->x : -999, moved.has_value() ? moved->y : -999));
		return 300;
	}
	case 61: { // a double-click reports item.activated
		Output *output = server->output_under_cursor();
		Item *a = server->items.find("canvas.notes/a");
		Point centre = output->canvas_to_local(a->x + a->width / 2.0,
			a->y + a->height / 2.0);
		wlr_surface *keyboard = server->seat->keyboard_state.focused_surface;
		click(centre.x, centre.y);
		click(centre.x, centre.y);
		check(keyboard != nullptr
				&& server->seat->keyboard_state.focused_surface == keyboard,
			"clicking the item gave it no keyboard: the window kept it");
		return 300;
	}
	case 62: { // the activation's box matches the item on screen
		std::optional<ItemEventLine> activated =
			item_event("item.activated", "canvas.notes/a");
		Output *output = server->output_under_cursor();
		Item *a = server->items.find("canvas.notes/a");
		Point top_left = output->canvas_to_local(a->x, a->y);
		double zoom = output->camera.drawn().zoom;
		bool box_matches = activated.has_value() && activated->output == output_name
			&& std::fabs(activated->bx - top_left.x) < 0.5
			&& std::fabs(activated->by - top_left.y) < 0.5
			&& std::fabs(activated->bw - a->width * zoom) < 0.5
			&& std::fabs(activated->bh - a->height * zoom) < 0.5;
		if (!box_matches && retries++ < 50) {
			return -100;
		}
	check(box_matches,
		std::format("the double-click fired item.activated with the item's box "
			"on {} in output-local px ({})", output_name,
			activated.has_value() ? std::format("{:.0f},{:.0f} {:.0f}x{:.0f}",
				activated->bx, activated->by, activated->bw, activated->bh)
				: "no event"));
		return 200;
	}
	case 63: // the items on screen, normal view, dark
		shot("item-normal-dark");
		return 500;
	case 64: // the overview, dark
		run_client("overview.toggle", "", "item-overview-enter");
		return 300;
	case 65: {
		Output *output = server->output_under_cursor();
		if ((!client_done("item-overview-enter") || output == nullptr
				|| !output->overview.active() || output->camera.zooming()
				|| output->camera.panning()) && retries++ < 50) {
			return -100;
		}
		return 600; // the settled overview re-renders the cards at its scale
	}
	case 66:
		shot("item-overview-dark");
		return 500;
	case 67: // back to the normal view, and the light palette
		run_client("overview.toggle", "", "item-overview-exit");
		run_client("theme.set", R"({"mode":"light"})", "item-theme-light");
		return 300;
	case 68: {
		Output *output = server->output_under_cursor();
		if ((!client_done("item-overview-exit") || !client_done("item-theme-light")
				|| output == nullptr || output->overview.active()
				|| output->camera.zooming() || output->camera.panning()
				|| server->theme.mode() != ThemeMode::light) && retries++ < 50) {
			return -100;
		}
		return 600;
	}
	case 69:
		shot("item-normal-light");
		return 500;
	case 70:
		run_client("overview.toggle", "", "item-overview-light-enter");
		return 300;
	case 71: {
		Output *output = server->output_under_cursor();
		if ((!client_done("item-overview-light-enter") || output == nullptr
				|| !output->overview.active() || output->camera.zooming()
				|| output->camera.panning()) && retries++ < 50) {
			return -100;
		}
		return 600;
	}
	case 72:
		shot("item-overview-light");
		return 500;
	case 73: // leave the overview, stop the item subscriber
		run_client("overview.toggle", "", "item-overview-light-exit");
		if (item_sub_pid > 0) {
			kill(item_sub_pid, SIGINT);
		}
		return 300;
	case 74: {
		bool exited = item_sub_pid > 0
			&& waitpid(item_sub_pid, &raw_status, WNOHANG) == item_sub_pid;
		if ((!exited || !client_done("item-overview-light-exit")) && retries++ < 50) {
			return -100;
		}
		check(exited, "the item-events subscriber ended with the SIGINT");
		retries = 0;
		run_client("state", "", "state-windows");
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
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr) {
		std::fprintf(stderr, "ipc: $WAYLAND_CANVAS_BIN is not set (meson passes it)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/ipc", root);
	mkdir_p(shot_dir);
	// Before the server starts: canvas.conf lives in this run's scratch,
	// never in the user's real ~/.config.
	setenv("XDG_CONFIG_HOME", std::format("{}/scenarios/ipc/xdg-config", root).c_str(), 1);
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
	server.reset(); // releases the control socket
	check(access(socket_path.c_str(), F_OK) != 0,
		"the control socket was removed when the compositor exited");
	return failures != 0;
}
