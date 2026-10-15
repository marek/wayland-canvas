#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <json-glib/json-glib.h>

#include "canvas/desktop.h"
#include "core/config.h"
#include "core/theme.h"
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

// ------------------------------------------------------------ the client

// Connects to $WAYLAND_CANVAS_SOCKET; -1 (explained on stderr) on failure.
int connect_to_socket() {
	const char *path = std::getenv("WAYLAND_CANVAS_SOCKET");
	if (path == nullptr || *path == '\0') {
		std::fputs("wayland-canvas: $WAYLAND_CANVAS_SOCKET is not set\n", stderr);
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
		std::fprintf(stderr, "wayland-canvas: cannot reach the compositor at %s\n", path);
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
	if (runtime == nullptr || *runtime == '\0' || display == nullptr || *display == '\0') {
		wlr_log(WLR_ERROR, "no $XDG_RUNTIME_DIR or $WAYLAND_DISPLAY: no control socket");
		return false;
	}
	path_ = std::string(runtime) + "/" + config::ipc_socket_prefix + "-" + display + ".sock";
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
	std::expected<std::string, std::string> result =
		dispatch_method(client, request.method, request.params);
	queue(client, response(request.id_json,
		result.has_value() ? *result : quote(result.error()), !result.has_value()));
}

std::expected<std::string, std::string> Ipc::dispatch_method(Client &client,
		std::string_view name, JsonObject *params) {
	// The protocol's method table; the canvas-items slice adds item.set and
	// item.remove rows here.
	constexpr std::array table = {
		std::pair{std::string_view{"state"}, &Ipc::m_state},
		std::pair{std::string_view{"subscribe"}, &Ipc::m_subscribe},
		std::pair{std::string_view{"launcher.toggle"}, &Ipc::m_launcher_toggle},
		std::pair{std::string_view{"overview.toggle"}, &Ipc::m_overview_toggle},
		std::pair{std::string_view{"quit"}, &Ipc::m_quit},
		std::pair{std::string_view{"theme.set"}, &Ipc::m_theme_set},
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
		json_builder_set_member_name(builder, "theme");
		std::string mode = std::string{theme_name(server_.theme.mode())};
		json_builder_add_string_value(builder, mode.c_str());
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
	if (strcmp(mode, "light") == 0) {
		server_.set_theme(ThemeMode::light);
	} else if (strcmp(mode, "dark") == 0) {
		server_.set_theme(ThemeMode::dark);
	} else {
		return std::unexpected(std::format(
			"params: mode must be \"light\" or \"dark\", not '{}'", mode));
	}
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

int msg_command(int argc, char *argv[]) {
	// argv[0] is "msg"; argv[1] the method, argv[2] optional params.
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
