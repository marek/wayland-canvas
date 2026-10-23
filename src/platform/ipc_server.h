#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/un.h>
#include <vector>
#include "core/util.h"

typedef struct _JsonObject JsonObject;

// The control socket of the compositor `display` names — a socket name under
// `runtime_dir`, or an absolute path as $WAYLAND_DISPLAY allows:
// <dir>/wayland-canvas-<name>.sock. Empty when either part is missing.
std::string ipc_socket_path(const char *runtime_dir, const char *display);
// Fills `addr` with `path`. False when the path does not fit.
bool fill_socket_address(sockaddr_un &addr, const std::string &path);

// The control socket's transport: JSON lines as docs/intents/platform/ipc.md
// defines them. It knows no method but `subscribe` and `quit`: each area of
// the protocol registers its own (platform/ipc_methods.h). The path is
// exported to spawned clients as $WAYLAND_CANVAS_SOCKET; `wayland-canvas msg`
// (platform/ipc_client.cpp) is the command-line client.
class IpcServer {
public:
	// What answers a request: a result's JSON text or an error message.
	using Answer = std::expected<std::string, std::string>;
	// The wait a method answers later, by the user's own action: the requests
	// that wait on one key are answered one at a time, and `key` names a
	// literal of the method's file.
	struct Wait {
		std::string_view key;
	};
	// What a method hands back: the answer to send now, or the wait that
	// answers it later.
	struct Reply {
		Answer body;
		std::optional<Wait> wait;
		Reply(std::string result) : body(std::move(result)) {}
		Reply(std::unexpected<std::string> error) : body(std::move(error)) {}
		Reply(Wait later) : wait(later) {}
	};
	// `params` is null when the request carries none.
	using Method = std::function<Reply(JsonObject *params)>;

	IpcServer();
	~IpcServer();
	IpcServer(const IpcServer &) = delete;
	IpcServer &operator=(const IpcServer &) = delete;

	// False (logged) when the socket cannot be created. Call after the Wayland
	// display socket exists (the name derives from it) and before any client
	// is spawned, so clients inherit $WAYLAND_CANVAS_SOCKET.
	bool init(wl_display *display, wl_event_loop *loop);
	void finish(); // drops the socket and every client connection

	// Adds a method of the protocol. A name given twice is a bug: the
	// compositor ends.
	void register_method(std::string_view name, Method run);

	// Sends the answer to the request waiting on `key`: the callbacks the
	// user's actions end in call it. No request waiting (its client went): a
	// no-op.
	void answer(std::string_view key, const Answer &body);
	bool waiting(std::string_view key) const;
	// Runs `abandoned` when the client of a request waiting on `key` goes
	// before the answer, and when the server finishes with one waiting. The
	// request is already forgotten when it runs, so an answer is a no-op.
	void on_abandoned(std::string_view key, std::function<void()> abandoned);

	// Pushes one event line to every subscriber; `params` is a JSON object's text.
	void push_event(std::string_view name, std::string_view params);
	bool has_subscriber() const; // a live connection that asked for events

private:
	struct Client {
		Client(IpcServer &ipc, int fd);
		~Client(); // removes the event source, then closes the fd
		Client(const Client &) = delete;
		Client &operator=(const Client &) = delete;

		IpcServer &ipc;
		int fd;
		EventSourcePtr source;
		std::string pending; // bytes before the next newline
		std::string outbuf; // responses and events not yet accepted by the kernel
		bool subscribed = false;
		bool doomed = false; // dropped; freed by reap()
	};

	struct Waiting {
		std::string key;
		Client *client = nullptr;
		std::string id; // the id its response echoes
		std::function<void()> abandoned;
	};

	// The methods of the connection itself, rows of dispatch's table.
	Reply m_subscribe(Client &client, JsonObject *params);
	Reply m_quit(Client &client, JsonObject *params);
	Reply dispatch(Client &client, std::string_view name, JsonObject *params);
	Waiting &slot(std::string_view key);
	// Forgets what `client` waits on and runs the abandon hooks of those waits.
	void abandon_waits(const Client *client);

	static int handle_listen(int fd, uint32_t mask, void *data);
	static int handle_client(int fd, uint32_t mask, void *data);
	void handle_line(Client &client, std::string_view line);
	// Sends the answer to the request `id_json` of `client`.
	void send(Client &client, std::string_view id_json, const Answer &body);
	void queue(Client &client, std::string_view line); // append, flush, cap-check
	void flush(Client &client); // nonblocking; keeps the remainder queued
	void drop(Client &client, const char *reason); // close now, free in reap()
	void close_client(const Client &client); // erase immediately (EOF path)
	void reap(); // frees doomed clients; never called while one is in use

	wl_display *display_ = nullptr;
	wl_event_loop *loop_ = nullptr;
	std::string path_; // socket file; unlinked in finish() once bound
	bool bound_ = false;
	bool quit_ = false; // `quit` answered: end the session after the flush
	int listen_fd_ = -1;
	EventSourcePtr listen_source_;
	std::map<std::string, Method, std::less<>> methods_;
	std::vector<Waiting> waiting_;
	std::vector<std::unique_ptr<Client>> clients_;
};
