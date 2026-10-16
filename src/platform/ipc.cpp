#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <json-glib/json-glib.h>

#include "canvas/desktop.h"
#include "canvas/items.h"
#include "core/config.h"
#include "core/theme.h"
#include "core/theme_prefs.h"
#include "core/wlr.h"
#include "platform/ipc.h"
#include "platform/output.h"
#include "core/server.h"

namespace {

constexpr int listen_backlog = 4;
constexpr std::size_t line_chunk = 512;

std::string_view trim(std::string_view line) {
	while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
		line.remove_prefix(1);
	}
	while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
		line.remove_suffix(1);
	}
	return line;
}

// Fills `addr` with `path`. False when the path does not fit.
bool fill_address(sockaddr_un &addr, const std::string &path) {
	addr.sun_family = AF_UNIX;
	if (path.size() >= sizeof(addr.sun_path)) {
		return false;
	}
	std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	return true;
}

// The control socket of the compositor `display` names — a socket name
// under `runtime_dir`, or an absolute path as $WAYLAND_DISPLAY allows:
// <dir>/wayland-canvas-<name>.sock. Empty when either part is missing.
std::string socket_path_for(const char *runtime_dir, const char *display) {
	if (display == nullptr || *display == '\0') {
		return {};
	}
	std::string_view name = display;
	std::string dir = runtime_dir != nullptr ? runtime_dir : "";
	if (std::size_t slash = name.rfind('/'); slash != std::string_view::npos) {
		dir = std::string(name.substr(0, slash));
		name = name.substr(slash + 1);
	}
	if (dir.empty() || name.empty()) {
		return {};
	}
	return std::format("{}/{}-{}.sock", dir, config::ipc_socket_prefix, name);
}

// ------------------------------------------------------ json-glib wrappers

// json-glib is C, so it is safe under -fno-exceptions; the RAII handles follow
// the util.h pattern. g_error_free is not null-safe, so GError gets its own.
using JsonParserPtr = Handle<JsonParser, g_object_unref>;
using JsonBuilderPtr = Handle<JsonBuilder, g_object_unref>;
using JsonGeneratorPtr = Handle<JsonGenerator, g_object_unref>;
using GCharPtr = Handle<char, g_free>;

struct GErrorHolder {
	GError *error = nullptr;
	~GErrorHolder() {
		if (error != nullptr) {
			g_error_free(error);
		}
	}
};

// Compact JSON text for a tree the caller still owns.
std::string node_text(JsonNode *node) {
	JsonGeneratorPtr generator(json_generator_new());
	json_generator_set_root(generator.get(), node); // copies the node
	gsize len = 0;
	GCharPtr data(json_generator_to_data(generator.get(), &len));
	return data != nullptr ? std::string(data.get(), len) : std::string{};
}

// Builds one JSON object as compact text; `build` fills it between the braces.
template <typename Build>
std::string object_text(Build &&build) {
	JsonBuilderPtr builder(json_builder_new());
	json_builder_begin_object(builder.get());
	build(builder.get());
	json_builder_end_object(builder.get());
	JsonNode *root = json_builder_get_root(builder.get()); // transfer full
	std::string text = root != nullptr ? node_text(root) : std::string{"{}"};
	json_node_unref(root);
	return text;
}

// A JSON string literal, for splicing text into a response or request.
// Hand-rolled: json-glib builders cannot hold a bare string as the root.
std::string quote(std::string_view text) {
	std::string out;
	out.reserve(text.size() + 2);
	out += '"';
	for (const char ch : text) {
		switch (ch) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(ch) < 0x20) {
				std::format_to(std::back_inserter(out), "\\u{:04x}",
					static_cast<unsigned char>(ch));
			} else {
				out += ch;
			}
		}
	}
	out += '"';
	return out;
}

// `{"id": <id>, "result"|"error": <payload>}` as one line. `payload` is
// already JSON: a result's text, or a quoted error string.
std::string response(std::string_view id_json, std::string_view payload, bool error) {
	std::string out = "{\"id\":";
	out += id_json;
	out += error ? ",\"error\":" : ",\"result\":";
	out += payload;
	out += "}\n";
	return out;
}

// `{"event": "<name>", "params": <params>}` as one line; every name is ours,
// so it needs no escaping.
std::string event_line(std::string_view name, std::string_view params) {
	std::string out = "{\"event\":\"";
	out += name;
	out += "\",\"params\":";
	out += params;
	out += "}\n";
	return out;
}

// ------------------------------------------------------------- params

// A string param: nullopt when absent; an error naming it when not one.
std::expected<std::optional<std::string>, std::string> opt_string(
		JsonObject *params, const char *name) {
	if (params == nullptr || !json_object_has_member(params, name)) {
		return std::optional<std::string>{};
	}
	JsonNode *node = json_object_get_member(params, name);
	if (json_node_get_node_type(node) != JSON_NODE_VALUE
			|| json_node_get_value_type(node) != G_TYPE_STRING) {
		return std::unexpected(std::format("params: {} must be a string", name));
	}
	return std::optional{std::string{json_node_get_string(node)}};
}

// A whole-number param: nullopt when absent; an error naming it when not one.
std::expected<std::optional<long long>, std::string> opt_int(
		JsonObject *params, const char *name) {
	if (params == nullptr || !json_object_has_member(params, name)) {
		return std::optional<long long>{};
	}
	JsonNode *node = json_object_get_member(params, name);
	if (json_node_get_node_type(node) != JSON_NODE_VALUE
			|| json_node_get_value_type(node) != G_TYPE_INT64) {
		return std::unexpected(std::format("params: {} must be a whole number", name));
	}
	return std::optional{json_node_get_int(node)};
}

// "#rrggbb" as 0xRRGGBBAA; nullopt when it is not that shape.
std::optional<uint32_t> parse_color(std::string_view text) {
	auto hex = [](char ch) -> std::optional<uint32_t> {
		if (ch >= '0' && ch <= '9') {
			return static_cast<uint32_t>(ch - '0');
		}
		if (ch >= 'a' && ch <= 'f') {
			return static_cast<uint32_t>(ch - 'a' + 10);
		}
		if (ch >= 'A' && ch <= 'F') {
			return static_cast<uint32_t>(ch - 'A' + 10);
		}
		return std::nullopt;
	};
	if (text.size() != 7 || text.front() != '#') {
		return std::nullopt;
	}
	uint32_t rgb = 0;
	for (std::size_t i = 1; i < text.size(); i++) {
		std::optional<uint32_t> digit = hex(text[i]);
		if (!digit.has_value()) {
			return std::nullopt;
		}
		rgb = rgb << 4 | *digit;
	}
	return rgb << 8 | 0xffu;
}

// A colour as "#rrggbb": what item.set was given.
std::string color_text(uint32_t rgba) {
	return std::format("#{:02x}{:02x}{:02x}",
		(rgba >> 24) & 0xff, (rgba >> 16) & 0xff, (rgba >> 8) & 0xff);
}

// -------------------------------------------------------- state as JSON

std::string_view theme_name(ThemeMode mode) {
	return mode == ThemeMode::light ? "light" : "dark";
}

void add_output(JsonBuilder *builder, const Output &output) {
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, output.wlr->name);
	json_builder_set_member_name(builder, "x");
	json_builder_add_int_value(builder, output.lx);
	json_builder_set_member_name(builder, "y");
	json_builder_add_int_value(builder, output.ly);
	json_builder_set_member_name(builder, "width");
	json_builder_add_int_value(builder, output.width);
	json_builder_set_member_name(builder, "height");
	json_builder_add_int_value(builder, output.height);
	json_builder_set_member_name(builder, "scale");
	json_builder_add_double_value(builder, output.wlr->scale);
	json_builder_set_member_name(builder, "overview");
	json_builder_add_boolean_value(builder, output.overview.active());
	json_builder_end_object(builder);
}

void add_outputs_array(JsonBuilder *builder, Server &server) {
	json_builder_set_member_name(builder, "outputs");
	json_builder_begin_array(builder);
	for (const std::unique_ptr<Output> &output : server.outputs) {
		add_output(builder, *output);
	}
	json_builder_end_array(builder);
}

void add_desktops_array(JsonBuilder *builder, Server &server) {
	json_builder_set_member_name(builder, "desktops");
	json_builder_begin_array(builder);
	for (const Desktop &desktop : server.canvas.desktops.all()) {
		const wlr_box &frame = desktop.frame();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, static_cast<int>(desktop.id()));
		json_builder_set_member_name(builder, "number");
		json_builder_add_int_value(builder, desktop.number());
		json_builder_set_member_name(builder, "x");
		json_builder_add_int_value(builder, frame.x);
		json_builder_set_member_name(builder, "y");
		json_builder_add_int_value(builder, frame.y);
		json_builder_set_member_name(builder, "width");
		json_builder_add_int_value(builder, frame.width);
		json_builder_set_member_name(builder, "height");
		json_builder_add_int_value(builder, frame.height);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
}
void add_item(JsonBuilder *builder, const Item &item) {
	std::string id = item.id; // NUL-terminated for the C API
	std::string color = color_text(item.color);
	std::string text = item.text;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, id.c_str());
	json_builder_set_member_name(builder, "color");
	json_builder_add_string_value(builder, color.c_str());
	json_builder_set_member_name(builder, "text");
	json_builder_add_string_value(builder, text.c_str());
	json_builder_set_member_name(builder, "x");
	json_builder_add_int_value(builder, item.x);
	json_builder_set_member_name(builder, "y");
	json_builder_add_int_value(builder, item.y);
	json_builder_set_member_name(builder, "width");
	json_builder_add_int_value(builder, item.width);
	json_builder_set_member_name(builder, "height");
	json_builder_add_int_value(builder, item.height);
	json_builder_end_object(builder);
}

void add_items_array(JsonBuilder *builder, Server &server) {
	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);
	for (const Item &item : server.items.all()) {
		add_item(builder, item);
	}
	json_builder_end_array(builder);
}

// ------------------------------------------------------------ the client

// Connects to $WAYLAND_CANVAS_SOCKET, else to the socket of the compositor
// $WAYLAND_DISPLAY names: a service the session bus starts (the portal's
// chooser) knows only the display. -1 (explained on stderr) on failure.
int connect_to_socket() {
	const char *exported = std::getenv("WAYLAND_CANVAS_SOCKET");
	std::string path = exported != nullptr && *exported != '\0' ? std::string(exported)
		: socket_path_for(std::getenv("XDG_RUNTIME_DIR"), std::getenv("WAYLAND_DISPLAY"));
	if (path.empty()) {
		std::fputs("wayland-canvas: neither $WAYLAND_CANVAS_SOCKET nor $WAYLAND_DISPLAY"
			" names a compositor\n", stderr);
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		std::fprintf(stderr, "wayland-canvas: socket: %s\n", std::strerror(errno));
		return -1;
	}
	sockaddr_un addr{};
	if (!fill_address(addr, path) ||
			connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
		std::fprintf(stderr, "wayland-canvas: cannot reach the compositor at %s\n", path.c_str());
		close(fd);
		return -1;
	}
	return fd;
}

// Blocking write of the whole buffer; false on failure.
bool write_request(int fd, std::string_view bytes) {
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

// Blocking: one newline-terminated line, or nullopt on EOF or error.
std::optional<std::string> read_line(int fd) {
	std::string line;
	char ch;
	for (;;) {
		ssize_t n = read(fd, &ch, 1);
		if (n == 1) {
			if (ch == '\n') {
				return line;
			}
			line += ch;
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		return line.empty() ? std::optional<std::string>{} : std::optional{line};
	}
}

void msg_usage() {
	std::fputs("usage: wayland-canvas msg <method> [<params as JSON>]\n"
		"       wayland-canvas msg subscribe\n", stderr);
}

} // namespace

// ------------------------------------------------------------ connection

Ipc::Client::Client(Ipc &ipc, int fd) : ipc(ipc), fd(fd) {
}

Ipc::Client::~Client() {
	if (ipc.pick_client_ == this) {
		ipc.pick_client_ = nullptr; // its pick can never be answered now
		ipc.pick_id_.clear();
	}
	source.reset(); // off the loop before the fd it watched is closed
	if (fd >= 0) { // a dropped client's fd is already closed
		close(fd);
	}
}

Ipc::Ipc(Server &server) : server_(server) {
}

Ipc::~Ipc() {
	finish();
}

bool Ipc::init() {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	path_ = socket_path_for(runtime, display);
	if (runtime == nullptr || *runtime == '\0' || path_.empty()) {
		wlr_log(WLR_ERROR, "no $XDG_RUNTIME_DIR or $WAYLAND_DISPLAY: no control socket");
		path_.clear();
		return false;
	}
	// A socket left behind by a crashed run would block this one.
	unlink(path_.c_str());
	// Nonblocking, so accepting pending connections can never stall the loop.
	listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (listen_fd_ < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: socket() failed");
		path_.clear();
		return false;
	}
	sockaddr_un addr{};
	if (!fill_address(addr, path_)) {
		wlr_log(WLR_ERROR, "ipc: socket path %s is too long", path_.c_str());
		finish();
		return false;
	}
	if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: bind %s failed", path_.c_str());
		path_.clear(); // not ours: the bind never happened
		finish();
		return false;
	}
	bound_ = true;
	if (listen(listen_fd_, listen_backlog) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: listen failed");
		finish();
		return false;
	}
	listen_source_.reset(wl_event_loop_add_fd(server_.loop, listen_fd_,
		WL_EVENT_READABLE, handle_listen, this));
	if (!listen_source_) {
		wlr_log(WLR_ERROR, "ipc: could not watch the socket");
		finish();
		return false;
	}
	setenv("WAYLAND_CANVAS_SOCKET", path_.c_str(), true);
	wlr_log(WLR_INFO, "control socket at %s", path_.c_str());
	return true;
}

void Ipc::finish() {
	clients_.clear();
	listen_source_.reset();
	if (listen_fd_ >= 0) {
		close(listen_fd_);
		listen_fd_ = -1;
	}
	if (bound_) {
		unlink(path_.c_str());
		bound_ = false;
	}
	path_.clear();
}

int Ipc::handle_listen(int, uint32_t mask, void *data) {
	auto *ipc = static_cast<Ipc *>(data);
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	for (;;) {
		int conn = accept4(ipc->listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (conn < 0) {
			break; // EAGAIN: every pending connection is taken
		}
		auto client = std::make_unique<Client>(*ipc, conn);
		client->source.reset(wl_event_loop_add_fd(ipc->server_.loop, conn,
			WL_EVENT_READABLE, handle_client, client.get()));
		if (!client->source) {
			wlr_log(WLR_ERROR, "ipc: could not watch a client connection");
			continue; // the Client destructor closed the fd
		}
		ipc->clients_.push_back(std::move(client));
	}
	return 0;
}

// ------------------------------------------------------------- protocol

namespace {

// A parsed request line. Owns the json-glib tree its pointers point into.
struct Request {
	JsonParserPtr parser;
	bool ok = false; // an object with a string method and object (or no) params
	std::string id_json = "null"; // the id as JSON text, null when unreadable
	std::string method; // empty when absent
	JsonObject *params = nullptr; // null when absent
	std::string error; // why the line is not a request; empty when ok
};

// Parses one line. `error` carries the reason when it is not a request.
Request parse_line(std::string_view line) {
	Request out;
	out.parser.reset(json_parser_new());
	std::string buffer(line); // NUL-terminated copy for the C API
	GErrorHolder gerr;
	if (!json_parser_load_from_data(out.parser.get(), buffer.c_str(),
			static_cast<gssize>(buffer.size()), &gerr.error)) {
		out.error = gerr.error != nullptr && gerr.error->message != nullptr
			? std::format("not JSON: {}", gerr.error->message)
			: std::string{"not JSON"};
		return out;
	}
	JsonNode *root = json_parser_get_root(out.parser.get());
	if (root == nullptr || json_node_get_node_type(root) != JSON_NODE_OBJECT) {
		out.error = "not a JSON object";
		return out;
	}
	JsonObject *object = json_node_get_object(root);
	if (json_object_has_member(object, "id")) {
		JsonNode *id = json_object_get_member(object, "id");
		if (json_node_get_node_type(id) == JSON_NODE_VALUE) {
			GType type = json_node_get_value_type(id);
			if (type == G_TYPE_INT64 || type == G_TYPE_DOUBLE || type == G_TYPE_STRING) {
				out.id_json = node_text(id);
			}
		}
	}
	if (!json_object_has_member(object, "method")) {
		out.error = "no method";
		return out;
	}
	JsonNode *method = json_object_get_member(object, "method");
	if (json_node_get_node_type(method) != JSON_NODE_VALUE
			|| json_node_get_value_type(method) != G_TYPE_STRING) {
		out.error = "method is not a string";
		return out;
	}
	out.method = json_node_get_string(method);
	if (json_object_has_member(object, "params")) {
		JsonNode *params = json_object_get_member(object, "params");
		if (json_node_get_node_type(params) != JSON_NODE_OBJECT) {
			out.error = "params is not an object";
			return out;
		}
		out.params = json_node_get_object(params);
	}
	out.ok = true;
	return out;
}

} // namespace

void Ipc::handle_line(Client &client, std::string_view line) {
	Request request = parse_line(line);
	if (!request.error.empty()) {
		queue(client, response(request.id_json, quote(request.error), true));
		return;
	}
	if (request.method == "capture.pick") {
		capture_pick(client, request.id_json, request.params);
		return;
	}
	std::expected<std::string, std::string> result =
		dispatch_method(client, request.method, request.params);
	queue(client, response(request.id_json,
		result.has_value() ? *result : quote(result.error()), !result.has_value()));
}

void Ipc::capture_pick(Client &client, std::string_view id_json,
		JsonObject *params) {
	auto bail = [&](std::string_view error) {
		queue(client, response(id_json, quote(error), true));
	};
	constexpr std::string_view usage =
		R"(params: what must be "region", "window", "output" or "any")";
	if (params == nullptr || !json_object_has_member(params, "what")) {
		bail(usage);
		return;
	}
	JsonNode *node = json_object_get_member(params, "what");
	const char *what = json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? json_node_get_string(node) : nullptr;
	constexpr std::array kinds = {
		std::pair{std::string_view{"region"}, CaptureUi::PickWhat::region},
		std::pair{std::string_view{"window"}, CaptureUi::PickWhat::window},
		std::pair{std::string_view{"output"}, CaptureUi::PickWhat::output},
		std::pair{std::string_view{"any"}, CaptureUi::PickWhat::any},
	};
	CaptureUi::PickWhat kind = CaptureUi::PickWhat::any;
	bool known = false;
	for (const auto &[name, value] : kinds) {
		if (name == what) {
			kind = value;
			known = true;
			break;
		}
	}
	if (what == nullptr || !known) {
		bail(usage);
		return;
	}
	std::expected<Output *, std::string> output = resolve_output(params);
	if (!output.has_value()) {
		bail(output.error());
		return;
	}
	if (*output == nullptr) {
		bail("no output to pick on");
		return;
	}
	if (pick_client_ != nullptr
			|| !server_.capture_ui.start_pick(*output, kind,
				CaptureUi::PickUse::ipc, CaptureUi::Options{})) {
		bail("a capture pick is already running");
		return;
	}
	pick_client_ = &client;
	pick_id_ = id_json;
	// No response yet: answer_pick() sends it once the user has picked.
}

void Ipc::answer_pick(bool ok, std::string_view payload) {
	if (pick_client_ == nullptr) {
		return;
	}
	Client &client = *pick_client_;
	std::string id = pick_id_;
	pick_client_ = nullptr;
	pick_id_.clear();
	queue(client, response(id, ok ? payload : quote(payload), !ok));
}

std::expected<std::string, std::string> Ipc::dispatch_method(Client &client,
		std::string_view name, JsonObject *params) {
	// The protocol's method table.
	constexpr std::array table = {
		std::pair{std::string_view{"state"}, &Ipc::m_state},
		std::pair{std::string_view{"subscribe"}, &Ipc::m_subscribe},
		std::pair{std::string_view{"launcher.toggle"}, &Ipc::m_launcher_toggle},
		std::pair{std::string_view{"overview.toggle"}, &Ipc::m_overview_toggle},
		std::pair{std::string_view{"quit"}, &Ipc::m_quit},
		std::pair{std::string_view{"theme.set"}, &Ipc::m_theme_set},
		std::pair{std::string_view{"item.set"}, &Ipc::m_item_set},
		std::pair{std::string_view{"item.remove"}, &Ipc::m_item_remove},
		std::pair{std::string_view{"item.activate"}, &Ipc::m_item_activate},
		std::pair{std::string_view{"action.run"}, &Ipc::m_action_run},
		std::pair{std::string_view{"capture.stop"}, &Ipc::m_capture_stop},
	};
	for (const auto &[method, run] : table) {
		if (method == name) {
			return (this->*run)(client, params);
		}
	}
	return std::unexpected(std::format("unknown method '{}'", name));
}

std::expected<std::string, std::string> Ipc::m_state(Client &, JsonObject *) {
	return object_text([&](JsonBuilder *builder) {
		add_outputs_array(builder, server_);
		add_desktops_array(builder, server_);
		add_items_array(builder, server_);
		json_builder_set_member_name(builder, "theme");
		std::string mode = std::string{theme_name(server_.theme.mode())};
		json_builder_add_string_value(builder, mode.c_str());
		json_builder_set_member_name(builder, "recording");
		if (server_.capture_ui.recording()) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "path");
			std::string path = server_.capture_ui.recording_path();
			json_builder_add_string_value(builder, path.c_str());
			json_builder_end_object(builder);
		} else {
			json_builder_add_null_value(builder);
		}
	});
}

std::expected<std::string, std::string> Ipc::m_subscribe(Client &client, JsonObject *) {
	client.subscribed = true;
	return std::string{"true"};
}

std::expected<Output *, std::string> Ipc::resolve_output(JsonObject *params) const {
	if (params == nullptr || !json_object_has_member(params, "output")) {
		return server_.output_under_cursor();
	}
	JsonNode *node = json_object_get_member(params, "output");
	if (json_node_get_node_type(node) != JSON_NODE_VALUE
			|| json_node_get_value_type(node) != G_TYPE_STRING) {
		return std::unexpected(std::string{"params: output must be a string"});
	}
	const char *name = json_node_get_string(node);
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		if (strcmp(output->wlr->name, name) == 0) {
			return output.get();
		}
	}
	return std::unexpected(std::format("unknown output '{}'", name));
}

std::expected<std::string, std::string> Ipc::m_launcher_toggle(Client &,
		JsonObject *params) {
	std::expected<Output *, std::string> output = resolve_output(params);
	if (!output.has_value()) {
		return std::unexpected(output.error());
	}
	server_.launcher.toggle(*output); // nullptr: no output, nothing to toggle
	return std::string{"true"};
}

std::expected<std::string, std::string> Ipc::m_overview_toggle(Client &,
		JsonObject *params) {
	std::expected<Output *, std::string> output = resolve_output(params);
	if (!output.has_value()) {
		return std::unexpected(output.error());
	}
	if (*output == nullptr) {
		return std::unexpected(std::string{"no output under the cursor"});
	}
	Output &o = **output;
	if (o.overview.active()) {
		o.overview.exit();
	} else {
		o.overview.enter();
	}
	return std::string{"true"};
}

std::expected<std::string, std::string> Ipc::m_quit(Client &, JsonObject *) {
	quit_ = true; // handle_client ends the session once the response is out
	return std::string{"true"};
}

std::expected<std::string, std::string> Ipc::m_theme_set(Client &, JsonObject *params) {
	constexpr std::string_view usage =
		"params: mode must be \"light\" or \"dark\"";
	if (params == nullptr || !json_object_has_member(params, "mode")) {
		return std::unexpected(std::string{usage});
	}
	JsonNode *node = json_object_get_member(params, "mode");
	const char *mode = json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? json_node_get_string(node) : nullptr;
	if (mode == nullptr) {
		return std::unexpected(std::string{usage});
	}
	if (strcmp(mode, "light") != 0 && strcmp(mode, "dark") != 0) {
		return std::unexpected(std::format(
			"params: mode must be \"light\" or \"dark\", not '{}'", mode));
	}
	ThemeMode resolved = strcmp(mode, "light") == 0 ? ThemeMode::light : ThemeMode::dark;
	server_.set_theme(resolved);
	// Every theme.set — the shell's switch, the shell's start push, an
	// outside client's call — also sets the applications' preference, but
	// only when this compositor is the real session, which here means it
	// opened a seat session from a TTY (docs/intents/shell/theme.md):
	// nested and headless runs never get one.
	theme_prefs::apply(resolved, server_.session != nullptr);
	return std::string{"true"};
}

std::expected<std::string, std::string> Ipc::m_item_set(Client &, JsonObject *params) {
	std::expected<std::optional<std::string>, std::string> id = opt_string(params, "id");
	if (!id.has_value()) {
		return std::unexpected(id.error());
	}
	if (!id->has_value() || (*id)->empty()) {
		return std::unexpected(std::string{"params: id must be a non-empty string"});
	}

	std::expected<std::optional<std::string>, std::string> color =
		opt_string(params, "color");
	if (!color.has_value()) {
		return std::unexpected(color.error());
	}
	std::optional<uint32_t> color_value;
	if (color->has_value()) {
		color_value = parse_color(**color);
		if (!color_value.has_value()) {
			return std::unexpected(std::format(
				"params: color must be #rrggbb, not '{}'", **color));
		}
	}

	std::expected<std::optional<std::string>, std::string> text =
		opt_string(params, "text");
	if (!text.has_value()) {
		return std::unexpected(text.error());
	}

	// A dimension: positive and int-sized, or an error naming it.
	auto dimension = [](std::string_view name,
			const std::expected<std::optional<long long>, std::string> &value)
			-> std::expected<std::optional<int>, std::string> {
		if (!value.has_value()) {
			return std::unexpected(value.error());
		}
		if (!value->has_value()) {
			return std::optional<int>{};
		}
		if (*value < 1 || *value > std::numeric_limits<int>::max()) {
			return std::unexpected(std::format("params: {} must be a positive size", name));
		}
		return std::optional<int>{static_cast<int>(**value)};
	};
	// A canvas coordinate: int-sized, or an error naming it.
	auto coordinate = [](std::string_view name,
			const std::expected<std::optional<long long>, std::string> &value)
			-> std::expected<std::optional<int>, std::string> {
		if (!value.has_value()) {
			return std::unexpected(value.error());
		}
		if (!value->has_value()) {
			return std::optional<int>{};
		}
		if (*value < std::numeric_limits<int>::min()
				|| *value > std::numeric_limits<int>::max()) {
			return std::unexpected(std::format("params: {} is too far out", name));
		}
		return std::optional<int>{static_cast<int>(**value)};
	};

	std::expected<std::optional<long long>, std::string> width = opt_int(params, "width");
	std::expected<std::optional<long long>, std::string> height =
		opt_int(params, "height");
	std::expected<std::optional<long long>, std::string> x = opt_int(params, "x");
	std::expected<std::optional<long long>, std::string> y = opt_int(params, "y");
	std::expected<std::optional<int>, std::string> w = dimension("width", width);
	if (!w.has_value()) {
		return std::unexpected(w.error());
	}
	std::expected<std::optional<int>, std::string> h = dimension("height", height);
	if (!h.has_value()) {
		return std::unexpected(h.error());
	}
	std::expected<std::optional<int>, std::string> px = coordinate("x", x);
	if (!px.has_value()) {
		return std::unexpected(px.error());
	}
	std::expected<std::optional<int>, std::string> py = coordinate("y", y);
	if (!py.has_value()) {
		return std::unexpected(py.error());
	}

	if (server_.items.find(**id) == nullptr && (!color_value.has_value()
			|| !text->has_value() || !w->has_value() || !h->has_value())) {
		return std::unexpected(std::string{
			"params: item.set needs color, text, width and height to create an item"});
	}

	ItemUpdate update{
		.color = color_value,
		.text = *text,
		.width = *w,
		.height = *h,
	};
	Item *item = server_.items.set(**id, update, *px, *py);
	if (item == nullptr) {
		return std::unexpected(
			std::string{"no output under the cursor to place the item"});
	}
	std::string id_text = item->id;
	return object_text([&](JsonBuilder *builder) {
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, id_text.c_str());
		json_builder_set_member_name(builder, "x");
		json_builder_add_int_value(builder, item->x);
		json_builder_set_member_name(builder, "y");
		json_builder_add_int_value(builder, item->y);
		json_builder_set_member_name(builder, "width");
		json_builder_add_int_value(builder, item->width);
		json_builder_set_member_name(builder, "height");
		json_builder_add_int_value(builder, item->height);
	});
}

std::expected<std::string, std::string> Ipc::m_item_remove(Client &, JsonObject *params) {
	std::expected<std::optional<std::string>, std::string> id = opt_string(params, "id");
	if (!id.has_value()) {
		return std::unexpected(id.error());
	}
	if (!id->has_value() || (*id)->empty()) {
		return std::unexpected(std::string{"params: id must be a non-empty string"});
	}
	if (!server_.items.remove(**id)) {
		return std::unexpected(std::format("unknown item id '{}'", **id));
	}
	return std::string{"true"};
}

std::expected<std::string, std::string> Ipc::m_item_activate(Client &,
		JsonObject *params) {
	std::expected<std::optional<std::string>, std::string> id = opt_string(params, "id");
	if (!id.has_value()) {
		return std::unexpected(id.error());
	}
	if (!id->has_value() || (*id)->empty()) {
		return std::unexpected(std::string{"params: id must be a non-empty string"});
	}
	Item *item = server_.items.find(**id);
	if (item == nullptr) {
		return std::unexpected(std::format("unknown item id '{}'", **id));
	}
	// The output the item is activated on: the one under the cursor when
	// it shows the item — where a just-placed item sits — else the first
	// output that does.
	Rect box{
		.x = static_cast<double>(item->x),
		.y = static_cast<double>(item->y),
		.w = static_cast<double>(item->width),
		.h = static_cast<double>(item->height),
	};
	Output *chosen = nullptr;
	if (Output *under = server_.output_under_cursor();
			under != nullptr && under->viewport().intersects(box)) {
		chosen = under;
	} else {
		for (const std::unique_ptr<Output> &output : server_.outputs) {
			if (output->viewport().intersects(box)) {
				chosen = output.get();
				break;
			}
		}
	}
	if (chosen == nullptr) {
		return std::unexpected(
			std::format("the item '{}' is on no output's screen", **id));
	}
	emit_item_activated(*item, *chosen);
	return std::string{"true"};
}

std::expected<std::string, std::string> Ipc::m_action_run(Client &,
		JsonObject *params) {
	constexpr std::string_view usage =
		"params: name must be a non-empty string, args an object";
	if (params == nullptr || !json_object_has_member(params, "name")) {
		return std::unexpected(std::string{usage});
	}
	JsonNode *name = json_object_get_member(params, "name");
	const char *name_text = name != nullptr
			&& json_node_get_node_type(name) == JSON_NODE_VALUE
			&& json_node_get_value_type(name) == G_TYPE_STRING
		? json_node_get_string(name) : nullptr;
	if (name_text == nullptr || *name_text == '\0') {
		return std::unexpected(std::string{usage});
	}
	std::string args_json = "{}";
	if (json_object_has_member(params, "args")) {
		JsonNode *args = json_object_get_member(params, "args");
		if (json_node_get_node_type(args) != JSON_NODE_OBJECT) {
			return std::unexpected(std::string{usage});
		}
		args_json = node_text(args);
	}
	emit_action(name_text, args_json);
	return std::string{"true"};
}

std::expected<std::string, std::string> Ipc::m_capture_stop(Client &,
		JsonObject *) {
	if (!server_.capture_ui.recording()) {
		return std::unexpected(std::string{"not recording"});
	}
	server_.capture_ui.stop_recording();
	return std::string{"true"};
}

// --------------------------------------------------------------- writing

void Ipc::queue(Client &client, std::string_view line) {
	if (client.doomed) {
		return;
	}
	client.outbuf.append(line);
	flush(client);
	if (client.outbuf.size() > config::ipc_max_queued) {
		drop(client, "over 1 MiB of unread events");
	}
}

void Ipc::flush(Client &client) {
	if (client.doomed || !client.source) {
		return;
	}
	std::size_t done = 0;
	while (done < client.outbuf.size()) {
		ssize_t n = write(client.fd, client.outbuf.data() + done,
			client.outbuf.size() - done);
		if (n > 0) {
			done += static_cast<std::size_t>(n);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			break; // EAGAIN or a dead peer: the queue keeps the rest
		}
	}
	// What went out is out: the queue keeps only the unwritten rest, or
	// the writable source would fire again and resend it forever.
	if (done > 0) {
		client.outbuf.erase(0, done);
	}
	uint32_t mask = WL_EVENT_READABLE;
	if (!client.outbuf.empty()) {
		mask |= WL_EVENT_WRITABLE; // the rest goes out once the client reads
	}
	wl_event_source_fd_update(client.source.get(), mask);
}

void Ipc::drop(Client &client, const char *reason) {
	if (client.doomed) {
		return;
	}
	client.doomed = true;
	client.source.reset(); // off the loop before the fd it watched closes
	close(client.fd);
	client.fd = -1;
	wlr_log(WLR_INFO, "ipc: dropped a client: %s", reason);
}

void Ipc::close_client(const Client &client) {
	auto it = std::ranges::find_if(clients_, [&client](const std::unique_ptr<Client> &p) {
		return p.get() == &client;
	});
	if (it != clients_.end()) {
		clients_.erase(it);
	}
}

void Ipc::reap() {
	std::erase_if(clients_, [](const std::unique_ptr<Client> &p) { return p->doomed; });
}

// ---------------------------------------------------------------- events

void Ipc::push_event(std::string_view name, std::string_view params) {
	std::string line = event_line(name, params);
	// drop() never erases from clients_, so an event may drop a client — even
	// the one whose request changed the state — while this loop runs.
	for (std::unique_ptr<Client> &client : clients_) {
		if (client->subscribed) {
			queue(*client, line);
		}
	}
}

void Ipc::emit_overview(const Output &output, bool active) {
	std::string name = output.wlr->name; // NUL-terminated for the C API
	push_event("overview", object_text([&](JsonBuilder *builder) {
		json_builder_set_member_name(builder, "output");
		json_builder_add_string_value(builder, name.c_str());
		json_builder_set_member_name(builder, "active");
		json_builder_add_boolean_value(builder, active);
	}));
}

void Ipc::emit_outputs() {
	push_event("outputs", object_text([&](JsonBuilder *builder) {
		add_outputs_array(builder, server_);
	}));
}

void Ipc::emit_desktops() {
	push_event("desktops", object_text([&](JsonBuilder *builder) {
		add_desktops_array(builder, server_);
	}));
}

void Ipc::emit_theme() {
	std::string mode = std::string{theme_name(server_.theme.mode())};
	push_event("theme", object_text([&](JsonBuilder *builder) {
		json_builder_set_member_name(builder, "mode");
		json_builder_add_string_value(builder, mode.c_str());
	}));
}

void Ipc::emit_recording(bool active, const std::string &path) {
	std::string copy = path; // NUL-terminated for the C API
	push_event("capture.recording", object_text([&](JsonBuilder *builder) {
		json_builder_set_member_name(builder, "active");
		json_builder_add_boolean_value(builder, active);
		json_builder_set_member_name(builder, "path");
		json_builder_add_string_value(builder, copy.c_str());
	}));
}

void Ipc::emit_item_moved(const Item &item) {
	std::string id = item.id;
	push_event("item.moved", object_text([&](JsonBuilder *builder) {
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, id.c_str());
		json_builder_set_member_name(builder, "x");
		json_builder_add_int_value(builder, item.x);
		json_builder_set_member_name(builder, "y");
		json_builder_add_int_value(builder, item.y);
	}));
}

void Ipc::emit_item_activated(const Item &item, const Output &output) {
	std::string id = item.id;
	std::string name = output.wlr->name;
	Point top_left = output.canvas_to_local(item.x, item.y);
	double zoom = output.camera.drawn().zoom;
	push_event("item.activated", object_text([&](JsonBuilder *builder) {
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, id.c_str());
		json_builder_set_member_name(builder, "output");
		json_builder_add_string_value(builder, name.c_str());
		json_builder_set_member_name(builder, "box");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "x");
		json_builder_add_double_value(builder, top_left.x);
		json_builder_set_member_name(builder, "y");
		json_builder_add_double_value(builder, top_left.y);
		json_builder_set_member_name(builder, "width");
		json_builder_add_double_value(builder, item.width * zoom);
		json_builder_set_member_name(builder, "height");
		json_builder_add_double_value(builder, item.height * zoom);
		json_builder_end_object(builder);
	}));
}

void Ipc::emit_action(std::string_view name, std::string_view args_json) {
	// Spliced by hand: a builder cannot hold a bare object as a value.
	std::string params = "{\"name\":";
	params += quote(name);
	params += ",\"args\":";
	params += args_json;
	params += "}";
	push_event("action", params);
}

// ------------------------------------------------------------ the source

int Ipc::handle_client(int, uint32_t mask, void *data) {
	auto *client = static_cast<Client *>(data);
	Ipc &ipc = client->ipc;
	bool done = false;
	if (mask & WL_EVENT_READABLE) {
		char buf[line_chunk];
		for (;;) {
			ssize_t n = read(client->fd, buf, sizeof buf);
			if (n > 0) {
				client->pending.append(buf, static_cast<std::size_t>(n));
				continue;
			}
			if (n == 0) {
				done = true; // orderly EOF
			} else if (errno == EINTR) {
				continue;
			} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
				done = true; // transport error: nothing more can arrive
			}
			break;
		}
	}
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		done = true;
	}

	// Dispatch line by line; a method may drop this or any other client.
	bool oversized = false;
	std::size_t start = 0;
	for (std::size_t nl = client->pending.find('\n');
			nl != std::string::npos && !client->doomed && !oversized;
			nl = client->pending.find('\n', start)) {
		std::string_view line =
			trim(std::string_view(client->pending).substr(start, nl - start));
		start = nl + 1;
		if (line.size() > config::ipc_max_line) {
			oversized = true;
			break;
		}
		if (line.empty()) {
			ipc.queue(*client, response("null", quote("not a JSON object"), true));
		} else {
			ipc.handle_line(*client, line);
		}
	}
	if (!oversized && !client->doomed
			&& client->pending.size() - start > config::ipc_max_line) {
		oversized = true; // no newline yet: the partial line passed the cap
	}
	if (start > 0) {
		client->pending.erase(0, start);
	}
	if (oversized) {
		ipc.queue(*client, response("null", quote("line over 64 KiB"), true));
		ipc.drop(*client, "a line over the 64 KiB cap");
		ipc.reap();
		return 0;
	}
	if (client->doomed) {
		ipc.reap();
		return 0;
	}
	ipc.flush(*client);
	if (ipc.quit_) {
		// The response is out; `quit` ends the session.
		wl_display_terminate(ipc.server_.display);
		return 0;
	}
	if (done) {
		ipc.close_client(*client); // destroys `client`: last use of it
		ipc.reap();
	}
	return 0;
}

// ---------------------------------------------------------- the msg client

namespace {

// One line of xdg-desktop-portal-wlr's chooser list: `Monitor: <name>
// <description>` or `Window: <title> (<identifier>)`.
struct ChooserEntry {
	std::string line; // echoed byte-exactly when it is the choice
	std::string monitor; // the monitor's name token; empty for a window
	std::string identifier; // the window's parenthesized identifier; else empty
};

std::vector<ChooserEntry> read_chooser_list() {
	std::vector<ChooserEntry> entries;
	for (std::string line; std::getline(std::cin, line);) {
		if (line.ends_with('\r')) {
			line.pop_back();
		}
		if (line.starts_with("Monitor: ")) {
			std::string rest = line.substr(9);
			entries.push_back({.line = line, .monitor = rest.substr(0, rest.find(' '))});
		} else if (line.starts_with("Window: ") && line.ends_with(')')
				&& line.rfind('(') != std::string::npos) {
			std::size_t open = line.rfind('(');
			entries.push_back({.line = line,
				.identifier = line.substr(open + 1, line.size() - open - 2)});
		}
	}
	return entries;
}

// The object's string `member`, or empty when absent or not a string.
std::string string_member(JsonObject *object, const char *member) {
	if (object == nullptr || !json_object_has_member(object, member)) {
		return {};
	}
	JsonNode *node = json_object_get_member(object, member);
	const char *text = json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? json_node_get_string(node) : nullptr;
	return text != nullptr ? std::string{text} : std::string{};
}

// `wayland-canvas msg capture.pick` as xdg-desktop-portal-wlr's chooser:
// reads the monitor and window list from stdin, runs the compositor's own
// picker, and prints the chosen line back byte-exactly. Silence means the
// user cancelled; only a transport failure is an error exit.
int capture_pick_bridge() {
	if (isatty(0)) {
		std::fputs("usage: wayland-canvas msg capture.pick < chooser list\n"
			"       (xdg-desktop-portal-wlr's chooser: the monitors and\n"
			"        windows arrive on stdin, the choice goes to stdout)\n", stderr);
		return 2;
	}
	std::vector<ChooserEntry> entries = read_chooser_list();
	bool windows = false, monitors = false;
	for (const ChooserEntry &entry : entries) {
		windows = windows || !entry.identifier.empty();
		monitors = monitors || !entry.monitor.empty();
	}
	if (!windows && !monitors) {
		return 0; // nothing to choose between: no answer is a cancel
	}
	const char *what = windows && monitors ? "any" : windows ? "window" : "output";
	int fd = connect_to_socket();
	if (fd < 0) {
		return 1;
	}
	std::string request = std::format(
		R"({{"id":1,"method":"capture.pick","params":{{"what":"{}"}}}})", what);
	request += '\n';
	if (!write_request(fd, request)) {
		std::fputs("wayland-canvas: could not send the request\n", stderr);
		close(fd);
		return 1;
	}
	std::optional<std::string> first = read_line(fd);
	if (!first.has_value()) {
		std::fputs("wayland-canvas: the compositor did not answer\n", stderr);
		close(fd);
		return 1;
	}
	JsonParserPtr parser(json_parser_new());
	std::string buffer(*first);
	GErrorHolder gerr;
	JsonNode *root = nullptr;
	if (json_parser_load_from_data(parser.get(), buffer.c_str(),
			static_cast<gssize>(buffer.size()), &gerr.error)) {
		root = json_parser_get_root(parser.get());
	}
	JsonObject *answer = root != nullptr && json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
	if (answer == nullptr || json_object_has_member(answer, "error")
			|| !json_object_has_member(answer, "result")) {
		close(fd); // an error answer is the user cancelling: silence, exit 0
		return 0;
	}
	JsonNode *result_node = json_object_get_member(answer, "result");
	JsonObject *result = result_node != nullptr
			&& json_node_get_node_type(result_node) == JSON_NODE_OBJECT
		? json_node_get_object(result_node) : nullptr;
	std::string want_monitor = string_member(result, "output");
	std::string want_identifier = string_member(result, "identifier");
	for (const ChooserEntry &entry : entries) {
		bool chosen = (!want_identifier.empty() && entry.identifier == want_identifier)
			|| (!want_monitor.empty() && entry.monitor == want_monitor);
		if (chosen) {
			std::printf("%s\n", entry.line.c_str());
			close(fd);
			return 0;
		}
	}
	close(fd); // a result matching no line is as good as no answer
	return 0;
}

} // namespace

int msg_command(int argc, char *argv[]) {
	// argv[0] is "msg"; argv[1] the method, argv[2] optional params.
	if (argc == 2 && strcmp(argv[1], "capture.pick") == 0) {
		return capture_pick_bridge();
	}
	if (argc < 2 || *argv[1] == '\0' || argc > 3
			|| (argc == 3 && strcmp(argv[1], "subscribe") == 0)) {
		msg_usage();
		return 2;
	}
	bool subscribe = strcmp(argv[1], "subscribe") == 0;
	std::string params_json;
	if (argc == 3) {
		// Re-serialized through json-glib: validates the text the user gave.
		std::string params_text(argv[2]);
		JsonParserPtr parser(json_parser_new());
		GErrorHolder gerr;
		if (!json_parser_load_from_data(parser.get(), params_text.c_str(),
				static_cast<gssize>(params_text.size()), &gerr.error)) {
			std::fprintf(stderr, "wayland-canvas: invalid params JSON: %s\n",
				gerr.error != nullptr && gerr.error->message != nullptr
					? gerr.error->message : "not JSON");
			return 1;
		}
		params_json = node_text(json_parser_get_root(parser.get()));
	}

	int fd = connect_to_socket();
	if (fd < 0) {
		return 1;
	}
	std::string request = std::format(R"({{"id":1,"method":{})", quote(argv[1]));
	if (!params_json.empty()) {
		request += std::format(R"(,"params":{})", params_json);
	}
	request += "}\n";
	if (!write_request(fd, request)) {
		std::fputs("wayland-canvas: could not send the request\n", stderr);
		close(fd);
		return 1;
	}

	std::optional<std::string> first = read_line(fd);
	if (!first.has_value()) {
		std::fputs("wayland-canvas: the compositor did not answer\n", stderr);
		close(fd);
		return 1;
	}
	JsonParserPtr parser(json_parser_new());
	std::string buffer(*first);
	GErrorHolder gerr;
	JsonNode *root = nullptr;
	if (json_parser_load_from_data(parser.get(), buffer.c_str(),
			static_cast<gssize>(buffer.size()), &gerr.error)) {
		root = json_parser_get_root(parser.get());
	}
	JsonObject *answer = root != nullptr && json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
	if (answer == nullptr) {
		std::fputs("wayland-canvas: unreadable response\n", stderr);
		close(fd);
		return 1;
	}
	if (json_object_has_member(answer, "error")) {
		JsonNode *error = json_object_get_member(answer, "error");
		const char *message = error != nullptr
				&& json_node_get_node_type(error) == JSON_NODE_VALUE
				&& json_node_get_value_type(error) == G_TYPE_STRING
			? json_node_get_string(error) : nullptr;
		std::fprintf(stderr, "wayland-canvas: %s\n",
			message != nullptr ? message : "error");
		close(fd);
		return 1;
	}
	if (!json_object_has_member(answer, "result")) {
		std::fputs("wayland-canvas: unreadable response\n", stderr);
		close(fd);
		return 1;
	}
	if (!subscribe) {
		std::string result = node_text(json_object_get_member(answer, "result"));
		std::printf("%s\n", result.c_str());
		close(fd);
		return 0;
	}
	// Events follow, one per line, until the compositor goes or we are killed.
	for (;;) {
		std::optional<std::string> line = read_line(fd);
		if (!line.has_value()) {
			break;
		}
		std::printf("%s\n", line->c_str());
		std::fflush(stdout); // a piped reader sees each event as it happens
	}
	close(fd);
	return 0;
}
