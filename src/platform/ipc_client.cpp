#include "platform/ipc_client.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sys/socket.h>
#include <sys/un.h>

#include "core/text.h"
#include "platform/ipc_server.h"
#include "platform/portal_chooser.h"

namespace {

void msg_usage() {
	std::fputs("usage: wayland-canvas msg <method> [<params as JSON>]\n"
		"       wayland-canvas msg subscribe\n", stderr);
}

} // namespace

int connect_to_socket() {
	const char *exported = std::getenv("WAYLAND_CANVAS_SOCKET");
	std::string path = exported != nullptr && *exported != '\0' ? std::string(exported)
		: ipc_socket_path(std::getenv("XDG_RUNTIME_DIR"), std::getenv("WAYLAND_DISPLAY"));
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
	if (!fill_socket_address(addr, path) ||
			connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
		std::fprintf(stderr, "wayland-canvas: cannot reach the compositor at %s\n", path.c_str());
		close(fd);
		return -1;
	}
	return fd;
}

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

std::optional<Exchange> round_trip(std::string_view method, std::string_view params) {
	OwnedFd socket(connect_to_socket());
	if (socket.get() < 0) {
		return std::nullopt;
	}
	std::string request = std::format(R"({{"id":1,"method":{})", text::json_quote(method));
	if (!params.empty()) {
		request += std::format(R"(,"params":{})", params);
	}
	request += "}\n";
	if (!write_request(socket.get(), request)) {
		std::fputs("wayland-canvas: could not send the request\n", stderr);
		return std::nullopt;
	}
	std::optional<std::string> first = read_line(socket.get());
	if (!first.has_value()) {
		std::fputs("wayland-canvas: the compositor did not answer\n", stderr);
		return std::nullopt;
	}
	Exchange exchange{.socket = std::move(socket), .parser = parse_json(*first)};
	exchange.answer = root_object(exchange.parser);
	return exchange;
}

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
		GErrorPtr reason;
		JsonParserPtr parser = parse_json(argv[2], &reason);
		if (parser == nullptr) {
			std::fprintf(stderr, "wayland-canvas: invalid params JSON: %s\n",
				reason != nullptr && reason->message != nullptr
					? reason->message : "not JSON");
			return 1;
		}
		params_json = node_text(json_parser_get_root(parser.get()));
	}

	std::optional<Exchange> exchange = round_trip(argv[1], params_json);
	if (!exchange.has_value()) {
		return 1;
	}
	JsonObject *answer = exchange->answer;
	if (answer == nullptr) {
		std::fputs("wayland-canvas: unreadable response\n", stderr);
		return 1;
	}
	if (json_object_has_member(answer, "error")) {
		const char *message = node_string(json_object_get_member(answer, "error"));
		std::fprintf(stderr, "wayland-canvas: %s\n",
			message != nullptr ? message : "error");
		return 1;
	}
	if (!json_object_has_member(answer, "result")) {
		std::fputs("wayland-canvas: unreadable response\n", stderr);
		return 1;
	}
	if (!subscribe) {
		std::string result = node_text(json_object_get_member(answer, "result"));
		std::printf("%s\n", result.c_str());
		return 0;
	}
	// Events follow, one per line, until the compositor goes or we are killed.
	for (;;) {
		std::optional<std::string> line = read_line(exchange->socket.get());
		if (!line.has_value()) {
			break;
		}
		std::printf("%s\n", line->c_str());
		std::fflush(stdout); // a piped reader sees each event as it happens
	}
	return 0;
}
