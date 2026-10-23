#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "canvas/desktop.h"
#include "core/settings.h"
#include "core/util.h"

// One mapped window as the app bar sees it (docs/intents/shell/app-bar.md).
struct WindowRow {
	uint64_t id = 0;
	std::string app_id, title;
	std::optional<DesktopId> desktop;
	bool minimized = false, focused = false;
	bool operator==(const WindowRow &) const = default;
};

class Output;
class Server;
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
	// The `emit_` functions are called when the state they report actually
	// changes. Each serializes its part of it and pushes one event line to
	// every subscriber.
	void emit_overview(const Output &output, bool active);
	void emit_outputs(); // an output appeared, went, or changed size or scale
	void emit_desktops(); // a desktop was created, moved, combined or abandoned
	// A window opened, closed, moved, was retitled, minimized, restored or
	// focused (docs/intents/shell/app-bar.md).
	void emit_windows();
	void emit_theme(); // the mode changed
	void emit_settings(); // the effective settings changed
	// A `surface.move` or `surface.resize` drag ended: answers its client
	// with the rectangle the surface ended at, or an error without one. No
	// request pending: a no-op.
	void answer_surface_drag(std::optional<wlr_box> box);
	// A recording started (with its file) or stopped (empty path).
	void emit_recording(bool active, const std::string &path);
	// Runs a shell action from inside the compositor (a lock trigger): the
	// same `action` event `action.run` pushes.
	void run_action(std::string_view name, std::string_view args_json);

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

	// What answers a request: a result's JSON text or an error message.
	using Answer = std::expected<std::string, std::string>;
	// The requests whose answer waits for the user, one of each kind at a time:
	// `capture.pick` for the pick, `shortcut.capture` for the chord, and
	// `surface.move` and `surface.resize` for the release.
	enum class Wait { pick, capture, drag };
	struct Waiting {
		Client *client = nullptr;
		std::string id; // the id its response echoes
	};
	// What a method hands back: the answer to send now, or the wait that
	// answers it later.
	struct Reply {
		Answer body;
		std::optional<Wait> wait;
		Reply(std::string result) : body(std::move(result)) {}
		Reply(std::unexpected<std::string> error) : body(std::move(error)) {}
		Reply(Wait kind) : wait(kind) {}
	};

	// One method of the protocol, a row of dispatch_method's table.
	Reply dispatch_method(Client &client, std::string_view name, JsonObject *params);
	Reply m_state(Client &client, JsonObject *params);
	Reply m_subscribe(Client &client, JsonObject *params);
	Reply m_launcher_toggle(Client &client, JsonObject *params);
	Reply m_overview_toggle(Client &client, JsonObject *params);
	Reply m_quit(Client &client, JsonObject *params);
	Reply m_theme_set(Client &client, JsonObject *params);
	Reply m_window_focus(Client &client, JsonObject *params);
	Reply m_action_run(Client &client, JsonObject *params);
	Reply m_surface_place(Client &client, JsonObject *params);
	Reply m_surface_unplace(Client &client, JsonObject *params);
	Reply m_surface_move(Client &client, JsonObject *params);
	Reply m_surface_resize(Client &client, JsonObject *params);
	Reply m_capture_pick(Client &client, JsonObject *params);
	Reply m_capture_stop(Client &client, JsonObject *params);
	Reply m_shortcut_capture(Client &client, JsonObject *params);
	Reply m_shortcut_cancel(Client &client, JsonObject *params);
	Reply m_settings_get(Client &client, JsonObject *params);
	Reply m_settings_set(Client &client, JsonObject *params);
	// Hands the press held on a plugin's surface to a compositor drag; the
	// release answers.
	Reply surface_drag(JsonObject *params, bool resize);
	// An `action` event: `args_json` is a whole JSON object's text.
	void emit_action(std::string_view name, std::string_view args_json);
	// An `output` param by name; nullptr when absent and none is under the
	// cursor. An error names a bad or unknown param.
	std::expected<Output *, std::string> resolve_output(JsonObject *params) const;

	static int handle_listen(int fd, uint32_t mask, void *data);
	static int handle_client(int fd, uint32_t mask, void *data);
	void handle_line(Client &client, std::string_view line);
	// Sends the answer to the request `id_json` of `client`.
	void send(Client &client, std::string_view id_json, const Answer &body);
	// Sends the answer to the request waiting on `kind`: the callbacks the user's
	// actions end in call it. No request waiting (its client went): a no-op.
	void answer(Wait kind, const Answer &body);
	Waiting &slot(Wait kind) { return waiting_[static_cast<std::size_t>(kind)]; }
	void push_event(std::string_view name, std::string_view params);
	bool has_subscriber() const; // a live connection that asked for events
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
	// The request waiting on each kind of user action, if any.
	std::array<Waiting, 3> waiting_;
	// The window list as it was last pushed, and the rows each frame's list
	// is read into: an identical list is no event. Both keep their rows'
	// strings between frames, so comparing allocates nothing. Unset while no
	// subscriber has heard a list, which is also what a list nobody listened
	// for leaves behind.
	std::vector<WindowRow> told_rows_, window_rows_;
	bool windows_told_ = false;
	std::vector<std::unique_ptr<Client>> clients_;
};

// `wayland-canvas msg <method> [<params as JSON>]`: sends one request,
// prints the result and exits 0, or prints the error to stderr and exits 1.
// `msg subscribe` prints each event as one line until interrupted. argv[1]
// is the method; a transport failure is an error too. Used by main().
int msg_command(int argc, char *argv[]);

// The client half of the control socket, which `msg_command` and the
// scenarios' raw clients share. Connects to $WAYLAND_CANVAS_SOCKET, else to
// the socket of the compositor $WAYLAND_DISPLAY names: a connected fd, or -1
// explained on stderr.
int connect_to_socket();
// Blocking write of the whole buffer; false on failure.
bool write_request(int fd, std::string_view bytes);
