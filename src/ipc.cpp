#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "output.h"
#include "server.h"

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

// Writes the whole buffer; gives up on a client that is not reading.
void write_all(int fd, std::string_view bytes) {
	std::size_t done = 0;
	while (done < bytes.size()) {
		ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
		if (n > 0) {
			done += static_cast<std::size_t>(n);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			wlr_log(WLR_ERROR, "ipc: could not answer a client: %s", std::strerror(errno));
			return;
		}
	}
}

} // namespace

Ipc::Client::Client(Ipc &ipc, int fd) : ipc(ipc), fd(fd) {
}

Ipc::Client::~Client() {
	source.reset(); // off the loop before the fd it watched is closed
	close(fd);
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
	listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
	std::size_t start = 0;
	for (std::size_t nl = client->pending.find('\n'); nl != std::string::npos;
		nl = client->pending.find('\n', start)) {
		std::string_view line =
			trim(std::string_view(client->pending).substr(start, nl - start));
		start = nl + 1;
		if (!line.empty()) {
			std::string response = ipc.dispatch(line);
			response += '\n';
			write_all(client->fd, response);
		}
	}
	if (start > 0) {
		client->pending.erase(0, start);
	}
	if (done) {
		ipc.drop_client(*client); // destroys `client`: last use of it
	}
	return 0;
}

void Ipc::drop_client(const Client &client) {
	auto it = std::ranges::find_if(clients_, [&client](const std::unique_ptr<Client> &p) {
		return p.get() == &client;
	});
	if (it != clients_.end()) {
		clients_.erase(it);
	}
}

std::string Ipc::dispatch(std::string_view command) {
	if (command == "launcher") {
		server_.launcher.toggle(server_.output_under_cursor());
		return "ok";
	}
	if (command == "overview") {
		Output *output = server_.output_under_cursor();
		if (output == nullptr) {
			return "error: no output under the cursor";
		}
		if (output->overview.active()) {
			output->overview.exit();
		} else {
			output->overview.enter();
		}
		return "ok";
	}
	if (command == "quit") {
		wl_display_terminate(server_.display);
		return "ok";
	}
	return std::format("error: unknown command '{}'", command);
}

std::optional<std::string> send_ipc_message(std::string_view command) {
	const char *path = getenv("WAYLAND_CANVAS_SOCKET");
	if (path == nullptr || *path == '\0') {
		std::fprintf(stderr, "wayland-canvas: $WAYLAND_CANVAS_SOCKET is not set\n");
		return std::nullopt;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		std::fprintf(stderr, "wayland-canvas: socket: %s\n", std::strerror(errno));
		return std::nullopt;
	}
	sockaddr_un addr{};
	if (!fill_address(addr, path) ||
		connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
		std::fprintf(stderr, "wayland-canvas: cannot reach the compositor at %s\n", path);
		close(fd);
		return std::nullopt;
	}
	std::string request(command);
	request += '\n';
	write_all(fd, request);
	shutdown(fd, SHUT_WR); // the compositor answers and drops the connection
	std::string response;
	char buf[line_chunk];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof buf);
		if (n > 0) {
			response.append(buf, static_cast<std::size_t>(n));
			if (response.find('\n') != std::string::npos) {
				break;
			}
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		break; // EOF or error: take what arrived
	}
	close(fd);
	while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
		response.pop_back();
	}
	return response;
}
