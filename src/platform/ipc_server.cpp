#include "platform/ipc_server.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iterator>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>

#include "core/chord.h"
#include "core/config.h"
#include "core/json.h"
#include "core/text.h"
#include "core/wlr.h"

namespace {

constexpr int listen_backlog = 4;
constexpr std::size_t line_chunk = 512;

// A byte count the way the messages and the docs say it: "1 MiB", "64 KiB".
std::string size_text(std::size_t bytes) {
	constexpr std::size_t kib = 1024;
	if (bytes % (kib * kib) == 0) {
		return std::format("{} MiB", bytes / (kib * kib));
	}
	return bytes % kib == 0 ? std::format("{} KiB", bytes / kib)
		: std::format("{} bytes", bytes);
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

// A parsed request line. Owns the json-glib tree its pointers point into.
struct Request {
	JsonParserPtr parser;
	std::string id_json = "null"; // the id as JSON text, null when unreadable
	std::string method; // empty when absent
	JsonObject *params = nullptr; // null when absent
	std::string error; // why the line is not a request; empty when it is one
};

// Parses one line. `error` carries the reason when it is not a request.
Request parse_line(std::string_view line) {
	Request out;
	GErrorPtr reason;
	out.parser = parse_json(line, &reason);
	if (out.parser == nullptr) {
		out.error = reason != nullptr && reason->message != nullptr
			? std::format("not JSON: {}", reason->message)
			: std::string{"not JSON"};
		return out;
	}
	JsonObject *object = root_object(out.parser);
	if (object == nullptr) {
		out.error = "not a JSON object";
		return out;
	}
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
	const char *method = node_string(json_object_get_member(object, "method"));
	if (method == nullptr) {
		out.error = "method is not a string";
		return out;
	}
	out.method = method;
	if (json_object_has_member(object, "params")) {
		out.params = node_object(json_object_get_member(object, "params"));
		if (out.params == nullptr) {
			out.error = "params is not an object";
			return out;
		}
	}
	return out;
}

} // namespace

std::string ipc_socket_path(const char *runtime_dir, const char *display) {
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

bool fill_socket_address(sockaddr_un &addr, const std::string &path) {
	addr.sun_family = AF_UNIX;
	if (path.size() >= sizeof(addr.sun_path)) {
		return false;
	}
	std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	return true;
}

IpcServer::Client::Client(IpcServer &ipc, int fd) : ipc(ipc), fd(fd) {
}

IpcServer::Client::~Client() {
	ipc.abandon_waits(this);
	source.reset(); // off the loop before the fd it watched is closed
	if (fd >= 0) { // a dropped client's fd is already closed
		close(fd);
	}
}

IpcServer::IpcServer() = default;

IpcServer::~IpcServer() {
	finish();
}

bool IpcServer::init(wl_display *display, wl_event_loop *loop) {
	display_ = display;
	loop_ = loop;
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	path_ = ipc_socket_path(runtime, getenv("WAYLAND_DISPLAY"));
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
	if (!fill_socket_address(addr, path_)) {
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
	listen_source_.reset(wl_event_loop_add_fd(loop_, listen_fd_,
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

void IpcServer::finish() {
	// A request still waiting on the user is told its wait is over while its
	// client is alive to hear it.
	abandon_waits(nullptr);
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

int IpcServer::handle_listen(int, uint32_t mask, void *data) {
	auto *ipc = static_cast<IpcServer *>(data);
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	for (;;) {
		int conn = accept4(ipc->listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (conn < 0) {
			break; // EAGAIN: every pending connection is taken
		}
		auto client = std::make_unique<Client>(*ipc, conn);
		client->source.reset(wl_event_loop_add_fd(ipc->loop_, conn,
			WL_EVENT_READABLE, handle_client, client.get()));
		if (!client->source) {
			wlr_log(WLR_ERROR, "ipc: could not watch a client connection");
			continue; // the Client destructor closed the fd
		}
		ipc->clients_.push_back(std::move(client));
	}
	return 0;
}

void IpcServer::register_method(std::string_view name, Method run) {
	bool own = name == "subscribe" || name == "quit";
	if (own || !methods_.emplace(std::string{name}, std::move(run)).second) {
		wlr_log(WLR_ERROR, "ipc: the method '%.*s' is registered twice",
			static_cast<int>(name.size()), name.data());
		std::abort();
	}
}

IpcServer::Reply IpcServer::m_subscribe(Client &client, JsonObject *) {
	client.subscribed = true;
	return std::string{"true"};
}

IpcServer::Reply IpcServer::m_quit(Client &, JsonObject *) {
	quit_ = true; // handle_client ends the session once the response is out
	return std::string{"true"};
}

IpcServer::Reply IpcServer::dispatch(Client &client, std::string_view name,
		JsonObject *params) {
	constexpr std::array own = {
		std::pair{std::string_view{"subscribe"}, &IpcServer::m_subscribe},
		std::pair{std::string_view{"quit"}, &IpcServer::m_quit},
	};
	for (const auto &[method, run] : own) {
		if (method == name) {
			return (this->*run)(client, params);
		}
	}
	auto found = methods_.find(name);
	if (found == methods_.end()) {
		return std::unexpected(std::format("unknown method '{}'", name));
	}
	return found->second(params);
}

void IpcServer::handle_line(Client &client, std::string_view line) {
	Request request = parse_line(line);
	if (!request.error.empty()) {
		queue(client, response(request.id_json, text::json_quote(request.error), true));
		return;
	}
	Reply reply = dispatch(client, request.method, request.params);
	if (reply.wait.has_value()) {
		// The method has begun what the user finishes; answer() sends the response then.
		Waiting &waiting = slot(reply.wait->key);
		waiting.client = &client;
		waiting.id = request.id_json;
		return;
	}
	send(client, request.id_json, reply.body);
}

void IpcServer::send(Client &client, std::string_view id_json, const Answer &body) {
	queue(client, response(id_json,
		body.has_value() ? *body : text::json_quote(body.error()), !body.has_value()));
}

IpcServer::Waiting &IpcServer::slot(std::string_view key) {
	auto found = std::ranges::find(waiting_, key, &Waiting::key);
	if (found == waiting_.end()) {
		return waiting_.emplace_back(Waiting{.key = std::string{key}});
	}
	return *found;
}

bool IpcServer::waiting(std::string_view key) const {
	auto found = std::ranges::find(waiting_, key, &Waiting::key);
	return found != waiting_.end() && found->client != nullptr;
}

void IpcServer::answer(std::string_view key, const Answer &body) {
	Waiting &waiting = slot(key);
	if (waiting.client == nullptr) {
		return;
	}
	Client &client = *waiting.client;
	std::string id = std::move(waiting.id);
	waiting.client = nullptr;
	waiting.id.clear();
	send(client, id, body);
}

void IpcServer::on_abandoned(std::string_view key, std::function<void()> abandoned) {
	slot(key).abandoned = std::move(abandoned);
}

void IpcServer::abandon_waits(const Client *client) {
	// Forgotten first: a hook that answers finds nothing waiting, or, when the
	// server is finishing, the client it answers still alive.
	std::vector<std::function<void()>> hooks;
	for (Waiting &waiting : waiting_) {
		if (waiting.client == nullptr || (client != nullptr && waiting.client != client)) {
			continue;
		}
		if (client != nullptr) {
			waiting.client = nullptr;
			waiting.id.clear();
		}
		if (waiting.abandoned) {
			hooks.push_back(waiting.abandoned);
		}
	}
	for (const std::function<void()> &hook : hooks) {
		hook();
	}
}

void IpcServer::queue(Client &client, std::string_view line) {
	if (client.doomed) {
		return;
	}
	client.outbuf.append(line);
	flush(client);
	if (client.outbuf.size() > config::ipc_max_queued) {
		drop(client, std::format("over {} of unread events",
			size_text(config::ipc_max_queued)).c_str());
	}
}

void IpcServer::flush(Client &client) {
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

void IpcServer::drop(Client &client, const char *reason) {
	if (client.doomed) {
		return;
	}
	client.doomed = true;
	client.source.reset(); // off the loop before the fd it watched closes
	close(client.fd);
	client.fd = -1;
	wlr_log(WLR_INFO, "ipc: dropped a client: %s", reason);
}

void IpcServer::close_client(const Client &client) {
	destroy_owned(clients_, &client);
}

void IpcServer::reap() {
	// Taken out first: a destructor never runs while the vector is mid-erase.
	auto first_doomed = std::ranges::stable_partition(clients_,
		[](const std::unique_ptr<Client> &p) { return !p->doomed; }).begin();
	std::vector<std::unique_ptr<Client>> doomed(std::make_move_iterator(first_doomed),
		std::make_move_iterator(clients_.end()));
	clients_.erase(first_doomed, clients_.end());
}

void IpcServer::push_event(std::string_view name, std::string_view params) {
	std::string line = event_line(name, params);
	// drop() never erases from clients_, so an event may drop a client — even
	// the one whose request changed the state — while this loop runs.
	for (std::unique_ptr<Client> &client : clients_) {
		if (client->subscribed) {
			queue(*client, line);
		}
	}
}

bool IpcServer::has_subscriber() const {
	return std::ranges::any_of(clients_, [](const std::unique_ptr<Client> &client) {
		return client->subscribed && !client->doomed;
	});
}

int IpcServer::handle_client(int, uint32_t mask, void *data) {
	auto *client = static_cast<Client *>(data);
	IpcServer &ipc = client->ipc;
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
			settings::trim(std::string_view(client->pending).substr(start, nl - start));
		start = nl + 1;
		if (line.size() > config::ipc_max_line) {
			oversized = true;
			break;
		}
		if (line.empty()) {
			ipc.queue(*client,
				response("null", text::json_quote("not a JSON object"), true));
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
		std::string cap = size_text(config::ipc_max_line);
		ipc.queue(*client,
			response("null", text::json_quote(std::format("line over {}", cap)), true));
		ipc.drop(*client, std::format("a line over the {} cap", cap).c_str());
		ipc.reap();
		return 0;
	}
	if (client->doomed) {
		ipc.reap();
		return 0;
	}
	ipc.flush(*client);
	if (ipc.quit_) {
		wl_display_terminate(ipc.display_);
		return 0;
	}
	if (done) {
		ipc.close_client(*client); // destroys `client`: last use of it
		ipc.reap();
	}
	return 0;
}
