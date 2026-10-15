// End-to-end control-IPC scenario: the socket the compositor exports, the
// `wayland-canvas msg` client driving the launcher, the overview and the
// session end through it. Writes only under $CANVAS_TMP (meson passes the
// repo's tmp/); meson passes the compositor binary as $WAYLAND_CANVAS_BIN.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform/ipc.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/wlr.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
uint32_t t = 90000;
pid_t grim_pid = -1;
std::string shot_dir;
std::string socket_path;

// Prints PASS/FAIL with the note; a failure counts and names its source line.
void check(bool cond, std::string_view note,
	std::source_location where = std::source_location::current()) {
	std::println("{}: {}", cond ? "PASS" : "FAIL", note);
	std::fflush(stdout);
	if (!cond) {
		failures++;
		std::println(stderr, "  at {}:{} in {}", where.file_name(), where.line(),
			where.function_name());
	}
}

void mkdir_p(const std::string &path) {
	for (std::size_t i = 1; i < path.size(); i++) {
		if (path[i] == '/') {
			mkdir(path.substr(0, i).c_str(), 0755);
		}
	}
	if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
		std::fprintf(stderr, "mkdir %s: %s\n", path.c_str(), std::strerror(errno));
	}
}

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

// The command line of the real client against this compositor's socket.
std::string client_cmd(std::string_view command) {
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	return std::format("\"{}\" msg {}", bin, command);
}

std::string read_file(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		return "";
	}
	std::string out;
	char buf[256];
	std::size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.append(buf, n);
	}
	std::fclose(f);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}

int run_step() {
	switch (step) {
	case 0: {
		const char *env_socket = std::getenv("WAYLAND_CANVAS_SOCKET");
		socket_path = env_socket != nullptr ? env_socket : "";
		struct stat st{};
		bool is_socket = stat(socket_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
		check(!socket_path.empty() && is_socket,
			std::format("the compositor exports a socket at $WAYLAND_CANVAS_SOCKET ({})",
				socket_path));
		return 200;
	}
	case 1:
		spawn(client_cmd("launcher").c_str());
		return 300;
	case 2:
		if (!server->launcher.is_open() && retries++ < 50) return -100;
		check(server->launcher.is_open(), "`msg launcher` opened the launcher");
		shot("launcher-open");
		return 500;
	case 3:
		spawn(client_cmd("launcher").c_str());
		return 300;
	case 4:
		if (server->launcher.is_open() && retries++ < 50) return -100;
		check(!server->launcher.is_open(), "`msg launcher` closed the open launcher");
		return 200;
	case 5:
		spawn(client_cmd("overview").c_str());
		return 300;
	case 6: {
		Output *output = server->output_under_cursor();
		bool overview = output && output->overview.active();
		if (!overview && retries++ < 50) return -100;
		check(overview, "`msg overview` entered the overview");
		shot("overview");
		return 500;
	}
	case 7:
		spawn(client_cmd("overview").c_str());
		return 300;
	case 8: {
		Output *output = server->output_under_cursor();
		bool normal = output && !output->overview.active();
		if (!normal && retries++ < 50) return -100;
		check(normal, "`msg overview` left the overview");
		return 200;
	}
	case 9: // the answer to an unknown command, captured from the real client
		spawn(std::format("\"{}\" msg nonsense > {}/unknown.out",
			std::getenv("WAYLAND_CANVAS_BIN"), shot_dir).c_str());
		return 300;
	case 10: {
		// Read in a later step: a blocking read here would stall the loop the
		// compositor answers on.
		std::string response = read_file(shot_dir + "/unknown.out");
		if (response.empty() && retries++ < 50) return -100;
		check(response.rfind("error", 0) == 0,
			std::format("an unknown command answers with an error ({})",
				response.empty() ? "no answer" : response));
		return 200;
	}
	case 11: // the compositor's own exit path, through the socket
		spawn(client_cmd("quit").c_str());
		return 300;
	default:
		std::println("{}: {} failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
		return -1; // `msg quit` already terminated the display
	}
}

int handle_timer() {
	if (grim_pid > 0) {
		if (waitpid(grim_pid, nullptr, WNOHANG) == 0) {
			wl_event_source_timer_update(timer, 30);
			return 0;
		}
		grim_pid = -1;
	}
	t += 1000;
	int delay = run_step();
	if (delay < -1) {
		wl_event_source_timer_update(timer, -delay);
		return 0;
	}
	if (delay == -1) {
		return 0;
	}
	step++;
	retries = 0;
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

} // namespace

int main() {
	wlr_log_init(WLR_ERROR, nullptr);
	if (std::getenv("WAYLAND_CANVAS_BIN") == nullptr) {
		std::fprintf(stderr, "ipc: $WAYLAND_CANVAS_BIN is not set (meson passes it)\n");
		return 2;
	}
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	shot_dir = std::format("{}/scenarios/ipc", root);
	mkdir_p(shot_dir);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *data) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display); // `msg quit` ends it
	wl_event_source_remove(timer);
	server.reset(); // releases the control socket
	check(access(socket_path.c_str(), F_OK) != 0,
		"the control socket was removed when the compositor exited");
	return failures != 0;
}
