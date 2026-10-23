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
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <format>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <variant>
#include <vector>
#include "core/server.h"
#include "core/settings.h"
#include "core/theme.h"
#include "core/wlr.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/control.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/run.h"
#include "platform/ipc_client.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "windows/view.h"

using namespace harness;

namespace {

Child subscriber; // `msg subscribe`, from the subscription steps to its interrupt
Child layer_client; // the layer-shell client drawing the plugin surface
std::string socket_path;
std::string output_name; // the headless output's name, for `output` params
std::string conf_before; // canvas.conf's text before a settings.set that must change nothing
// A plugin surface name no client draws: what the socket says of it.
const std::string surface_name = "wayland-canvas-canvas-ipc.scenario/one";
pid_t raw_pid = -1; // the raw client or slow subscriber being waited for
pid_t pending_pid = -1; // a client that disconnects with a call pending
// The desktops lifecycle's two desktops, and the desktops events heard
// before it began.
std::optional<DesktopId> life_a, life_b;
int lifecycle_heard = 0;
int flooded = 0; // desktops the flood step has created

// A settings.set the table says must fail, named for its client's files.
struct Refusal {
	std::string name, params;
	const settings::Field *field;
};
std::vector<Refusal> asked; // the refusals the settings step sent

// True when the subscriber has printed an event line containing `needle`.
bool heard(std::string_view needle) {
	return subscriber.said().find(needle) != std::string_view::npos;
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

// ---------------------------------------------------------- raw clients

// A client that sends one request line, lingers `linger_ms` without reading
// the answer, and hangs up: a call left pending when its client goes.
pid_t fork_hangup(const std::string &request, int linger_ms) {
	return fork_client([=] {
		OwnedFd fd(connect_to_socket());
		if (fd.get() < 0 || !write_request(fd.get(), request + "\n")) {
			_exit(125);
		}
		usleep(static_cast<useconds_t>(linger_ms) * 1000);
	});
}

// A client that subscribes, writes "ready" to the file `ready` once the
// subscription is answered, then reads nothing for 3 s while the parent
// floods events, and finally drains: it writes "dropped <bytes>" to `result`
// when the compositor dropped it (EOF), or "alive <bytes>" when the
// connection survived 10 s.
pid_t fork_slow_subscriber(const std::string &ready, const std::string &result) {
	return fork_client([=] {
		OwnedFd fd(connect_to_socket());
		if (fd.get() < 0 || !write_request(fd.get(), "{\"id\":1,\"method\":\"subscribe\"}\n")) {
			_exit(125);
		}
		// The subscribe response is read first, so `ready` means really subscribed.
		if (!read_line(fd.get()).has_value()) {
			_exit(124);
		}
		write_file(ready, "ready\n");
		// Not a single read while the parent floods: the backlog must build.
		sleep(3);
		long bytes = 0;
		bool eof = false;
		char buf[65536];
		auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!eof && std::chrono::steady_clock::now() < give_up) {
			pollfd waiting{.fd = fd.get(), .events = POLLIN, .revents = 0};
			if (poll(&waiting, 1, 200) == 0) {
				continue;
			}
			if ((waiting.revents & (POLLERR | POLLNVAL)) != 0) {
				break;
			}
			ssize_t got = read(fd.get(), buf, sizeof buf);
			if (got <= 0) {
				eof = true;
				break;
			}
			bytes += got;
		}
		write_file(result, std::format("{} {}\n", eof ? "dropped" : "alive", bytes));
	});
}

// ------------------------------------------------------ state checking

// The first failed expectation about a `msg state` result, or empty when the
// result is well-formed: `want_theme`, at least `want_outputs` outputs (the
// first reporting `want_overview`), at least `want_desktops` desktops.
std::string state_mismatch(const std::string &text, std::string_view want_theme,
		int want_outputs, bool want_overview, int want_desktops) {
	JsonAnswer answer(text);
	JsonObject *object = answer.object();
	if (object == nullptr) {
		return "the result is not a JSON object";
	}
	if (!json_object_has_member(object, "theme")) {
		return "the result has no theme";
	}
	std::string theme = member_string(object, "theme");
	if (theme != want_theme) {
		return std::format("theme is {}, not {}", theme.empty() ? "not a string" : theme,
			want_theme);
	}
	if (json_object_has_member(object, "outputs")) {
		JsonNode *outputs = json_object_get_member(object, "outputs");
		if (json_node_get_node_type(outputs) != JSON_NODE_ARRAY) {
			return "outputs is not an array";
		}
		JsonArray *list = json_node_get_array(outputs);
		if (static_cast<int>(json_array_get_length(list)) < want_outputs) {
			return std::format("only {} outputs", json_array_get_length(list));
		}
		JsonObject *first = json_array_get_object_element(list, 0);
		if (!json_object_has_member(first, "name") || !json_object_has_member(first, "overview")
				|| !json_object_has_member(first, "scale")) {
			return "an output entry lacks name, overview or scale";
		}
		if (json_object_get_boolean_member(first, "overview") != want_overview) {
			return want_overview ? "the output is not in the overview"
				: "the output is in the overview";
		}
	}
	if (json_object_has_member(object, "desktops")) {
		JsonNode *desktops = json_object_get_member(object, "desktops");
		if (json_node_get_node_type(desktops) != JSON_NODE_ARRAY) {
			return "desktops is not an array";
		}
		if (static_cast<int>(json_array_get_length(json_node_get_array(desktops)))
				< want_desktops) {
			return std::format("only {} desktops",
				json_array_get_length(json_node_get_array(desktops)));
		}
	}
	return {};
}

// One entry of a `msg state` result's windows array, by its id: what the app
// bar draws (docs/intents/shell/app-bar.md).
struct WindowEntry {
	std::string app_id, title;
	bool has_desktop = false, minimized = false, focused = false;
};

std::optional<WindowEntry> parse_state_window(const std::string &text, long long want_id) {
	JsonAnswer answer(text);
	JsonObject *object = answer.object();
	JsonNode *windows = object != nullptr && json_object_has_member(object, "windows")
		? json_object_get_member(object, "windows") : nullptr;
	if (windows == nullptr || json_node_get_node_type(windows) != JSON_NODE_ARRAY) {
		return std::nullopt;
	}
	JsonArray *array = json_node_get_array(windows);
	for (guint i = 0; i < json_array_get_length(array); i++) {
		JsonObject *entry = json_array_get_object_element(array, i);
		if (entry == nullptr || member_int(entry, "id") != want_id) {
			continue;
		}
		JsonNode *desktop = json_object_has_member(entry, "desktop")
			? json_object_get_member(entry, "desktop") : nullptr;
		return WindowEntry{
			.app_id = member_string(entry, "appId"),
			.title = member_string(entry, "title"),
			.has_desktop = desktop != nullptr
				&& json_node_get_node_type(desktop) != JSON_NODE_NULL,
			.minimized = json_object_get_boolean_member(entry, "minimized") != FALSE,
			.focused = json_object_get_boolean_member(entry, "focused") != FALSE,
		};
	}
	return std::nullopt;
}

// How `answer` fails to describe `field`: its section and key, its bounds or
// names, and its default; empty when it does.
std::string field_problem(JsonObject *answer, JsonObject *fields, const settings::Field &field) {
	std::string param{field.param};
	JsonObject *section = json_object_has_member(answer, std::string{field.section}.c_str())
		? json_object_get_object_member(answer, std::string{field.section}.c_str()) : nullptr;
	JsonObject *row = json_object_has_member(fields, param.c_str())
		? json_object_get_object_member(fields, param.c_str()) : nullptr;
	if (section == nullptr || !json_object_has_member(section, param.c_str())) {
		return std::format("{} has no value in [{}]", param, field.section);
	}
	if (row == nullptr) {
		return std::format("{} has no row in fields", param);
	}
	if (member_string(row, "section") != field.section || member_string(row, "key") != field.key) {
		return std::format("{} is not given its section and key", param);
	}
	if (field.kind == settings::Kind::integer) {
		std::optional<long long> low = member_int(row, "min");
		std::optional<long long> high = member_int(row, "max");
		std::optional<long long> fallback = member_int(row, "default");
		if (member_string(row, "kind") != "integer" || low != (field.off ? 0 : field.min)
				|| high != field.max || fallback != field.fallback) {
			return std::format("{} does not report its bounds and default", param);
		}
	} else if (field.kind == settings::Kind::toggle) {
		if (member_string(row, "kind") != "toggle" || !json_object_has_member(row, "default")
				|| (json_object_get_boolean_member(row, "default") != FALSE)
					!= (field.fallback != 0)) {
			return std::format("{} does not report its default", param);
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
			return std::format("{} does not report its names and default", param);
		}
	}
	return {};
}

// The first way a `settings.get` answer fails to describe the settings
// table, or empty when its `fields` give every row its section, key, kind,
// bounds or names, and default, and its sections hold a value for each.
std::string settings_get_problem(const std::string &text) {
	JsonAnswer parsed(text);
	JsonObject *answer = parsed.object();
	if (answer == nullptr) {
		return "the answer is not a JSON object";
	}
	JsonObject *fields = json_object_has_member(answer, "fields")
		? json_object_get_object_member(answer, "fields") : nullptr;
	if (fields == nullptr) {
		return "the answer has no fields";
	}
	for (const settings::Field &field : settings::fields) {
		std::string problem = field_problem(answer, fields, field);
		if (!problem.empty()) {
			return problem;
		}
	}
	return {};
}

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

// Commits a new output scale; the layout follows, and the outputs event.
void commit_scale(float scale) {
	Output *output = server->output_under_cursor();
	wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_scale(&state, scale);
	wlr_output_commit_state(output->wlr, &state);
	wlr_output_state_finish(&state);
}

// ---------------------------------------------------------------- steps

Result socket_exported() {
	const char *env_socket = std::getenv("WAYLAND_CANVAS_SOCKET");
	socket_path = env_socket != nullptr ? env_socket : "";
	struct stat st{};
	bool is_socket = stat(socket_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
	check(!socket_path.empty() && is_socket,
		std::format("the compositor exports a socket at $WAYLAND_CANVAS_SOCKET ({})",
			socket_path));
	output_name = server->outputs.empty() ? "" : server->outputs.front()->wlr->name;
	check(!output_name.empty(), "the headless output has a name for params");
	return after(200);
}

// A real foot window, so the title bars have something to frame.
Result window_spawned() {
	spawn("foot -T ipc sh -c 'echo ipc scenario; exec sleep 600'");
	return after(400);
}

Result window_mapped() {
	if (auto wait = wait_for(first_mapped_view() != nullptr)) {
		return *wait;
	}
	int mapped = 0;
	View *view = nullptr;
	for (View *v : server->views) {
		if (v->mapped) {
			mapped++;
			view = v;
		}
	}
	check(mapped == 1, std::format("one foot window mapped ({})", mapped));
	check(view != nullptr && view->deco.ssd, "foot negotiated a server-side title bar");
	if (view != nullptr) {
		server->focus_view(view); // an active title bar for the screenshot
	}
	shot("titlebar-dark");
	return after(500);
}

// The state method through the real client.
Result state_asked() {
	run_msg("state", "", "state-dark");
	return after(300);
}

Result state_answered() {
	if (auto wait = wait_for(command_done("state-dark"))) {
		return *wait;
	}
	std::string out = command_out("state-dark");
	std::string problem = state_mismatch(out, "dark", 1, false, 1);
	check(problem.empty(),
		std::format("`msg state` reports the dark theme, the output, the desktops ({})",
			problem.empty() ? out : problem));
	check(command_code("state-dark") == 0, "`msg state` exits 0");
	return after(200);
}

// Ids echoed, responses in order, on one connection.
Result order_asked() {
	raw_pid = fork_control_client(
		"{\"id\":1,\"method\":\"state\"}\n{\"id\":\"two\",\"method\":\"subscribe\"}\n",
		"raw-order.out");
	return after(600);
}

Result order_answered() {
	if (auto wait = wait_for(control_client_done(raw_pid))) {
		return *wait;
	}
	std::string out = control_answer("raw-order.out");
	std::size_t first = out.find("\"id\":1,");
	std::size_t second = out.find("\"id\":\"two\"");
	check(first != std::string::npos && second != std::string::npos && first < second
			&& out.find("\"result\"") != std::string::npos,
		std::format("responses come in request order with their ids ({})", out));
	return after(200);
}

// Malformed lines are answered, and the connection stays.
Result malformed_asked() {
	raw_pid = fork_control_client("nonsense\n"
		"{\"id\":\"abc\"}\n"
		"{\"id\":5,\"method\":\"theme.set\",\"params\":\"x\"}\n"
		"{\"id\":7,\"method\":\"state\"}\n",
		"raw-malformed.out");
	return after(700);
}

Result malformed_answered() {
	if (auto wait = wait_for(control_client_done(raw_pid))) {
		return *wait;
	}
	std::string out = control_answer("raw-malformed.out");
	check(out.find("\"id\":null") != std::string::npos && out.find("error") != std::string::npos,
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
	return after(200);
}

// An unknown method is answered with an error naming it.
Result unknown_asked() {
	raw_pid = fork_control_client("{\"id\":9,\"method\":\"frobnicate\"}\n", "raw-unknown.out");
	return after(500);
}

Result unknown_answered() {
	if (auto wait = wait_for(control_client_done(raw_pid))) {
		return *wait;
	}
	std::string out = control_answer("raw-unknown.out");
	check(out.find("\"id\":9") != std::string::npos && out.find("frobnicate") != std::string::npos
			&& out.find("error") != std::string::npos,
		std::format("an unknown method is answered naming it ({})", out));
	return after(200);
}

// The msg client's own error paths.
Result client_errors_asked() {
	run_msg("theme.set", R"({"mode":"blue"})", "theme-blue");
	run_msg("launcher.toggle", R"({"output":"NOPE"})", "nope-output");
	run_msg("state", "{oops", "bad-params");
	run_command(std::format("\"{}\" msg", std::getenv("WAYLAND_CANVAS_BIN")), "no-method");
	return after(400);
}

Result client_errors_answered() {
	if (auto wait = wait_for(command_done("theme-blue") && command_done("nope-output")
			&& command_done("bad-params") && command_done("no-method"))) {
		return *wait;
	}
	check(command_code("theme-blue") == 1 && command_out("theme-blue").empty()
			&& command_err("theme-blue").find("mode") != std::string::npos,
		std::format("`msg theme.set` with a bad mode: exit 1, the error on stderr ({})",
			command_err("theme-blue")));
	check(command_code("nope-output") == 1
			&& command_err("nope-output").find("NOPE") != std::string::npos,
		std::format("`msg launcher.toggle` naming an unknown output: exit 1 ({})",
			command_err("nope-output")));
	check(command_code("bad-params") == 1
			&& command_err("bad-params").find("params") != std::string::npos,
		std::format("`msg` with params that are not JSON: exit 1 ({})",
			command_err("bad-params")));
	check(command_code("no-method") == 2, "`msg` without a method: usage, exit 2");
	return after(200);
}

// launcher.toggle, default output.
Result launcher_toggled() {
	run_msg("launcher.toggle", "", "launcher-open");
	return after(300);
}

Result launcher_opened() {
	if (auto wait = wait_for(command_done("launcher-open"))) {
		return *wait;
	}
	if (auto wait = wait_for(server->launcher.is_open())) {
		return *wait;
	}
	check(server->launcher.is_open() && command_code("launcher-open") == 0,
		"`msg launcher.toggle` opened the launcher");
	shot("launcher-open");
	return after(500);
}

// launcher.toggle with a named output closes it again.
Result launcher_toggled_by_output() {
	run_msg("launcher.toggle", std::format(R"({{"output":"{}"}})", output_name),
		"launcher-close");
	return after(300);
}

Result launcher_closed() {
	if (auto wait = wait_for(command_done("launcher-close"))) {
		return *wait;
	}
	if (auto wait = wait_for(!server->launcher.is_open())) {
		return *wait;
	}
	check(!server->launcher.is_open(), "`msg launcher.toggle` closed the open launcher");
	return after(200);
}

// overview.toggle, default output.
Result overview_toggled() {
	run_msg("overview.toggle", "", "overview-enter");
	return after(300);
}

Result overview_entered() {
	Output *output = server->output_under_cursor();
	if (auto wait = wait_for(command_done("overview-enter")
			&& output != nullptr && output->overview.active())) {
		return *wait;
	}
	check(output != nullptr && output->overview.active(),
		"`msg overview.toggle` entered the overview");
	shot("overview-dark");
	return after(500);
}

// overview.toggle with a named output leaves it.
Result overview_toggled_by_output() {
	run_msg("overview.toggle", std::format(R"({{"output":"{}"}})", output_name),
		"overview-exit");
	return after(300);
}

Result overview_left() {
	Output *output = server->output_under_cursor();
	if (auto wait = wait_for(command_done("overview-exit")
			&& output != nullptr && !output->overview.active())) {
		return *wait;
	}
	check(output != nullptr && !output->overview.active(),
		"`msg overview.toggle` left the overview");
	return after(200);
}

// theme.set light: the palette switch every output repaints in.
Result theme_light_set() {
	run_msg("theme.set", R"({"mode":"light"})", "theme-light");
	return after(300);
}

Result theme_light_answered() {
	if (auto wait = wait_for(command_done("theme-light"))) {
		return *wait;
	}
	if (auto wait = wait_for(server->theme.mode() == ThemeMode::light)) {
		return *wait;
	}
	check(server->theme.mode() == ThemeMode::light,
		"`msg theme.set {\"mode\":\"light\"}` switched the palette");
	check(command_code("theme-light") == 0 && command_out("theme-light") == "true",
		"`msg theme.set` prints true and exits 0");
	shot("titlebar-light");
	return after(500);
}

// The overview in light mode, then its state while inside it.
Result light_overview_toggled() {
	run_msg("overview.toggle", "", "overview-light-enter");
	return after(300);
}

Result light_overview_entered() {
	Output *output = server->output_under_cursor();
	if (auto wait = wait_for(command_done("overview-light-enter")
			&& output != nullptr && output->overview.active())) {
		return *wait;
	}
	check(output != nullptr && output->overview.active(),
		"the overview opened again, in light mode");
	shot("overview-light");
	run_msg("state", "", "state-overview");
	return after(400);
}

Result light_overview_state() {
	if (auto wait = wait_for(command_done("state-overview"))) {
		return *wait;
	}
	std::string problem = state_mismatch(command_out("state-overview"), "light", 1, true, 1);
	check(problem.empty(),
		std::format("`msg state` reports the output in the overview ({})",
			problem.empty() ? "as it should be" : problem));
	run_msg("overview.toggle", "", "overview-light-exit");
	return after(300);
}

Result light_overview_left() {
	Output *output = server->output_under_cursor();
	if (auto wait = wait_for(command_done("overview-light-exit")
			&& output != nullptr && !output->overview.active())) {
		return *wait;
	}
	check(output != nullptr && !output->overview.active(), "the overview closed");
	return after(200);
}

// `msg subscribe`: its stdout is what the steps below read as events.
Result subscribed() {
	check(subscriber.start({std::getenv("WAYLAND_CANVAS_BIN"), "msg", "subscribe"}),
		"the events subscriber started");
	return after(300);
}

// The subscription goes live: desktop creations are events. Each poll makes
// a desktop until one is heard, in case the subscriber was not connected for
// the first.
Result subscription_live() {
	bool live = heard("\"event\":\"desktops\"");
	if (!live) {
		server->canvas.desktops.create(wlr_box{2560, 0, 1280, 720});
	}
	if (auto wait = wait_for(live, {.poll_ms = 200, .budget_ms = 12'000})) {
		return *wait;
	}
	check(live, "`msg subscribe` receives desktops events");
	return after(200);
}

// The rest of the desktops lifecycle: create, move, combine, abandon. The
// subscribers hear a change once per frame: each change is a step of its
// own, so each gets a frame and each is told.
Result desktop_a_created() {
	lifecycle_heard = subscriber.count("\"event\":\"desktops\"");
	life_a = server->canvas.desktops.create(wlr_box{3840, 0, 1280, 720}).id();
	return after(100);
}

Result desktop_b_created() {
	life_b = server->canvas.desktops.create(wlr_box{5120, 0, 1280, 720}).id();
	return after(100);
}

Result desktop_b_moved() {
	server->canvas.move_desktop(*life_b, 5120, 720);
	return after(100);
}

Result desktops_combined() {
	// combine() removes `a`; nothing may touch it afterwards.
	wlr_box from = server->canvas.desktops.find(*life_a)->frame();
	server->canvas.combine(*life_b, *life_a, from);
	return after(100);
}

Result desktop_b_abandoned() {
	server->canvas.remove_desktop(*life_b);
	return after(300);
}

Result lifecycle_told() {
	int told = subscriber.count("\"event\":\"desktops\"") - lifecycle_heard;
	if (auto wait = wait_for(told >= 4)) {
		return *wait;
	}
	check(told >= 4,
		std::format("create, move, combine and abandon each fired a desktops event ({})",
			told));
	return after(200);
}

// Overview events reach the subscriber.
Result overview_event_asked() {
	run_msg("overview.toggle", "", "sub-overview");
	return after(300);
}

Result overview_event_heard() {
	if (auto wait = wait_for(heard("\"event\":\"overview\""))) {
		return *wait;
	}
	check(heard("\"event\":\"overview\""), "`msg subscribe` receives overview events");
	run_msg("overview.toggle", "", "sub-overview-exit");
	return after(300);
}

// Outputs events: a scale change and a second output.
Result scale_committed() {
	if (auto wait = wait_for(command_done("sub-overview-exit"))) {
		return *wait;
	}
	commit_scale(2);
	return after(300);
}

Result scale_heard() {
	if (auto wait = wait_for(heard("\"event\":\"outputs\""))) {
		return *wait;
	}
	check(heard("\"event\":\"outputs\"") && heard("\"scale\":2"),
		"a committed scale change fired an outputs event naming the scale");
	commit_scale(1);
	return after(300);
}

// A second headless output appears, then goes.
Result second_output_added() {
	check(add_output(640, 480) != nullptr, "a second headless output was added");
	return after(400);
}

Result second_output_heard() {
	if (auto wait = wait_for(server->outputs.size() >= 2)) {
		return *wait;
	}
	check(heard("\"event\":\"outputs\"") && server->outputs.size() == 2,
		"an appearing output fired an outputs event");
	wlr_output_destroy(server->outputs.back()->wlr);
	return after(400);
}

// The theme event: back to dark through the client.
Result second_output_gone() {
	if (auto wait = wait_for(server->outputs.size() == 1)) {
		return *wait;
	}
	check(server->outputs.size() == 1,
		"a going output fired an outputs event and left one output");
	run_msg("theme.set", R"({"mode":"dark"})", "sub-theme");
	return after(300);
}

Result theme_event_heard() {
	if (auto wait = wait_for(command_done("sub-theme"))) {
		return *wait;
	}
	if (auto wait = wait_for(heard("\"event\":\"theme\""))) {
		return *wait;
	}
	check(heard("\"event\":\"theme\"") && heard("\"mode\":\"dark\""),
		"a theme change fired a theme event naming the mode");
	subscriber.signal(SIGINT);
	return after(300);
}

// The subscriber is interrupted, as the intent says.
Result subscriber_interrupted() {
	if (auto wait = wait_for(!subscriber.running())) {
		return *wait;
	}
	check(!subscriber.running(), "`msg subscribe` runs until interrupted");
	return after(200);
}

// A subscriber that stops reading is dropped, not waited for.
Result slow_subscriber_started() {
	raw_pid = fork_slow_subscriber(scratch() + "/slow.ready", scratch() + "/slow.result");
	return after(300);
}

// A flood of desktops events: a long list first, then one change a frame,
// each event carrying the whole list. Past 1 MiB of backlog the drop is
// immediate, while the child sleeps without reading.
Result events_flooded() {
	if (auto wait = wait_for(file_exists(scratch() + "/slow.ready"))) {
		return *wait;
	}
	int batch = flooded == 0 ? 300 : 1;
	for (int i = 0; i < batch; i++, flooded++) {
		server->canvas.desktops.create(wlr_box{5120 + (flooded % 8) * 1280,
			720 + (flooded / 8) * 720, 1280, 720});
	}
	if (flooded < 420) {
		return again(20); // another desktop a frame, so each change is an event
	}
	return after(300);
}

Result slow_subscriber_dropped() {
	std::string result = read_text(scratch() + "/slow.result");
	if (auto wait = wait_for(!result.empty(), {.poll_ms = 200, .budget_ms = 20'000})) {
		return *wait;
	}
	check(result.rfind("dropped", 0) == 0,
		std::format("a subscriber with over 1 MiB unread events was dropped ({})", result));
	// The compositor stayed responsive through the flood and the drop.
	run_msg("state", "", "state-after-drop");
	return after(300);
}

Result still_responsive() {
	if (auto wait = wait_for(command_done("state-after-drop") && control_client_done(raw_pid))) {
		return *wait;
	}
	check(command_code("state-after-drop") == 0,
		"the compositor still answered after dropping the slow subscriber");
	return after(200);
}

// A line over 64 KiB: answered, then the connection is dropped.
Result oversized_asked() {
	std::string big(70 * 1024, 'a');
	big += '\n';
	raw_pid = fork_control_client("{\"id\":2,\"method\":\"state\"}\n" + big, "raw-oversized.out");
	return after(700);
}

Result oversized_answered() {
	if (auto wait = wait_for(control_client_done(raw_pid))) {
		return *wait;
	}
	std::string out = control_answer("raw-oversized.out");
	check(out.find("\"id\":2") != std::string::npos
			&& out.find("\"result\"") != std::string::npos,
		"the request before the oversized line was answered");
	check(out.find("64 KiB") != std::string::npos && out.find("EOF") != std::string::npos,
		std::format("the oversized line was answered with an error, then dropped ({})", out));
	return after(200);
}

// A connection that ends mid-line is dropped with its partial line.
Result midline_asked() {
	raw_pid = fork_control_client("{\"id\":3,\"method\":\"state\"", "raw-midline.out",
		{.half_close = true});
	return after(700);
}

Result midline_answered() {
	if (auto wait = wait_for(control_client_done(raw_pid))) {
		return *wait;
	}
	std::string out = control_answer("raw-midline.out");
	check(out.find("\"result\"") == std::string::npos && out.find("error") == std::string::npos
			&& out.find("EOF") != std::string::npos,
		std::format("a connection ending mid-line got no answer and was dropped ({})", out));
	return after(200);
}

// The surface role's own answers, for a name no client draws.
Result surface_role_asked() {
	attach_pointer();
	attach_keyboard();
	run_msg("surface.place", std::format(
		R"({{"surface":"{}","x":10,"y":20,"width":0,"height":5}})", surface_name),
		"place-flat");
	run_msg("surface.place", std::format(
		R"({{"surface":"{}","x":10,"y":20,"width":40,"height":30}})", surface_name),
		"place-ahead");
	run_msg("surface.move", std::format(R"({{"surface":"{}"}})", surface_name), "move-unheld");
	run_msg("surface.resize", std::format(
		R"({{"surface":"{}","edges":["sideways"]}})", surface_name), "resize-edges");
	return after(400);
}

Result surface_role_answered() {
	if (auto wait = wait_for(command_done("place-flat") && command_done("place-ahead")
			&& command_done("move-unheld") && command_done("resize-edges"))) {
		return *wait;
	}
	check(command_code("place-flat") == 1
			&& command_err("place-flat").find("area") != std::string::npos,
		std::format("`surface.place` with no area: an error naming it ({})",
			command_err("place-flat")));
	auto remembered = server->canvas_places.find(surface_name);
	check(command_code("place-ahead") == 0 && command_out("place-ahead") == "true"
			&& remembered != server->canvas_places.end()
			&& remembered->second.x == 10 && remembered->second.width == 40,
		"`surface.place` for a name no surface carries yet is remembered for it");
	check(command_code("move-unheld") == 1
			&& command_err("move-unheld").find("no button") != std::string::npos,
		std::format("`surface.move` with no button held on the surface: an error ({})",
			command_err("move-unheld")));
	check(command_code("resize-edges") == 1
			&& command_err("resize-edges").find("edges") != std::string::npos,
		std::format("`surface.resize` naming an edge there is not: the usage ({})",
			command_err("resize-edges")));
	check(!server->input.grab.active(), "no drag began from any of them");
	run_msg("surface.unplace", std::format(R"({{"surface":"{}"}})", surface_name), "unplace");
	return after(300);
}

Result unplaced() {
	if (auto wait = wait_for(command_done("unplace"))) {
		return *wait;
	}
	check(command_code("unplace") == 0 && !server->canvas_places.contains(surface_name),
		"`surface.unplace` forgets the remembered place");
	run_msg("state", "", "state-windows"); // read by the window list's check
	return after(300);
}

// A plugin's surface on the plane, drawn by a real layer-shell client.
Result layer_client_started() {
	check(layer_client.start_client("CANVAS_LAYER_CLIENT", {surface_name}),
		"the layer client started");
	return after(500);
}

// It maps, and `surface.place` puts it where the pointer reaches it.
Result layer_placed() {
	Output *output = server->output_under_cursor();
	LayerSurface *layer = plugin_surface();
	if (auto wait = wait_for(layer != nullptr && output != nullptr)) {
		return *wait;
	}
	if (layer == nullptr || output == nullptr) {
		check(false, "the layer client's surface mapped");
		return go_to("pick whose client leaves", 1); // nothing to press: on to the picks
	}
	Rect area = output->viewport();
	run_msg("surface.place", std::format(
		R"({{"surface":"{}","x":{},"y":{},"width":200,"height":100}})", surface_name,
		static_cast<int>(area.x) + 40, static_cast<int>(area.y) + 400), "layer-place");
	return after(400);
}

// The press the plugin answers, then a `surface.move` whose client leaves.
Result surface_pressed() {
	Output *output = server->output_under_cursor();
	LayerSurface *layer = plugin_surface();
	if (auto wait = wait_for(command_done("layer-place") && layer != nullptr
			&& layer->canvas_box.has_value())) {
		return *wait;
	}
	check(command_code("layer-place") == 0 && layer != nullptr && layer->canvas_box.has_value(),
		"the layer client's surface is on the plane at its place");
	if (layer == nullptr || output == nullptr || !layer->canvas_box.has_value()) {
		return go_to("pick whose client leaves", 1);
	}
	Point at = output->canvas_to_local(layer->canvas_box->x + 100, layer->canvas_box->y + 50);
	move_to(at.x, at.y);
	button(BTN_LEFT, true);
	check(server->input.router.button_held_on(layer->wlr->surface),
		"the pointer's press is held on the plugin's surface");
	pending_pid = fork_hangup(std::format(
		R"({{"id":1,"method":"surface.move","params":{{"surface":"{}"}}}})", surface_name),
		1000);
	return after(500);
}

// The drag runs while its client lingers, and goes on after it hangs up. The
// step comes round for each poll of the wait, so its check goes through
// act_when: made once, not once a poll.
Result drag_outlives_client() {
	act_when(true, [] {
		check(surface_drag_running(), "the compositor began the drag the client asked for");
	});
	if (auto wait = wait_for(control_client_done(pending_pid))) {
		return *wait;
	}
	return after(400); // the compositor reads the hang-up and drops the client
}

// The drag ends at the release with nobody left to answer.
Result drag_released() {
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
	return after(300);
}

// A `capture.pick` whose client leaves while the picker is up.
Result pick_whose_client_leaves() {
	pending_pid = fork_hangup(R"({"id":1,"method":"capture.pick","params":{"what":"window"}})",
		1000);
	return after(500);
}

Result picker_outlives_client() {
	act_when(true, [] {
		check(server->capture_ui.picking(), "the picker opened for the client's pick");
	});
	if (auto wait = wait_for(control_client_done(pending_pid))) {
		return *wait;
	}
	return after(400);
}

// The picker stays up for the user; cancelling it answers nobody.
Result picker_cancelled() {
	check(server->capture_ui.picking(), "the picker outlived the client that asked for it");
	tap(KEY_ESC);
	check(!server->capture_ui.picking(), "Escape closed the picker with its client gone");
	return after(300);
}

// A `shortcut.capture` whose client leaves while the capture waits.
Result shortcut_capture_left() {
	pending_pid = fork_hangup(R"({"id":1,"method":"shortcut.capture","params":{}})", 1000);
	return after(500);
}

Result shortcut_capture_dropped() {
	if (auto wait = wait_for(control_client_done(pending_pid))) {
		return *wait;
	}
	return after(400);
}

// The capture ended with its client: keys act again.
Result keys_act_again() {
	chord(KEY_SPACE, mods::super);
	check(server->launcher.is_open(),
		"Super+Space opened the launcher: no capture swallowed it for a dead client");
	return after(300);
}

Result launcher_closed_again() {
	tap(KEY_ESC);
	check(!server->launcher.is_open(), "Escape closed the launcher again");
	run_msg("state", "", "pending-alive");
	return after(400);
}

// The socket still answers; the plugin's surface goes with its client.
Result socket_still_answers() {
	if (auto wait = wait_for(command_done("pending-alive"))) {
		return *wait;
	}
	check(command_code("pending-alive") == 0 && !command_out("pending-alive").empty(),
		"the control socket still answers after three clients left calls pending");
	layer_client.signal(SIGTERM);
	return after(300);
}

// The state lists the open window, and a jump goes to it.
Result window_listed() {
	if (auto wait = wait_for(command_done("state-windows"))) {
		return *wait;
	}
	View *view = first_mapped_view();
	auto id = static_cast<long long>(view != nullptr ? view->session_id : 0);
	std::optional<WindowEntry> entry = parse_state_window(command_out("state-windows"), id);
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
	run_msg("window.focus", std::format(R"({{"window":{}}})", id), "window-focus");
	run_msg("window.focus", R"({"window":99999})", "window-focus-bad");
	return after(400);
}

// The camera came back to the window; the unknown id errored.
Result window_focused() {
	Output *output = server->output_under_cursor();
	View *view = first_mapped_view();
	if (auto wait = wait_for(command_done("window-focus") && command_done("window-focus-bad")
			&& output != nullptr && !output->camera.panning() && !output->camera.zooming())) {
		return *wait;
	}
	check(command_code("window-focus") == 0 && view != nullptr && output != nullptr
			&& view->frame_rect().intersects(output->viewport())
			&& server->focused_view == view,
		"window.focus brought the view back to the window and focused it");
	check(command_code("window-focus-bad") != 0,
		"window.focus with an unknown id answers an error");
	// A window on no desktop: moved far out onto free canvas, with a second
	// desktop beside the first so the jump has a choice to make
	// (docs/intents/shell/app-bar.md).
	long long free_window_id = 0;
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
	run_msg("window.focus", std::format(R"({{"window":{}}})", free_window_id), "window-free");
	return after(500);
}

// The view centred on the window, no desktop involved.
Result free_window_focused() {
	Output *output = server->output_under_cursor();
	View *view = first_mapped_view();
	if (auto wait = wait_for(command_done("window-free") && output != nullptr
			&& !output->camera.panning() && !output->camera.zooming())) {
		return *wait;
	}
	wlr_box frame = view != nullptr ? view->frame_box() : wlr_box{};
	Camera cam = output != nullptr ? output->camera.current() : Camera{};
	double off_x = cam.cx - (frame.x + frame.width / 2.0);
	double off_y = cam.cy - (frame.y + frame.height / 2.0);
	check(view != nullptr && server->canvas.desktop_of(*view) == nullptr,
		"the window sits on no desktop");
	check(std::fabs(off_x) <= 1 && std::fabs(off_y) <= 1 && std::fabs(cam.zoom - 1) < 1e-9,
		std::format("window.focus centred the view on a window with no"
			" desktop (off by {:.0f},{:.0f} at zoom {:.2f})", off_x, off_y, cam.zoom));
	return after(300);
}

// settings.set compares the number with its range before narrowing it.
Result settings_wrap_asked() {
	conf_before = server->conf_text;
	// 4294967301 is 2^32 + 5 and 4294971296 is 2^32 + 4000: as an int, the
	// first is a valid idle time and the second a valid temperature.
	run_msg("settings.set", R"({"lockIdleMinutes":4294967301})", "idle-wraps");
	run_msg("settings.set", R"({"nightLightTemperature":4294971296})", "temperature-wraps");
	return after(400);
}

Result settings_wrap_refused() {
	if (auto wait = wait_for(command_done("idle-wraps") && command_done("temperature-wraps"))) {
		return *wait;
	}
	check(command_code("idle-wraps") == 1
			&& command_err("idle-wraps").find("lockIdleMinutes") != std::string::npos,
		std::format("`settings.set` of a lockIdleMinutes past an int is an error ({})",
			command_err("idle-wraps")));
	check(command_code("temperature-wraps") == 1
			&& command_err("temperature-wraps").find("nightLightTemperature")
				!= std::string::npos,
		std::format("`settings.set` of a temperature past an int is an error ({})",
			command_err("temperature-wraps")));
	check(server->conf_text == conf_before
			&& server->conf.lock_idle_minutes == settings::lock_idle.fallback
			&& server->conf.night_light_k == settings::night_light_temperature.fallback,
		"canvas.conf and the settings in force are unchanged");
	run_msg("settings.set", R"({"lockIdleMinutes":7})", "idle-seven");
	return after(400);
}

// The settings table, end to end: what settings.get reports of every row,
// what settings.set refuses of each, and the usage naming all.
Result settings_table_asked() {
	if (auto wait = wait_for(command_done("idle-seven") && server->conf.lock_idle_minutes == 7)) {
		return *wait;
	}
	check(command_code("idle-seven") == 0 && server->conf.lock_idle_minutes == 7,
		"a lockIdleMinutes in range is still written");
	conf_before = server->conf_text;
	asked = refusals();
	run_msg("settings.get", "", "settings-get");
	run_msg("settings.set", "", "settings-usage");
	for (const Refusal &refusal : asked) {
		run_msg("settings.set", refusal.params, refusal.name);
	}
	return after(600);
}

Result settings_table_answered() {
	bool done = command_done("settings-get") && command_done("settings-usage");
	for (const Refusal &refusal : asked) {
		done = done && command_done(refusal.name);
	}
	if (auto wait = wait_for(done)) {
		return *wait;
	}
	std::string problem = settings_get_problem(command_out("settings-get"));
	check(command_code("settings-get") == 0 && problem.empty(),
		std::format("`settings.get` describes every row of the settings table ({})", problem));
	bool names_all = command_code("settings-usage") == 1;
	for (const settings::Field &field : settings::fields) {
		names_all = names_all
			&& command_err("settings-usage").find(field.param) != std::string::npos;
	}
	check(names_all, std::format("`settings.set` with no params names every setting "
		"in its usage ({})", command_err("settings-usage")));
	for (const Refusal &refusal : asked) {
		check(command_code(refusal.name) == 1
				&& command_err(refusal.name).find(refusal.field->param) != std::string::npos,
			std::format("`settings.set` {} is refused naming the setting ({})",
				refusal.params, command_err(refusal.name)));
	}
	check(server->conf_text == conf_before, "no refused settings.set changed canvas.conf");
	return after(300);
}

Result quit() {
	return quit_by_msg();
}

} // namespace

int main() {
	if (!begin("ipc", {"WAYLAND_CANVAS_BIN", "CANVAS_LAYER_CLIENT"})) {
		return 2;
	}
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	int status = run(options, {
		{"socket exported", socket_exported},
		{"window spawned", window_spawned},
		{"window mapped", window_mapped},
		{"state asked", state_asked},
		{"state answered", state_answered},
		{"order asked", order_asked},
		{"order answered", order_answered},
		{"malformed asked", malformed_asked},
		{"malformed answered", malformed_answered},
		{"unknown asked", unknown_asked},
		{"unknown answered", unknown_answered},
		{"client errors asked", client_errors_asked},
		{"client errors answered", client_errors_answered},
		{"launcher toggled", launcher_toggled},
		{"launcher opened", launcher_opened},
		{"launcher toggled by output", launcher_toggled_by_output},
		{"launcher closed", launcher_closed},
		{"overview toggled", overview_toggled},
		{"overview entered", overview_entered},
		{"overview toggled by output", overview_toggled_by_output},
		{"overview left", overview_left},
		{"theme light set", theme_light_set},
		{"theme light answered", theme_light_answered},
		{"light overview toggled", light_overview_toggled},
		{"light overview entered", light_overview_entered},
		{"light overview state", light_overview_state},
		{"light overview left", light_overview_left},
		{"subscribed", subscribed},
		{"subscription live", subscription_live},
		{"desktop a created", desktop_a_created},
		{"desktop b created", desktop_b_created},
		{"desktop b moved", desktop_b_moved},
		{"desktops combined", desktops_combined},
		{"desktop b abandoned", desktop_b_abandoned},
		{"lifecycle told", lifecycle_told},
		{"overview event asked", overview_event_asked},
		{"overview event heard", overview_event_heard},
		{"scale committed", scale_committed},
		{"scale heard", scale_heard},
		{"second output added", second_output_added},
		{"second output heard", second_output_heard},
		{"second output gone", second_output_gone},
		{"theme event heard", theme_event_heard},
		{"subscriber interrupted", subscriber_interrupted},
		{"slow subscriber started", slow_subscriber_started},
		{"events flooded", events_flooded},
		{"slow subscriber dropped", slow_subscriber_dropped},
		{"still responsive", still_responsive},
		{"oversized asked", oversized_asked},
		{"oversized answered", oversized_answered},
		{"midline asked", midline_asked},
		{"midline answered", midline_answered},
		{"surface role asked", surface_role_asked},
		{"surface role answered", surface_role_answered},
		{"unplaced", unplaced},
		{"layer client started", layer_client_started},
		{"layer placed", layer_placed},
		{"surface pressed", surface_pressed},
		{"drag outlives client", drag_outlives_client},
		{"drag released", drag_released},
		{"pick whose client leaves", pick_whose_client_leaves},
		{"picker outlives client", picker_outlives_client},
		{"picker cancelled", picker_cancelled},
		{"shortcut capture left", shortcut_capture_left},
		{"shortcut capture dropped", shortcut_capture_dropped},
		{"keys act again", keys_act_again},
		{"launcher closed again", launcher_closed_again},
		{"socket still answers", socket_still_answers},
		{"window listed", window_listed},
		{"window focused", window_focused},
		{"free window focused", free_window_focused},
		{"settings wrap asked", settings_wrap_asked},
		{"settings wrap refused", settings_wrap_refused},
		{"settings table asked", settings_table_asked},
		{"settings table answered", settings_table_answered},
		{"quit", quit},
	});
	if (status == 2) {
		return 2;
	}
	check(access(socket_path.c_str(), F_OK) != 0,
		"the control socket was removed when the compositor exited");
	return failures() != 0;
}
