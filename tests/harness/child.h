#pragma once

// Child processes of a scenario: the test clients, the bus, the stand-ins.
// Each has its stdout on a pipe the scenario reads without blocking, dies
// with the scenario (a killed run leaks nothing), and is stopped, SIGTERM
// first, when the run ends.

#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace harness {

class Child {
public:
	Child() = default;
	~Child();
	Child(const Child &) = delete;
	Child &operator=(const Child &) = delete;

	// Runs argv[0], found along $PATH. False when it cannot be started.
	bool start(const std::vector<std::string> &argv);
	// Runs the test client meson names in the environment variable `env_name`
	// (for example CANVAS_LOCK_CLIENT).
	bool start_client(const char *env_name, const std::vector<std::string> &args = {});

	pid_t pid() const { return pid_; }
	// False once the child has exited.
	bool running();

	// Everything the child has printed so far.
	std::string_view said();
	// How many times it said `word`.
	int count(std::string_view word);
	// Whether it said `line` as a whole line, after offset `from` of what it said.
	bool said_line(std::string_view line, std::size_t from = 0);
	// What it said after offset `from`, on one line, for a failure's note.
	std::string since(std::size_t from);
	// Blocks, for startup outside the event loop, until the child has printed
	// `marker`; false when it exits first or `timeout_ms` passes.
	bool prints(std::string_view marker, int timeout_ms);

	void signal(int number);
	// SIGKILL, reaped before this returns.
	void kill();
	// SIGTERM, then SIGKILL when the child is still there after two seconds.
	void stop();

private:
	void drain();
	void close_pipe();

	pid_t pid_ = -1;
	int out_ = -1;
	std::string said_;
};

// Stops every child still running.
void stop_children();

} // namespace harness
