#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "core/util.h"

class Output;
class Server;
struct Item;
typedef struct _JsonObject JsonObject;

// The compositor's control socket: JSON lines as
// docs/intents/platform/ipc.md defines them — a request names a method and
// carries an id, the response echoes the id with a result or an error,
// and a connection that subscribed also receives events. The path is
// exported to spawned clients as $WAYLAND_CANVAS_SOCKET; `wayland-canvas
// msg` is the command-line client.
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

	// The event entry points, called from where the compositor's state
	// actually changes. Each serializes its part of the state and pushes one
	// event line to every subscriber.
	void emit_overview(const Output &output, bool active);
	void emit_outputs(); // an output appeared, went, or changed size or scale
	void emit_desktops(); // a desktop was created, moved, combined or abandoned
	void emit_theme(); // the mode changed
	void emit_item_moved(const Item &item); // the user dropped a dragged item
	// The user double-clicked an item; `box` is the item on `output` in
	// output-local logical px.
	void emit_item_activated(const Item &item, const Output &output);

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
		std::string outbuf; // responses and events not yet accepted by the kernel
		bool subscribed = false;
		bool doomed = false; // dropped; freed by reap()
	};

	// One method of the protocol: `run` returns the result's JSON text or an
	// error message.
	using MethodFn = std::expected<std::string, std::string> (Ipc::*)(Client &,
		JsonObject *params);
	std::expected<std::string, std::string> dispatch_method(Client &client,
		std::string_view name, JsonObject *params);

	std::expected<std::string, std::string> m_state(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_subscribe(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_launcher_toggle(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_overview_toggle(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_quit(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_theme_set(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_item_set(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_item_remove(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_item_activate(Client &client,
		JsonObject *params);
	std::expected<std::string, std::string> m_action_run(Client &client,
		JsonObject *params);
	// An `action` event: `args_json` is a whole JSON object's text.
	void emit_action(std::string_view name, std::string_view args_json);
	// An `output` param by name; nullptr when absent and none is under the
	// cursor. An error names a bad or unknown param.
	std::expected<Output *, std::string> resolve_output(JsonObject *params) const;

	static int handle_listen(int fd, uint32_t mask, void *data);
	static int handle_client(int fd, uint32_t mask, void *data);
	void handle_line(Client &client, std::string_view line);
	void push_event(std::string_view name, std::string_view params);
	void queue(Client &client, std::string_view line); // append, flush, cap-check
	void flush(Client &client); // nonblocking; keeps the remainder queued
	void drop(Client &client, const char *reason); // close now, free in reap()
	void close_client(const Client &client); // erase immediately (EOF path)
	void reap(); // frees doomed clients; never called while one is in use

	Server &server_;
	std::string path_; // socket file; unlinked in finish() once bound
	bool bound_ = false;
	bool quit_ = false; // `quit` answered: end the session after the flush
	int listen_fd_ = -1;
	EventSourcePtr listen_source_;
	std::vector<std::unique_ptr<Client>> clients_;
};

// `wayland-canvas msg <method> [<params as JSON>]`: sends one request,
// prints the result and exits 0, or prints the error to stderr and exits 1.
// `msg subscribe` prints each event as one line until interrupted. argv[1]
// is the method; a transport failure is an error too. Used by main().
int msg_command(int argc, char *argv[]);
