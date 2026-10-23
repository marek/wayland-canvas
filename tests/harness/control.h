#pragma once

// A raw client of the compositor's control socket (docs/intents/platform/
// ipc.md), for what `wayland-canvas msg` cannot send: malformed lines, many
// requests at once, a half-closed connection. It runs in a forked child,
// because the compositor shares the scenario's thread and nothing here may
// block it; what the compositor sends back is written to a file in the
// scratch directory, which the scenario reads as it grows.
//
// The real `wayland-canvas msg` client is run the same way, beside the
// compositor, its answers read from files; so is any shell command a
// scenario starts and then waits for.

#include <functional>
#include <json-glib/json-glib.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include "harness/steps.h"

namespace harness {

struct ControlClient {
	// Half-closes the connection after sending: the compositor sees EOF.
	bool half_close = false;
	// The client goes on listening through this many quiet 400 ms polls; it
	// stops at once on EOF or after 10 s.
	int quiet_polls = 2;
};

// Runs `body` in a forked child, which exits with status 0 once it returns
// (`_exit` in it for another status): a client of the socket with a life of
// its own, a half-read answer or a hang-up. Killed when the run ends, if it
// is still there. Returns the child's pid; -1 when the fork failed.
pid_t fork_client(const std::function<void()> &body);

// Sends `to_send` (raw lines) on a new connection and captures the answer in
// <scratch>/<out>, with a closing "EOF" line when the compositor hung up.
// Returns the child's pid; -1 when the fork failed.
pid_t fork_control_client(std::string_view to_send, std::string_view out,
	ControlClient options = {});

// What the client writing <scratch>/<out> has captured so far.
std::string control_answer(std::string_view out);

// Reaps a client that has exited; true once it is gone. A pid of 0 or less
// is gone already.
bool control_client_done(pid_t pid);

// Kills the clients still running: called by run().
void stop_control_clients();

// ------------------------------------------------ the real msg client

// Runs a shell command through the compositor's own spawn, so that it never
// blocks the scenario, with its stdout, stderr and exit status in
// <scratch>/<name>.out, .err and .code; the .code file appears when it has
// ended. A name used before forgets the earlier run's answer.
void run_command(const std::string &shell, std::string_view name);
// The same for the real `wayland-canvas msg <method>`; `params`, when not
// empty, are single-quoted for the shell.
void run_msg(std::string_view method, std::string_view params, std::string_view name);

bool command_done(std::string_view name);
// What it wrote, trailing newlines trimmed; empty until it has.
std::string command_out(std::string_view name);
std::string command_err(std::string_view name);
// Its exit status; 0 until it has ended.
int command_code(std::string_view name);

// An answer parsed as JSON. Its objects live as long as it does.
class JsonAnswer {
public:
	explicit JsonAnswer(std::string_view text);
	~JsonAnswer();
	JsonAnswer(const JsonAnswer &) = delete;
	JsonAnswer &operator=(const JsonAnswer &) = delete;

	// The root when it is an object; null when the text is not JSON or no object.
	JsonObject *object() const;

private:
	JsonParser *parser_;
	bool parsed_ = false;
};

// An integer member: nullopt when absent or not a whole number.
std::optional<long long> member_int(JsonObject *object, const char *name);
// A string member; empty when absent or not a string.
std::string member_string(JsonObject *object, const char *name);

// Ends the run the way a user ends the compositor: the real `msg quit`. A
// step returns this as the run's last; the compositor stopping is then the
// run's end, and one still up after 5 s is a failure.
Result quit_by_msg();

} // namespace harness
