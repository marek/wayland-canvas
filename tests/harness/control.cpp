#include "harness/control.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <format>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "harness/files.h"
#include "platform/ipc_client.h"

namespace harness {

namespace {

constexpr int poll_ms = 400;
constexpr int give_up_ms = 10'000;

std::vector<pid_t> forked;

// The forked client's whole life; never returns.
[[noreturn]] void talk(std::string_view to_send, const std::string &out_path,
		ControlClient options) {
	int fd = connect_to_socket();
	FILE *out = std::fopen(out_path.c_str(), "w");
	if (fd < 0 || out == nullptr) {
		if (out != nullptr) {
			std::fclose(out);
		}
		_exit(125);
	}
	if (!write_request(fd, to_send)) {
		std::fclose(out);
		close(fd);
		_exit(124);
	}
	if (options.half_close) {
		shutdown(fd, SHUT_WR);
	}
	char buf[1024];
	int idle = 0;
	bool eof = false;
	// No cap on what is read: a large answer must be drained whole.
	for (int elapsed = 0; !eof && idle < options.quiet_polls && elapsed < give_up_ms;
			elapsed += poll_ms) {
		pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&waiting, 1, poll_ms) == 0) {
			idle++;
			continue;
		}
		idle = 0;
		if ((waiting.revents & (POLLERR | POLLNVAL)) != 0) {
			break;
		}
		ssize_t got = read(fd, buf, sizeof buf);
		if (got <= 0) {
			eof = true;
			break;
		}
		std::fwrite(buf, 1, static_cast<std::size_t>(got), out);
		std::fflush(out); // the scenario reads the file while this child lives
	}
	if (eof) {
		std::fputs("\nEOF\n", out);
	}
	std::fclose(out);
	close(fd);
	_exit(0);
}

} // namespace

pid_t fork_control_client(std::string_view to_send, std::string_view out,
		ControlClient options) {
	std::string out_path = std::format("{}/{}", scratch(), out);
	pid_t pid = fork();
	if (pid == 0) {
		talk(to_send, out_path, options);
	}
	if (pid > 0) {
		forked.push_back(pid);
	}
	return pid;
}

std::string control_answer(std::string_view out) {
	return read_text(std::format("{}/{}", scratch(), out));
}

bool control_client_done(pid_t pid) {
	if (pid <= 0) {
		return true;
	}
	pid_t got = waitpid(pid, nullptr, WNOHANG);
	if (got == 0) {
		return false;
	}
	std::erase(forked, pid);
	return true;
}

void stop_control_clients() {
	for (pid_t pid : forked) {
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
	}
	forked.clear();
}

} // namespace harness
