#pragma once

#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "util.h"

class Server;

// The compositor's control socket: newline-terminated command lines in, one
// response line out. The path is exported to spawned clients as
// $WAYLAND_CANVAS_SOCKET; `wayland-canvas msg` is the command-line client.
class Ipc {
public:
	explicit Ipc(Server &server);
	~Ipc();
	Ipc(const Ipc &) = delete;
	Ipc &operator=(const Ipc &) = delete;

	// False (logged) when the socket cannot be created. Call after the Wayland
	// display socket exists (the name derives from it) and before any client
	// is spawned, so clients inherit $WAYLAND_CANVAS_SOCKET.
	bool init();
	void finish(); // drops the socket and every client connection

private:
	struct Client {
		Client(Ipc &ipc, int fd);
		~Client(); // removes the event source, then closes the fd
		Client(const Client &) = delete;
		Client &operator=(const Client &) = delete;

		Ipc &ipc;
		int fd;
		EventSourcePtr source;
		std::string pending; // bytes before the next newline
	};

	Server &server_;
	std::string path_; // socket file; unlinked in finish() once bound
	bool bound_ = false;
	int listen_fd_ = -1;
	EventSourcePtr listen_source_;
	std::vector<std::unique_ptr<Client>> clients_;
	static int handle_listen(int fd, uint32_t mask, void *data);
	static int handle_client(int fd, uint32_t mask, void *data);
	void drop_client(const Client &client);
	std::string dispatch(std::string_view command);
};

// Sends one command over $WAYLAND_CANVAS_SOCKET and returns the response
// line; nullopt when there is no socket or the compositor never answers.
// Used by `wayland-canvas msg`.
std::optional<std::string> send_ipc_message(std::string_view command);
