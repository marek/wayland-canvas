// A plugin's calls to the compositor's socket: answered, refused and held.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// Shuts the compositor's end of the shell's control-socket connection, so
// the shell's socket goes as it does when the compositor does. That end is
// the socket bound at $WAYLAND_CANVAS_SOCKET whose peer is the shell's
// process.
bool drop_shell_socket() {
	const char *path = std::getenv("WAYLAND_CANVAS_SOCKET");
	if (path == nullptr || bar_client() == nullptr) {
		return false;
	}
	pid_t shell_pid = 0;
	wl_client_get_credentials(bar_client(), &shell_pid, nullptr, nullptr);
	for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
		int fd = std::atoi(entry.path().filename().c_str());
		sockaddr_un address{};
		socklen_t address_size = sizeof address;
		ucred peer{};
		socklen_t peer_size = sizeof peer;
		if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &address_size) == 0
				&& address.sun_family == AF_UNIX && std::strcmp(address.sun_path, path) == 0
				&& getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) == 0
				&& peer.pid == shell_pid) {
			return shutdown(fd, SHUT_RDWR) == 0;
		}
	}
	return false;
}

} // namespace

std::vector<harness::Step> socket_steps() {
	return {
		{"socket_run_wait_calls", [] {
			run_msg("action.run", R"({"name":"canvas.scratch/wait"})", "wait-calls");
			return after(800);
		}},
		{"socket_two_answers_in_the_third", [] {
			std::string answers = read_storage("answers");
			if (auto wait = wait_for(answers.find("\"call\": \"capture.stop\"") != std::string::npos, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(answers.find("\"call\": \"subscribe\"") != std::string::npos
					&& answers.find("\"result\": true") != std::string::npos
					&& answers.find("\"error\": null") != std::string::npos,
				"a call the compositor answers gets its result and a null error");
			check(answers.find("\"error\": \"not recording\"") != std::string::npos,
				"a call the compositor refuses gets its error as text");
			check(answers.find("shortcut.capture") == std::string::npos,
				"the chord capture waits, unanswered, for a chord");
			check(drop_shell_socket(),
				"the compositor's end of the shell's control socket was found and shut");
			return after(600);
		}},
		{"socket_waiting_call_is_answered_with", [] {
			std::string answers = read_storage("answers");
			if (auto wait = wait_for(answers.find("\"call\": \"shortcut.capture\"") != std::string::npos, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(answers.find("\"call\": \"shortcut.capture\"") != std::string::npos
					&& answers.find("\"error\": \"disconnected\"") != std::string::npos,
				"the call in flight when the shell's socket dropped got the error "
				"\"disconnected\"");
			return after(3500); // the shell's reconnect timer is 2 s
		}},
		{"socket_subscribed_again", [] {
			run_msg("action.run",
				R"({"name":"canvas.scratch/mark","args":{"kind":"after-drop"}})",
				"mark-after-drop");
			return after(600);
		}},
		{"socket_shell_runs_socket_action", [] {
			std::string marks = read_storage("marks");
			if (auto wait = wait_for(marks.find("\"kind\": \"after-drop\"") != std::string::npos, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(marks.find("\"kind\": \"after-drop\"") != std::string::npos,
				"the shell reconnected to the compositor and subscribed again");
			return after(100);
		}},
	};
}

} // namespace bar
